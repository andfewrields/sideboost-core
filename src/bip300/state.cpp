// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bip300/state.h>

#include <crypto/common.h>
#include <hash.h>
#include <script/script.h>
#include <streams.h>

#include <algorithm>
#include <set>
#include <string>

namespace bip300 {

namespace {

using Bytes = std::vector<unsigned char>;

/** `obj` serialized, or nothing if it is empty (so an empty part has no
 *  coins).  */
template <typename T>
Bytes ToBytes(const T& obj)
{
    if (obj.empty()) return {};
    DataStream stream;
    stream << obj;
    const auto data{std::as_bytes(std::span{stream})};
    return {reinterpret_cast<const unsigned char*>(data.data()), reinterpret_cast<const unsigned char*>(data.data()) + data.size()};
}

template <typename T>
std::optional<T> FromBytes(const Bytes& bytes)
{
    T obj;
    if (bytes.empty()) return obj;
    try {
        SpanReader reader{bytes};
        reader >> obj;
        if (!reader.empty()) return std::nullopt;
    } catch (const std::ios_base::failure&) {
        return std::nullopt;
    }
    return obj;
}

/** The data `coin` holds: <data> OP_DROP OP_FALSE.  */
std::optional<Bytes> ParseDataCoin(const Coin& coin)
{
    const CScript& script{coin.out.scriptPubKey};
    CScript::const_iterator pc{script.begin()};
    opcodetype op;
    Bytes data;
    if (!script.GetOp(pc, op, data) || op > OP_PUSHDATA4) return std::nullopt;
    if (!script.GetOp(pc, op) || op != OP_DROP || !script.GetOp(pc, op) || op != OP_FALSE || pc != script.end()) return std::nullopt;
    return data;
}

Coin MakeDataCoin(std::span<const unsigned char> data, CAmount value, int height)
{
    return Coin{CTxOut{value, CScript() << Bytes(data.begin(), data.end()) << OP_DROP << OP_FALSE}, height, /*fCoinBaseIn=*/false};
}

std::optional<Bytes> ReadPart(const CCoinsViewCache& view, const Txid& txid)
{
    Bytes res;
    for (uint32_t i{0};; ++i) {
        const Coin& coin{view.AccessCoin(COutPoint{txid, i})};
        if (coin.IsSpent()) return res;
        const auto chunk{ParseDataCoin(coin)};
        if (!chunk || chunk->empty() || coin.out.nValue != 0) return std::nullopt;
        res.insert(res.end(), chunk->begin(), chunk->end());
    }
}

template <typename T>
std::optional<T> Read(const CCoinsViewCache& view, const Txid& txid)
{
    const auto bytes{ReadPart(view, txid)};
    if (!bytes) return std::nullopt;
    return FromBytes<T>(*bytes);
}

bool SameCoin(const Coin& a, const Coin& b)
{
    return a.out == b.out && a.nHeight == b.nHeight && a.IsCoinBase() == b.IsCoinBase();
}

/** Record and make the change of the coin at `outpoint` to `after`.  */
void ChangeCoin(CCoinsViewCache& view, const COutPoint& outpoint, Coin after, std::vector<StateCoinChange>& changes)
{
    Coin before;
    if (const Coin& coin{view.AccessCoin(outpoint)}; !coin.IsSpent()) before = coin;
    if (!before.IsSpent()) view.SpendCoin(outpoint);
    if (!after.IsSpent()) view.AddCoin(outpoint, Coin{after}, /*possible_overwrite=*/false);
    changes.push_back({outpoint, std::move(before), std::move(after)});
}

void WritePart(CCoinsViewCache& view, const Txid& txid, const Bytes& before, const Bytes& after, int height, std::vector<StateCoinChange>& changes)
{
    if (before == after) return;
    const auto chunk{[](const Bytes& bytes, size_t i) -> std::span<const unsigned char> {
        const size_t start{std::min(bytes.size(), i * MAX_CHUNK_SIZE)};
        return std::span{bytes}.subspan(start, std::min(MAX_CHUNK_SIZE, bytes.size() - start));
    }};
    const size_t chunks{(std::max(before.size(), after.size()) + MAX_CHUNK_SIZE - 1) / MAX_CHUNK_SIZE};
    for (size_t i{0}; i < chunks; ++i) {
        const auto old_chunk{chunk(before, i)};
        const auto new_chunk{chunk(after, i)};
        if (std::ranges::equal(old_chunk, new_chunk)) continue;
        ChangeCoin(view, COutPoint{txid, uint32_t(i)}, new_chunk.empty() ? Coin{} : MakeDataCoin(new_chunk, 0, height), changes);
    }
}

} // namespace

const Sidechain* State::FindSidechain(uint8_t slot) const
{
    const auto it{sidechains.find(slot)};
    return it == sidechains.end() ? nullptr : &it->second;
}

std::vector<uint8_t> State::ActiveSlots() const
{
    std::vector<uint8_t> res;
    for (const auto& [slot, _] : sidechains) res.push_back(slot);
    return res;
}

Txid StateTxid(Part part, uint8_t slot)
{
    HashWriter hasher{};
    hasher << std::string{"SideBoost sidechain state"} << uint8_t(part);
    if (part == Part::BUNDLES) hasher << slot;
    return Txid::FromUint256(hasher.GetSHA256());
}

COutPoint ProposedBundleOutpoint(uint8_t slot, const uint256& m6id)
{
    return COutPoint{Txid::FromUint256((HashWriter{} << std::string{"SideBoost sidechain bundle"} << slot << m6id).GetSHA256()), 0};
}

Coin MakeTreasuryCoin(const Treasury& treasury, int height)
{
    Bytes sequence(8);
    WriteLE64(sequence.data(), treasury.next_sequence);
    return MakeDataCoin(sequence, treasury.value, height);
}

std::optional<Treasury> ParseTreasuryCoin(const Coin& coin)
{
    if (coin.IsSpent() || !MoneyRange(coin.out.nValue)) return std::nullopt;
    const auto data{ParseDataCoin(coin)};
    if (!data || data->size() != 8) return std::nullopt;
    return Treasury{.value = coin.out.nValue, .next_sequence = ReadLE64(data->data())};
}

std::optional<State> ReadState(const CCoinsViewCache& view)
{
    State state;
    auto proposals{Read<std::vector<Proposal>>(view, StateTxid(Part::PROPOSALS))};
    auto sidechains{Read<std::map<uint8_t, Sidechain>>(view, StateTxid(Part::SIDECHAINS))};
    auto last_votes{Read<std::vector<Vote>>(view, StateTxid(Part::LAST_VOTES))};
    if (!proposals || !sidechains || !last_votes) return std::nullopt;
    state.proposals = std::move(*proposals);
    state.sidechains = std::move(*sidechains);
    state.last_votes = std::move(*last_votes);
    for (const auto& [slot, sidechain] : state.sidechains) {
        if (sidechain.slot != slot) return std::nullopt;
        auto bundles{Read<std::vector<PendingBundle>>(view, StateTxid(Part::BUNDLES, slot))};
        if (!bundles) return std::nullopt;
        state.bundles.emplace(slot, std::move(*bundles));
        const Coin& coin{view.AccessCoin(COutPoint{SidechainTreasuryTxid(), slot})};
        if (coin.IsSpent()) continue;
        const auto treasury{ParseTreasuryCoin(coin)};
        if (!treasury) return std::nullopt;
        state.treasuries.emplace(slot, *treasury);
    }
    return state;
}

std::vector<StateCoinChange> WriteState(CCoinsViewCache& view, const State& before, const State& after, int height)
{
    std::vector<StateCoinChange> changes;
    WritePart(view, StateTxid(Part::PROPOSALS), ToBytes(before.proposals), ToBytes(after.proposals), height, changes);
    WritePart(view, StateTxid(Part::SIDECHAINS), ToBytes(before.sidechains), ToBytes(after.sidechains), height, changes);
    WritePart(view, StateTxid(Part::LAST_VOTES), ToBytes(before.last_votes), ToBytes(after.last_votes), height, changes);
    std::set<uint8_t> slots;
    for (const auto& [slot, _] : before.bundles) slots.insert(slot);
    for (const auto& [slot, _] : after.bundles) slots.insert(slot);
    for (const uint8_t slot : slots) {
        const auto old_it{before.bundles.find(slot)};
        const auto new_it{after.bundles.find(slot)};
        WritePart(view, StateTxid(Part::BUNDLES, slot), old_it == before.bundles.end() ? Bytes{} : ToBytes(old_it->second),
                  new_it == after.bundles.end() ? Bytes{} : ToBytes(new_it->second), height, changes);
    }
    slots.clear();
    for (const auto& [slot, _] : before.treasuries) slots.insert(slot);
    for (const auto& [slot, _] : after.treasuries) slots.insert(slot);
    for (const uint8_t slot : slots) {
        const auto old_it{before.treasuries.find(slot)};
        const auto new_it{after.treasuries.find(slot)};
        const std::optional<Treasury> old_treasury{old_it == before.treasuries.end() ? std::nullopt : std::optional{old_it->second}};
        const std::optional<Treasury> new_treasury{new_it == after.treasuries.end() ? std::nullopt : std::optional{new_it->second}};
        if (old_treasury == new_treasury) continue;
        ChangeCoin(view, COutPoint{SidechainTreasuryTxid(), slot}, new_treasury ? MakeTreasuryCoin(*new_treasury, height) : Coin{}, changes);
    }
    for (size_t i{before.proposed.size()}; i < after.proposed.size(); ++i) {
        const auto& [slot, m6id]{after.proposed[i]};
        ChangeCoin(view, ProposedBundleOutpoint(slot, m6id), MakeDataCoin({}, 0, height), changes);
    }
    return changes;
}

bool UndoStateChanges(CCoinsViewCache& view, const std::vector<StateCoinChange>& changes)
{
    for (auto it{changes.rbegin()}; it != changes.rend(); ++it) {
        const Coin& current{view.AccessCoin(it->outpoint)};
        if (it->after.IsSpent() ? !current.IsSpent() : !SameCoin(current, it->after)) return false;
        if (!it->after.IsSpent()) view.SpendCoin(it->outpoint);
        if (!it->before.IsSpent()) view.AddCoin(it->outpoint, Coin{it->before}, /*possible_overwrite=*/false);
    }
    return true;
}

} // namespace bip300
