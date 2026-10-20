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
    std::vector<BundleEvent> previous_bundles;
    ParentEvents added;
    if (fork) {
        added = m_source.GetEvents(m_slot, m_headers.at(*fork), tip.hash);
        for (auto it{m_events.begin()}; it != m_events.end();) {
            if (it->second.height > *fork) {
                previous.insert(*it);
                it = m_events.erase(it);
            } else {
                ++it;
            }
        }
        const auto kept{std::ranges::find_if(m_bundle_events, [&](const BundleEvent& event) { return event.height > *fork; })};
        previous_bundles.assign(kept, m_bundle_events.end());
        m_bundle_events.erase(kept, m_bundle_events.end());
        m_headers.erase(m_headers.upper_bound(*fork), m_headers.end());
        for (const ParentHeader& header : fresh) m_headers[header.height] = header.hash;
    } else {
        added = m_source.GetEvents(m_slot, std::nullopt, tip.hash);
        previous = std::move(m_events);
        m_events.clear();
        previous_bundles = std::move(m_bundle_events);
        m_bundle_events.clear();
        m_headers.clear();
        for (const ParentHeader& header : m_source.GetHeaders(tip.hash, HEADER_WINDOW)) m_headers[header.height] = header.hash;
    }
    for (TreasuryEvent& event : added.treasury) {
        const uint64_t sequence{event.sequence};
        m_events[sequence] = std::move(event);
    }
    m_bundle_events.insert(m_bundle_events.end(), added.bundles.begin(), added.bundles.end());
    while (!m_headers.empty() && m_headers.begin()->first < tip.height - HEADER_WINDOW) m_headers.erase(m_headers.begin());

    const bool first{!m_tip};
    m_tip = tip;
    m_tip_changed = true;
    m_proposed.reset();
    if (first) {
        m_changed = 0;
        m_bundles_changed = true;
        return;
    }
    // A bundle event that is gone
    for (const BundleEvent& event : previous_bundles) {
        if (std::ranges::none_of(m_bundle_events, [&](const BundleEvent& other) { return other.SameAs(event); })) {
            m_bundles_changed = true;
            break;
        }
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
        if (it->second.deposit) return std::pair{*it->second.deposit, Depth(it->second.height) >= m_min_depth};
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
    if (Depth(it->second.height) < m_min_depth) return DepositStatus::TOO_SHALLOW;
    return DepositStatus::OK;
}

bool ParentChainView::IsFinal(uint64_t sequence) const
{
    LOCK(m_mutex);
    const auto it{m_events.find(sequence)};
    return it != m_events.end() && Depth(it->second.height) >= DEPOSIT_FINALITY_DEPTH;
}

std::vector<Deposit> ParentChainView::ConfirmedDeposits(uint64_t sequence) const
{
    LOCK(m_mutex);
    std::vector<Deposit> res;
    if (!m_tip) return res;
    for (auto it{m_events.lower_bound(sequence)}; it != m_events.end(); ++it) {
        if (!it->second.deposit) continue;
        if (Depth(it->second.height) < m_min_depth) break;
        res.push_back(*it->second.deposit);
    }
    return res;
}

bool ParentChainView::TakeBundleChanges()
{
    LOCK(m_mutex);
    return std::exchange(m_bundles_changed, false);
}

const BundleEvent* ParentChainView::Outcome(const uint256& m6id) const
{
    for (const BundleEvent& event : m_bundle_events) {
        if (event.m6id == m6id && event.kind != BundleEvent::Kind::SUBMITTED) return &event;
    }
    return nullptr;
}

std::optional<std::pair<BundleOutcome, bool>> ParentChainView::BundleOutcomeOf(const uint256& m6id) const
{
    LOCK(m_mutex);
    if (!m_tip) return std::nullopt;
    const BundleEvent* const event{Outcome(m6id)};
    if (!event) return std::nullopt;
    const BundleOutcome outcome{event->kind == BundleEvent::Kind::SUCCEEDED ? BundleOutcome::SUCCEEDED : BundleOutcome::FAILED};
    return std::pair{outcome, Depth(event->height) >= m_min_depth};
}

