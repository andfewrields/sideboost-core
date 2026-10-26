// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <drivechain/deposit.h>
#include <drivechain/parentchain.h>
#include <drivechain/withdrawal.h>
#include <key.h>
#include <key_io.h>
#include <script/interpreter.h>
#include <streams.h>
#include <test/util/parentchain.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <undo.h>
#include <util/chaintype.h>

#include <set>

#include <boost/test/unit_test.hpp>

using namespace drivechain;

BOOST_FIXTURE_TEST_SUITE(deposit_tests, BasicTestingSetup)

static Deposit RandomDeposit(FastRandomContext& rng, uint64_t sequence, const std::string& address)
{
    return Deposit{
        .sequence = sequence,
        .outpoint = COutPoint{Txid::FromUint256(rng.rand256()), static_cast<uint32_t>(rng.randbits<16>())},
        .address = {address.begin(), address.end()},
        .value = static_cast<CAmount>(rng.randrange(MAX_MONEY)) + 1,
    };
}

BOOST_AUTO_TEST_CASE(deposit_transaction)
{
    const CKey key{GenerateRandomKey()};
    const CTxDestination dest{WitnessV0KeyHash{key.GetPubKey()}};
    const Deposit deposit{RandomDeposit(m_rng, 7, EncodeDestination(dest))};

    const CTransactionRef tx{MakeDepositTransaction(deposit)};
    BOOST_CHECK(tx->IsDeposit());
    BOOST_CHECK(!tx->IsCoinBase());
    BOOST_CHECK(tx->vin[0].prevout == deposit.outpoint);
    BOOST_CHECK_EQUAL(tx->vout.size(), 1U);
    BOOST_CHECK_EQUAL(tx->vout[0].nValue, deposit.value);
    BOOST_CHECK(tx->vout[0].scriptPubKey == GetScriptForDestination(dest));

    BOOST_CHECK_EQUAL(ParseDepositTransaction(*tx).value_or(0), deposit.sequence);

    // An address that doesn't decode burns the deposit
    const CTransactionRef burn{MakeDepositTransaction(RandomDeposit(m_rng, 0, "not-an-address"))};
    BOOST_CHECK(burn->vout[0].scriptPubKey.IsUnspendable());

    // Malformed claims
    CMutableTransaction bad{*tx};
    bad.vin[0].scriptSig = CScript() << OP_RETURN << std::vector<unsigned char>(7);
    BOOST_CHECK(CTransaction{bad}.IsParentChainEvent());
    BOOST_CHECK(!CTransaction{bad}.IsDeposit());
    BOOST_CHECK(!ParseDepositTransaction(CTransaction{bad}));
    bad.vin[0].scriptSig = tx->vin[0].scriptSig;
    bad.vin[0].scriptSig << OP_TRUE;
    BOOST_CHECK(!ParseDepositTransaction(CTransaction{bad}));

    // Ordinary spends and coinbases are not deposits
    bad.vin[0].scriptSig = CScript() << OP_TRUE << OP_RETURN;
    BOOST_CHECK(!CTransaction{bad}.IsDeposit());
    bad.vin[0].scriptSig = tx->vin[0].scriptSig;
    bad.vin[0].prevout.SetNull();
    BOOST_CHECK(!CTransaction{bad}.IsDeposit());
}

BOOST_AUTO_TEST_CASE(deposit_state)
{
    const DepositState state{42};
    const Coin coin{MakeDepositStateCoin(state, 100)};
    const auto parsed{ParseDepositStateCoin(coin)};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK_EQUAL(parsed->next_sequence, state.next_sequence);
    BOOST_CHECK(!coin.out.scriptPubKey.IsUnspendable()); // kept in the UTXO set

    // No scriptSig can spend it
    for (const CScript& script_sig : {CScript{}, CScript() << OP_TRUE, CScript() << OP_1 << OP_1 << OP_1}) {
        ScriptError err;
        BOOST_CHECK(!VerifyScript(script_sig, coin.out.scriptPubKey, nullptr, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, &err));
    }
}

BOOST_AUTO_TEST_CASE(deposit_coins)
{
    CCoinsViewCache view{&CoinsViewEmpty::Get()};
    BOOST_CHECK(ReadDepositState(view).IsInitial());

    // Credit two deposits, then undo them
    std::vector<CTransactionRef> txs;
    std::vector<CTxUndo> undos(2);
    for (uint64_t sequence = 0; sequence < 2; ++sequence) {
        txs.push_back(MakeDepositTransaction(RandomDeposit(m_rng, sequence, "not-an-address")));
        UpdateCoinsForDeposit(*txs.back(), view, undos[sequence], /*height=*/10 + sequence);
        const DepositState state{ReadDepositState(view)};
        BOOST_CHECK_EQUAL(state.next_sequence, sequence + 1);
    }
    BOOST_CHECK(UndoDepositState(std::move(undos[1].vprevout[0]), view));
    BOOST_CHECK_EQUAL(ReadDepositState(view).next_sequence, 1U);
    BOOST_CHECK(UndoDepositState(std::move(undos[0].vprevout[0]), view));
    BOOST_CHECK(ReadDepositState(view).IsInitial());
    BOOST_CHECK(!view.HaveCoin(DepositStateOutpoint()));

    // Nothing to undo
    BOOST_CHECK(!UndoDepositState(MakeDepositStateCoin({}, 1), view));
}

