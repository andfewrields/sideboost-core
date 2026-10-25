// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/devtax.h>
#include <node/miner.h>
#include <primitives/transaction.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(devtax_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(dev_tax)
{
    // None without a reward, or with a reward of 1 sat (left to the miner)
    BOOST_CHECK_EQUAL(GetDevTax(-1), 0);
    BOOST_CHECK_EQUAL(GetDevTax(0), 0);
    BOOST_CHECK_EQUAL(GetDevTax(1), 0);
    // At least the floor
    BOOST_CHECK_EQUAL(GetDevTax(2), 1);
    BOOST_CHECK_EQUAL(GetDevTax(99), 1);
    BOOST_CHECK_EQUAL(GetDevTax(199), 1);
    // 1%, rounded down
    BOOST_CHECK_EQUAL(GetDevTax(200), 2);
    BOOST_CHECK_EQUAL(GetDevTax(9'999), 99);
    // At most the ceiling
    BOOST_CHECK_EQUAL(GetDevTax(10'000), 100);
    BOOST_CHECK_EQUAL(GetDevTax(MAX_MONEY), 100);
}

BOOST_AUTO_TEST_CASE(coinbase_reward)
{
    const auto params{CreateChainParams(*m_node.args, ChainType::MAIN)};
    const Consensus::Params& consensus{params->GetConsensus()};
    const CScript dev_script(consensus.dev_tax_script.begin(), consensus.dev_tax_script.end());
    BOOST_REQUIRE(!dev_script.empty());
    CMutableTransaction coinbase;
    coinbase.vout.resize(1);
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;

    // The tax in an output after the miner's
    node::SetCoinbaseReward(coinbase, 4'250, consensus);
    BOOST_REQUIRE_EQUAL(coinbase.vout.size(), 2U);
    BOOST_CHECK_EQUAL(coinbase.vout[0].nValue, 4'208);
    BOOST_CHECK(coinbase.vout[1] == CTxOut(42, dev_script));
    // Set again, it is updated in place
    node::SetCoinbaseReward(coinbase, 2, consensus);
    BOOST_REQUIRE_EQUAL(coinbase.vout.size(), 2U);
    BOOST_CHECK_EQUAL(coinbase.vout[0].nValue, 1);
    BOOST_CHECK_EQUAL(coinbase.vout[1].nValue, 1);
    // A reward of 1 sat goes to the miner, without a tax output
    node::SetCoinbaseReward(coinbase, 1, consensus);
    BOOST_REQUIRE_EQUAL(coinbase.vout.size(), 1U);
    BOOST_CHECK_EQUAL(coinbase.vout[0].nValue, 1);
    // No reward, no tax output
    node::SetCoinbaseReward(coinbase, 0, consensus);
    BOOST_CHECK_EQUAL(coinbase.vout.size(), 1U);

    // Regtest has no dev tax by default
    const auto regtest{CreateChainParams(*m_node.args, ChainType::REGTEST)};
    node::SetCoinbaseReward(coinbase, 4'250, regtest->GetConsensus());
    BOOST_CHECK_EQUAL(coinbase.vout.size(), 1U);
    BOOST_CHECK_EQUAL(coinbase.vout[0].nValue, 4'250);
}

BOOST_AUTO_TEST_SUITE_END()