DepositStatus ParentChainView::CheckBundleOutcome(const CTransaction& tx) const
{
    const auto recorded{ParseBundleOutcomeTransaction(tx)};
    if (!recorded) return DepositStatus::MISMATCH;
    const auto outcome{BundleOutcomeOf(recorded->first)};
    if (!outcome) return DepositStatus::UNKNOWN;
    if (outcome->first != recorded->second) return DepositStatus::MISMATCH;
    if (!outcome->second) return DepositStatus::TOO_SHALLOW;
    return DepositStatus::OK;
}

bool ParentChainView::IsOutcomeFinal(const uint256& m6id) const
{
    LOCK(m_mutex);
    const BundleEvent* const event{m_tip ? Outcome(m6id) : nullptr};
    return event && Depth(event->height) >= DEPOSIT_FINALITY_DEPTH;
}

void ParentChainView::ProposeBundle(const CTransaction& blinded_m6)
{
    const uint256 m6id{blinded_m6.GetHash().ToUint256()};
    LOCK(m_mutex);
    if (m_proposed == m6id || Outcome(m6id)) return;
    m_source.ProposeWithdrawalBundle(m_slot, blinded_m6);
    if (m_announced != m6id) LogInfo("Proposed withdrawal bundle %s to the parent chain", m6id.ToString());
    m_proposed = m_announced = m6id;
}

namespace {

/** The deposit transactions of a block: right after the coinbase.  */
std::span<const CTransactionRef> Deposits(const CBlock& block)
{
    size_t end{1};
    while (end < block.vtx.size() && block.vtx[end]->IsDeposit()) ++end;
    return std::span{block.vtx}.subspan(1, end - 1);
}

/** Whether the view has the parent chain events `block` records.  */
bool HasEvents(const ParentChainView& parent, const CBlock& block)
{
    if (!std::ranges::all_of(Deposits(block), [&](const auto& tx) { return parent.CheckDeposit(*tx) == DepositStatus::OK; })) return false;
    const auto outcome{BundleOutcomeIndex(block)};
    return !outcome || parent.CheckBundleOutcome(*block.vtx[*outcome]) == DepositStatus::OK;
}

/** The earliest block of the active chain crediting a deposit the parent
 *  chain no longer has, from sequence number `changed` on.  Blocks crediting
 *  deposits are linked through the deposit state's undo data, from the latest
 *  back.  */
CBlockIndex* FirstLostDeposit(ChainstateManager& chainman, const ParentChainView& parent, uint64_t changed) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    Chainstate& chainstate{chainman.ActiveChainstate()};
    CBlockIndex* invalid{nullptr};
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
            final = final && parent.IsFinal(sequence);
            if (sequence < changed) continue;
            const DepositStatus status{parent.CheckDeposit(*tx)};
            if (status == DepositStatus::UNKNOWN || status == DepositStatus::MISMATCH) invalid = index;
        }
        if (lowest < changed || final || undo.vtxundo.empty()) break;
        const Coin& before{undo.vtxundo[0].vprevout.at(0)};
        if (Assert(ParseDepositStateCoin(before))->IsInitial()) break;
        height = before.nHeight;
    }
    return invalid;
}

/** The earliest block of the active chain recording a bundle outcome the
 *  parent chain no longer has.  Blocks recording outcomes are linked through
 *  the withdrawal state, from the latest back.  */
CBlockIndex* FirstLostOutcome(ChainstateManager& chainman, const ParentChainView& parent) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    Chainstate& chainstate{chainman.ActiveChainstate()};
    CBlockIndex* invalid{nullptr};
    int height{ReadWithdrawalState(chainstate.CoinsTip()).last_outcome_height};
    while (height >= 0) {
        CBlockIndex* const index{chainstate.m_chain[height]};
        CBlock block;
        CBlockUndo undo;
        if (!index || !chainman.m_blockman.ReadBlock(block, *index) || !chainman.m_blockman.ReadBlockUndo(undo, *index)) {
            LogWarning("Can't read block at height %d to check its bundle outcome", height);
            break;
        }
        const auto i{BundleOutcomeIndex(block)};
        const auto recorded{i ? ParseBundleOutcomeTransaction(*block.vtx[*i]) : std::nullopt};
        if (!recorded || *i > undo.vtxundo.size() || undo.vtxundo[*i - 1].vprevout.empty()) {
            LogWarning("Block at height %d records no bundle outcome", height);
            break;
        }
        const DepositStatus status{parent.CheckBundleOutcome(*block.vtx[*i])};
        if (status == DepositStatus::UNKNOWN || status == DepositStatus::MISMATCH) invalid = index;
        if (parent.IsOutcomeFinal(recorded->first)) break;
        const auto before{ParseWithdrawalStateCoin(undo.vtxundo[*i - 1].vprevout[0])};
        if (!before) break;
        height = before->last_outcome_height;
    }
    return invalid;
}

