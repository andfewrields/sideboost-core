// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <coins.h>
#include <crypto/common.h>
#include <drivechain/parentchain.h>
#include <drivechain/withdrawal.h>
#include <key.h>
#include <script/interpreter.h>
#include <test/util/parentchain.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <undo.h>

#include <boost/test/unit_test.hpp>

using namespace drivechain;

BOOST_FIXTURE_TEST_SUITE(withdrawal_tests, BasicTestingSetup)

namespace {

CScript RandomPayoutScript(FastRandomContext& rng)
{
    return GetScriptForDestination(WitnessV0KeyHash{uint160{rng.randbytes(20)}});
}

/** An ordinary transaction requesting `count` withdrawals.  */
CTransactionRef WithdrawalTx(FastRandomContext& rng, size_t count)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(rng.rand256()), 0});
    for (size_t i = 0; i < count; ++i) {
        tx.vout.emplace_back(1000 + i, MakeWithdrawalScript(RandomPayoutScript(rng), 100));
    }
    tx.vout.emplace_back(5000, RandomPayoutScript(rng)); // change, an ordinary output
    return MakeTransactionRef(std::move(tx));
}

CBlock MakeBlock(FastRandomContext& rng, std::vector<CTransactionRef> txs)
{
    CMutableTransaction coinbase;
    coinbase.vin.emplace_back(COutPoint{});
    coinbase.vin[0].scriptSig = CScript() << rng.randbits(30);
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.vtx.insert(block.vtx.end(), txs.begin(), txs.end());
    block.nNonce = rng.rand32();
    return block;
}

/** The view's coins at the queue's positions.  */
size_t QueueCoins(const CCoinsViewCache& view, uint32_t end)
{
    size_t res{0};
    for (uint32_t position{0}; position < end; ++position) res += view.HaveCoin(QueueOutpoint(position));
    return res;
}

/** Blocks connected to a view, to disconnect them again.  */
struct Connected {
    CBlock block;
    CBlockUndo undo;
    int height;
};

} // namespace

BOOST_AUTO_TEST_CASE(withdrawal_script)
{
    const CScript payout{RandomPayoutScript(m_rng)};
    const CTxOut out{12345, MakeWithdrawalScript(payout, 345)};
    BOOST_CHECK(IsWithdrawalScript(out.scriptPubKey));
    BOOST_CHECK(out.scriptPubKey.IsUnspendable()); // never a coin itself
    const auto withdrawal{ParseWithdrawal(out)};
    BOOST_REQUIRE(withdrawal);
    BOOST_CHECK(withdrawal->payout_script == payout);
    BOOST_CHECK_EQUAL(withdrawal->payout, 12000);
    BOOST_CHECK_EQUAL(withdrawal->fee, 345);
    BOOST_CHECK_EQUAL(withdrawal->Value(), out.nValue);

    // It must pay out something
    BOOST_CHECK(ParseWithdrawal(CTxOut{346, out.scriptPubKey}));
    BOOST_CHECK(!ParseWithdrawal(CTxOut{345, out.scriptPubKey}));
    BOOST_CHECK(!ParseWithdrawal(CTxOut{0, out.scriptPubKey}));

    // Malformed ones still claim to be withdrawals
    const CScript too_long{MakeWithdrawalScript(CScript() << std::vector<unsigned char>(MAX_PAYOUT_SCRIPT_SIZE), 1)};
    BOOST_CHECK(IsWithdrawalScript(too_long));
    BOOST_CHECK(!ParseWithdrawal(CTxOut{1000, too_long}));
    BOOST_CHECK(!ParseWithdrawal(CTxOut{1000, MakeWithdrawalScript(CScript{}, 1)}));
    CScript extra{out.scriptPubKey};
    extra << OP_TRUE;
    BOOST_CHECK(IsWithdrawalScript(extra));
    BOOST_CHECK(!ParseWithdrawal(CTxOut{12345, extra}));

    // Other OP_RETURNs don't
    BOOST_CHECK(!IsWithdrawalScript(CScript() << OP_RETURN << std::vector<unsigned char>{'S', 'B', 'W', 'X'}));
    BOOST_CHECK(!IsWithdrawalScript(CScript() << OP_RETURN));
    BOOST_CHECK(!IsWithdrawalScript(payout));
}

