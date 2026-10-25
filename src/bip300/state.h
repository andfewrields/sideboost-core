// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BIP300_STATE_H
#define BITCOIN_BIP300_STATE_H

#include <coins.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>
#include <undo.h>

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

/**
 * BIP300 and BIP301 for this chain's own sidechains: the state.
 *
 * The UTXO set holds it, in coins no transaction creates and no script can
 * spend, so flushing, assumeutxo and the UTXO set hash cover it.  A block's
 * changes to them are recorded, before and after, in its undo data
 * (CBlockUndo::vstate).  A chain where nothing happened has none.
 *
 * - Each slot's treasury: the coin at (SidechainTreasuryTxid(), slot), whose
 *   value is all the sidechain's coins, created by the first deposit.
 * - Each bundle ever proposed: a coin at ProposedBundleOutpoint().
 * - Sidechain proposals, active sidechains, each active slot's pending
 *   bundles and the previous block's resolved M4: serialized, in coins of up
 *   to MAX_CHUNK_SIZE bytes (StateTxid(), 0, 1, ...).
 */
namespace bip300 {

/** A proposal (M1) for a sidechain to take a slot.  Its description is in
 *  the coinbase of the block at `proposal_height`.  */
struct Proposal {
    uint8_t slot{0};
    uint256 description_hash;
    uint32_t vote_count{0};
    int32_t proposal_height{0};

    bool operator==(const Proposal&) const = default;
    SERIALIZE_METHODS(Proposal, obj) { READWRITE(obj.slot, obj.description_hash, obj.vote_count, obj.proposal_height); }
};

/** The sidechain active in a slot: the proposal that won it.  */
struct Sidechain {
    uint8_t slot{0};
    uint256 description_hash;
    uint32_t vote_count{0};
    int32_t proposal_height{0};
    int32_t activation_height{0};

    bool operator==(const Sidechain&) const = default;
    SERIALIZE_METHODS(Sidechain, obj) { READWRITE(obj.slot, obj.description_hash, obj.vote_count, obj.proposal_height, obj.activation_height); }
};

/** A bundle proposed (M3) for a slot, waiting for votes.  */
struct PendingBundle {
    uint256 m6id;
    uint32_t vote_count{0};
    int32_t proposal_height{0};

    bool operator==(const PendingBundle&) const = default;
    SERIALIZE_METHODS(PendingBundle, obj) { READWRITE(obj.m6id, obj.vote_count, obj.proposal_height); }
};

/** A slot's treasury.  */
struct Treasury {
    /** The sidechain's coins.  */
    CAmount value{0};
    /** The number of the next deposit or payout (both are numbered).  */
    uint64_t next_sequence{0};

    bool operator==(const Treasury&) const = default;
};

/** A vote an M4 cast in a slot, which REPEAT_PREVIOUS can cast again.  */
struct Vote {
    enum class Kind : uint8_t {
        UPVOTE = 0,
        ALARM = 1,
    };
    uint8_t slot{0};
    Kind kind{Kind::UPVOTE};
    /** UPVOTE: the bundle upvoted.  */
    uint256 m6id;

    bool operator==(const Vote&) const = default;
    SERIALIZE_METHODS(Vote, obj)
    {
        uint8_t kind{uint8_t(obj.kind)};
        READWRITE(obj.slot, kind, obj.m6id);
        SER_READ(obj, obj.kind = Kind{kind});
    }
};

struct State {
    /** Oldest first.  */
    std::vector<Proposal> proposals;
    std::map<uint8_t, Sidechain> sidechains;
    /** Each active slot's pending bundles, oldest first (M4 indexes them in
     *  that order).  */
    std::map<uint8_t, std::vector<PendingBundle>> bundles;
    std::map<uint8_t, Treasury> treasuries;
    /** The votes the previous block's M4 cast.  */
    std::vector<Vote> last_votes;
    /** The bundles (slot, M6ID) proposed while connecting a block, which
     *  get a coin each so they can't be proposed again (ReadState leaves it
     *  empty).  */
    std::vector<std::pair<uint8_t, uint256>> proposed;

    bool operator==(const State&) const = default;

    const Sidechain* FindSidechain(uint8_t slot) const;
    /** The active slots, in order: what M4's votes refer to.  */
    std::vector<uint8_t> ActiveSlots() const;
};

/** The largest piece of serialized state one coin holds.  */
static constexpr size_t MAX_CHUNK_SIZE{9'000};

/** The txid under which a piece of state is kept: proposals, sidechains,
 *  last votes, or the pending bundles of `slot`.  */
enum class Part : uint8_t {
    PROPOSALS,
    SIDECHAINS,
    LAST_VOTES,
    BUNDLES,
};
Txid StateTxid(Part part, uint8_t slot = 0);

/** Where the UTXO set records that bundle `m6id` was proposed in `slot`: a
 *  bundle is proposed only once (see rules.h).  */
COutPoint ProposedBundleOutpoint(uint8_t slot, const uint256& m6id);

/** The coin holding treasury `treasury` (changed at `height`).  */
Coin MakeTreasuryCoin(const Treasury& treasury, int height);
std::optional<Treasury> ParseTreasuryCoin(const Coin& coin);

/** The sidechain state of `view`, or nullopt if its state coins are corrupt.  */
std::optional<State> ReadState(const CCoinsViewCache& view);

/** Change `view` from state `before` (what it holds) to `after`, at block
 *  `height`.  Returns the coins changed, before and after.  */
std::vector<StateCoinChange> WriteState(CCoinsViewCache& view, const State& before, const State& after, int height);

/** Undo `changes` in `view`.  Returns false if the view doesn't hold their
 *  results.  */
bool UndoStateChanges(CCoinsViewCache& view, const std::vector<StateCoinChange>& changes);

} // namespace bip300

#endif // BITCOIN_BIP300_STATE_H
