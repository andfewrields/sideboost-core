// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/parentchain.h>

#include <chain.h>
#include <consensus/validation.h>
#include <hash.h>
#include <kernel/chainparams.h>
#include <kernel/notifications_interface.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <streams.h>
#include <undo.h>
#include <util/check.h>
#include <util/fs_helpers.h>
#include <util/string.h>
#include <util/thread.h>
#include <util/translation.h>
#include <validation.h>

#include <algorithm>
#include <limits>

namespace drivechain {

namespace {

/** How much of the parent chain's best chain the view remembers, to find
 *  where a reorg forks off.  A deeper reorg reloads the view.  */
constexpr int HEADER_WINDOW{1000};

// The blocks recording events a parent chain reorg can still change, which
// following it reads, are within what pruned nodes keep, even if the parent
// chain runs at half speed
static_assert(2 * DEPOSIT_FINALITY_DEPTH * PARENT_CHAIN_BLOCK_SPACING <= MIN_TIME_TO_KEEP);

/** The format of the view's file: the version, the slot, the view, then a
 *  checksum (SHA256d) of it all.  */
constexpr uint8_t VIEW_FILE_VERSION{1};

template <typename Stream>
void WriteHeader(Stream& s, const ParentHeader& header) { s << header.hash << header.prev << header.height; }

template <typename Stream>
ParentHeader ReadHeader(Stream& s)
{
    ParentHeader header;
    s >> header.hash >> header.prev >> header.height;
    return header;
}

template <typename Stream>
void WriteEvent(Stream& s, const TreasuryEvent& event)
{
    s << event.sequence << event.block_hash << event.height << event.deposit.has_value();
    if (event.deposit) s << event.deposit->sequence << event.deposit->outpoint << event.deposit->address << event.deposit->value;
}

template <typename Stream>
TreasuryEvent ReadEvent(Stream& s)
{
    TreasuryEvent event;
    bool deposit;
    s >> event.sequence >> event.block_hash >> event.height >> deposit;
    if (deposit) {
        event.deposit.emplace();
        s >> event.deposit->sequence >> event.deposit->outpoint >> event.deposit->address >> event.deposit->value;
    }
    return event;
}

template <typename Stream>
void WriteBundleEvent(Stream& s, const BundleEvent& event)
{
    s << event.m6id << static_cast<uint8_t>(event.kind) << event.block_hash << event.height << event.commitment.has_value();
    if (event.commitment) s << event.commitment->block_hash << event.commitment->height;
}

template <typename Stream>
BundleEvent ReadBundleEvent(Stream& s)
{
    BundleEvent event;
    uint8_t kind;
    bool commitment;
    s >> event.m6id >> kind >> event.block_hash >> event.height >> commitment;
    if (kind > static_cast<uint8_t>(BundleEvent::Kind::FAILED)) throw std::ios_base::failure("bad bundle event kind");
    event.kind = static_cast<BundleEvent::Kind>(kind);
    if (commitment) {
        event.commitment.emplace();
        s >> event.commitment->block_hash >> event.commitment->height;
    }
    return event;
}

} // namespace

ParentChainView::ParentChainView(DepositSource& source, uint8_t slot, int min_depth, bool manage_votes, fs::path file)
    : m_source{source}, m_slot{slot}, m_min_depth{min_depth}, m_manage_votes{manage_votes}, m_file{std::move(file)} {}

bool ParentChainView::Load()
{
    if (m_file.empty()) return false;
    AutoFile file{fsbridge::fopen(m_file, "rb")};
    if (file.IsNull()) return false;
    std::optional<ParentHeader> tip;
    std::map<int, uint256> headers;
    std::map<uint64_t, TreasuryEvent> events;
    std::vector<BundleEvent> bundle_events;
    try {
        HashVerifier verifier{file};
        uint8_t version, slot;
        verifier >> version >> slot;
        if (version != VIEW_FILE_VERSION || slot != m_slot) {
            LogWarning("Not loading %s: it is for another version or slot", fs::PathToString(m_file));
            return false;
        }
        tip = ReadHeader(verifier);
        verifier >> headers;
        uint64_t count;
        verifier >> count;
        for (uint64_t i{0}; i < count; ++i) {
            TreasuryEvent event{ReadEvent(verifier)};
            const uint64_t sequence{event.sequence};
            events.emplace(sequence, std::move(event));
        }
        verifier >> count;
        for (uint64_t i{0}; i < count; ++i) bundle_events.push_back(ReadBundleEvent(verifier));
        uint256 checksum;
        file >> checksum;
        if (checksum != verifier.GetHash()) throw std::ios_base::failure("checksum mismatch");
    } catch (const std::exception& e) {
        LogWarning("Not loading %s: %s", fs::PathToString(m_file), e.what());
        return false;
    }
    LOCK(m_mutex);
    m_tip = tip;
    m_headers = std::move(headers);
    m_events = std::move(events);
    m_bundle_events = std::move(bundle_events);
    LogInfo("Loaded the parent chain view at tip %s (height %d) from %s", m_tip->hash.ToString(), m_tip->height, fs::PathToString(m_file));
    return true;
}

bool ParentChainView::Save() const
{
    if (m_file.empty()) return true;
    DataStream data;
    {
        LOCK(m_mutex);
        if (!m_tip) return true;
        data << VIEW_FILE_VERSION << m_slot;
        WriteHeader(data, *m_tip);
        data << m_headers << uint64_t{m_events.size()};
        for (const auto& [_, event] : m_events) WriteEvent(data, event);
        data << uint64_t{m_bundle_events.size()};
        for (const BundleEvent& event : m_bundle_events) WriteBundleEvent(data, event);
    }
    HashWriter hasher;
    hasher.write(std::span{data});
    data << hasher.GetHash();

    const fs::path tmp{m_file + ".new"};
    AutoFile file{fsbridge::fopen(tmp, "wb")};
    if (file.IsNull()) {
        LogWarning("Can't write %s", fs::PathToString(tmp));
        return false;
    }
    file.write(std::span{data});
    if (!file.Commit() || file.fclose() != 0 || !RenameOver(tmp, m_file)) {
        LogWarning("Can't write %s", fs::PathToString(m_file));
        fs::remove(tmp);
        return false;
    }
    return true;
}

void ParentChainView::Update()
{
    LOCK(m_mutex);
    const ParentHeader tip{m_source.GetTip()};
    if (m_tip && m_tip->hash == tip.hash) {
        // From the view Load kept: check everything again, as the first
        // Update does
        if (!std::exchange(m_started, true)) {
            m_tip_changed = true;
            m_changed = 0;
            m_bundles_changed = true;
        }
        return;
    }

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

    const bool first{!std::exchange(m_started, true)};
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

std::optional<BundleCommitment> ParentChainView::Checkpoint() const
{
    LOCK(m_mutex);
    if (!m_tip) return std::nullopt;
    for (auto it{m_bundle_events.rbegin()}; it != m_bundle_events.rend(); ++it) {
        if (it->kind == BundleEvent::Kind::SUCCEEDED && it->commitment && Depth(it->height) >= m_min_depth) return it->commitment;
    }
    return std::nullopt;
}

bool ViolatesCheckpoint(const CBlockIndex& index, const std::optional<BundleCommitment>& checkpoint)
{
    return checkpoint && index.nHeight >= checkpoint->height &&
           Assert(index.GetAncestor(checkpoint->height))->GetBlockHash() != checkpoint->block_hash;
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

void ParentChainView::VoteForBundle(const std::optional<uint256>& m6id)
{
    if (!m_manage_votes) return;
    LOCK(m_mutex);
    if (!m_tip || m_voted == std::pair{m_tip->hash, m6id}) return;
    if (!m_automatic_votes_off) {
        m_source.DisableAutomaticBundleVotes();
        m_automatic_votes_off = true;
        LogInfo("Turned off the parent chain block producer's automatic withdrawal bundle votes");
    }
    // The producer forgets an ACK for a bundle that isn't pending yet
    const bool pending{m6id && !Outcome(*m6id) && std::ranges::any_of(m_bundle_events, [&](const BundleEvent& event) {
        return event.m6id == *m6id && event.kind == BundleEvent::Kind::SUBMITTED;
    })};
    bool acked{false};
    for (const uint256& other : m_source.GetBundleAcks(m_slot)) {
        if (pending && other == *m6id) {
            acked = true;
        } else {
            m_source.SetBundleAck(m_slot, other, false);
            LogInfo("No longer voting for withdrawal bundle %s on the parent chain", other.ToString());
        }
    }
    if (pending && !acked) m_source.SetBundleAck(m_slot, *m6id, true);
    const std::optional<uint256> voted{pending ? m6id : std::nullopt};
    if (voted && m_announced_vote != voted) LogInfo("Voting for withdrawal bundle %s on the parent chain", voted->ToString());
    m_announced_vote = voted;
    m_voted = std::pair{m_tip->hash, m6id};
}

namespace {

/** Stop the node: following the parent chain needs the data of the active
 *  chain's block at `height`, which it can't read (pruned, beyond
 *  MinBlocksToKeep).  As a pruned Bitcoin node facing a reorg deeper than
 *  what it kept.  */
void MissingBlockData(ChainstateManager& chainman, int height)
{
    chainman.GetNotifications().fatalError(strprintf(
        _("Following the parent chain needs block %d, which this node can't read. If it prunes, the block was deleted: restart with -reindex to download the chain again."),
        height));
}

/** The deposit transactions of a block: right after the coinbase.  */
std::span<const CTransactionRef> Deposits(const CBlock& block)
{
    size_t end{1};
    while (end < block.vtx.size() && block.vtx[end]->IsDeposit()) ++end;
    return std::span{block.vtx}.subspan(1, end - 1);
}

/** Whether `block` (of `index`) is valid as far as the view is concerned: the
 *  view has the parent chain events it records, and its chain the
 *  checkpoint.  */
bool HasEvents(const ParentChainView& parent, const CBlock& block, const CBlockIndex& index)
{
    if (ViolatesCheckpoint(index, parent.Checkpoint())) return false;
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
            MissingBlockData(chainman, height);
            return nullptr;
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
            MissingBlockData(chainman, height);
            return nullptr;
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
 *  again could pay it out twice), and vote for it alone.  While in initial
 *  block download, whose pending bundle may be long gone, vote for none.  */
void ProposePendingBundle(ChainstateManager& chainman, ParentChainView& parent)
{
    CTransactionRef blinded;
    bool syncing;
    {
        LOCK(cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const CCoinsViewCache& view{chainstate.CoinsTip()};
        const WithdrawalState state{ReadWithdrawalState(view)};
        syncing = chainman.IsInitialBlockDownload();
        if (state.bundle && chainstate.m_chain.Height() - state.bundle->height >= BundleProposalDepth(chainman.GetConsensus())) {
            blinded = PendingBlindedM6(view, state);
        }
    }
    try {
        if (blinded) parent.ProposeBundle(*blinded);
        parent.VoteForBundle(blinded && !syncing ? std::optional{blinded->GetHash().ToUint256()} : std::nullopt);
    } catch (const DepositSourceError& e) {
        LogDebug(BCLog::VALIDATION, "Can't propose or vote for the pending withdrawal bundle: %s", e.what());
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
        parent->Save();
    }

    if (tip_changed) {
        // Blocks the parent chain made invalid whose events it has again
        LOCK(cs_main);
        bool reconsidered{false};
        for (auto& [_, index] : chainman.m_blockman.m_block_index) {
            // Not those pruned since (beyond MinBlocksToKeep)
            if (!(index.nStatus & BLOCK_FAILED_PARENT_CHAIN) || !(index.nStatus & BLOCK_HAVE_DATA)) continue;
            CBlock block;
            if (!chainman.m_blockman.ReadBlock(block, index) || !HasEvents(*parent, block, index)) continue;
            LogInfo("The parent chain has the events of block %s again", index.GetBlockHash().ToString());
            chainstate.SetParentChainFailure(index, false);
            chainstate.ResetBlockFailureFlags(&index);
            reconsidered = true;
        }
        if (reconsidered) chainman.RecalculateBestHeader();
    }

    {
        // The earliest block of the active chain recording an event the
        // parent chain no longer has, or lacking its checkpoint
        CBlockIndex* invalid{nullptr};
        {
            LOCK(cs_main);
            if (changed) invalid = FirstLostDeposit(chainman, *parent, *changed);
            if (bundles_changed) {
                CBlockIndex* const outcome{FirstLostOutcome(chainman, *parent)};
                if (outcome && (!invalid || outcome->nHeight < invalid->nHeight)) invalid = outcome;
            }
            if (const auto checkpoint{parent->Checkpoint()}; checkpoint && ViolatesCheckpoint(*chainstate.m_chain.Tip(), checkpoint)) {
                CBlockIndex* const other{chainstate.m_chain[checkpoint->height]};
                LogInfo("The parent chain paid out a bundle from block %s at height %d, which the active chain lacks", checkpoint->block_hash.ToString(), checkpoint->height);
                if (!invalid || other->nHeight < invalid->nHeight) invalid = other;
            }
        }
        if (invalid) {
            // Disconnecting the blocks down to it reads their data
            LOCK(cs_main);
            for (const CBlockIndex* index{chainstate.m_chain.Tip()}; index && index->nHeight >= invalid->nHeight; index = index->pprev) {
                if ((index->nStatus & BLOCK_HAVE_MASK) != BLOCK_HAVE_MASK) {
                    MissingBlockData(chainman, index->nHeight);
                    return;
                }
            }
        }
        if (invalid) {
            LogInfo("Block %s is invalid in our view of the parent chain: invalidating it", invalid->GetBlockHash().ToString());
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

ParentChainSync::ParentChainSync(ChainstateManager& chainman) : m_chainman{chainman}
{
    m_thread = std::thread(&util::TraceThread, "parentchain", [this] {
        WAIT_LOCK(m_mutex, lock);
        while (true) {
            m_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_triggered || m_stop; });
            if (m_stop) return;
            m_triggered = false;
            REVERSE_LOCK(lock, m_mutex);
            SyncWithParentChain(m_chainman);
        }
    });
}

ParentChainSync::~ParentChainSync()
{
    WITH_LOCK(m_mutex, m_stop = true);
    m_cv.notify_one();
    if (m_thread.joinable()) m_thread.join();
}

void ParentChainSync::Trigger()
{
    WITH_LOCK(m_mutex, m_triggered = true);
    m_cv.notify_one();
}

} // namespace drivechain
