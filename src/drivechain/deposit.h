// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_DEPOSIT_H
#define BITCOIN_DRIVECHAIN_DEPOSIT_H

#include <coins.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <undo.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/**
 * Deposits from the parent chain (ECX), BIP300's M5.
 *
 * A deposit pays into this chain's treasury output on the parent chain, with
 * an OP_RETURN naming an address on this chain.  The parent chain's
 * bip300301_enforcer numbers the slot's treasury outputs from 0 in the order
 * they are created: by deposits, and by withdrawals (M6), which take numbers
 * too.  Each deposit is credited here by a deposit transaction, placed right
 * after the coinbase:
 *
 *   input:   prevout   the treasury output the deposit created on the parent
 *                      chain (so every deposit transaction is unique)
 *            scriptSig OP_RETURN <sequence number>
 *   output:  the deposited amount, to the address (or burned in an OP_RETURN
 *            if the address doesn't decode)
 *
 * The transaction is fully determined by the deposit, and not by the parent
 * block holding it, so a parent chain reorg that mines the deposit again
 * leaves it valid.  Nodes check deposit transactions against their view of
 * the parent chain (see parentchain.h).  Deposits are credited in sequence
 * order, skipping none; the UTXO set records the next sequence number in a
 * coin no transaction can spend (see DepositState), so the usual undo data
 * rolls it back on a reorg.
 */
namespace drivechain {

/** A deposit into this chain's slot on the parent chain.  */
struct Deposit {
    /** The treasury output's sequence number in the slot.  */
    uint64_t sequence{0};
    /** The treasury output the deposit created on the parent chain.  */
    COutPoint outpoint;
    /** The OP_RETURN payload: an address on this chain, as text.  */
    std::vector<unsigned char> address;
    /** The amount deposited.  */
    CAmount value{0};

    bool operator==(const Deposit&) const = default;
};

/** The output script a deposit to `address` pays: the address decoded on the
 *  current chain, or an OP_RETURN (burning the amount) if it doesn't decode.  */
CScript DepositPayoutScript(std::span<const unsigned char> address);

/** The deposit transaction that credits `deposit`.  */
CTransactionRef MakeDepositTransaction(const Deposit& deposit);

/** The sequence number a deposit transaction claims, or nullopt if its
 *  scriptSig is malformed.  */
std::optional<uint64_t> ParseDepositTransaction(const CTransaction& tx);

/** The deposit state recorded in the UTXO set: the sequence number from which
 *  the next deposit is credited.  */
struct DepositState {
    uint64_t next_sequence{0};

    bool IsInitial() const { return next_sequence == 0; }
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

/** Update `view` for deposit transaction `tx`.  The previous state coin goes
 *  to `txundo` (as an initial-state coin if there was none), so its height is
 *  that of the block that credited the previous deposit.  */
void UpdateCoinsForDeposit(const CTransaction& tx, CCoinsViewCache& view, CTxUndo& txundo, int height);

/** The state coin deposit transaction `tx` created at `height`.  */
Coin DepositStateCoinAfter(const CTransaction& tx, int height);

/** Undo UpdateCoinsForDeposit's change to the state coin (the outputs are
 *  removed separately).  Returns false if the view is inconsistent.  */
bool UndoDepositState(Coin&& undo, CCoinsViewCache& view);

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_DEPOSIT_H
