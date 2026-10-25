// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bip300/rules.h>

#include <tinyformat.h>
#include <util/overloaded.h>

#include <algorithm>
#include <limits>

namespace bip300 {

namespace {

bool Invalid(BlockValidationState& vstate, const std::string& reason, const std::string& debug = "")
{
    return vstate.Invalid(BlockValidationResult::BLOCK_CONSENSUS, reason, debug);
}

bool HasPositiveVotes(const std::vector<PendingBundle>& bundles)
{
    return std::ranges::any_of(bundles, [](const PendingBundle& bundle) { return bundle.vote_count > 0; });
}

/** Proposals that can no longer activate fail: past their max age, or with
 *  so many blocks not ACKing them that the rest of it can't make up.  */
bool ProposalFailed(const Consensus::SidechainParams& params, bool used, const Proposal& proposal, int height)
{
    const uint32_t max_age{used ? params.used_slot_proposal_max_age : params.unused_slot_proposal_max_age};
    const uint32_t threshold{used ? params.used_slot_activation_threshold : params.unused_slot_activation_threshold};
    const uint32_t max_fails{max_age > threshold ? max_age - threshold : 0};
    const uint32_t age{static_cast<uint32_t>(std::max(0, height - proposal.proposal_height))};
    const uint32_t fails{age > proposal.vote_count ? age - proposal.vote_count : 0};
    return age > max_age || (age > max_fails && fails >= max_fails);
}

} // namespace

bool Activates(const Consensus::SidechainParams& params, bool used, uint32_t vote_count, int age)
{
    if (used) return vote_count > params.used_slot_activation_threshold && age <= int64_t{params.used_slot_proposal_max_age};
    return vote_count > params.unused_slot_activation_threshold && age <= int64_t{params.unused_slot_proposal_max_age};
}

std::optional<std::vector<Vote>> ResolveVotes(const AckBundles& m4, const State& state)
{
    std::vector<Vote> res;
    const auto upvote{[&](uint8_t slot, const PendingBundle& bundle) {
        if (bundle.vote_count < std::numeric_limits<uint32_t>::max()) res.push_back({slot, Vote::Kind::UPVOTE, bundle.m6id});
    }};
    const auto alarm{[&](uint8_t slot, const std::vector<PendingBundle>& bundles) {
        if (HasPositiveVotes(bundles)) res.push_back({slot, Vote::Kind::ALARM, uint256{}});
    }};
    switch (m4.version) {
    case AckBundles::Version::ONE_BYTE:
    case AckBundles::Version::TWO_BYTES: {
        const std::vector<uint8_t> slots{state.ActiveSlots()};
        if (m4.votes.size() != slots.size()) return std::nullopt;
        // Two bytes only when an index needs them
        if (m4.version == AckBundles::Version::TWO_BYTES &&
            std::ranges::all_of(m4.votes, [](uint16_t vote) { return vote <= AckBundles::MAX_ONE_BYTE_INDEX; })) {
            return std::nullopt;
        }
        for (size_t i{0}; i < slots.size(); ++i) {
            const std::vector<PendingBundle>& bundles{state.bundles.at(slots[i])};
            const uint16_t vote{m4.votes[i]};
            if (vote == AckBundles::ABSTAIN) continue;
            if (vote == AckBundles::ALARM) {
                alarm(slots[i], bundles);
                continue;
            }
            if (vote >= bundles.size()) return std::nullopt;
            upvote(slots[i], bundles[vote]);
        }
        return res;
    }
    case AckBundles::Version::LEADING_BY_50:
        for (const auto& [slot, bundles] : state.bundles) {
            if (bundles.empty()) continue;
            const auto leader{std::ranges::max_element(bundles, {}, &PendingBundle::vote_count)};
            uint32_t runner_up{0};
            for (auto it{bundles.begin()}; it != bundles.end(); ++it) {
                if (it != leader) runner_up = std::max(runner_up, it->vote_count);
            }
            if (leader->vote_count - runner_up >= AckBundles::LEADING_BY_MARGIN) upvote(slot, *leader);
        }
        return res;
    case AckBundles::Version::REPEAT_PREVIOUS:
        for (const Vote& vote : state.last_votes) {
            const auto it{state.bundles.find(vote.slot)};
            if (it == state.bundles.end()) continue;
            if (vote.kind == Vote::Kind::ALARM) {
                alarm(vote.slot, it->second);
                continue;
            }
            // Cast again only while the bundle is pending
            const auto bundle{std::ranges::find(it->second, vote.m6id, &PendingBundle::m6id)};
            if (bundle != it->second.end()) upvote(vote.slot, *bundle);
        }
        return res;
    }
    return std::nullopt;
}

void CastVotes(std::span<const Vote> votes, State& state)
{
    for (const Vote& vote : votes) {
        for (PendingBundle& bundle : state.bundles.at(vote.slot)) {
            if (vote.kind == Vote::Kind::UPVOTE && bundle.m6id == vote.m6id) {
                ++bundle.vote_count;
            } else if (bundle.vote_count > 0) {
                --bundle.vote_count;
            }
        }
    }
}

bool ConnectCoinbase(const CTransaction& coinbase, int height, const Consensus::SidechainParams& params, const ProposedLookup& proposed,
                     State& state, CoinbaseResult& result, BlockValidationState& vstate)
{
    if (!ApplyCoinbaseMessages(coinbase, height, params, proposed, state, result, vstate)) return false;
    FinishCoinbase(height, params, state, result);
    return true;
}

bool ApplyCoinbaseMessages(const CTransaction& coinbase, int height, const Consensus::SidechainParams& params, const ProposedLookup& proposed,
                           State& state, CoinbaseResult& result, BlockValidationState& vstate)
{
    std::set<std::pair<uint8_t, uint256>> proposals;
    std::set<uint8_t> acked;
    std::optional<std::vector<Vote>> votes;
    for (const CTxOut& out : coinbase.vout) {
        if (IsDepositScript(out.scriptPubKey)) return Invalid(vstate, "bad-cb-sidechain-deposit", "deposit in a coinbase");
        const auto message{ParseCoinbaseMessage(out.scriptPubKey)};
        if (!message) continue;
        const bool ok{std::visit(util::Overloaded{
            [&](const ProposeSidechain& m1) {
                const uint256 hash{DescriptionHash(m1.description)};
                if (!proposals.emplace(m1.slot, hash).second) {
                    return Invalid(vstate, "bad-cb-sidechain-proposal-duplicate", strprintf("slot %d proposed twice with description hash %s", m1.slot, hash.ToString()));
                }
                // Proposing it again would reset its ACKs
                if (std::ranges::none_of(state.proposals, [&](const Proposal& p) { return p.slot == m1.slot && p.description_hash == hash; })) {
                    state.proposals.push_back({.slot = m1.slot, .description_hash = hash, .vote_count = 0, .proposal_height = height});
                    result.events.proposals.emplace_back(m1.slot, hash);
                }
                return true;
            },
            [&](const AckSidechain& m2) {
                if (!acked.insert(m2.slot).second) {
                    return Invalid(vstate, "bad-cb-sidechain-ack-duplicate", strprintf("slot %d ACKed twice", m2.slot));
                }
                const auto it{std::ranges::find_if(state.proposals, [&](const Proposal& p) { return p.slot == m2.slot && p.description_hash == m2.description_hash; })};
                // Only proposals of earlier blocks count
                if (it == state.proposals.end() || it->proposal_height == height) return true;
                ++it->vote_count;
                if (Activates(params, state.FindSidechain(m2.slot) != nullptr, it->vote_count, height - it->proposal_height)) {
                    state.sidechains[m2.slot] = {
                        .slot = it->slot,
                        .description_hash = it->description_hash,
                        .vote_count = it->vote_count,
                        .proposal_height = it->proposal_height,
                        .activation_height = height,
                    };
                    state.bundles.try_emplace(m2.slot);
                    state.proposals.erase(it);
                    result.events.activations.push_back(m2.slot);
                }
                return true;
            },
            [&](const ProposeBundle& m3) {
                const auto bundles{state.bundles.find(m3.slot)};
                if (bundles == state.bundles.end()) {
                    return Invalid(vstate, "bad-cb-bundle-inactive-slot", strprintf("bundle %s proposed for inactive slot %d", m3.m6id.ToString(), m3.slot));
                }
                if (std::ranges::find(bundles->second, m3.m6id, &PendingBundle::m6id) != bundles->second.end()) {
                    return Invalid(vstate, "bad-cb-bundle-duplicate", strprintf("bundle %s already pending in slot %d", m3.m6id.ToString(), m3.slot));
                }
                if (proposed(m3.slot, m3.m6id) || std::ranges::find(state.proposed, std::pair{m3.slot, m3.m6id}) != state.proposed.end()) {
                    return Invalid(vstate, "bad-cb-bundle-reproposed", strprintf("bundle %s was proposed in slot %d before", m3.m6id.ToString(), m3.slot));
                }
                bundles->second.push_back({.m6id = m3.m6id, .vote_count = 1, .proposal_height = height});
                state.proposed.emplace_back(m3.slot, m3.m6id);
                result.events.submitted.emplace_back(m3.slot, m3.m6id);
                return true;
            },
            [&](const AckBundles& m4) {
                if (votes) return Invalid(vstate, "bad-cb-bundle-votes-duplicate", "more than one M4");
                votes = ResolveVotes(m4, state);
                if (!votes) return Invalid(vstate, "bad-cb-bundle-votes", "M4 votes don't match the active slots' pending bundles");
                CastVotes(*votes, state);
                return true;
            },
            [&](const BmmAccept& m7) {
                if (!result.bmm_accepts.emplace(m7.slot, m7.hash).second) {
                    return Invalid(vstate, "bad-cb-bmm-accept-duplicate", strprintf("slot %d BMM accepted twice", m7.slot));
                }
                return true;
            },
        }, *message)};
        if (!ok) return false;
    }
    state.last_votes = votes ? std::move(*votes) : std::vector<Vote>{};
    return true;
}

void FinishCoinbase(int height, const Consensus::SidechainParams& params, State& state, CoinbaseResult& result)
{
    std::erase_if(state.proposals, [&](const Proposal& proposal) {
        return ProposalFailed(params, state.FindSidechain(proposal.slot) != nullptr, proposal, height);
    });
    for (auto& [slot, bundles] : state.bundles) {
        std::erase_if(bundles, [&](const PendingBundle& bundle) {
            if (height - bundle.proposal_height <= int64_t{params.withdrawal_bundle_max_age}) return false;
            result.events.failed.emplace_back(slot, bundle.m6id);
            return true;
        });
        for (const PendingBundle& bundle : bundles) {
            if (bundle.vote_count > params.withdrawal_bundle_inclusion_threshold) result.required_payouts.emplace(slot, bundle.m6id);
        }
    }
}

bool ConnectTransaction(const CTransaction& tx, const uint256& prev_block, State& state, CoinbaseResult& result,
                        std::set<uint8_t>& bmm_slots, BlockValidationState& vstate)
{
    // Parent chain events credit what happened on ECX, nothing of ours
    if (tx.IsParentChainEvent()) return true;

    if (tx.IsSidechainPayout()) {
        const uint8_t slot{static_cast<uint8_t>(tx.vin[0].prevout.n)};
        const uint256 m6id{ComputeM6Id(tx)};
        if (!tx.vin[0].scriptSig.empty() || !tx.vin[0].scriptWitness.IsNull()) {
            return Invalid(vstate, "bad-txns-sidechain-payout", strprintf("payout %s spends the treasury with a script", tx.GetHash().ToString()));
        }
        if (!result.required_payouts.erase({slot, m6id})) {
            return Invalid(vstate, "bad-txns-sidechain-payout-unapproved", strprintf("bundle %s of slot %d has no approval to pay out in this block", m6id.ToString(), slot));
        }
        const auto fee{ParseM6Fee(tx)};
        if (!fee) return Invalid(vstate, "bad-txns-sidechain-payout", strprintf("payout %s without its fee output", tx.GetHash().ToString()));
        if (std::ranges::any_of(tx.vout, [](const CTxOut& out) { return IsDepositScript(out.scriptPubKey); })) {
            return Invalid(vstate, "bad-txns-sidechain-deposit", "deposit in a payout");
        }
        const CAmount value{*fee + tx.GetValueOut()};
        Treasury& treasury{state.treasuries[slot]};
        if (!MoneyRange(value) || treasury.value < value) {
            return Invalid(vstate, "bad-txns-sidechain-payout-treasury", strprintf("payout of %d from slot %d's treasury of %d", value, slot, treasury.value));
        }
        treasury.value -= value;
        result.events.payouts.push_back({.slot = slot, .m6id = m6id, .sequence = treasury.next_sequence++, .txid = tx.GetHash()});
        std::erase_if(state.bundles.at(slot), [&](const PendingBundle& bundle) { return bundle.m6id == m6id; });
        return true;
    }

    for (uint32_t i{0}; i < tx.vout.size(); ++i) {
        const CTxOut& out{tx.vout[i]};
        if (!IsDepositScript(out.scriptPubKey)) continue;
        const auto deposit{ParseDeposit(out)};
        if (!deposit) return Invalid(vstate, "bad-txns-sidechain-deposit", strprintf("malformed deposit in %s", tx.GetHash().ToString()));
        if (!state.FindSidechain(deposit->slot)) {
            return Invalid(vstate, "bad-txns-sidechain-deposit-inactive", strprintf("deposit to inactive slot %d in %s", deposit->slot, tx.GetHash().ToString()));
        }
        Treasury& treasury{state.treasuries[deposit->slot]};
        if (!MoneyRange(treasury.value + out.nValue)) return Invalid(vstate, "bad-txns-sidechain-deposit", "treasury out of range");
        treasury.value += out.nValue;
        result.events.deposits.push_back({.slot = deposit->slot, .sequence = treasury.next_sequence++, .outpoint = COutPoint{tx.GetHash(), i}, .value = out.nValue});
    }

    if (const auto request{ParseBmmRequest(tx)}) {
        if (request->prev_block != prev_block) {
            return Invalid(vstate, "bad-txns-bmm-request-prev", strprintf("BMM request %s made on another block", tx.GetHash().ToString()));
        }
        const auto accept{result.bmm_accepts.find(request->slot)};
        if (accept == result.bmm_accepts.end() || accept->second != request->hash) {
            return Invalid(vstate, "bad-txns-bmm-request-unaccepted", strprintf("BMM request %s without its M7", tx.GetHash().ToString()));
        }
        if (!bmm_slots.insert(request->slot).second) {
            return Invalid(vstate, "bad-txns-bmm-request-duplicate", strprintf("second BMM request for slot %d", request->slot));
        }
    }
    return true;
}

bool ConnectBlock(const CBlock& block, int height, const Consensus::SidechainParams& params, const ProposedLookup& proposed,
                  State& state, BlockValidationState& vstate, BlockEvents* events)
{
    if (block.vtx.empty()) return Invalid(vstate, "bad-cb-missing");
    CoinbaseResult result;
    if (!ConnectCoinbase(*block.vtx[0], height, params, proposed, state, result, vstate)) return false;
    std::set<uint8_t> bmm_slots;
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        if (!ConnectTransaction(*block.vtx[i], block.hashPrevBlock, state, result, bmm_slots, vstate)) return false;
    }
    if (!result.required_payouts.empty()) {
        const auto& [slot, m6id]{*result.required_payouts.begin()};
        return Invalid(vstate, "bad-blk-sidechain-payout-missing", strprintf("bundle %s of slot %d reached its votes but isn't paid out", m6id.ToString(), slot));
    }
    if (events) *events = std::move(result.events);
    return true;
}

} // namespace bip300
