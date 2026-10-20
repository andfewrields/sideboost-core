// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_PARENTCHAIN_H
#define BITCOIN_DRIVECHAIN_PARENTCHAIN_H

#include <drivechain/deposit.h>
#include <drivechain/withdrawal.h>
#include <sync.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

class ChainstateManager;

/**
 * The node's view of the parent chain (ECX), for deposits and withdrawals.
 *
 * The view follows the parent chain's best chain through a DepositSource
 * (the parent chain's bip300301_enforcer) and keeps this chain's treasury
 * events on it, by sequence number, and its withdrawal bundles' events.
 * Validation checks deposit and bundle outcome transactions against it.  When
 * the parent chain reorgs and an event changes or disappears, blocks that
 * recorded events it no longer has are invalidated (reversibly, see
 * BLOCK_FAILED_PARENT_CHAIN), and made valid again if a later reorg brings
 * their events back.
 */
namespace drivechain {

/** Blocks on top of a parent chain block after which deposits and bundle
 *  outcomes in it are no longer checked again: a parent chain reorg deeper
 *  than this is not undone automatically.  */
static constexpr int DEPOSIT_FINALITY_DEPTH{100};

/** The source of a node's information about the parent chain could not
 *  answer (unreachable, not synced, malformed reply).  Not a verdict on any
 *  block: validation waits and tries again.  */
struct DepositSourceError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/** A parent chain block header.  */
struct ParentHeader {
    uint256 hash;
    uint256 prev;
    int height{0};
};

/** The creation of a treasury output of this chain's slot: a deposit or a
 *  withdrawal.  */
struct TreasuryEvent {
    uint64_t sequence{0};
    /** The parent chain block containing it.  */
    uint256 block_hash;
    int height{0};
    /** The deposit, or nullopt for a withdrawal.  */
    std::optional<Deposit> deposit;

    /** The same event, wherever it was mined.  */
    bool SameAs(const TreasuryEvent& other) const { return sequence == other.sequence && deposit == other.deposit; }
};

/** Something that happened to one of this chain's withdrawal bundles on the
 *  parent chain.  */
struct BundleEvent {
    enum class Kind {
        /** Proposed (M3).  */
        SUBMITTED,
        /** Paid out (M6).  */
        SUCCEEDED,
        /** Expired.  */
        FAILED,
    };

    uint256 m6id;
    Kind kind{Kind::SUBMITTED};
    /** The parent chain block it happened in.  */
    uint256 block_hash;
    int height{0};

    /** The same event, wherever it happened.  */
    bool SameAs(const BundleEvent& other) const { return m6id == other.m6id && kind == other.kind; }
};

/** A slot's events in a range of parent chain blocks, in chain order.  */
struct ParentEvents {
    std::vector<TreasuryEvent> treasury;
    std::vector<BundleEvent> bundles;
};

/** Raw access to the parent chain, as its enforcer serves it.  Methods throw
 *  DepositSourceError when they can't answer.  */
class DepositSource
{
public:
    virtual ~DepositSource() = default;

    /** The tip of the parent chain's best chain.  */
    virtual ParentHeader GetTip() = 0;

    /** The header of `hash` and up to `max_ancestors` of its ancestors,
     *  newest first.  Empty if `hash` is unknown.  */
    virtual std::vector<ParentHeader> GetHeaders(const uint256& hash, uint32_t max_ancestors) = 0;

    /** The events of `slot` in the blocks after `start` (or from genesis) up
     *  to and including `end`.  */
    virtual ParentEvents GetEvents(uint8_t slot, const std::optional<uint256>& start, const uint256& end) = 0;

    /** Have the parent chain's block producer propose (M3) a withdrawal
     *  bundle for `slot`.  */
    virtual void ProposeWithdrawalBundle(uint8_t slot, const CTransaction& blinded_m6) = 0;
};

/** What the view knows about the event a deposit or bundle outcome
 *  transaction records.  */
enum class DepositStatus {
    /** On the best chain with enough confirmations, and it matches.  */
    OK,
    /** Not on the best chain as far as the view knows: not seen yet, or gone
     *  in a reorg.  */
    UNKNOWN,
    /** On the best chain and matching, but with too few confirmations.  */
    TOO_SHALLOW,
    /** The view knows something else: another deposit or a withdrawal for
     *  the sequence number, or the other outcome for the bundle.  */
    MISMATCH,
};

class ParentChainView
{
public:
    ParentChainView(DepositSource& source, uint8_t slot, int min_depth);

