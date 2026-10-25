// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BIP300_RULES_H
#define BITCOIN_BIP300_RULES_H

#include <bip300/messages.h>
#include <bip300/state.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <serialize.h>
#include <uint256.h>

#include <functional>
#include <map>
#include <set>
#include <span>
#include <utility>
#include <vector>

/**
 * BIP300 and BIP301 for this chain's own sidechains: the block rules.
 *
 * A block's coinbase is applied first, its messages in output order:
 *
 * - M1 adds a proposal, unless one for the same slot and description exists.
 * - M2 ACKs a proposal made in an earlier block (others are ignored); one
 *   with more ACKs than its threshold, at most its max age old, activates,
 *   replacing the slot's sidechain if any (its treasury and pending bundles
 *   stay).
 * - M3 adds a pending bundle with 1 vote, for an active slot.  A bundle can
 *   be proposed only once in a slot (BIP300 allows proposing a failed one
 *   again, which could pay it out after its sidechain refunded it).
 * - M4 votes: an upvote adds one to a bundle and takes one off each other
 *   bundle of the slot that has any; an alarm takes one off each.  Its votes
 *   are recorded for the next block's REPEAT_PREVIOUS.
 * - M7 commits to a sidechain block, at most once per slot.
 *
 * Then proposals that can no longer activate, and bundles older than their
 * max age, fail.  A bundle left with more votes than its threshold must be
 * paid out by the block (and is only payable then).
 *
 * The block's other transactions follow, in order: deposits add to their
 * slot's treasury, payouts take from it, BMM requests must match an M7 and
 * be made on the block's parent, at most one per slot.  Each deposit and
 * payout takes the slot's next sequence number.
 */
namespace bip300 {

/** Whether bundle `m6id` was ever proposed in `slot`, before the block
 *  (ProposedBundleOutpoint).  */
using ProposedLookup = std::function<bool(uint8_t slot, const uint256& m6id)>;

/** What happened to the sidechains in a block, besides its BMM: what
 *  sidechains learn from it (through RPCs).  */
struct BlockEvents {
    struct Deposit {
        uint8_t slot{0};
        uint64_t sequence{0};
        /** The deposit output.  */
        COutPoint outpoint;
        CAmount value{0};

        bool operator==(const Deposit&) const = default;
        SERIALIZE_METHODS(Deposit, obj) { READWRITE(obj.slot, obj.sequence, obj.outpoint, obj.value); }
    };
    struct Payout {
        uint8_t slot{0};
        uint256 m6id;
        uint64_t sequence{0};
        Txid txid;

        bool operator==(const Payout&) const = default;
        SERIALIZE_METHODS(Payout, obj) { READWRITE(obj.slot, obj.m6id, obj.sequence, obj.txid); }
    };

    /** New proposals (slot, description hash): M1s not ignored.  */
    std::vector<std::pair<uint8_t, uint256>> proposals;
    /** Slots a sidechain activated in (replacing any).  */
    std::vector<uint8_t> activations;
    /** Bundles (slot, M6ID) proposed (M3), failed (expired), paid out.  */
    std::vector<std::pair<uint8_t, uint256>> submitted;
    std::vector<std::pair<uint8_t, uint256>> failed;
    std::vector<Payout> payouts;
    std::vector<Deposit> deposits;

    bool empty() const
    {
        return proposals.empty() && activations.empty() && submitted.empty() && failed.empty() && payouts.empty() && deposits.empty();
    }
    bool operator==(const BlockEvents&) const = default;
    SERIALIZE_METHODS(BlockEvents, obj) { READWRITE(obj.proposals, obj.activations, obj.submitted, obj.failed, obj.payouts, obj.deposits); }
};

/** What applying a coinbase found out, for its block's transactions.  */
struct CoinbaseResult {
    /** M7s: the sidechain block each slot's commits to.  */
    std::map<uint8_t, uint256> bmm_accepts;
    /** The bundles (slot, M6ID) the block must pay out.  */
    std::set<std::pair<uint8_t, uint256>> required_payouts;
    BlockEvents events;
};

/** Apply `coinbase`, of the block at `height`, to `state`: its messages
 *  (ApplyCoinbaseMessages), then the failures (FinishCoinbase).  */
bool ConnectCoinbase(const CTransaction& coinbase, int height, const Consensus::SidechainParams& params, const ProposedLookup& proposed,
                     State& state, CoinbaseResult& result, BlockValidationState& vstate);

/** Apply the messages of `coinbase` to `state`.  */
bool ApplyCoinbaseMessages(const CTransaction& coinbase, int height, const Consensus::SidechainParams& params, const ProposedLookup& proposed,
                           State& state, CoinbaseResult& result, BlockValidationState& vstate);

/** Fail the proposals and bundles that can no longer succeed, after the
 *  messages of the block at `height`, and find the bundles it must pay out.  */
void FinishCoinbase(int height, const Consensus::SidechainParams& params, State& state, CoinbaseResult& result);

/** Apply the block's transaction `tx` (not its coinbase) to `state`.
 *  `prev_block` is the block's parent; `bmm_slots` the slots whose BMM
 *  request the block already has.  */
bool ConnectTransaction(const CTransaction& tx, const uint256& prev_block, State& state, CoinbaseResult& result,
                        std::set<uint8_t>& bmm_slots, BlockValidationState& vstate);

/** Apply `block` (at `height`) to `state`, checking its sidechain rules.  */
bool ConnectBlock(const CBlock& block, int height, const Consensus::SidechainParams& params, const ProposedLookup& proposed,
                  State& state, BlockValidationState& vstate, BlockEvents* events = nullptr);

/** Whether a proposal with `vote_count` ACKs, `age` blocks old, activates in
 *  a slot that is `used` or not.  */
bool Activates(const Consensus::SidechainParams& params, bool used, uint32_t vote_count, int age);

/** The votes `m4` casts on `state` (whose slots and pending bundles it
 *  refers to), or nullopt if it is invalid there.  */
std::optional<std::vector<Vote>> ResolveVotes(const AckBundles& m4, const State& state);

/** Cast `votes` on `state`'s pending bundles.  */
void CastVotes(std::span<const Vote> votes, State& state);

} // namespace bip300

#endif // BITCOIN_BIP300_RULES_H
