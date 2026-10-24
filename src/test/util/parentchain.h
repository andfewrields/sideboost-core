// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_PARENTCHAIN_H
#define BITCOIN_TEST_UTIL_PARENTCHAIN_H

#include <drivechain/parentchain.h>
#include <primitives/transaction.h>
#include <random.h>
#include <uint256.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <vector>

/** A parent chain in memory: blocks with treasury and bundle events, and a
 *  best tip.  */
class FakeParentChain final : public drivechain::DepositSource
{
public:
    struct Block {
        drivechain::ParentHeader header;
        std::vector<drivechain::TreasuryEvent> events;
        std::vector<drivechain::BundleEvent> bundle_events;
    };
    std::map<uint256, Block> blocks;
    uint256 tip;
    /** The withdrawal bundles proposed to it, oldest first.  */
    std::vector<CTransactionRef> proposals;
    /** Whether the block producer votes for bundles it isn't told to.  */
    bool automatic_votes{true};
    /** The bundles it is told to vote for.  */
    std::set<uint256> acks;
    /** How often it was asked for them.  */
    int ack_queries{0};
    /** Where each GetEvents started (nullopt: genesis).  */
    std::vector<std::optional<uint256>> event_starts;
    FastRandomContext& rng;

    explicit FakeParentChain(FastRandomContext& rng_in) : rng{rng_in}
    {
        const uint256 genesis{rng.rand256()};
        blocks[genesis] = {{genesis, uint256{}, 0}, {}, {}};
        tip = genesis;
    }

    /** Mine a block on `prev` (default: the tip) and make it the tip.  */
    uint256 Mine(std::vector<drivechain::TreasuryEvent> events = {}, std::optional<uint256> prev = {},
                 std::vector<drivechain::BundleEvent> bundle_events = {})
    {
        const drivechain::ParentHeader& parent{blocks.at(prev.value_or(tip)).header};
        const uint256 hash{rng.rand256()};
        for (drivechain::TreasuryEvent& event : events) {
            event.block_hash = hash;
            event.height = parent.height + 1;
        }
        for (drivechain::BundleEvent& event : bundle_events) {
            event.block_hash = hash;
            event.height = parent.height + 1;
        }
        blocks[hash] = {{hash, parent.hash, parent.height + 1}, std::move(events), std::move(bundle_events)};
        tip = hash;
        return hash;
    }

    drivechain::ParentHeader GetTip() override { return blocks.at(tip).header; }

    std::vector<drivechain::ParentHeader> GetHeaders(const uint256& hash, uint32_t max_ancestors) override
    {
        std::vector<drivechain::ParentHeader> res;
        for (auto it{blocks.find(hash)}; it != blocks.end() && res.size() <= max_ancestors; it = blocks.find(it->second.header.prev)) {
            res.push_back(it->second.header);
        }
        return res;
    }

    drivechain::ParentEvents GetEvents(uint8_t, const std::optional<uint256>& start, const uint256& end) override
    {
        event_starts.push_back(start);
        std::vector<const Block*> range;
        for (auto it{blocks.find(end)}; it != blocks.end() && it->first != start; it = blocks.find(it->second.header.prev)) {
            range.push_back(&it->second);
        }
        std::ranges::reverse(range);
        drivechain::ParentEvents res;
        for (const Block* block : range) {
            res.treasury.insert(res.treasury.end(), block->events.begin(), block->events.end());
            res.bundles.insert(res.bundles.end(), block->bundle_events.begin(), block->bundle_events.end());
        }
        return res;
    }

    void ProposeWithdrawalBundle(uint8_t, const CTransaction& blinded_m6) override
    {
        proposals.push_back(MakeTransactionRef(blinded_m6));
    }

    void DisableAutomaticBundleVotes() override { automatic_votes = false; }

    std::vector<uint256> GetBundleAcks(uint8_t) override
    {
        ++ack_queries;
        return {acks.begin(), acks.end()};
    }

    void SetBundleAck(uint8_t, const uint256& m6id, bool ack) override
    {
        if (ack) {
            acks.insert(m6id);
        } else {
            acks.erase(m6id);
        }
    }
};

#endif // BITCOIN_TEST_UTIL_PARENTCHAIN_H
