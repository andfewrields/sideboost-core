// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/register.h> // IWYU pragma: associated

#include <bip300/messages.h>
#include <bip300/producer.h>
#include <bip300/rules.h>
#include <bip300/state.h>
#include <chain.h>
#include <coins.h>
#include <core_io.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <primitives/block.h>
#include <rpc/blockchain.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <streams.h>
#include <undo.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <validation.h>

#include <algorithm>
#include <optional>

using node::NodeContext;

namespace {

uint8_t ParseSlot(const UniValue& value)
{
    const int slot{value.getInt<int>()};
    if (slot < 0 || slot > 255) throw JSONRPCError(RPC_INVALID_PARAMETER, "slot must be 0 to 255");
    return uint8_t(slot);
}

bip300::State ReadTipState(ChainstateManager& chainman) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const auto state{bip300::ReadState(chainman.ActiveChainstate().CoinsTip())};
    if (!state) throw JSONRPCError(RPC_DATABASE_ERROR, "Sidechain state corrupt");
    return *state;
}

bip300::Producer& EnsureProducer(const JSONRPCRequest& request)
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    if (!node.sidechain_producer) throw JSONRPCError(RPC_INTERNAL_ERROR, "No sidechain block producer");
    return *node.sidechain_producer;
}

/** The description proposed in the active chain's block at `height` for
 *  `slot` with `hash`, if that block is on disk.  */
std::optional<std::vector<unsigned char>> ReadDescription(ChainstateManager& chainman, int height, uint8_t slot, const uint256& hash)
{
    CBlock block;
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.ActiveChain()[height]};
        if (!index || !(index->nStatus & BLOCK_HAVE_DATA)) return std::nullopt;
        if (!chainman.m_blockman.ReadBlock(block, *index)) return std::nullopt;
    }
    for (const CTxOut& out : block.vtx.at(0)->vout) {
        const auto message{bip300::ParseCoinbaseMessage(out.scriptPubKey)};
        const auto* const m1{message ? std::get_if<bip300::ProposeSidechain>(&*message) : nullptr};
        if (m1 && m1->slot == slot && bip300::DescriptionHash(m1->description) == hash) return m1->description;
    }
    return std::nullopt;
}

void PushDescription(UniValue& obj, ChainstateManager& chainman, int height, uint8_t slot, const uint256& hash)
{
    if (const auto description{ReadDescription(chainman, height, slot, hash)}) obj.pushKV("description", HexStr(*description));
}

UniValue BundleList(const std::vector<std::pair<uint8_t, uint256>>& bundles)
{
    UniValue res{UniValue::VARR};
    for (const auto& [slot, m6id] : bundles) {
        UniValue obj{UniValue::VOBJ};
        obj.pushKV("slot", slot);
        obj.pushKV("m6id", m6id.GetHex());
        res.push_back(std::move(obj));
    }
    return res;
}

const RPCResult SLOT_RESULT{RPCResult::Type::NUM, "slot", "The sidechain slot"};
const RPCResult DESCRIPTION_RESULT{RPCResult::Type::STR_HEX, "description", /*optional=*/true, "The description (absent if its block isn't on disk)"};

