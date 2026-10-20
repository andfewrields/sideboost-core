// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/withdrawal.h>

#include <crypto/common.h>
#include <hash.h>
#include <script/script.h>
#include <util/check.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace drivechain {

namespace {

constexpr std::array<unsigned char, 4> WITHDRAWAL_TAG{'S', 'B', 'W', 'D'};

std::vector<unsigned char> LE64(uint64_t value)
{
    std::vector<unsigned char> res(8);
    WriteLE64(res.data(), value);
    return res;
}

/** The next push of `script` at `pc`, if it is one.  */
std::optional<std::vector<unsigned char>> NextPush(const CScript& script, CScript::const_iterator& pc)
{
    opcodetype op;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, op, data) || op > OP_PUSHDATA4) return std::nullopt;
    return data;
}

bool NextOp(const CScript& script, CScript::const_iterator& pc, opcodetype expected)
{
    opcodetype op;
    return script.GetOp(pc, op) && op == expected;
}

/** A withdrawal's payout script and fee, checked against its value.  */
std::optional<Withdrawal> MakeWithdrawal(std::vector<unsigned char> payout_script, const std::vector<unsigned char>& fee, CAmount value)
{
    if (payout_script.empty() || payout_script.size() > MAX_PAYOUT_SCRIPT_SIZE || fee.size() != 8) return std::nullopt;
    const uint64_t fee_value{ReadLE64(fee.data())};
    if (!MoneyRange(value) || fee_value >= uint64_t(value)) return std::nullopt; // pays out at least 1
    return Withdrawal{
        .payout_script = CScript(payout_script.begin(), payout_script.end()),
        .payout = value - static_cast<CAmount>(fee_value),
        .fee = static_cast<CAmount>(fee_value),
    };
}

std::vector<unsigned char> SerializeState(const WithdrawalState& state)
{
    std::vector<unsigned char> res(13);
    WriteLE32(res.data(), state.head);
    WriteLE32(res.data() + 4, state.tail);
    WriteLE32(res.data() + 8, static_cast<uint32_t>(state.last_outcome_height));
    res[12] = state.bundle.has_value();
    if (state.bundle) {
        res.resize(53);
        WriteLE32(res.data() + 13, state.bundle->size);
        WriteLE32(res.data() + 17, static_cast<uint32_t>(state.bundle->height));
        std::copy(state.bundle->block_hash.begin(), state.bundle->block_hash.end(), res.begin() + 21);
    }
    return res;
}

/** Whether `tx` can request withdrawals: an ordinary transaction.  */
bool RequestsWithdrawals(const CTransaction& tx)
{
    return !tx.IsCoinBase() && !tx.IsParentChainEvent();
}

} // namespace

int BundleProposalDepth(const Consensus::Params& consensus)
{
    return std::max<int>(1, std::chrono::seconds{BUNDLE_PROPOSAL_DELAY} / consensus.PowTargetSpacing());
}

AddressFormat ParentAddressFormat(ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN:
        return {.pubkey_prefix = {0}, .script_prefix = {5}, .bech32_hrp = "bc"};
    case ChainType::REGTEST:
        return {.pubkey_prefix = {111}, .script_prefix = {196}, .bech32_hrp = "bcrt"};
    case ChainType::TESTNET:
    case ChainType::TESTNET4:
    case ChainType::SIGNET:
        return {.pubkey_prefix = {111}, .script_prefix = {196}, .bech32_hrp = "tb"};
    }
    assert(false);
}

CScript MakeWithdrawalScript(const CScript& payout_script, CAmount fee)
{
    return CScript() << OP_RETURN << std::vector<unsigned char>(WITHDRAWAL_TAG.begin(), WITHDRAWAL_TAG.end())
                     << std::vector<unsigned char>(payout_script.begin(), payout_script.end()) << LE64(fee);
}

bool IsWithdrawalScript(const CScript& script)
{
    return script.size() >= 2 + WITHDRAWAL_TAG.size() && script[0] == OP_RETURN && script[1] == WITHDRAWAL_TAG.size() &&
           std::equal(WITHDRAWAL_TAG.begin(), WITHDRAWAL_TAG.end(), script.begin() + 2);
}

std::optional<Withdrawal> ParseWithdrawal(const CTxOut& out)
{
    const CScript& script{out.scriptPubKey};
    if (!IsWithdrawalScript(script)) return std::nullopt;
    CScript::const_iterator pc{script.begin() + 2 + WITHDRAWAL_TAG.size()};
    auto payout_script{NextPush(script, pc)};
    const auto fee{NextPush(script, pc)};
    if (!payout_script || !fee || pc != script.end()) return std::nullopt;
    return MakeWithdrawal(std::move(*payout_script), *fee, out.nValue);
}