namespace {

TreasuryEvent DepositEvent(const Deposit& deposit) { return {deposit.sequence, {}, 0, deposit}; }
TreasuryEvent WithdrawalEvent(uint64_t sequence) { return {sequence, {}, 0, std::nullopt}; }

} // namespace

BOOST_AUTO_TEST_CASE(parent_chain_view)
{
    FakeParentChain parent{m_rng};
    ParentChainView view{parent, /*slot=*/83, /*min_depth=*/3};

    const Deposit d0{RandomDeposit(m_rng, 0, "a")}, d2{RandomDeposit(m_rng, 2, "b")};
    parent.Mine({DepositEvent(d0), WithdrawalEvent(1)});
    const uint256 block2{parent.Mine({DepositEvent(d2)})};
    view.Update();
    BOOST_CHECK_EQUAL(view.TakeChanges().value_or(99), 0U); // first update: check everything
    BOOST_CHECK(view.TakeTipChanged());

    // Confirmations: d0 has 1 block on top, d2 none
    BOOST_CHECK(view.CheckDeposit(*MakeDepositTransaction(d0)) == DepositStatus::TOO_SHALLOW);
    BOOST_CHECK(view.ConfirmedDeposits(0).empty());
    parent.Mine();
    parent.Mine();
    view.Update();
    BOOST_CHECK(!view.TakeChanges());
    BOOST_CHECK(view.CheckDeposit(*MakeDepositTransaction(d0)) == DepositStatus::OK);
    BOOST_CHECK(view.CheckDeposit(*MakeDepositTransaction(d2)) == DepositStatus::TOO_SHALLOW);
    BOOST_CHECK(view.ConfirmedDeposits(0) == std::vector{d0});
    parent.Mine();
    view.Update();
    // The withdrawal's number is skipped
    BOOST_CHECK(view.ConfirmedDeposits(0) == (std::vector{d0, d2}));
    BOOST_CHECK(view.ConfirmedDeposits(1) == std::vector{d2});
    BOOST_CHECK_EQUAL(view.NextDeposit(1)->first.sequence, 2U);
    BOOST_CHECK(view.CheckDeposit(*MakeDepositTransaction(RandomDeposit(m_rng, 1, "c"))) == DepositStatus::MISMATCH);
    BOOST_CHECK(view.CheckDeposit(*MakeDepositTransaction(RandomDeposit(m_rng, 2, "c"))) == DepositStatus::MISMATCH);
    BOOST_CHECK(view.CheckDeposit(*MakeDepositTransaction(RandomDeposit(m_rng, 3, "c"))) == DepositStatus::UNKNOWN);

    // A reorg that mines d2 again elsewhere changes nothing
    const uint256 fork{parent.blocks.at(block2).header.prev};
    parent.Mine({DepositEvent(d2)}, fork);
    for (int i = 0; i < 4; ++i) parent.Mine();
    view.Update();
    BOOST_CHECK(!view.TakeChanges());
    BOOST_CHECK(view.CheckDeposit(*MakeDepositTransaction(d2)) == DepositStatus::OK);

    // A reorg without d2 changes it, and so does one with another deposit 2
    parent.tip = fork;
    for (int i = 0; i < 6; ++i) parent.Mine();
    view.Update();
    BOOST_CHECK_EQUAL(view.TakeChanges().value_or(99), 2U);
    BOOST_CHECK(view.CheckDeposit(*MakeDepositTransaction(d2)) == DepositStatus::UNKNOWN);
    const Deposit other2{RandomDeposit(m_rng, 2, "d")};
    parent.Mine({DepositEvent(other2)}, fork);
    for (int i = 0; i < 9; ++i) parent.Mine();
    view.Update();
    BOOST_CHECK(!view.TakeChanges()); // nothing it had before is gone
    BOOST_CHECK(view.CheckDeposit(*MakeDepositTransaction(d2)) == DepositStatus::MISMATCH);
    BOOST_CHECK(view.ConfirmedDeposits(2) == std::vector{other2});
}