RPCMethod getsidechains()
{
    return RPCMethod{
        "getsidechains",
        "Returns the sidechains active in this chain's slots (BIP300), with their treasuries, at the tip of the active chain.",
        {},
        RPCResult{RPCResult::Type::ARR, "", "", {
            {RPCResult::Type::OBJ, "", "", {
                SLOT_RESULT,
                {RPCResult::Type::STR_HEX, "description_hash", "sha256d of its description, which ACKs referred to"},
                DESCRIPTION_RESULT,
                {RPCResult::Type::NUM, "vote_count", "The ACKs it activated with"},
                {RPCResult::Type::NUM, "proposal_height", "The height of the block that proposed it"},
                {RPCResult::Type::NUM, "activation_height", "The height of the block that activated it"},
                {RPCResult::Type::STR_AMOUNT, "treasury", "The coins in its treasury"},
                {RPCResult::Type::NUM, "next_sequence", "The sequence number of the next deposit or payout"},
                {RPCResult::Type::STR_HEX, "treasury_txid", "The treasury coin's txid (its vout is the slot)"},
            }},
        }},
        RPCExamples{HelpExampleCli("getsidechains", "") + HelpExampleRpc("getsidechains", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            ChainstateManager& chainman{EnsureAnyChainman(request.context)};
            const bip300::State state{WITH_LOCK(::cs_main, return ReadTipState(chainman))};
            UniValue res{UniValue::VARR};
            for (const auto& [slot, sidechain] : state.sidechains) {
                UniValue obj{UniValue::VOBJ};
                obj.pushKV("slot", slot);
                obj.pushKV("description_hash", sidechain.description_hash.GetHex());
                PushDescription(obj, chainman, sidechain.proposal_height, slot, sidechain.description_hash);
                obj.pushKV("vote_count", sidechain.vote_count);
                obj.pushKV("proposal_height", sidechain.proposal_height);
                obj.pushKV("activation_height", sidechain.activation_height);
                const auto treasury{state.treasuries.find(slot)};
                const bip300::Treasury value{treasury == state.treasuries.end() ? bip300::Treasury{} : treasury->second};
                obj.pushKV("treasury", ValueFromAmount(value.value));
                obj.pushKV("next_sequence", value.next_sequence);
                obj.pushKV("treasury_txid", SidechainTreasuryTxid().GetHex());
                res.push_back(std::move(obj));
            }
            return res;
        },
    };
}

RPCMethod getsidechainproposals()
{
    return RPCMethod{
        "getsidechainproposals",
        "Returns the proposals for sidechains to take this chain's slots (BIP300 M1), at the tip of the active chain.",
        {},
        RPCResult{RPCResult::Type::ARR, "", "Oldest first", {
            {RPCResult::Type::OBJ, "", "", {
                SLOT_RESULT,
                {RPCResult::Type::STR_HEX, "description_hash", "sha256d of its description, which ACKs refer to"},
                DESCRIPTION_RESULT,
                {RPCResult::Type::NUM, "vote_count", "Its ACKs (M2) so far"},
                {RPCResult::Type::NUM, "proposal_height", "The height of the block that proposed it"},
                {RPCResult::Type::BOOL, "replacement", "Whether a sidechain uses the slot, which it would replace"},
            }},
        }},
        RPCExamples{HelpExampleCli("getsidechainproposals", "") + HelpExampleRpc("getsidechainproposals", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            ChainstateManager& chainman{EnsureAnyChainman(request.context)};
            const bip300::State state{WITH_LOCK(::cs_main, return ReadTipState(chainman))};
            UniValue res{UniValue::VARR};
            for (const bip300::Proposal& proposal : state.proposals) {
                UniValue obj{UniValue::VOBJ};
                obj.pushKV("slot", proposal.slot);
                obj.pushKV("description_hash", proposal.description_hash.GetHex());
                PushDescription(obj, chainman, proposal.proposal_height, proposal.slot, proposal.description_hash);
                obj.pushKV("vote_count", proposal.vote_count);
                obj.pushKV("proposal_height", proposal.proposal_height);
                obj.pushKV("replacement", state.FindSidechain(proposal.slot) != nullptr);
                res.push_back(std::move(obj));
            }
            return res;
        },
    };
}

RPCMethod getpendingbundles()
{
    return RPCMethod{
        "getpendingbundles",
        "Returns the withdrawal bundles pending in this chain's sidechain slots (BIP300 M3), at the tip of the active chain.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Only this slot's"},
        },
        RPCResult{RPCResult::Type::ARR, "", "By slot, oldest first: the order M4 votes refer to them in", {
            {RPCResult::Type::OBJ, "", "", {
                SLOT_RESULT,
                {RPCResult::Type::STR_HEX, "m6id", "Its M6ID"},
                {RPCResult::Type::NUM, "vote_count", "Its votes"},
                {RPCResult::Type::NUM, "proposal_height", "The height of the block that proposed it"},
                {RPCResult::Type::STR_HEX, "hex", /*optional=*/true, "Its blinded M6, if this node knows it"},
            }},
        }},
        RPCExamples{HelpExampleCli("getpendingbundles", "") + HelpExampleRpc("getpendingbundles", "5")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            ChainstateManager& chainman{EnsureAnyChainman(request.context)};
            const std::optional<uint8_t> only{request.params[0].isNull() ? std::nullopt : std::optional{ParseSlot(request.params[0])}};
            const bip300::State state{WITH_LOCK(::cs_main, return ReadTipState(chainman))};
            NodeContext& node{EnsureAnyNodeContext(request.context)};
            UniValue res{UniValue::VARR};
            for (const auto& [slot, bundles] : state.bundles) {
                if (only && slot != *only) continue;
                for (const bip300::PendingBundle& bundle : bundles) {
                    UniValue obj{UniValue::VOBJ};
                    obj.pushKV("slot", slot);
                    obj.pushKV("m6id", bundle.m6id.GetHex());
                    obj.pushKV("vote_count", bundle.vote_count);
                    obj.pushKV("proposal_height", bundle.proposal_height);
                    if (node.sidechain_producer) {
                        if (const auto known{node.sidechain_producer->GetBundle(bundle.m6id)}) obj.pushKV("hex", EncodeHexTx(*known->blinded));
                    }
                    res.push_back(std::move(obj));
                }
            }
            return res;
        },
    };
}