COutPoint QueueOutpoint(uint32_t position)
{
    static const Txid queue{Txid::FromUint256((HashWriter{} << std::string{"SideBoost withdrawal queue"}).GetSHA256())};
    return {queue, position};
}

Coin MakeQueueEntryCoin(const QueueEntry& entry, int height)
{
    const uint256 txid{entry.origin.hash.ToUint256()};
    std::vector<unsigned char> origin(txid.begin(), txid.end());
    origin.resize(36);
    WriteLE32(origin.data() + 32, entry.origin.n);
    // Pushes the withdrawal and where it came from, drops them and leaves
    // false: no spend can succeed.
    CScript script{CScript() << std::vector<unsigned char>(entry.withdrawal.payout_script.begin(), entry.withdrawal.payout_script.end())
                             << LE64(entry.withdrawal.fee) << origin << OP_2DROP << OP_DROP << OP_FALSE};
    return Coin{CTxOut{entry.withdrawal.Value(), std::move(script)}, height, /*fCoinBaseIn=*/false};
}

std::optional<QueueEntry> ParseQueueEntryCoin(const Coin& coin)
{
    if (coin.IsSpent()) return std::nullopt;
    const CScript& script{coin.out.scriptPubKey};
    CScript::const_iterator pc{script.begin()};
    auto payout_script{NextPush(script, pc)};
    const auto fee{NextPush(script, pc)};
    const auto origin{NextPush(script, pc)};
    if (!payout_script || !fee || !origin || origin->size() != 36) return std::nullopt;
    if (!NextOp(script, pc, OP_2DROP) || !NextOp(script, pc, OP_DROP) || !NextOp(script, pc, OP_FALSE) || pc != script.end()) return std::nullopt;
    auto withdrawal{MakeWithdrawal(std::move(*payout_script), *fee, coin.out.nValue)};
    if (!withdrawal) return std::nullopt;
    uint256 txid;
    std::copy(origin->begin(), origin->begin() + 32, txid.begin());
    return QueueEntry{
        .origin = COutPoint{Txid::FromUint256(txid), ReadLE32(origin->data() + 32)},
        .withdrawal = std::move(*withdrawal),
    };
}

const COutPoint& WithdrawalStateOutpoint()
{
    static const COutPoint outpoint{
        Txid::FromUint256((HashWriter{} << std::string{"SideBoost withdrawal state"}).GetSHA256()), 0};
    return outpoint;
}

Coin MakeWithdrawalStateCoin(const WithdrawalState& state)
{
    CScript script{CScript() << SerializeState(state) << OP_DROP << OP_FALSE};
    return Coin{CTxOut{0, std::move(script)}, /*nHeightIn=*/0, /*fCoinBaseIn=*/false};
}

std::optional<WithdrawalState> ParseWithdrawalStateCoin(const Coin& coin)
{
    if (coin.IsSpent() || coin.out.nValue != 0 || coin.nHeight != 0) return std::nullopt;
    const CScript& script{coin.out.scriptPubKey};
    CScript::const_iterator pc{script.begin()};
    const auto data{NextPush(script, pc)};
    if (!data || !NextOp(script, pc, OP_DROP) || !NextOp(script, pc, OP_FALSE) || pc != script.end()) return std::nullopt;
    if (data->size() != 13 && data->size() != 53) return std::nullopt;
    WithdrawalState state{
        .head = ReadLE32(data->data()),
        .tail = ReadLE32(data->data() + 4),
        .bundle = std::nullopt,
        .last_outcome_height = static_cast<int32_t>(ReadLE32(data->data() + 8)),
    };
    if ((*data)[12] != (data->size() == 53)) return std::nullopt;
    if (data->size() == 53) {
        uint256 block_hash;
        std::copy(data->begin() + 21, data->end(), block_hash.begin());
        PendingBundle bundle{.size = ReadLE32(data->data() + 13), .height = static_cast<int32_t>(ReadLE32(data->data() + 17)), .block_hash = block_hash};
        state.bundle = bundle;
    }
    if (state.head > state.tail || (state.bundle && (state.bundle->size == 0 || state.bundle->size > state.head))) return std::nullopt;
    return state;
}

WithdrawalState ReadWithdrawalState(const CCoinsViewCache& view)
{
    const Coin& coin{view.AccessCoin(WithdrawalStateOutpoint())};
    if (coin.IsSpent()) return {};
    return *Assert(ParseWithdrawalStateCoin(coin));
}

