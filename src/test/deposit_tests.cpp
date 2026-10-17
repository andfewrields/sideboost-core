// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <drivechain/deposit.h>
#include <key.h>
#include <key_io.h>
#include <script/interpreter.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <undo.h>

#include <boost/test/unit_test.hpp>

using namespace drivechain;

BOOST_FIXTURE_TEST_SUITE(deposit_tests, BasicTestingSetup)

static Deposit RandomDeposit(FastRandomContext& rng, uint64_t sequence, const std::string& address)
{
    return Deposit{
        .sequence = sequence,
        .block_hash = rng.rand256(),
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

    const auto claim{ParseDepositTransaction(*tx)};
    BOOST_REQUIRE(claim);
    BOOST_CHECK(claim->block_hash == deposit.block_hash);
    BOOST_CHECK_EQUAL(claim->sequence, deposit.sequence);

    // An address that doesn't decode burns the deposit
    const CTransactionRef burn{MakeDepositTransaction(RandomDeposit(m_rng, 0, "not-an-address"))};
    BOOST_CHECK(burn->vout[0].scriptPubKey.IsUnspendable());

    // Malformed claims
    CMutableTransaction bad{*tx};
    bad.vin[0].scriptSig = CScript() << OP_RETURN << ToByteVector(deposit.block_hash);
    BOOST_CHECK(CTransaction{bad}.IsDeposit());
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
    const DepositState state{42, m_rng.rand256()};
    const Coin coin{MakeDepositStateCoin(state, 100)};
    const auto parsed{ParseDepositStateCoin(coin)};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK_EQUAL(parsed->next_sequence, state.next_sequence);
    BOOST_CHECK(parsed->last_block == state.last_block);
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
        BOOST_CHECK(state.last_block == ParseDepositTransaction(*txs.back())->block_hash);
    }
    BOOST_CHECK(UndoDepositState(std::move(undos[1].vprevout[0]), view));
    BOOST_CHECK_EQUAL(ReadDepositState(view).next_sequence, 1U);
    BOOST_CHECK(UndoDepositState(std::move(undos[0].vprevout[0]), view));
    BOOST_CHECK(ReadDepositState(view).IsInitial());
    BOOST_CHECK(!view.HaveCoin(DepositStateOutpoint()));

    // Nothing to undo
    BOOST_CHECK(!UndoDepositState(MakeDepositStateCoin({}, 1), view));
}

BOOST_AUTO_TEST_SUITE_END()