/** Propose the pending withdrawal bundle to the parent chain, once it is
 *  BundleProposalDepth deep, unless the view knows how it ended (proposing it
 *  again could pay it out twice).  */
void ProposePendingBundle(ChainstateManager& chainman, ParentChainView& parent)
{
    CTransactionRef blinded;
    {
        LOCK(cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const CCoinsViewCache& view{chainstate.CoinsTip()};
        const WithdrawalState state{ReadWithdrawalState(view)};
        if (!state.bundle || chainstate.m_chain.Height() - state.bundle->height < BundleProposalDepth(chainman.GetConsensus())) return;
        blinded = PendingBlindedM6(view, state);
    }
    if (!blinded) return;
    try {
        parent.ProposeBundle(*blinded);
    } catch (const DepositSourceError& e) {
        LogDebug(BCLog::VALIDATION, "Can't propose withdrawal bundle %s: %s", blinded->GetHash().ToString(), e.what());
    }
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
    const bool bundles_changed{parent->TakeBundleChanges()};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    if (tip_changed) {
        LogDebug(BCLog::VALIDATION, "Following the parent chain to tip %s (deposits changed from: %s, bundle events changed: %s)",
                 Assert(parent->Tip())->hash.ToString(), changed ? util::ToString(*changed) : "none", bundles_changed ? "yes" : "no");
    }

    if (tip_changed) {
        // Blocks the parent chain made invalid whose events it has again
        LOCK(cs_main);
        bool reconsidered{false};
        for (auto& [_, index] : chainman.m_blockman.m_block_index) {
            if (!(index.nStatus & BLOCK_FAILED_PARENT_CHAIN)) continue;
            CBlock block;
            if (!chainman.m_blockman.ReadBlock(block, index) || !HasEvents(*parent, block)) continue;
            LogInfo("The parent chain has the events of block %s again", index.GetBlockHash().ToString());
            chainstate.SetParentChainFailure(index, false);
            chainstate.ResetBlockFailureFlags(&index);
            reconsidered = true;
        }
        if (reconsidered) chainman.RecalculateBestHeader();
    }

    if (changed || bundles_changed) {
        // The earliest block of the active chain recording an event the
        // parent chain no longer has
        CBlockIndex* invalid{nullptr};
        {
            LOCK(cs_main);
            if (changed) invalid = FirstLostDeposit(chainman, *parent, *changed);
            if (bundles_changed) {
                CBlockIndex* const outcome{FirstLostOutcome(chainman, *parent)};
                if (outcome && (!invalid || outcome->nHeight < invalid->nHeight)) invalid = outcome;
            }
        }
        if (invalid) {
            LogInfo("The parent chain no longer has an event recorded by block %s: invalidating it", invalid->GetBlockHash().ToString());
            BlockValidationState state;
            if (chainstate.InvalidateBlock(state, invalid)) {
                LOCK(cs_main);
                chainstate.SetParentChainFailure(*invalid, true);
            } else {
                LogWarning("Failed to invalidate block %s: %s", invalid->GetBlockHash().ToString(), state.ToString());
            }
        }
    }

    // Also connects blocks whose events were waiting for the parent chain
    BlockValidationState state;
    if (!chainstate.ActivateBestChain(state)) {
        LogDebug(BCLog::VALIDATION, "Blocks with parent chain events still waiting: %s", state.ToString());
    }

    ProposePendingBundle(chainman, *parent);
}

} // namespace drivechain