RPCMethod getsidechainevents()
{
    return RPCMethod{
        "getsidechainevents",
        "Returns what happened to this chain's sidechains in a block (BIP300/301): what sidechains learn from it.\n"
        "Needs the block and its undo data.",
        {
            {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The block hash"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_HEX, "hash", "The block hash"},
            {RPCResult::Type::NUM, "height", "Its height"},
            {RPCResult::Type::STR_HEX, "previousblockhash", "Its parent's hash"},
            {RPCResult::Type::STR_HEX, "coinbase_txid", "Its coinbase's txid"},
            {RPCResult::Type::ARR, "bmm", "The sidechain blocks it commits to (M7)", {
                {RPCResult::Type::OBJ, "", "", {SLOT_RESULT, {RPCResult::Type::STR_HEX, "hash", "The sidechain block hash (as in the M7, hex of its bytes)"}}},
            }},
            {RPCResult::Type::ARR, "proposals", "New sidechain proposals (M1)", {
                {RPCResult::Type::OBJ, "", "", {SLOT_RESULT, {RPCResult::Type::STR_HEX, "description_hash", ""}, DESCRIPTION_RESULT, {RPCResult::Type::NUM, "vout", "The coinbase output proposing it"}}},
            }},
            {RPCResult::Type::ARR, "activations", "Slots sidechains activated in", {SLOT_RESULT}},
            {RPCResult::Type::ARR, "deposits", "Deposits, in order", {
                {RPCResult::Type::OBJ, "", "", {
                    SLOT_RESULT,
                    {RPCResult::Type::NUM, "sequence", "Its sequence number"},
                    {RPCResult::Type::STR_HEX, "txid", "The transaction"},
                    {RPCResult::Type::NUM, "vout", "Its deposit output"},
                    {RPCResult::Type::STR_HEX, "address", "The sidechain address (bytes)"},
                    {RPCResult::Type::STR_AMOUNT, "amount", "The amount"},
                }},
            }},
            {RPCResult::Type::ARR, "submitted", "Bundles proposed (M3)", {{RPCResult::Type::OBJ, "", "", {SLOT_RESULT, {RPCResult::Type::STR_HEX, "m6id", ""}}}}},
            {RPCResult::Type::ARR, "succeeded", "Bundles paid out", {
                {RPCResult::Type::OBJ, "", "", {
                    SLOT_RESULT,
                    {RPCResult::Type::STR_HEX, "m6id", ""},
                    {RPCResult::Type::NUM, "sequence", "Its sequence number"},
                    {RPCResult::Type::STR_HEX, "txid", "The payout"},
                    {RPCResult::Type::STR_HEX, "hex", "The payout transaction"},
                }},
            }},
            {RPCResult::Type::ARR, "failed", "Bundles that expired", {{RPCResult::Type::OBJ, "", "", {SLOT_RESULT, {RPCResult::Type::STR_HEX, "m6id", ""}}}}},
        }},
        RPCExamples{HelpExampleCli("getsidechainevents", "\"<blockhash>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            ChainstateManager& chainman{EnsureAnyChainman(request.context)};
            const uint256 hash{ParseHashV(request.params[0], "blockhash")};
            const CBlockIndex* index;
            {
                LOCK(::cs_main);
                index = chainman.m_blockman.LookupBlockIndex(hash);
                if (!index) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
                CheckBlockDataAvailability(chainman.m_blockman, *index, /*check_for_undo=*/index->nHeight > 0);
            }
            CBlock block;
            CBlockUndo undo;
            if (!chainman.m_blockman.ReadBlock(block, *index)) throw JSONRPCError(RPC_MISC_ERROR, "Can't read block from disk");
            if (index->nHeight > 0 && !chainman.m_blockman.ReadBlockUndo(undo, *index)) throw JSONRPCError(RPC_MISC_ERROR, "Can't read undo data from disk");
            bip300::BlockEvents events;
            if (!undo.sidechain_events.empty()) {
                SpanReader{undo.sidechain_events} >> events;
            }

            UniValue res{UniValue::VOBJ};
            res.pushKV("hash", hash.GetHex());
            res.pushKV("height", index->nHeight);
            res.pushKV("previousblockhash", block.hashPrevBlock.GetHex());
            res.pushKV("coinbase_txid", block.vtx.at(0)->GetHash().GetHex());
            UniValue bmm{UniValue::VARR}, proposals{UniValue::VARR};
            const std::vector<CTxOut>& coinbase_outs{block.vtx.at(0)->vout};
            for (uint32_t i{0}; i < coinbase_outs.size(); ++i) {
                const auto message{bip300::ParseCoinbaseMessage(coinbase_outs[i].scriptPubKey)};
                if (!message) continue;
                if (const auto* const m7{std::get_if<bip300::BmmAccept>(&*message)}) {
                    UniValue obj{UniValue::VOBJ};
                    obj.pushKV("slot", m7->slot);
                    obj.pushKV("hash", HexStr(m7->hash));
                    bmm.push_back(std::move(obj));
                } else if (const auto* const m1{std::get_if<bip300::ProposeSidechain>(&*message)}) {
                    const uint256 desc_hash{bip300::DescriptionHash(m1->description)};
                    const std::pair key{m1->slot, desc_hash};
                    // The first such M1, if it was a new proposal
                    if (std::ranges::find(events.proposals, key) == events.proposals.end()) continue;
                    std::erase(events.proposals, key);
                    UniValue obj{UniValue::VOBJ};
                    obj.pushKV("slot", m1->slot);
                    obj.pushKV("description_hash", desc_hash.GetHex());
                    obj.pushKV("description", HexStr(m1->description));
                    obj.pushKV("vout", i);
                    proposals.push_back(std::move(obj));
                }
            }
            res.pushKV("bmm", std::move(bmm));
            res.pushKV("proposals", std::move(proposals));
            UniValue activations{UniValue::VARR};
            for (const uint8_t slot : events.activations) activations.push_back(slot);
            res.pushKV("activations", std::move(activations));
            UniValue deposits{UniValue::VARR};
            for (const bip300::BlockEvents::Deposit& deposit : events.deposits) {
                const auto tx{std::ranges::find_if(block.vtx, [&](const CTransactionRef& tx) { return tx->GetHash() == deposit.outpoint.hash; })};
                CHECK_NONFATAL(tx != block.vtx.end() && deposit.outpoint.n < (*tx)->vout.size());
                const auto parsed{CHECK_NONFATAL(bip300::ParseDeposit((*tx)->vout[deposit.outpoint.n]))};
                UniValue obj{UniValue::VOBJ};
                obj.pushKV("slot", deposit.slot);
                obj.pushKV("sequence", deposit.sequence);
                obj.pushKV("txid", deposit.outpoint.hash.GetHex());
                obj.pushKV("vout", deposit.outpoint.n);
                obj.pushKV("address", HexStr(parsed->address));
                obj.pushKV("amount", ValueFromAmount(deposit.value));
                deposits.push_back(std::move(obj));
            }
            res.pushKV("deposits", std::move(deposits));
            res.pushKV("submitted", BundleList(events.submitted));
            UniValue succeeded{UniValue::VARR};
            for (const bip300::BlockEvents::Payout& payout : events.payouts) {
                const auto tx{std::ranges::find_if(block.vtx, [&](const CTransactionRef& tx) { return tx->GetHash() == payout.txid; })};
                CHECK_NONFATAL(tx != block.vtx.end());
                UniValue obj{UniValue::VOBJ};
                obj.pushKV("slot", payout.slot);
                obj.pushKV("m6id", payout.m6id.GetHex());
                obj.pushKV("sequence", payout.sequence);
                obj.pushKV("txid", payout.txid.GetHex());
                obj.pushKV("hex", EncodeHexTx(**tx));
                succeeded.push_back(std::move(obj));
            }
            res.pushKV("succeeded", std::move(succeeded));
            res.pushKV("failed", BundleList(events.failed));
            return res;
        },
    };
}

RPCMethod getsidechainproducer()
{
    return RPCMethod{
        "getsidechainproducer",
        "Returns what this node puts in the blocks it builds for this chain's sidechains (BIP300): its proposals, ACKs and\n"
        "policies, and the bundles it was given.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::ARR, "proposals", "Sidechains it proposes (M1) until they activate", {
                {RPCResult::Type::OBJ, "", "", {SLOT_RESULT, {RPCResult::Type::STR_HEX, "description", ""}, {RPCResult::Type::STR_HEX, "description_hash", ""}}},
            }},
            {RPCResult::Type::ARR, "sidechain_acks", "Proposals it ACKs (M2) explicitly", {
                {RPCResult::Type::OBJ, "", "", {SLOT_RESULT, {RPCResult::Type::STR_HEX, "description_hash", ""}}},
            }},
            {RPCResult::Type::STR, "ack_policy", "Which other proposals it ACKs: none, new_slots or all"},
            {RPCResult::Type::ARR, "bundle_acks", "Bundles it upvotes (M4) explicitly", {
                {RPCResult::Type::OBJ, "", "", {SLOT_RESULT, {RPCResult::Type::STR_HEX, "m6id", ""}}},
            }},
            {RPCResult::Type::STR, "bundle_policy", "Which bundle it upvotes otherwise: none, known, all or alarm"},
            {RPCResult::Type::ARR, "bundles", "The bundles it was given, to propose (M3) and pay out", {
                {RPCResult::Type::OBJ, "", "", {SLOT_RESULT, {RPCResult::Type::STR_HEX, "m6id", ""}, {RPCResult::Type::BOOL, "seen_pending", "Whether it saw it pending"}, {RPCResult::Type::STR_HEX, "hex", "Its blinded M6"}}},
            }},
        }},
        RPCExamples{HelpExampleCli("getsidechainproducer", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            const bip300::Producer& producer{EnsureProducer(request)};
            const bip300::Producer::Settings settings{producer.GetSettings()};
            UniValue res{UniValue::VOBJ};
            UniValue proposals{UniValue::VARR};
            for (const auto& [slot, description] : settings.proposals) {
                UniValue obj{UniValue::VOBJ};
                obj.pushKV("slot", slot);
                obj.pushKV("description", HexStr(description));
                obj.pushKV("description_hash", bip300::DescriptionHash(description).GetHex());
                proposals.push_back(std::move(obj));
            }
            res.pushKV("proposals", std::move(proposals));
            UniValue sidechain_acks{UniValue::VARR};
            for (const auto& [slot, hash] : settings.sidechain_acks) {
                UniValue obj{UniValue::VOBJ};
                obj.pushKV("slot", slot);
                obj.pushKV("description_hash", hash.GetHex());
                sidechain_acks.push_back(std::move(obj));
            }
            res.pushKV("sidechain_acks", std::move(sidechain_acks));
            res.pushKV("ack_policy", bip300::AckPolicyName(settings.ack_policy));
            res.pushKV("bundle_acks", BundleList({settings.bundle_acks.begin(), settings.bundle_acks.end()}));
            res.pushKV("bundle_policy", bip300::BundlePolicyName(settings.bundle_policy));
            UniValue bundles{UniValue::VARR};
            for (const auto& [m6id, bundle] : producer.GetBundles()) {
                UniValue obj{UniValue::VOBJ};
                obj.pushKV("slot", bundle.slot);
                obj.pushKV("m6id", m6id.GetHex());
                obj.pushKV("seen_pending", bundle.seen_pending);
                obj.pushKV("hex", EncodeHexTx(*bundle.blinded));
                bundles.push_back(std::move(obj));
            }
            res.pushKV("bundles", std::move(bundles));
            return res;
        },
    };
}