BOOST_AUTO_TEST_CASE(state_coins)
{
    const QueueEntry entry{
        .origin = COutPoint{Txid::FromUint256(m_rng.rand256()), 3},
        .withdrawal = {.payout_script = RandomPayoutScript(m_rng), .payout = 5000, .fee = 70},
    };
    const Coin entry_coin{MakeQueueEntryCoin(entry, 17)};
    BOOST_CHECK_EQUAL(entry_coin.out.nValue, 5070);
    BOOST_CHECK_EQUAL(entry_coin.nHeight, 17U);
    const auto parsed{ParseQueueEntryCoin(entry_coin)};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(parsed->origin == entry.origin);
    BOOST_CHECK(parsed->withdrawal == entry.withdrawal);

    WithdrawalState state{.head = 4, .tail = 9, .bundle = PendingBundle{.size = 3, .height = 12, .block_hash = m_rng.rand256()}, .last_outcome_height = 11};
    const Coin state_coin{MakeWithdrawalStateCoin(state)};
    BOOST_CHECK_EQUAL(state_coin.nHeight, 0U);
    BOOST_CHECK(ParseWithdrawalStateCoin(state_coin) == state);
    BOOST_CHECK_EQUAL(state.BundleStart(), 1U);
    BOOST_CHECK(ParseWithdrawalStateCoin(MakeWithdrawalStateCoin({})) == WithdrawalState{});
    state.bundle.reset();
    BOOST_CHECK(ParseWithdrawalStateCoin(MakeWithdrawalStateCoin(state)) == state);

    // Both are kept in the UTXO set, and no scriptSig can spend them
    for (const Coin* coin : {&entry_coin, &state_coin}) {
        BOOST_CHECK(!coin->out.scriptPubKey.IsUnspendable());
        for (const CScript& script_sig : {CScript{}, CScript() << OP_TRUE, CScript() << OP_1 << OP_1 << OP_1}) {
            ScriptError err;
            BOOST_CHECK(!VerifyScript(script_sig, coin->out.scriptPubKey, nullptr, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, &err));
        }
    }
    BOOST_CHECK(!ParseQueueEntryCoin(state_coin));
    BOOST_CHECK(!ParseWithdrawalStateCoin(entry_coin));
}

BOOST_AUTO_TEST_CASE(blinded_m6)
{
    const std::vector<Withdrawal> withdrawals{
        {.payout_script = RandomPayoutScript(m_rng), .payout = 1000, .fee = 10},
        {.payout_script = RandomPayoutScript(m_rng), .payout = 2000, .fee = 20},
    };
    const BundleCommitment commitment{.block_hash = m_rng.rand256(), .height = 1234};
    const CTransactionRef m6{MakeBlindedM6(withdrawals, commitment)};
    BOOST_CHECK(m6->vin.empty());
    BOOST_CHECK_EQUAL(m6->version, 2U);
    BOOST_REQUIRE_EQUAL(m6->vout.size(), 4U);
    // The total fee, big endian, where the treasury output goes
    std::vector<unsigned char> fee(8);
    WriteBE64(fee.data(), 30);
    BOOST_CHECK(m6->vout[0] == CTxOut(0, CScript() << OP_RETURN << fee));
    // The block that created the bundle: its hash and height
    std::vector<unsigned char> block(commitment.block_hash.begin(), commitment.block_hash.end());
    block.insert(block.end(), {0xd2, 0x04, 0, 0});
    BOOST_CHECK(m6->vout[1] == CTxOut(0, CScript() << OP_RETURN << block));
    BOOST_CHECK(ParseBundleCommitment(*m6) == commitment);
    BOOST_CHECK(m6->vout[2] == CTxOut(1000, withdrawals[0].payout_script));
    BOOST_CHECK(m6->vout[3] == CTxOut(2000, withdrawals[1].payout_script));
    // Another bundle of the same withdrawals has another M6ID
    BOOST_CHECK(MakeBlindedM6(withdrawals, {m_rng.rand256(), 1234})->GetHash() != m6->GetHash());

    // So does the mined M6, with the treasury in output 0
    CMutableTransaction mined{*m6};
    mined.vin.emplace_back(COutPoint{Txid::FromUint256(m_rng.rand256()), 0});
    mined.vout[0] = CTxOut{100000, CScript() << OP_NOP8 << std::vector<unsigned char>{83} << OP_TRUE};
    BOOST_CHECK(ParseBundleCommitment(CTransaction{mined}) == commitment);
    // Others' don't
    mined.vout[1].scriptPubKey = CScript() << OP_RETURN << m_rng.randbytes(32);
    BOOST_CHECK(!ParseBundleCommitment(CTransaction{mined}));
    mined.vout.resize(1);
    BOOST_CHECK(!ParseBundleCommitment(CTransaction{mined}));
}