BOOST_AUTO_TEST_CASE(parent_chain_network)
{
    FakeParentChain parent{m_rng};
    parent.Mine();
    const uint256 genesis{parent.GetHeaders(parent.tip, 10).back().hash};

    // The source has the genesis block of the network the view is for
    ParentChainView view{parent, /*slot=*/83, /*min_depth=*/3, /*manage_votes=*/false, /*file=*/{}, genesis};
    BOOST_CHECK(view.CheckNetwork());
    view.Update();

    // Another network's: nothing from it
    ParentChainView other{parent, /*slot=*/83, /*min_depth=*/3, /*manage_votes=*/false, /*file=*/{}, m_rng.rand256()};
    BOOST_CHECK(!other.CheckNetwork());
    BOOST_CHECK_THROW(other.Update(), DepositSourceError);
    BOOST_CHECK(other.ConfirmedDeposits(0).empty());

    // Every network has its own parent genesis block
    std::set<uint256> geneses;
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET, ChainType::REGTEST}) {
        geneses.insert(ParentGenesisHash(chain));
    }
    BOOST_CHECK_EQUAL(geneses.size(), 5U);
}

BOOST_AUTO_TEST_CASE(parent_chain_view_file)
{
    FakeParentChain parent{m_rng};
    const fs::path file{m_path_root / "parentchain.dat"};
    const Deposit d0{RandomDeposit(m_rng, 0, "a")}, d2{RandomDeposit(m_rng, 2, "b")};
    const BundleEvent submitted{.m6id = m_rng.rand256(), .kind = BundleEvent::Kind::SUBMITTED, .block_hash = {}, .height = 0, .commitment = std::nullopt};
    const BundleEvent succeeded{.m6id = submitted.m6id, .kind = BundleEvent::Kind::SUCCEEDED, .block_hash = {}, .height = 0,
                                .commitment = BundleCommitment{m_rng.rand256(), 7}};
    parent.Mine({DepositEvent(d0), WithdrawalEvent(1)}, {}, {submitted});
    parent.Mine({}, {}, {succeeded});
    for (int i = 0; i < 3; ++i) parent.Mine();
    {
        ParentChainView view{parent, /*slot=*/83, /*min_depth=*/3, /*manage_votes=*/false, file};
        BOOST_CHECK(!view.Load()); // no file yet
        BOOST_CHECK(view.Save()); // nothing to save yet
        BOOST_CHECK(!fs::exists(file));
        view.Update();
        BOOST_CHECK(view.Save());
    }
    BOOST_CHECK(parent.event_starts.back() == std::nullopt); // from genesis

    // A view loaded from the file catches up from where it was
    parent.Mine({DepositEvent(d2)});
    ParentChainView view{parent, /*slot=*/83, /*min_depth=*/3, /*manage_votes=*/false, file};
    BOOST_CHECK(view.Load());
    view.Update();
    BOOST_CHECK(parent.event_starts.back() == parent.blocks.at(parent.tip).header.prev);
    // It checks everything again, as a first update does
    BOOST_CHECK_EQUAL(view.TakeChanges().value_or(99), 0U);
    BOOST_CHECK(view.TakeBundleChanges());
    BOOST_CHECK(view.TakeTipChanged());
    BOOST_CHECK(view.CheckDeposit(*MakeDepositTransaction(d0)) == DepositStatus::OK);
    BOOST_CHECK(view.CheckDeposit(*MakeDepositTransaction(d2)) == DepositStatus::TOO_SHALLOW);
    BOOST_CHECK_EQUAL(view.NextDeposit(1)->first.sequence, 2U); // the withdrawal is kept too
    BOOST_CHECK(view.BundleOutcomeOf(submitted.m6id) == std::pair(BundleOutcome::SUCCEEDED, true));
    BOOST_CHECK(view.Checkpoint() == succeeded.commitment);

    // Loaded at the parent chain's tip, it still checks everything again
    BOOST_CHECK(view.Save());
    ParentChainView same{parent, /*slot=*/83, /*min_depth=*/3, /*manage_votes=*/false, file};
    BOOST_CHECK(same.Load());
    const size_t calls{parent.event_starts.size()};
    same.Update();
    BOOST_CHECK_EQUAL(parent.event_starts.size(), calls);
    BOOST_CHECK_EQUAL(same.TakeChanges().value_or(99), 0U);
    BOOST_CHECK(same.TakeTipChanged());

    // Not a file for another slot, nor a damaged one
    ParentChainView other{parent, /*slot=*/84, /*min_depth=*/3, /*manage_votes=*/false, file};
    BOOST_CHECK(!other.Load());
    std::vector<std::byte> bytes;
    {
        AutoFile in{fsbridge::fopen(file, "rb")};
        bytes.resize(in.size());
        in.read(bytes);
    }
    bytes[bytes.size() / 2] ^= std::byte{1};
    {
        AutoFile out{fsbridge::fopen(file, "wb")};
        out.write(bytes);
        BOOST_CHECK_EQUAL(out.fclose(), 0);
    }
    ParentChainView damaged{parent, /*slot=*/83, /*min_depth=*/3, /*manage_votes=*/false, file};
    BOOST_CHECK(!damaged.Load());
}

BOOST_AUTO_TEST_SUITE_END()