RPCMethod proposesidechain()
{
    return RPCMethod{
        "proposesidechain",
        "Propose (M1) a sidechain for one of this chain's slots (BIP300) in the blocks this node builds, until it activates.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The slot (0 to 255)"},
            {"description", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The sidechain's description"},
        },
        RPCResult{RPCResult::Type::STR_HEX, "", "The description's hash, which ACKs refer to"},
        RPCExamples{HelpExampleCli("proposesidechain", "9 \"00075468756e646572\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            const uint8_t slot{ParseSlot(request.params[0])};
            std::vector<unsigned char> description{ParseHexV(request.params[1], "description")};
            const uint256 hash{bip300::DescriptionHash(description)};
            EnsureProducer(request).ProposeSidechain(slot, std::move(description));
            return hash.GetHex();
        },
    };
}

RPCMethod setsidechainack()
{
    return RPCMethod{
        "setsidechainack",
        "Set whether the blocks this node builds ACK (M2) a sidechain proposal, whatever the ACK policy.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The slot (0 to 255)"},
            {"description_hash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The proposal's description hash"},
            {"ack", RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether to ACK it"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{HelpExampleCli("setsidechainack", "9 \"<description_hash>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            const uint8_t slot{ParseSlot(request.params[0])};
            const uint256 hash{ParseHashV(request.params[1], "description_hash")};
            EnsureProducer(request).SetSidechainAck(slot, hash, request.params[2].isNull() || request.params[2].get_bool());
            return UniValue::VNULL;
        },
    };
}

