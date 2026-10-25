// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BIP300_PRODUCER_H
#define BITCOIN_BIP300_PRODUCER_H

#include <bip300/rules.h>
#include <bip300/state.h>
#include <consensus/params.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/result.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

/**
 * BIP300 and BIP301 for this chain's own sidechains: what this node puts in
 * the blocks it builds, as the enforcer's block producer does.
 *
 * - M1: the sidechain proposals made through it, until they activate.
 * - M2: ACKs for proposals, those ACKed explicitly and those its policy
 *   covers (by default the proposals for unused slots).
 * - M3: the bundles (blinded M6s) sidechains gave it, once each.
 * - M4: in each slot an upvote for the first pending bundle ACKed
 *   explicitly, or else its policy's choice (by default the first it knows).
 *   It never casts the vote taking a bundle over the threshold unless it can
 *   pay the bundle out in the same block: it knows its blinded M6, the
 *   treasury covers it and it fits.
 * - The payouts the block must make.
 *
 * BMM (M7 for the best M8 per slot) is up to the block assembler, which
 * picks the requests from the mempool.  Settings and bundles are kept in a
 * file, rewritten on every change.
 */
namespace bip300 {

/** Which proposals get ACKed besides those ACKed explicitly.  */
enum class AckPolicy : uint8_t {
    NONE = 0,
    /** Proposals for a slot no sidechain uses (not replacements).  */
    NEW_SLOTS = 1,
    ALL = 2,
};

/** Which bundle gets upvoted in a slot without an explicit ACK.  */
enum class BundlePolicy : uint8_t {
    NONE = 0,
    /** The first pending one this node knows (was given).  */
    KNOWN = 1,
    /** The first pending one.  */
    ALL = 2,
    /** None: alarm, downvoting them all.  */
    ALARM = 3,
};

std::optional<AckPolicy> ParseAckPolicy(const std::string& name);
std::string AckPolicyName(AckPolicy policy);
std::optional<BundlePolicy> ParseBundlePolicy(const std::string& name);
std::string BundlePolicyName(BundlePolicy policy);

class Producer
{
public:
    /** A bundle this node was given.  */
    struct KnownBundle {
        uint8_t slot{0};
        CTransactionRef blinded;
        /** Whether it was seen pending: once it isn't any more it was paid
         *  out or failed, and is forgotten.  */
        bool seen_pending{false};
    };

    struct Settings {
        std::vector<std::pair<uint8_t, std::vector<unsigned char>>> proposals;
        std::set<std::pair<uint8_t, uint256>> sidechain_acks;
        AckPolicy ack_policy{AckPolicy::NEW_SLOTS};
        std::set<std::pair<uint8_t, uint256>> bundle_acks;
        BundlePolicy bundle_policy{BundlePolicy::KNOWN};
    };

    /** The sidechain part of a block.  */
    struct BlockPart {
        /** M1 to M4, in coinbase order.  */
        std::vector<CScript> messages;
        /** The payouts the block must make.  */
        std::vector<CTransactionRef> payouts;
    };

    /** Keeps its settings and bundles in `file` (none if empty).  */
    explicit Producer(fs::path file);

    bool Load();

    void ProposeSidechain(uint8_t slot, std::vector<unsigned char> description) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetSidechainAck(uint8_t slot, const uint256& description_hash, bool ack) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetAckPolicy(AckPolicy policy) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetBundleAck(uint8_t slot, const uint256& m6id, bool ack) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetBundlePolicy(BundlePolicy policy) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Take bundle `blinded` (a blinded M6) of `slot`, to propose and pay
     *  out.  Returns its M6ID.  */
    util::Result<uint256> AddBundle(uint8_t slot, const CTransactionRef& blinded) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The bundle `m6id`, if this node knows it.  */
    std::optional<KnownBundle> GetBundle(const uint256& m6id) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    Settings GetSettings() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::map<uint256, KnownBundle> GetBundles() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The sidechain part of a block at `height` on a chain whose sidechain
     *  state is `state` (and earlier proposals `proposed`), with at most
     *  `max_payout_weight` for payouts.  Forgets the bundles no longer
     *  pending.  */
    BlockPart MakeBlockPart(const State& state, int height, const Consensus::SidechainParams& params, const ProposedLookup& proposed,
                            int64_t max_payout_weight) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    const fs::path m_file;
    mutable Mutex m_mutex;
    Settings m_settings GUARDED_BY(m_mutex);
    std::map<uint256, KnownBundle> m_bundles GUARDED_BY(m_mutex);

    bool Save() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
};

} // namespace bip300

#endif // BITCOIN_BIP300_PRODUCER_H
