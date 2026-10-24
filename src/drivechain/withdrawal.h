// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_WITHDRAWAL_H
#define BITCOIN_DRIVECHAIN_WITHDRAWAL_H

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <key_io.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>
#include <undo.h>
#include <util/chaintype.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

/**
 * Withdrawals to the parent chain (ECX), BIP300's M3 to M6.
 *
 * A withdrawal is an output of an ordinary transaction that names a payout
 * on the parent chain:
 *
 *   OP_RETURN <"SBWD"> <payout script> <parent chain fee, 8 bytes LE>
 *
 * Its value is the payout plus the fee.  Being an OP_RETURN, the output never
 * enters the UTXO set: the block puts the withdrawal at the tail of a queue
 * that the UTXO set holds instead, as coins no transaction can spend (see
 * QueueOutpoint).  Withdrawals keep their place in the queue until they are
 * paid out.
 *
 * Whenever no bundle is pending, the end of a block takes the withdrawals
 * waiting at the head of the queue, up to MAX_BUNDLE_WITHDRAWALS, into a
 * bundle: the M6 that pays them out of the parent chain's treasury.  Its M6ID
 * is the txid of the blinded M6 (see MakeBlindedM6), which nodes propose to
 * the parent chain's enforcer, and the parent chain's miners vote on.  The
 * enforcer reports when the bundle succeeds (its M6 is mined) or fails (it
 * expires); a block records that with a bundle outcome transaction right
 * after the deposits:
 *
 *   input:   prevout   (M6ID, 0)
 *            scriptSig OP_RETURN OP_1 (succeeded) or OP_RETURN OP_2 (failed)
 *   output:  0, OP_RETURN
 *
 * Nodes check it against their view of the parent chain, as deposits.  A
 * success removes the bundle's withdrawals from the queue (their coins have
 * left this chain); a failure puts them back at its head, so the next bundle
 * takes them again.
 *
 * A bundle's M6 commits to the block that created it.  Once the parent chain
 * has paid a bundle out, that block is a checkpoint: chains without it are
 * invalid (see ParentChainView::Checkpoint), so no reorg can undo the
 * withdrawals' removal from this chain after they were paid.  The queue's state is a coin too (WithdrawalState), so
 * the UTXO set, undo data and the coinstatsindex cover it all.
 */
namespace drivechain {

/** The largest payout script a withdrawal can name: a 40-byte witness
 *  program, the longest standard one.  */
static constexpr size_t MAX_PAYOUT_SCRIPT_SIZE{42};
/** Most withdrawals one bundle pays out.  Its M6 stays far below the parent
 *  chain's standard transaction weight, and the work per block small.  */
static constexpr uint32_t MAX_BUNDLE_WITHDRAWALS{1000};
/** How long nodes wait before proposing a bundle to the parent chain, after
 *  the block that created it.  A bundle's M6ID depends on that block, so a
 *  reorg replacing it makes a new bundle of the same withdrawals: had the old
 *  one been proposed, the parent chain could pay both out.  Waiting makes
 *  that need a reorg this deep.  */
static constexpr std::chrono::minutes BUNDLE_PROPOSAL_DELAY{20};

/** BUNDLE_PROPOSAL_DELAY in blocks (120 at 10 s).  */
int BundleProposalDepth(const Consensus::Params& consensus);

/** A payout on the parent chain.  Its value on this chain is payout + fee.  */
struct Withdrawal {
    CScript payout_script;
    CAmount payout{0};
    /** Paid to the parent chain's miners.  */
    CAmount fee{0};

    CAmount Value() const { return payout + fee; }
    bool operator==(const Withdrawal&) const = default;
};

/** The address format of the parent chain network this chain takes deposits
 *  from (Bitcoin's).  */
AddressFormat ParentAddressFormat(ChainType chain);

/** The output script requesting a withdrawal.  */
CScript MakeWithdrawalScript(const CScript& payout_script, CAmount fee);

/** Whether `script` claims to be a withdrawal (OP_RETURN <"SBWD">).  Such an
 *  output must be well-formed (ParseWithdrawal), or its transaction is
 *  invalid.  */
bool IsWithdrawalScript(const CScript& script);

/** The withdrawal `out` requests, or nullopt if it doesn't (well).  */
std::optional<Withdrawal> ParseWithdrawal(const CTxOut& out);

/** Where the UTXO set keeps the queue's withdrawal at `position`.  */
COutPoint QueueOutpoint(uint32_t position);

/** A queued withdrawal, and the output that requested it.  */
struct QueueEntry {
    COutPoint origin;
    Withdrawal withdrawal;
};

/** The coin holding a queued withdrawal, requested at `height`.  It holds the
 *  withdrawal's value; its script can't be satisfied.  */
Coin MakeQueueEntryCoin(const QueueEntry& entry, int height);
std::optional<QueueEntry> ParseQueueEntryCoin(const Coin& coin);

/** The bundle the queue's head was taken into.  */
struct PendingBundle {
    /** Withdrawals in the bundle: the queue's last `size` before its head.  */
    uint32_t size{0};
    /** The block that created it, whose hash makes its M6ID unique.  */
    int height{0};
    uint256 block_hash;