BOOST_AUTO_TEST_CASE(bundle_outcome_transaction)
{
    const uint256 m6id{m_rng.rand256()};
    for (const BundleOutcome outcome : {BundleOutcome::SUCCEEDED, BundleOutcome::FAILED}) {
        const CTransactionRef tx{MakeBundleOutcomeTransaction(m6id, outcome)};
        BOOST_CHECK(tx->IsParentChainEvent());
        BOOST_CHECK(tx->IsBundleOutcome());
        BOOST_CHECK(!tx->IsDeposit());
        BOOST_CHECK(!tx->IsCoinBase());
        const auto parsed{ParseBundleOutcomeTransaction(*tx)};
        BOOST_REQUIRE(parsed);
        BOOST_CHECK(parsed->first == m6id);
        BOOST_CHECK(parsed->second == outcome);
    }
    CMutableTransaction bad{*MakeBundleOutcomeTransaction(m6id, BundleOutcome::FAILED)};
    bad.vout[0].nValue = 1;
    BOOST_CHECK(CTransaction{bad}.IsBundleOutcome());
    BOOST_CHECK(!ParseBundleOutcomeTransaction(CTransaction{bad}));
    bad = CMutableTransaction{*MakeBundleOutcomeTransaction(m6id, BundleOutcome::FAILED)};
    bad.vin[0].scriptSig = CScript() << OP_RETURN << OP_3;
    BOOST_CHECK(CTransaction{bad}.IsParentChainEvent());
    BOOST_CHECK(!CTransaction{bad}.IsBundleOutcome());
}

BOOST_AUTO_TEST_CASE(withdrawal_queue)
{
    CCoinsViewCache view{&CoinsViewEmpty::Get()};
    std::vector<Connected> connected;
    const auto connect{[&](std::vector<CTransactionRef> txs) {
        Connected c{MakeBlock(m_rng, std::move(txs)), {}, static_cast<int>(connected.size()) + 1};
        c.undo.vtxundo.resize(c.block.vtx.size() - 1);
        const auto outcome{BundleOutcomeIndex(c.block)};
        BOOST_REQUIRE(ConnectWithdrawals(c.block, c.height, c.block.GetHash(), view, outcome ? &c.undo.vtxundo[*outcome - 1] : nullptr));
        connected.push_back(std::move(c));
        return ReadWithdrawalState(view);
    }};
    const auto pending_m6id{[&] { return PendingBlindedM6(view, ReadWithdrawalState(view))->GetHash().ToUint256(); }};

    // Two withdrawals: bundled at once
    WithdrawalState state{connect({WithdrawalTx(m_rng, 2)})};
    BOOST_CHECK_EQUAL(state.tail, 2U);
    BOOST_CHECK_EQUAL(state.head, 2U);
    BOOST_REQUIRE(state.bundle);
    BOOST_CHECK_EQUAL(state.bundle->size, 2U);
    BOOST_CHECK_EQUAL(state.bundle->height, 1);
    BOOST_CHECK(state.bundle->block_hash == connected[0].block.GetHash());
    BOOST_CHECK_EQUAL(QueueCoins(view, 2), 2U);
    const auto bundled{BundleWithdrawals(view, state)};
    BOOST_REQUIRE(bundled);
    BOOST_CHECK(bundled->at(1) == *ParseWithdrawal(connected[0].block.vtx[1]->vout[1]));
    const uint256 first{pending_m6id()};

    // Another one waits
    state = connect({WithdrawalTx(m_rng, 1)});
    BOOST_CHECK_EQUAL(state.tail, 3U);
    BOOST_CHECK_EQUAL(state.head, 2U);
    BOOST_CHECK_EQUAL(state.bundle->size, 2U);
    BOOST_CHECK(pending_m6id() == first);

    // The bundle fails: its withdrawals and the waiting one form a new bundle
    state = connect({MakeBundleOutcomeTransaction(first, BundleOutcome::FAILED)});
    BOOST_CHECK_EQUAL(state.head, 3U);
    BOOST_CHECK_EQUAL(state.bundle->size, 3U);
    BOOST_CHECK_EQUAL(state.bundle->height, 3);
    BOOST_CHECK_EQUAL(state.last_outcome_height, 3);
    BOOST_CHECK_EQUAL(QueueCoins(view, 3), 3U);
    const uint256 second{pending_m6id()};
    BOOST_CHECK(second != first);
    // Its undo data has the state before
    BOOST_CHECK_EQUAL(connected[2].undo.vtxundo[0].vprevout.size(), 1U);
    BOOST_CHECK(ParseWithdrawalStateCoin(connected[2].undo.vtxundo[0].vprevout[0])->bundle->size == 2U);

    // Which bundle an outcome is for is checked by validation, not here
    {
        const CBlock wrong{MakeBlock(m_rng, {MakeBundleOutcomeTransaction(first, BundleOutcome::FAILED)})};
        WithdrawalState copy{state};
        BOOST_CHECK(BlockWithdrawalChanges(wrong, 4, wrong.GetHash(), copy, [](uint32_t) { return std::nullopt; }));
    }

    // It succeeds: its withdrawals leave the chain
    state = connect({MakeBundleOutcomeTransaction(second, BundleOutcome::SUCCEEDED), WithdrawalTx(m_rng, 0)});
    BOOST_CHECK_EQUAL(state.head, 3U);
    BOOST_CHECK_EQUAL(state.tail, 3U);
    BOOST_CHECK(!state.bundle);
    BOOST_CHECK_EQUAL(state.last_outcome_height, 4);
    BOOST_CHECK_EQUAL(QueueCoins(view, 3), 0U);
    BOOST_CHECK_EQUAL(connected[3].undo.vtxundo[0].vprevout.size(), 4U); // the state, then the 3 withdrawals

    // A success without a pending bundle is inconsistent
    {
        CCoinsViewCache scratch{&view};
        const CBlock again{MakeBlock(m_rng, {MakeBundleOutcomeTransaction(second, BundleOutcome::SUCCEEDED)})};
        CTxUndo undo;
        BOOST_CHECK(!ConnectWithdrawals(again, 5, again.GetHash(), scratch, &undo));
    }

    // Bundles are at most MAX_BUNDLE_WITHDRAWALS
    state = connect({WithdrawalTx(m_rng, MAX_BUNDLE_WITHDRAWALS + 1)});
    BOOST_CHECK_EQUAL(state.bundle->size, MAX_BUNDLE_WITHDRAWALS);
    BOOST_CHECK_EQUAL(state.tail - state.head, 1U);

    // The state before each block, and disconnecting them all
    for (auto it{connected.rbegin()}; it != connected.rend(); ++it) {
        const WithdrawalState after{ReadWithdrawalState(view)};
        const auto before{WithdrawalStateBefore(it->block, it->undo, it->height, after)};
        BOOST_REQUIRE(before);
        BOOST_REQUIRE(DisconnectWithdrawals(it->block, it->undo, it->height, it->block.GetHash(), view));
        BOOST_CHECK(ReadWithdrawalState(view) == *before);
    }
    BOOST_CHECK(ReadWithdrawalState(view).IsInitial());
    BOOST_CHECK(!view.HaveCoin(WithdrawalStateOutpoint()));
    BOOST_CHECK_EQUAL(QueueCoins(view, MAX_BUNDLE_WITHDRAWALS + 4), 0U);
}