    /** Catch up with the parent chain's best chain.  Throws
     *  DepositSourceError.  */
    void Update() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The lowest sequence number whose event an Update changed or removed
     *  since the last call (0 after the first Update, so a starting node
     *  checks what it credited before), or nullopt.  */
    std::optional<uint64_t> TakeChanges() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Whether an Update moved to a new tip since the last call.  */
    bool TakeTipChanged() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The parent chain tip the view is at (nullopt before an Update).  */
    std::optional<ParentHeader> Tip() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The first deposit with a sequence number at or after `sequence`
     *  (withdrawals are skipped), and whether it has enough confirmations.  */
    std::optional<std::pair<Deposit, bool>> NextDeposit(uint64_t sequence) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Whether deposit transaction `tx` credits the deposit the view has for
     *  its sequence number.  */
    DepositStatus CheckDeposit(const CTransaction& tx) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Whether the deposit with this sequence number is in a parent chain
     *  block DEPOSIT_FINALITY_DEPTH deep or deeper.  */
    bool IsFinal(uint64_t sequence) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The deposits from sequence number `sequence` on that can be credited
     *  now, in order: up to the first one without enough confirmations.  */
    std::vector<Deposit> ConfirmedDeposits(uint64_t sequence) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Whether an Update changed or removed a bundle event since the last
     *  call (true after the first Update).  */
    bool TakeBundleChanges() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** How bundle `m6id` ended (its first success or failure on the best
     *  chain), and whether that has enough confirmations.  */
    std::optional<std::pair<BundleOutcome, bool>> BundleOutcomeOf(const uint256& m6id) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Whether bundle outcome transaction `tx` records how its bundle ended.  */
    DepositStatus CheckBundleOutcome(const CTransaction& tx) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Whether bundle `m6id` ended in a parent chain block
     *  DEPOSIT_FINALITY_DEPTH deep or deeper.  */
    bool IsOutcomeFinal(const uint256& m6id) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Propose withdrawal bundle `blinded_m6` to the parent chain, unless it
     *  was proposed since the view last moved to a new tip, or the view knows
     *  how it ended (proposing it again could pay it out twice).  Throws
     *  DepositSourceError.  */
    void ProposeBundle(const CTransaction& blinded_m6) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    DepositSource& m_source;
    const uint8_t m_slot;
    const int m_min_depth;

    mutable Mutex m_mutex;
    std::optional<ParentHeader> m_tip GUARDED_BY(m_mutex);
    /** The recent part of the best chain, by height.  */
    std::map<int, uint256> m_headers GUARDED_BY(m_mutex);
    std::map<uint64_t, TreasuryEvent> m_events GUARDED_BY(m_mutex);
    /** In chain order.  */
    std::vector<BundleEvent> m_bundle_events GUARDED_BY(m_mutex);
    std::optional<uint64_t> m_changed GUARDED_BY(m_mutex);
    bool m_bundles_changed GUARDED_BY(m_mutex){false};
    bool m_tip_changed GUARDED_BY(m_mutex){false};
    /** The bundle last proposed, if the view hasn't moved since.  */
    std::optional<uint256> m_proposed GUARDED_BY(m_mutex);
    /** The bundle last logged as proposed.  */
    uint256 m_announced GUARDED_BY(m_mutex);

    int Depth(int height) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_tip->height - height; }
    /** The event ending bundle `m6id`, if any.  */
    const BundleEvent* Outcome(const uint256& m6id) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
};

/** Bring the node in line with its view of the parent chain: invalidate the
 *  active chain's blocks that record deposits or bundle outcomes the parent
 *  chain no longer has, make valid again blocks whose events are back, and
 *  propose the pending withdrawal bundle.  */
void SyncWithParentChain(ChainstateManager& chainman);

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_PARENTCHAIN_H
