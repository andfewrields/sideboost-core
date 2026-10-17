// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/deposit.h>

#include <addresstype.h>
#include <crypto/common.h>
#include <hash.h>
#include <key_io.h>
#include <script/script.h>
#include <util/check.h>

#include <string>

namespace drivechain {

namespace {

std::vector<unsigned char> EncodeSequence(uint64_t sequence)
{
    std::vector<unsigned char> res(8);
    WriteLE64(res.data(), sequence);
    return res;
}

} // namespace

CScript DepositPayoutScript(std::span<const unsigned char> address)
{
    const CTxDestination dest{DecodeDestination(std::string{address.begin(), address.end()})};
    if (!IsValidDestination(dest)) return CScript() << OP_RETURN;
    return GetScriptForDestination(dest);
}

CTransactionRef MakeDepositTransaction(const Deposit& deposit)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 0;
    tx.vin.emplace_back(deposit.outpoint, CScript() << OP_RETURN << EncodeSequence(deposit.sequence));
    tx.vout.emplace_back(deposit.value, DepositPayoutScript(deposit.address));
    return MakeTransactionRef(std::move(tx));
}

std::optional<uint64_t> ParseDepositTransaction(const CTransaction& tx)
{
    if (!tx.IsDeposit()) return std::nullopt;
    const CScript& script{tx.vin[0].scriptSig};
    CScript::const_iterator pc{script.begin()};
    opcodetype op;
    std::vector<unsigned char> sequence;
    if (!script.GetOp(pc, op) || op != OP_RETURN) return std::nullopt;
    if (!script.GetOp(pc, op, sequence) || sequence.size() != 8) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    return ReadLE64(sequence.data());
}

const COutPoint& DepositStateOutpoint()
{
    static const COutPoint outpoint{
        Txid::FromUint256((HashWriter{} << std::string{"SideBoost deposit state"}).GetSHA256()), 0};
    return outpoint;
}

Coin MakeDepositStateCoin(const DepositState& state, int height)
{
    // Pushes the state, drops it and leaves false: no spend can succeed.
    CScript script{CScript() << EncodeSequence(state.next_sequence) << OP_DROP << OP_FALSE};
    return Coin{CTxOut{0, std::move(script)}, height, /*fCoinBaseIn=*/false};
}

std::optional<DepositState> ParseDepositStateCoin(const Coin& coin)
{
    const CScript& script{coin.out.scriptPubKey};
    CScript::const_iterator pc{script.begin()};
    opcodetype op;
    std::vector<unsigned char> sequence;
    if (coin.IsSpent() || coin.out.nValue != 0) return std::nullopt;
    if (!script.GetOp(pc, op, sequence) || sequence.size() != 8) return std::nullopt;
    if (!script.GetOp(pc, op) || op != OP_DROP) return std::nullopt;
    if (!script.GetOp(pc, op) || op != OP_FALSE) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    return DepositState{ReadLE64(sequence.data())};
}

DepositState ReadDepositState(const CCoinsViewCache& view)
{
    const Coin& coin{view.AccessCoin(DepositStateOutpoint())};
    if (coin.IsSpent()) return {};
    return *Assert(ParseDepositStateCoin(coin));
}

void UpdateCoinsForDeposit(const CTransaction& tx, CCoinsViewCache& view, CTxUndo& txundo, int height)
{
    Assert(tx.IsDeposit());
    txundo.vprevout.emplace_back();
    if (!view.SpendCoin(DepositStateOutpoint(), &txundo.vprevout.back())) {
        txundo.vprevout.back() = MakeDepositStateCoin({}, height);
    }
    view.AddCoin(DepositStateOutpoint(), DepositStateCoinAfter(tx, height), /*possible_overwrite=*/false);
    AddCoins(view, tx, height);
}

Coin DepositStateCoinAfter(const CTransaction& tx, int height)
{
    return MakeDepositStateCoin({*Assert(ParseDepositTransaction(tx)) + 1}, height);
}

bool UndoDepositState(Coin&& undo, CCoinsViewCache& view)
{
    const std::optional<DepositState> previous{ParseDepositStateCoin(undo)};
    if (!previous || !view.SpendCoin(DepositStateOutpoint())) return false;
    if (!previous->IsInitial()) view.AddCoin(DepositStateOutpoint(), std::move(undo), /*possible_overwrite=*/false);
    return true;
}

} // namespace drivechain