BOOST_AUTO_TEST_CASE(parent_chain_bundles)
{
    FakeParentChain parent{m_rng};
    ParentChainView view{parent, /*slot=*/83, /*min_depth=*/3};
    const uint256 m6id{m_rng.rand256()}, other{m_rng.rand256()};
    using Kind = BundleEvent::Kind;
    const auto event{[](const uint256& id, Kind kind) { return BundleEvent{.m6id = id, .kind = kind, .block_hash = {}, .height = 0, .commitment = std::nullopt}; }};
    const auto status{[&](BundleOutcome outcome) { return view.CheckBundleOutcome(*MakeBundleOutcomeTransaction(m6id, outcome)); }};

    parent.Mine({}, {}, {event(m6id, Kind::SUBMITTED), event(other, Kind::SUBMITTED)});
    view.Update();
    BOOST_CHECK(view.TakeBundleChanges()); // first update: check everything
    BOOST_CHECK(!view.BundleOutcomeOf(m6id));
    BOOST_CHECK(status(BundleOutcome::FAILED) == DepositStatus::UNKNOWN);

    // Proposals: once per tip, and never for a bundle that ended
    const CTransactionRef blinded{MakeBlindedM6({}, {m_rng.rand256(), 1})};
    view.ProposeBundle(*blinded);
    view.ProposeBundle(*blinded);
    BOOST_CHECK_EQUAL(parent.proposals.size(), 1U);

    // It fails; a later success (proposed again) doesn't count
    const uint256 failed_in{parent.Mine({}, {}, {event(m6id, Kind::FAILED)})};
    parent.Mine({}, {}, {event(m6id, Kind::SUBMITTED)});
    view.Update();
    BOOST_CHECK(!view.TakeBundleChanges());
    BOOST_CHECK(view.BundleOutcomeOf(m6id) == std::pair(BundleOutcome::FAILED, false));
    BOOST_CHECK(status(BundleOutcome::FAILED) == DepositStatus::TOO_SHALLOW);
    BOOST_CHECK(status(BundleOutcome::SUCCEEDED) == DepositStatus::MISMATCH);
    parent.Mine({}, {}, {event(m6id, Kind::SUCCEEDED)});
    parent.Mine();
    view.Update();
    BOOST_CHECK(status(BundleOutcome::FAILED) == DepositStatus::OK); // 3 blocks on top
    BOOST_CHECK(!view.IsOutcomeFinal(m6id));
    view.ProposeBundle(*MakeBlindedM6({}, {m6id, 1})); // another bundle, new tip
    BOOST_CHECK_EQUAL(parent.proposals.size(), 2U);

    // A reorg that drops the failure changes it
    parent.tip = parent.blocks.at(failed_in).header.prev;
    for (int i = 0; i < 5; ++i) parent.Mine();
    view.Update();
    BOOST_CHECK(view.TakeBundleChanges());
    BOOST_CHECK(status(BundleOutcome::FAILED) == DepositStatus::UNKNOWN);

    // One that fails it elsewhere, deep enough, doesn't
    parent.Mine({}, {}, {event(m6id, Kind::FAILED)});
    for (int i = 0; i < DEPOSIT_FINALITY_DEPTH; ++i) parent.Mine();
    view.Update();
    BOOST_CHECK(!view.TakeBundleChanges());
    BOOST_CHECK(status(BundleOutcome::FAILED) == DepositStatus::OK);
    BOOST_CHECK(view.IsOutcomeFinal(m6id));
}