CTransactionRef MakeBlindedM6(std::span<const Withdrawal> withdrawals, const uint256& block_hash)
{
    CAmount fee{0};
    for (const Withdrawal& withdrawal : withdrawals) fee += withdrawal.fee;
    std::vector<unsigned char> fee_be(8);
    WriteBE64(fee_be.data(), fee);
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 0;
    tx.vout.emplace_back(0, CScript() << OP_RETURN << fee_be);
    tx.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>(block_hash.begin(), block_hash.end()));
    for (const Withdrawal& withdrawal : withdrawals) tx.vout.emplace_back(withdrawal.payout, withdrawal.payout_script);
    return MakeTransactionRef(std::move(tx));
}

std::optional<std::vector<Withdrawal>> BundleWithdrawals(const CCoinsViewCache& view, const WithdrawalState& state)
{
    if (!state.bundle) return std::nullopt;
    std::vector<Withdrawal> res;
    for (uint32_t position{state.BundleStart()}; position < state.head; ++position) {
        auto entry{ParseQueueEntryCoin(view.AccessCoin(QueueOutpoint(position)))};
        if (!entry) return std::nullopt;
        res.push_back(std::move(entry->withdrawal));
    }
    return res;
}

CTransactionRef PendingBlindedM6(const CCoinsViewCache& view, const WithdrawalState& state)
{
    const auto withdrawals{BundleWithdrawals(view, state)};
    if (!withdrawals) return nullptr;
    return MakeBlindedM6(*withdrawals, state.bundle->block_hash);
}

CTransactionRef MakeBundleOutcomeTransaction(const uint256& m6id, BundleOutcome outcome)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 0;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(m6id), 0}, CScript() << OP_RETURN << (outcome == BundleOutcome::SUCCEEDED ? OP_1 : OP_2));
    tx.vout.emplace_back(0, CScript() << OP_RETURN);
    return MakeTransactionRef(std::move(tx));
}

std::optional<std::pair<uint256, BundleOutcome>> ParseBundleOutcomeTransaction(const CTransaction& tx)
{
    if (!tx.IsBundleOutcome()) return std::nullopt;
    const uint256 m6id{tx.vin[0].prevout.hash.ToUint256()};
    const BundleOutcome outcome{tx.vin[0].scriptSig[1] == OP_1 ? BundleOutcome::SUCCEEDED : BundleOutcome::FAILED};
    if (MakeBundleOutcomeTransaction(m6id, outcome)->GetWitnessHash() != tx.GetWitnessHash()) return std::nullopt;
    return std::pair{m6id, outcome};
}

std::optional<size_t> BundleOutcomeIndex(const CBlock& block)
{
    size_t i{1};
    while (i < block.vtx.size() && block.vtx[i]->IsDeposit()) ++i;
    if (i < block.vtx.size() && block.vtx[i]->IsBundleOutcome()) return i;
    return std::nullopt;
}

std::optional<std::vector<CoinChange>> BlockWithdrawalChanges(const CBlock& block, int height, const uint256& block_hash,
                                                              WithdrawalState& state, const EntryLookup& entry)
{
    const WithdrawalState before{state};
    std::vector<CoinChange> changes;

    // The pending bundle's outcome
    if (const auto index{BundleOutcomeIndex(block)}) {
        const auto outcome{ParseBundleOutcomeTransaction(*block.vtx[*index])};
        if (!outcome || !state.bundle) return std::nullopt;
        if (outcome->second == BundleOutcome::SUCCEEDED) {
            // Paid out: the withdrawals leave this chain
            for (uint32_t position{state.BundleStart()}; position < state.head; ++position) {
                std::optional<Coin> coin{entry(position)};
                if (!coin || !ParseQueueEntryCoin(*coin)) return std::nullopt;
                changes.push_back({QueueOutpoint(position), std::move(*coin), /*added=*/false});
            }
        } else {
            // Expired: back to waiting, at the head of the queue
            state.head = state.BundleStart();
        }
        state.bundle.reset();
        state.last_outcome_height = height;
    }

    // Withdrawals requested by the block's transactions
    for (const CTransactionRef& tx : block.vtx) {
        if (!RequestsWithdrawals(*tx)) continue;
        for (uint32_t i{0}; i < tx->vout.size(); ++i) {
            auto withdrawal{ParseWithdrawal(tx->vout[i])};
            if (!withdrawal) continue;
            if (state.tail == std::numeric_limits<uint32_t>::max()) return std::nullopt;
            changes.push_back({QueueOutpoint(state.tail), MakeQueueEntryCoin({COutPoint{tx->GetHash(), i}, std::move(*withdrawal)}, height), /*added=*/true});
            ++state.tail;
        }
    }

    // A new bundle, if none is pending
    if (!state.bundle && state.head < state.tail) {
        const uint32_t size{std::min(state.tail - state.head, MAX_BUNDLE_WITHDRAWALS)};
        state.bundle = PendingBundle{.size = size, .height = height, .block_hash = block_hash};
        state.head += size;
    }

    if (state != before) {
        if (!before.IsInitial()) changes.push_back({WithdrawalStateOutpoint(), MakeWithdrawalStateCoin(before), /*added=*/false});
        if (!state.IsInitial()) changes.push_back({WithdrawalStateOutpoint(), MakeWithdrawalStateCoin(state), /*added=*/true});
    }
    return changes;
}