    bool operator==(const PendingBundle&) const = default;
};

/** The withdrawal queue, as recorded in the UTXO set.  */
struct WithdrawalState {
    /** The first withdrawal waiting for a bundle.  */
    uint32_t head{0};
    /** Where the next withdrawal goes.  */
    uint32_t tail{0};
    std::optional<PendingBundle> bundle;
    /** The last block recording a bundle outcome (-1 if none).  */
    int last_outcome_height{-1};

    bool IsInitial() const { return *this == WithdrawalState{}; }
    /** The queue position of the pending bundle's first withdrawal.  */
    uint32_t BundleStart() const { return head - bundle->size; }
    bool operator==(const WithdrawalState&) const = default;
};

/** Where the UTXO set keeps the withdrawal state.  */
const COutPoint& WithdrawalStateOutpoint();

/** The coin recording `state`.  Always at height 0, so a block's changes to
 *  it can be undone without undo data.  */
Coin MakeWithdrawalStateCoin(const WithdrawalState& state);
std::optional<WithdrawalState> ParseWithdrawalStateCoin(const Coin& coin);

/** The withdrawal state of `view` (the initial state if none is recorded).  */
WithdrawalState ReadWithdrawalState(const CCoinsViewCache& view);

/** The block that created a bundle, to which its M6 commits.  */
struct BundleCommitment {
    uint256 block_hash;
    int height{0};

    bool operator==(const BundleCommitment&) const = default;
};

/** The blinded M6 (BIP300) paying `withdrawals` in a bundle created by block
 *  `commitment`: no inputs, then outputs
 *    0: 0, OP_RETURN <total fee, 8 bytes BE>     (the treasury, once mined)
 *    1: 0, OP_RETURN <block hash, height 4 LE>   (makes the M6ID unique)
 *    2...: the payouts, in queue order
 *  Its txid is the bundle's M6ID.  */
CTransactionRef MakeBlindedM6(std::span<const Withdrawal> withdrawals, const BundleCommitment& commitment);

/** The block an M6 (blinded or not) commits to, if it's one of this chain's.  */
std::optional<BundleCommitment> ParseBundleCommitment(const CTransaction& m6);

/** The pending bundle's withdrawals, or nullopt if there's no bundle (or the
 *  view is inconsistent).  */
std::optional<std::vector<Withdrawal>> BundleWithdrawals(const CCoinsViewCache& view, const WithdrawalState& state);

/** The pending bundle's blinded M6, or nullptr if there's none.  */
CTransactionRef PendingBlindedM6(const CCoinsViewCache& view, const WithdrawalState& state);

/** How a bundle ended on the parent chain.  */
enum class BundleOutcome : uint8_t {
    SUCCEEDED = 1,
    FAILED = 2,
};

/** The transaction recording that bundle `m6id` ended with `outcome`.  */
CTransactionRef MakeBundleOutcomeTransaction(const uint256& m6id, BundleOutcome outcome);

/** The bundle and outcome a bundle outcome transaction records, or nullopt if
 *  it is malformed.  */
std::optional<std::pair<uint256, BundleOutcome>> ParseBundleOutcomeTransaction(const CTransaction& tx);

/** The index of `block`'s bundle outcome transaction, if it has one.  */
std::optional<size_t> BundleOutcomeIndex(const CBlock& block);

/** A coin a block's withdrawals add to the UTXO set, or remove from it.  */
struct CoinChange {
    COutPoint outpoint;
    Coin coin;
    bool added{false};
};

/** The queue entry at a position, for the withdrawals a success removes.  */
using EntryLookup = std::function<std::optional<Coin>(uint32_t position)>;

/** Apply the withdrawal effects of `block` (connected at `height`) to
 *  `state`: its bundle outcome, the withdrawals it requests, and the bundle
 *  its end creates.  Returns the coin changes, in order (the state coin's
 *  last), or nullopt if `state` or `entry` is inconsistent with the block.
 *  Doesn't check the outcome against the parent chain.  */
std::optional<std::vector<CoinChange>> BlockWithdrawalChanges(const CBlock& block, int height, const uint256& block_hash,
                                                              WithdrawalState& state, const EntryLookup& entry);

/** The withdrawal state before `block` (connected at `height`, with
 *  `blockundo`), given the state after it.  */
std::optional<WithdrawalState> WithdrawalStateBefore(const CBlock& block, const CBlockUndo& blockundo, int height, WithdrawalState after);

/** An EntryLookup for the withdrawals `block`'s success removed, from its
 *  undo data.  */
EntryLookup UndoEntryLookup(const CBlock& block, const CBlockUndo& blockundo, const WithdrawalState& before);

/** Apply `block`'s withdrawal effects to `view`.  The bundle outcome
 *  transaction's undo data (`outcome_undo`, if it has one) gets the state
 *  coin before, then the withdrawals a success removes.  Returns false if the
 *  view is inconsistent.  */
bool ConnectWithdrawals(const CBlock& block, int height, const uint256& block_hash, CCoinsViewCache& view, CTxUndo* outcome_undo);

/** Undo ConnectWithdrawals.  Returns false if the view is inconsistent.  */
bool DisconnectWithdrawals(const CBlock& block, const CBlockUndo& blockundo, int height, const uint256& block_hash, CCoinsViewCache& view);

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_WITHDRAWAL_H
