// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bip300/producer.h>

#include <bip300/messages.h>
#include <bip300/rules.h>
#include <consensus/validation.h>
#include <hash.h>
#include <logging.h>
#include <streams.h>
#include <util/fs_helpers.h>
#include <util/translation.h>

#include <algorithm>

namespace bip300 {

namespace {

/** The format of the producer's file: the version, the settings and bundles,
 *  then a checksum (SHA256d) of it all.  */
constexpr uint8_t PRODUCER_FILE_VERSION{1};

/** A coinbase holding `messages`, to apply them as a block would.  */
CTransaction MessagesCoinbase(const std::vector<CScript>& messages)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{});
    for (const CScript& message : messages) tx.vout.emplace_back(0, message);
    return CTransaction{tx};
}

} // namespace

std::optional<AckPolicy> ParseAckPolicy(const std::string& name)
{
    if (name == "none") return AckPolicy::NONE;
    if (name == "new_slots") return AckPolicy::NEW_SLOTS;
    if (name == "all") return AckPolicy::ALL;
    return std::nullopt;
}

std::string AckPolicyName(AckPolicy policy)
{
    switch (policy) {
    case AckPolicy::NONE: return "none";
    case AckPolicy::NEW_SLOTS: return "new_slots";
    case AckPolicy::ALL: return "all";
    }
    assert(false);
}

std::optional<BundlePolicy> ParseBundlePolicy(const std::string& name)
{
    if (name == "none") return BundlePolicy::NONE;
    if (name == "known") return BundlePolicy::KNOWN;
    if (name == "all") return BundlePolicy::ALL;
    if (name == "alarm") return BundlePolicy::ALARM;
    return std::nullopt;
}

std::string BundlePolicyName(BundlePolicy policy)
{
    switch (policy) {
    case BundlePolicy::NONE: return "none";
    case BundlePolicy::KNOWN: return "known";
    case BundlePolicy::ALL: return "all";
    case BundlePolicy::ALARM: return "alarm";
    }
    assert(false);
}

Producer::Producer(fs::path file) : m_file{std::move(file)} {}

bool Producer::Load()
{
    if (m_file.empty()) return false;
    AutoFile file{fsbridge::fopen(m_file, "rb")};
    if (file.IsNull()) return false;
    Settings settings;
    std::map<uint256, KnownBundle> bundles;
    try {
        HashVerifier verifier{file};
        uint8_t version, ack_policy, bundle_policy;
        verifier >> version;
        if (version != PRODUCER_FILE_VERSION) {
            LogWarning("Not loading %s: it is for another version", fs::PathToString(m_file));
            return false;
        }
        verifier >> settings.proposals >> settings.sidechain_acks >> ack_policy >> settings.bundle_acks >> bundle_policy;
        if (ack_policy > uint8_t(AckPolicy::ALL) || bundle_policy > uint8_t(BundlePolicy::ALARM)) throw std::ios_base::failure("unknown policy");
        settings.ack_policy = AckPolicy{ack_policy};
        settings.bundle_policy = BundlePolicy{bundle_policy};
        uint64_t count;
        verifier >> count;
        for (uint64_t i{0}; i < count; ++i) {
            KnownBundle bundle;
            CMutableTransaction blinded;
            verifier >> bundle.slot >> TX_WITH_WITNESS(blinded) >> bundle.seen_pending;
            bundle.blinded = MakeTransactionRef(std::move(blinded));
            bundles.emplace(bundle.blinded->GetHash().ToUint256(), std::move(bundle));
        }
        uint256 checksum;
        file >> checksum;
        if (checksum != verifier.GetHash()) throw std::ios_base::failure("checksum mismatch");
    } catch (const std::exception& e) {
        LogWarning("Not loading %s: %s", fs::PathToString(m_file), e.what());
        return false;
    }
    LOCK(m_mutex);
    m_settings = std::move(settings);
    m_bundles = std::move(bundles);
    return true;
}

bool Producer::Save() const
{
    AssertLockHeld(m_mutex);
    if (m_file.empty()) return true;
    DataStream data;
    data << PRODUCER_FILE_VERSION << m_settings.proposals << m_settings.sidechain_acks << uint8_t(m_settings.ack_policy)
         << m_settings.bundle_acks << uint8_t(m_settings.bundle_policy) << uint64_t{m_bundles.size()};
    for (const auto& [_, bundle] : m_bundles) data << bundle.slot << TX_WITH_WITNESS(*bundle.blinded) << bundle.seen_pending;
    HashWriter hasher;
    hasher.write(std::span{data});
    data << hasher.GetHash();

    const fs::path tmp{m_file + ".new"};
    AutoFile file{fsbridge::fopen(tmp, "wb")};
    if (file.IsNull()) {
        LogWarning("Can't write %s", fs::PathToString(tmp));
        return false;
    }
    file.write(std::span{data});
    if (!file.Commit() || file.fclose() != 0 || !RenameOver(tmp, m_file)) {
        LogWarning("Can't write %s", fs::PathToString(m_file));
        fs::remove(tmp);
        return false;
    }
    return true;
}