std::optional<WithdrawalState> WithdrawalStateBefore(const CBlock& block, const CBlockUndo& blockundo, int height, WithdrawalState after)
{
    if (const auto index{BundleOutcomeIndex(block)}) {
        // Its undo data starts with the state coin before
        if (*index > blockundo.vtxundo.size() || blockundo.vtxundo[*index - 1].vprevout.empty()) return std::nullopt;
        return ParseWithdrawalStateCoin(blockundo.vtxundo[*index - 1].vprevout[0]);
    }
    if (after.bundle && after.bundle->height == height) {
        after.head = after.BundleStart();
        after.bundle.reset();
    }
    uint32_t requested{0};
    for (const CTransactionRef& tx : block.vtx) {
        if (!RequestsWithdrawals(*tx)) continue;
        requested += std::ranges::count_if(tx->vout, [](const CTxOut& out) { return ParseWithdrawal(out).has_value(); });
    }
    if (after.tail - after.head < requested) return std::nullopt;
    after.tail -= requested;
    return after;
}

EntryLookup UndoEntryLookup(const CBlock& block, const CBlockUndo& blockundo, const WithdrawalState& before)
{
    const auto index{BundleOutcomeIndex(block)};
    if (!index || *index > blockundo.vtxundo.size() || !before.bundle) return [](uint32_t) { return std::nullopt; };
    const std::vector<Coin>& coins{blockundo.vtxundo[*index - 1].vprevout};
    const uint32_t start{before.BundleStart()};
    return [&coins, start](uint32_t position) -> std::optional<Coin> {
        if (position < start || position - start + 1 >= coins.size()) return std::nullopt;
        return coins[position - start + 1];
    };
}

bool ConnectWithdrawals(const CBlock& block, int height, const uint256& block_hash, CCoinsViewCache& view, CTxUndo* outcome_undo)
{
    WithdrawalState state{ReadWithdrawalState(view)};
    const WithdrawalState before{state};
    const auto changes{BlockWithdrawalChanges(block, height, block_hash, state, [&](uint32_t position) -> std::optional<Coin> {
        const Coin& coin{view.AccessCoin(QueueOutpoint(position))};
        if (coin.IsSpent()) return std::nullopt;
        return coin;
    })};
    if (!changes) return false;
    if (outcome_undo) {
        outcome_undo->vprevout.clear();
        outcome_undo->vprevout.push_back(MakeWithdrawalStateCoin(before));
        for (const CoinChange& change : *changes) {
            if (!change.added && change.outpoint != WithdrawalStateOutpoint()) outcome_undo->vprevout.push_back(change.coin);
        }
    }
    for (const CoinChange& change : *changes) {
        if (change.added) {
            view.AddCoin(change.outpoint, Coin{change.coin}, /*possible_overwrite=*/false);
        } else if (!view.SpendCoin(change.outpoint)) {
            return false;
        }
    }
    return true;
}

bool DisconnectWithdrawals(const CBlock& block, const CBlockUndo& blockundo, int height, const uint256& block_hash, CCoinsViewCache& view)
{
    const WithdrawalState after{ReadWithdrawalState(view)};
    const auto before{WithdrawalStateBefore(block, blockundo, height, after)};
    if (!before) return false;
    WithdrawalState state{*before};
    const auto changes{BlockWithdrawalChanges(block, height, block_hash, state, UndoEntryLookup(block, blockundo, *before))};
    if (!changes || state != after) return false;
    for (auto it{changes->rbegin()}; it != changes->rend(); ++it) {
        if (!it->added) {
            view.AddCoin(it->outpoint, Coin{it->coin}, /*possible_overwrite=*/false);
        } else if (!view.SpendCoin(it->outpoint)) {
            return false;
        }
    }
    return true;
}

} // namespace drivechain
