// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/parentchain.h>

#include <chain.h>
#include <consensus/validation.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <undo.h>
#include <util/check.h>
#include <util/string.h>
#include <validation.h>

#include <algorithm>
#include <limits>

namespace drivechain {

namespace {

/** How much of the parent chain's best chain the view remembers, to find
 *  where a reorg forks off.  A deeper reorg reloads the view.  */
constexpr int HEADER_WINDOW{1000};

} // namespace

ParentChainView::ParentChainView(DepositSource& source, uint8_t slot, int min_depth)
    : m_source{source}, m_slot{slot}, m_min_depth{min_depth} {}

void ParentChainView::Update()
{
    LOCK(m_mutex);
    const ParentHeader tip{m_source.GetTip()};
    if (m_tip && m_tip->hash == tip.hash) return;

    // Walk the new best chain back to a block the view knows, collecting the
    // headers above it
    std::optional<int> fork;
    std::vector<ParentHeader> fresh;
    if (m_tip && !m_headers.empty()) {
        const int lowest{m_headers.begin()->first};
        std::vector<ParentHeader> pending{tip};
        uint32_t batch(std::max(1, tip.height - m_tip->height + 1));
        for (size_t i{0}; !fork;) {
            if (i == pending.size()) {
                const ParentHeader last{pending.back()};
                if (last.height <= lowest) break; // forks off below the window
                const auto more{m_source.GetHeaders(last.hash, std::min<uint32_t>(batch, last.height - lowest))};
                if (more.size() < 2 || more[1].hash != last.prev) throw DepositSourceError("cannot walk the parent chain");
                pending.insert(pending.end(), more.begin() + 1, more.end());
                batch *= 2;
            }
            const ParentHeader& header{pending[i++]};
            if (const auto it{m_headers.find(header.height)}; it != m_headers.end() && it->second == header.hash) {
                fork = header.height;
            } else {
                fresh.push_back(header);
            }
        }
    }

    std::map<uint64_t, TreasuryEvent> previous;
    std::vector<TreasuryEvent> added;
    if (fork) {
        added = m_source.GetTreasuryEvents(m_slot, m_headers.at(*fork), tip.hash);
        for (auto it{m_events.begin()}; it != m_events.end();) {
            if (it->second.height > *fork) {
                previous.insert(*it);
                it = m_events.erase(it);
            } else {
                ++it;
            }
        }
        m_headers.erase(m_headers.upper_bound(*fork), m_headers.end());
        for (const ParentHeader& header : fresh) m_headers[header.height] = header.hash;
    } else {
        added = m_source.GetTreasuryEvents(m_slot, std::nullopt, tip.hash);
        previous = std::move(m_events);
        m_events.clear();
        m_headers.clear();
        for (const ParentHeader& header : m_source.GetHeaders(tip.hash, HEADER_WINDOW)) m_headers[header.height] = header.hash;
    }
    for (TreasuryEvent& event : added) {
        const uint64_t sequence{event.sequence};
        m_events[sequence] = std::move(event);
    }
    while (!m_headers.empty() && m_headers.begin()->first < tip.height - HEADER_WINDOW) m_headers.erase(m_headers.begin());

    const bool first{!m_tip};
    m_tip = tip;
    m_tip_changed = true;
    if (first) {
        m_changed = 0;
        return;
    }
    // The lowest sequence number whose event is gone or different
    for (const auto& [sequence, event] : previous) {
        const auto it{m_events.find(sequence)};
        if (it == m_events.end() || !it->second.SameAs(event)) {
            m_changed = std::min(m_changed.value_or(sequence), sequence);
            break;
        }
    }
}

std::optional<uint64_t> ParentChainView::TakeChanges()
{
    LOCK(m_mutex);
    return std::exchange(m_changed, std::nullopt);
}

bool ParentChainView::TakeTipChanged()
{
    LOCK(m_mutex);
    return std::exchange(m_tip_changed, false);
}

std::optional<ParentHeader> ParentChainView::Tip() const
{
    LOCK(m_mutex);
    return m_tip;
}

std::optional<std::pair<Deposit, bool>> ParentChainView::NextDeposit(uint64_t sequence) const
{
    LOCK(m_mutex);
    if (!m_tip) return std::nullopt;
    for (auto it{m_events.lower_bound(sequence)}; it != m_events.end(); ++it) {
        if (it->second.deposit) return std::pair{*it->second.deposit, Depth(it->second) >= m_min_depth};
    }
    return std::nullopt;
}

DepositStatus ParentChainView::CheckDeposit(const CTransaction& tx) const
{
    const std::optional<uint64_t> sequence{ParseDepositTransaction(tx)};
    if (!sequence) return DepositStatus::MISMATCH;
    LOCK(m_mutex);
    const auto it{m_events.find(*sequence)};
    if (it == m_events.end()) return DepositStatus::UNKNOWN;
    if (!it->second.deposit || MakeDepositTransaction(*it->second.deposit)->GetWitnessHash() != tx.GetWitnessHash()) {
        return DepositStatus::MISMATCH;
    }
    if (Depth(it->second) < m_min_depth) return DepositStatus::TOO_SHALLOW;
    return DepositStatus::OK;
}

bool ParentChainView::IsFinal(uint64_t sequence) const
{
    LOCK(m_mutex);
    const auto it{m_events.find(sequence)};
    return it != m_events.end() && Depth(it->second) >= DEPOSIT_FINALITY_DEPTH;
}

std::vector<Deposit> ParentChainView::ConfirmedDeposits(uint64_t sequence) const
{
    LOCK(m_mutex);
    std::vector<Deposit> res;
    if (!m_tip) return res;
    for (auto it{m_events.lower_bound(sequence)}; it != m_events.end(); ++it) {
        if (!it->second.deposit) continue;
        if (Depth(it->second) < m_min_depth) break;
        res.push_back(*it->second.deposit);
    }
    return res;
}

namespace {

/** The deposit transactions of a block: right after the coinbase.  */
std::span<const CTransactionRef> Deposits(const CBlock& block)
{
    size_t end{1};
    while (end < block.vtx.size() && block.vtx[end]->IsDeposit()) ++end;
    return std::span{block.vtx}.subspan(1, end - 1);
}

} // namespace

void SyncWithParentChain(ChainstateManager& chainman)
{
    ParentChainView* const parent{chainman.m_options.parent_chain};
    if (!parent) return;
    try {
        parent->Update();
    } catch (const DepositSourceError& e) {
        LogDebug(BCLog::VALIDATION, "Can't follow the parent chain: %s", e.what());
        return;
    }
    const bool tip_changed{parent->TakeTipChanged()};
    const std::optional<uint64_t> changed{parent->TakeChanges()};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    if (tip_changed) {
        LogDebug(BCLog::VALIDATION, "Following the parent chain to tip %s (deposits changed from: %s)",
                 Assert(parent->Tip())->hash.ToString(), changed ? util::ToString(*changed) : "none");
    }

    if (tip_changed) {
        // Blocks the parent chain made invalid whose deposits it has again
        LOCK(cs_main);
        bool reconsidered{false};
        for (auto& [_, index] : chainman.m_blockman.m_block_index) {
            if (!(index.nStatus & BLOCK_FAILED_PARENT_CHAIN)) continue;
            CBlock block;
            if (!chainman.m_blockman.ReadBlock(block, index)) continue;
            if (!std::ranges::all_of(Deposits(block), [&](const auto& tx) { return parent->CheckDeposit(*tx) == DepositStatus::OK; })) continue;
            LogInfo("The parent chain has the deposits of block %s again", index.GetBlockHash().ToString());
            chainstate.SetParentChainFailure(index, false);
            chainstate.ResetBlockFailureFlags(&index);
            reconsidered = true;
        }
        if (reconsidered) chainman.RecalculateBestHeader();
    }

    if (changed) {
        // Find the earliest block of the active chain crediting a deposit the
        // parent chain no longer has.  Blocks crediting deposits are linked
        // through the deposit state's undo data, from the latest back.
        CBlockIndex* invalid{nullptr};
        {
            LOCK(cs_main);
            const Coin& state{chainstate.CoinsTip().AccessCoin(DepositStateOutpoint())};
            int height{state.IsSpent() ? -1 : static_cast<int>(state.nHeight)};
            while (height >= 0) {
                CBlockIndex* const index{chainstate.m_chain[height]};
                CBlock block;
                CBlockUndo undo;
                if (!index || !chainman.m_blockman.ReadBlock(block, *index) || !chainman.m_blockman.ReadBlockUndo(undo, *index)) {
                    LogWarning("Can't read block at height %d to check its deposits", height);
                    break;
                }
                uint64_t lowest{std::numeric_limits<uint64_t>::max()};
                bool final{true};
                for (const CTransactionRef& tx : Deposits(block)) {
                    const uint64_t sequence{*Assert(ParseDepositTransaction(*tx))};
                    lowest = std::min(lowest, sequence);
                    final = final && parent->IsFinal(sequence);
                    if (sequence < *changed) continue;
                    const DepositStatus status{parent->CheckDeposit(*tx)};
                    if (status == DepositStatus::UNKNOWN || status == DepositStatus::MISMATCH) invalid = index;
                }
                if (lowest < *changed || final || undo.vtxundo.empty()) break;
                const Coin& before{undo.vtxundo[0].vprevout.at(0)};
                if (Assert(ParseDepositStateCoin(before))->IsInitial()) break;
                height = before.nHeight;
            }
        }
        if (invalid) {
            LogInfo("The parent chain no longer has a deposit credited by block %s: invalidating it", invalid->GetBlockHash().ToString());
            BlockValidationState state;
            if (chainstate.InvalidateBlock(state, invalid)) {
                LOCK(cs_main);
                chainstate.SetParentChainFailure(*invalid, true);
            } else {
                LogWarning("Failed to invalidate block %s: %s", invalid->GetBlockHash().ToString(), state.ToString());
            }
        }
    }

    // Also connects blocks whose deposits were waiting for the parent chain
    BlockValidationState state;
    if (!chainstate.ActivateBestChain(state)) {
        LogDebug(BCLog::VALIDATION, "Blocks with deposits still waiting: %s", state.ToString());
    }
}

} // namespace drivechain