RPCMethod setbundleack()
{
    return RPCMethod{
        "setbundleack",
        "Set whether the blocks this node builds upvote (M4) a pending bundle, whatever the bundle policy.\n"
        "An explicit ACK wins over the policy; in each slot the first pending bundle ACKed gets the vote.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The slot (0 to 255)"},
            {"m6id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The bundle's M6ID"},
            {"ack", RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether to ACK it"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{HelpExampleCli("setbundleack", "9 \"<m6id>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            const uint8_t slot{ParseSlot(request.params[0])};
            const uint256 m6id{ParseHashV(request.params[1], "m6id")};
            EnsureProducer(request).SetBundleAck(slot, m6id, request.params[2].isNull() || request.params[2].get_bool());
            return UniValue::VNULL;
        },
    };
}

RPCMethod setsidechainpolicy()
{
    return RPCMethod{
        "setsidechainpolicy",
        "Set the policies of the blocks this node builds for this chain's sidechains (BIP300).",
        {
            {"options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::NO, "",
            {
                {"ack_policy", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Which proposals to ACK besides explicit ACKs: \"none\", \"new_slots\" (for unused slots) or \"all\""},
                {"bundle_policy", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Which bundle to upvote in a slot without an explicit ACK: \"none\", \"known\" (the first one this node was given), \"all\" (the first one) or \"alarm\" (downvote them all)"},
            }},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{HelpExampleCli("-named setsidechainpolicy", "bundle_policy=none")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            bip300::Producer& producer{EnsureProducer(request)};
            const UniValue& options{request.params[0]};
            std::optional<bip300::AckPolicy> ack_policy;
            std::optional<bip300::BundlePolicy> bundle_policy;
            if (!options["ack_policy"].isNull()) {
                ack_policy = bip300::ParseAckPolicy(options["ack_policy"].get_str());
                if (!ack_policy) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown ack_policy");
            }
            if (!options["bundle_policy"].isNull()) {
                bundle_policy = bip300::ParseBundlePolicy(options["bundle_policy"].get_str());
                if (!bundle_policy) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown bundle_policy");
            }
            if (ack_policy) producer.SetAckPolicy(*ack_policy);
            if (bundle_policy) producer.SetBundlePolicy(*bundle_policy);
            return UniValue::VNULL;
        },
    };
}

RPCMethod submitbundle()
{
    return RPCMethod{
        "submitbundle",
        "Give this node a withdrawal bundle (a blinded M6, BIP300) of one of this chain's sidechains: the blocks it builds\n"
        "propose it (M3) once, and pay it out when the votes for it make that due.",
        {
            {"slot", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sidechain's slot (0 to 255)"},
            {"hexstring", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The blinded M6: no inputs, a first output OP_RETURN <total fee, 8 bytes BE> of value 0, then the sidechain's"},
        },
        RPCResult{RPCResult::Type::STR_HEX, "", "Its M6ID"},
        RPCExamples{HelpExampleCli("submitbundle", "9 \"<hex>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
            const uint8_t slot{ParseSlot(request.params[0])};
            CMutableTransaction mtx;
            if (!DecodeHexTx(mtx, request.params[1].get_str(), /*try_no_witness=*/true, /*try_witness=*/false)) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
            }
            auto m6id{EnsureProducer(request).AddBundle(slot, MakeTransactionRef(std::move(mtx)))};
            if (!m6id) throw JSONRPCError(RPC_INVALID_PARAMETER, util::ErrorString(m6id).original);
            return m6id->GetHex();
        },
    };
}

} // namespace

void RegisterSidechainRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"sidechains", &getsidechains},
        {"sidechains", &getsidechainproposals},
        {"sidechains", &getpendingbundles},
        {"sidechains", &getsidechainevents},
        {"sidechains", &getsidechainproducer},
        {"sidechains", &proposesidechain},
        {"sidechains", &setsidechainack},
        {"sidechains", &setbundleack},
        {"sidechains", &setsidechainpolicy},
        {"sidechains", &submitbundle},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