BOOST_AUTO_TEST_CASE(checkpoint)
{
    FakeParentChain parent{m_rng};
    ParentChainView view{parent, /*slot=*/83, /*min_depth=*/3};
    const auto success{[&](std::optional<BundleCommitment> commitment) {
        return BundleEvent{.m6id = m_rng.rand256(), .kind = BundleEvent::Kind::SUCCEEDED, .block_hash = {}, .height = 0, .commitment = commitment};
    }};
    const BundleCommitment first{m_rng.rand256(), 10}, second{m_rng.rand256(), 20};

    // A success's block is the checkpoint once it has 3 confirmations
    parent.Mine({}, {}, {success(first)});
    view.Update();
    BOOST_CHECK(!view.Checkpoint());
    for (int i = 0; i < 3; ++i) parent.Mine();
    view.Update();
    BOOST_CHECK(view.Checkpoint() == first);

    // The latest one is; others' M6s (without our commitment) don't count
    const uint256 second_in{parent.Mine({}, {}, {success(second), success(std::nullopt)})};
    for (int i = 0; i < 3; ++i) parent.Mine({}, {}, {success(std::nullopt)});
    view.Update();
    BOOST_CHECK(view.Checkpoint() == second);

    // A parent chain reorg can take it away
    parent.tip = parent.blocks.at(second_in).header.prev;
    for (int i = 0; i < 5; ++i) parent.Mine();
    view.Update();
    BOOST_CHECK(view.Checkpoint() == first);

    // Chains violate it if they reach its height with another block
    std::vector<uint256> hashes(30);
    std::vector<CBlockIndex> chain(30), fork(30);
    for (int height = 0; height < 30; ++height) {
        hashes[height] = height == 10 ? first.block_hash : m_rng.rand256();
        chain[height].nHeight = fork[height].nHeight = height;
        chain[height].phashBlock = &hashes[height];
        chain[height].pprev = height ? &chain[height - 1] : nullptr;
        // The fork leaves the chain after height 5
        fork[height].phashBlock = height <= 5 ? &hashes[height] : &hashes[(height + 15) % 30];
        fork[height].pprev = height ? (height <= 6 ? &chain[height - 1] : &fork[height - 1]) : nullptr;
    }
    BOOST_CHECK(!ViolatesCheckpoint(chain[29], first));
    BOOST_CHECK(!ViolatesCheckpoint(chain[10], first));
    BOOST_CHECK(!ViolatesCheckpoint(fork[9], first)); // below it: may still lead to it, through a reorg
    BOOST_CHECK(ViolatesCheckpoint(fork[10], first));
    BOOST_CHECK(ViolatesCheckpoint(fork[29], first));
    BOOST_CHECK(!ViolatesCheckpoint(fork[29], std::nullopt));
}

BOOST_AUTO_TEST_SUITE_END()