void Producer::ProposeSidechain(uint8_t slot, std::vector<unsigned char> description)
{
    LOCK(m_mutex);
    const std::pair proposal{slot, std::move(description)};
    if (std::ranges::find(m_settings.proposals, proposal) == m_settings.proposals.end()) m_settings.proposals.push_back(proposal);
    Save();
}

void Producer::SetSidechainAck(uint8_t slot, const uint256& description_hash, bool ack)
{
    LOCK(m_mutex);
    if (ack) {
        m_settings.sidechain_acks.emplace(slot, description_hash);
    } else {
        m_settings.sidechain_acks.erase({slot, description_hash});
    }
    Save();
}

void Producer::SetAckPolicy(AckPolicy policy)
{
    LOCK(m_mutex);
    m_settings.ack_policy = policy;
    Save();
}

void Producer::SetBundleAck(uint8_t slot, const uint256& m6id, bool ack)
{
    LOCK(m_mutex);
    if (ack) {
        m_settings.bundle_acks.emplace(slot, m6id);
    } else {
        m_settings.bundle_acks.erase({slot, m6id});
    }
    Save();
}

void Producer::SetBundlePolicy(BundlePolicy policy)
{
    LOCK(m_mutex);
    m_settings.bundle_policy = policy;
    Save();
}

util::Result<uint256> Producer::AddBundle(uint8_t slot, const CTransactionRef& blinded)
{
    if (!blinded || !M6Value(*blinded)) return util::Error{Untranslated("not a blinded M6: it needs no inputs, and a first output OP_RETURN <total fee, 8 bytes BE> of value 0")};
    const uint256 m6id{blinded->GetHash().ToUint256()};
    LOCK(m_mutex);
    if (const auto it{m_bundles.find(m6id)}; it != m_bundles.end()) {
        if (it->second.slot != slot) return util::Error{Untranslated("the bundle is known for another slot")};
        return m6id;
    }
    m_bundles.emplace(m6id, KnownBundle{.slot = slot, .blinded = blinded, .seen_pending = false});
    Save();
    return m6id;
}

std::optional<Producer::KnownBundle> Producer::GetBundle(const uint256& m6id) const
{
    LOCK(m_mutex);
    const auto it{m_bundles.find(m6id)};
    if (it == m_bundles.end()) return std::nullopt;
    return it->second;
}

Producer::Settings Producer::GetSettings() const
{
    LOCK(m_mutex);
    return m_settings;
}

std::map<uint256, Producer::KnownBundle> Producer::GetBundles() const
{
    LOCK(m_mutex);
    return m_bundles;
}

