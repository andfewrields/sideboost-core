// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_DEPOSIT_H
#define BITCOIN_DRIVECHAIN_DEPOSIT_H

#include <coins.h>
#include <undo.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

/**
 * Deposits from the parent chain (ECX), BIP300's M5.
 *
 * A deposit pays into this chain's treasury output on the parent chain, with
 * an OP_RETURN naming an address on this chain.  The parent chain's
 * bip300301_enforcer tracks them and numbers them from 0 in the order they
 * happen.  Each is credited here by a deposit transaction, placed right after
 * the coinbase:
 *
 *   input:   prevout   the treasury output the deposit created on the parent
 *                      chain (so every deposit transaction is unique)
 *            scriptSig OP_RETURN <parent block hash> <sequence number>
 *   output:  the deposited amount, to the address (or burned in an OP_RETURN
 *            if the address doesn't decode)
 *
 * The transaction is fully determined by the deposit, so a node checks it by
 * rebuilding it from what its own enforcer reports.  Deposits are credited in
 * sequence order with no gaps; the UTXO set records the next sequence number
 * in a coin no transaction can spend (see DepositState), so the usual undo
 * data rolls it back on a reorg.
 */
namespace drivechain {

/** A deposit into this chain's slot, as the parent chain's enforcer reports it.  */
struct Deposit {
    /** Position among all deposits into the slot, counting from 0.  */
    uint64_t sequence{0};
    /** The parent chain block containing the deposit.  */
    uint256 block_hash;
    /** The treasury output the deposit created on the parent chain.  */
    COutPoint outpoint;
    /** The OP_RETURN payload: an address on this chain, as text.  */
    std::vector<unsigned char> address;
    /** The amount deposited.  */
    CAmount value{0};

    bool operator==(const Deposit&) const = default;
};

/** The source of a node's information about the parent chain could not
 *  answer (unreachable, not synced, malformed reply).  Not a verdict on any
 *  block: validation waits and tries again.  */
struct DepositSourceError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/** What a node's view of the parent chain says about one parent block.  */
struct BlockDeposits {
    enum class Status {
        OK,
        /** Not known (yet): the view may be behind.  */
        UNKNOWN,
        /** Known, but not on the parent chain's best chain.  */
        NOT_IN_BEST_CHAIN,
        /** On the best chain, but with fewer confirmations than required.  */
        TOO_SHALLOW,
    };
    Status status;
    /** The deposits into the slot in this block (if OK).  */
    std::vector<Deposit> deposits;
};

/** What a node knows about deposits on the parent chain: its view through the
 *  parent chain's enforcer.  Methods throw DepositSourceError when they can't
 *  answer.  */
class DepositSource
{
public:
    virtual ~DepositSource() = default;

    /** What is known about the deposits into `slot` in parent block
     *  `block_hash`, which must have at least `min_depth` confirmations.  */
    virtual BlockDeposits GetBlockDeposits(uint8_t slot, const uint256& block_hash, int min_depth) = 0;

    /** The deposits into `slot` from sequence number `first_sequence` on, in
     *  order and without gaps, that have at least `min_depth` confirmations
     *  on the parent chain's best chain.  `hint` is the parent block of the
     *  deposit before `first_sequence` (or null), where the search can
     *  start.  */
    virtual std::vector<Deposit> GetNewDeposits(uint8_t slot, uint64_t first_sequence, const uint256& hint, int min_depth) = 0;
};

/** The output script a deposit to `address` pays: the address decoded on the
 *  current chain, or an OP_RETURN (burning the amount) if it doesn't decode.  */
CScript DepositPayoutScript(std::span<const unsigned char> address);

/** The deposit transaction that credits `deposit`.  */
CTransactionRef MakeDepositTransaction(const Deposit& deposit);

/** What a deposit transaction claims to credit.  */
struct DepositClaim {
    uint256 block_hash;
    uint64_t sequence;
};

/** The parent block and sequence number a deposit transaction claims, or
 *  nullopt if its scriptSig is malformed.  */
std::optional<DepositClaim> ParseDepositTransaction(const CTransaction& tx);

/** The deposit state recorded in the UTXO set: the next sequence number to
 *  credit, and the parent block of the last credited deposit (null before the
 *  first).  */
struct DepositState {
    uint64_t next_sequence{0};
    uint256 last_block;

    bool IsInitial() const { return next_sequence == 0 && last_block.IsNull(); }
};

/** Where the UTXO set keeps the deposit state.  No transaction creates it,
 *  and its script can't be satisfied.  */
const COutPoint& DepositStateOutpoint();

/** The coin recording `state`, created at `height`.  */
Coin MakeDepositStateCoin(const DepositState& state, int height);

/** The deposit state recorded in `coin`, or nullopt if `coin` is not a
 *  deposit state coin.  */
std::optional<DepositState> ParseDepositStateCoin(const Coin& coin);

/** The deposit state of `view` (the initial state if none is recorded).  */
DepositState ReadDepositState(const CCoinsViewCache& view);

/** Update `view` for deposit transaction `tx`, which credits the deposit with
 *  the next sequence number.  The previous state coin goes to `txundo` (as an
 *  initial-state coin if there was none).  */
void UpdateCoinsForDeposit(const CTransaction& tx, CCoinsViewCache& view, CTxUndo& txundo, int height);

/** The state coin deposit transaction `tx` created at `height`, given the one
 *  it replaced (its undo data).  */
Coin DepositStateCoinAfter(const CTransaction& tx, const Coin& undo, int height);

/** Undo UpdateCoinsForDeposit's change to the state coin (the outputs are
 *  removed separately).  Returns false if the view is inconsistent.  */
bool UndoDepositState(Coin&& undo, CCoinsViewCache& view);

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_DEPOSIT_H
