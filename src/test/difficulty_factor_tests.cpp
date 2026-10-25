// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chainparams.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(difficulty_factor_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(encoding)
{
    BOOST_CHECK_EQUAL(GetDifficultyFactor(1), 1);
    BOOST_CHECK_EQUAL(GetDifficultyFactor(0x0002'0002), 2);
    BOOST_CHECK_EQUAL(GetDifficultyFactor(0x00FF'0002), 255);
    // Other bits don't count
    BOOST_CHECK_EQUAL(GetDifficultyFactor(0x0100'0002), 1);
    BOOST_CHECK_EQUAL(WithDifficultyFactor(2, 9), 0x0009'0002U);
    // BIP68 applies to all of them, as to version 2
    BOOST_CHECK(WithDifficultyFactor(1, 2) >= 2);

    CMutableTransaction mtx;
    mtx.version = WithDifficultyFactor(2, 7);
    BOOST_CHECK_EQUAL(CTransaction{mtx}.DifficultyFactor(), 7);

    // Standard: version 2 with a factor, besides versions 1 to 3
    BOOST_CHECK(IsStandardTxVersion(1));
    BOOST_CHECK(IsStandardTxVersion(3));
    BOOST_CHECK(!IsStandardTxVersion(4));
    BOOST_CHECK(IsStandardTxVersion(WithDifficultyFactor(2, 255)));
    BOOST_CHECK(!IsStandardTxVersion(WithDifficultyFactor(1, 2)));
    BOOST_CHECK(!IsStandardTxVersion(WithDifficultyFactor(3, 2)));
    BOOST_CHECK(!IsStandardTxVersion(WithDifficultyFactor(2, 2) | 0x0100'0000));
}

BOOST_AUTO_TEST_CASE(proof_of_work)
{
    const auto params{CreateChainParams(*m_node.args, ChainType::REGTEST)};
    const Consensus::Params& consensus{params->GetConsensus()};
    const unsigned int bits{0x207fffff};
    const arith_uint256 target{*DeriveTarget(bits, consensus.powLimit)};
    BOOST_CHECK(*DeriveFactorTarget(bits, consensus, 3) == target / 3);
    BOOST_CHECK(!DeriveFactorTarget(bits, consensus, 0));

    const uint256 at_third{ArithToUint256(target / 3)};
    const uint256 above_third{ArithToUint256(target / 3 + 1)};
    BOOST_CHECK(CheckProofOfWork(above_third, bits, consensus));
    BOOST_CHECK(CheckProofOfWork(at_third, bits, consensus, 3));
    BOOST_CHECK(!CheckProofOfWork(above_third, bits, consensus, 3));

    // A block's factor is its transactions' highest
    CBlock block;
    CMutableTransaction tx;
    block.vtx.push_back(MakeTransactionRef(tx));
    BOOST_CHECK_EQUAL(GetBlockDifficultyFactor(block), 1);
    tx.version = WithDifficultyFactor(2, 5);
    block.vtx.push_back(MakeTransactionRef(tx));
    tx.version = WithDifficultyFactor(2, 3);
    block.vtx.push_back(MakeTransactionRef(tx));
    BOOST_CHECK_EQUAL(GetBlockDifficultyFactor(block), 5);
}

BOOST_AUTO_TEST_SUITE_END()