Producer::BlockPart Producer::MakeBlockPart(const State& state, int height, const Consensus::SidechainParams& params, const ProposedLookup& proposed,
                                            int64_t max_payout_weight)
{
    LOCK(m_mutex);
    bool changed{false};
    BlockPart part;

    // M1: our proposals, until they activate
    changed |= std::erase_if(m_settings.proposals, [&](const auto& proposal) {
        const Sidechain* sidechain{state.FindSidechain(proposal.first)};
        return sidechain && sidechain->description_hash == DescriptionHash(proposal.second);
    }) > 0;
    for (const auto& [slot, description] : m_settings.proposals) {
        const uint256 hash{DescriptionHash(description)};
        if (std::ranges::any_of(state.proposals, [&](const Proposal& p) { return p.slot == slot && p.description_hash == hash; })) continue;
        part.messages.push_back(MakeCoinbaseMessageScript(bip300::ProposeSidechain{slot, description}));
    }

    // M2: one per slot, for an explicitly ACKed proposal first
    std::map<uint8_t, uint256> acks;
    for (const bool explicitly : {true, false}) {
        for (const Proposal& proposal : state.proposals) {
            if (acks.contains(proposal.slot)) continue;
            const bool used{state.FindSidechain(proposal.slot) != nullptr};
            const bool ack{explicitly ? m_settings.sidechain_acks.contains({proposal.slot, proposal.description_hash}) :
                                        m_settings.ack_policy == AckPolicy::ALL || (m_settings.ack_policy == AckPolicy::NEW_SLOTS && !used)};
            if (ack) acks.emplace(proposal.slot, proposal.description_hash);
        }
    }
    for (const auto& [slot, hash] : acks) part.messages.push_back(MakeCoinbaseMessageScript(AckSidechain{slot, hash}));

    // M3: the bundles we know, once; we forget them once they were pending
    for (auto it{m_bundles.begin()}; it != m_bundles.end();) {
        auto& [m6id, bundle]{*it};
        const auto pending{state.bundles.find(bundle.slot)};
        const bool is_pending{pending != state.bundles.end() && std::ranges::find(pending->second, m6id, &PendingBundle::m6id) != pending->second.end()};
        if (is_pending) {
            changed |= !bundle.seen_pending;
            bundle.seen_pending = true;
        } else if (bundle.seen_pending || proposed(bundle.slot, m6id)) {
            it = m_bundles.erase(it);
            changed = true;
            continue;
        } else if (pending != state.bundles.end()) {
            part.messages.push_back(MakeCoinbaseMessageScript(ProposeBundle{bundle.slot, m6id}));
        }
        ++it;
    }

    // M4, on the state the messages so far leave
    State voting{state};
    CoinbaseResult result;
    BlockValidationState vstate;
    if (!ApplyCoinbaseMessages(MessagesCoinbase(part.messages), height, params, proposed, voting, result, vstate)) {
        LogWarning("Building a block without sidechain messages: %s", vstate.ToString());
        if (changed) Save();
        return {};
    }
    std::vector<uint16_t> votes;
    int64_t payout_weight{0};
    bool votes_something{false};
    for (const uint8_t slot : voting.ActiveSlots()) {
        const std::vector<PendingBundle>& pending{voting.bundles.at(slot)};
        const auto first{[&](const auto& worth_it) -> std::optional<uint16_t> {
            const auto it{std::ranges::find_if(pending, worth_it)};
            if (it == pending.end() || it - pending.begin() >= AckBundles::ALARM) return std::nullopt;
            return uint16_t(it - pending.begin());
        }};
        std::optional<uint16_t> vote{first([&](const PendingBundle& b) { return m_settings.bundle_acks.contains({slot, b.m6id}); })};
        if (!vote) {
            switch (m_settings.bundle_policy) {
            case BundlePolicy::NONE: break;
            case BundlePolicy::KNOWN: vote = first([&](const PendingBundle& b) { return m_bundles.contains(b.m6id); }); break;
            case BundlePolicy::ALL: vote = first([](const PendingBundle&) { return true; }); break;
            case BundlePolicy::ALARM: vote = AckBundles::ALARM; break;
            }
        }
        // The vote taking a bundle over the threshold must come with its
        // payout: cast it only if we can make that
        if (vote && *vote != AckBundles::ALARM) {
            const PendingBundle& bundle{pending[*vote]};
            if (bundle.vote_count + 1 > params.withdrawal_bundle_inclusion_threshold &&
                height - bundle.proposal_height <= int64_t{params.withdrawal_bundle_max_age}) {
                const auto known{m_bundles.find(bundle.m6id)};
                const auto treasury{voting.treasuries.find(slot)};
                const CTransactionRef payout{known == m_bundles.end() ? nullptr : MakePayout(*known->second.blinded, slot)};
                if (!payout || treasury == voting.treasuries.end() || treasury->second.value < *M6Value(*known->second.blinded) ||
                    payout_weight + GetTransactionWeight(*payout) > max_payout_weight) {
                    vote.reset();
                } else {
                    payout_weight += GetTransactionWeight(*payout);
                }
            }
        }
        votes.push_back(vote.value_or(AckBundles::ABSTAIN));
        votes_something |= vote.has_value();
    }
    if (votes_something) part.messages.push_back(MakeCoinbaseMessageScript(MakeAckBundles(std::move(votes))));

    // The payouts the messages require
    State after{state};
    result = {};
    if (!ConnectCoinbase(MessagesCoinbase(part.messages), height, params, proposed, after, result, vstate)) {
        LogWarning("Building a block without sidechain messages: %s", vstate.ToString());
        if (changed) Save();
        return {};
    }
    for (const auto& [slot, m6id] : result.required_payouts) {
        const auto known{m_bundles.find(m6id)};
        if (known == m_bundles.end() || known->second.slot != slot) {
            LogWarning("Building a block without sidechain messages: bundle %s of slot %d is due but unknown", m6id.ToString(), slot);
            if (changed) Save();
            return {};
        }
        part.payouts.push_back(MakePayout(*known->second.blinded, slot));
    }
    if (changed) Save();
    return part;
}

} // namespace bip300
