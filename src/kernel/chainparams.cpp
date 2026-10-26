// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/chainparams.h>

#include <chain.h>
#include <chainparamsseeds.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <crypto/hex_base.h>
#include <hash.h>
#include <kernel/messagestartchars.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/verify_flags.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/log.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <span>
#include <utility>

using namespace util::hex_literals;

static CBlock CreateGenesisBlock(const CScript& genesisInputScript, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.version = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = genesisInputScript;
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Build the SideBoost genesis block.  Its output is provably unspendable
 * (OP_RETURN); SideBoost has no block subsidy, all coins enter through
 * deposits.  Mined with a local script (tmp/mine-genesis.py, not committed)
 * that must mirror this function.
 */
static CBlock CreateSideBoostGenesisBlock(const char* pszTimestamp, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion)
{
    const CScript genesisInputScript = CScript() << nBits << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    const CScript genesisOutputScript = CScript() << OP_RETURN;
    return CreateGenesisBlock(genesisInputScript, genesisOutputScript, nTime, nNonce, nBits, nVersion, 0);
}

/**
 * Build genesis block for testnet.  In Namecoin, it has a changed timestamp
 * and output script (it uses Bitcoin's).
 */
static CBlock CreateTestnetGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "The Times 03/Jan/2009 Chancellor on brink of second bailout for banks";
    const CScript genesisInputScript = CScript() << 0x1d00ffff << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    const CScript genesisOutputScript = CScript() << "04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f"_hex_v_u8 << OP_CHECKSIG;
    return CreateGenesisBlock(genesisInputScript, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

void CChainParams::ApplyDeploymentOptions(const DeploymentOptions& opts)
{
    for (const auto& [dep, height] : opts.activation_heights) {
        switch (dep) {
        case Consensus::BuriedDeployment::DEPLOYMENT_P2SH:
            consensus.BIP16Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_SEGWIT:
            consensus.SegwitHeight = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_HEIGHTINCB:
            consensus.BIP34Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_DERSIG:
            consensus.BIP66Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_CLTV:
            consensus.BIP65Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_CSV:
            consensus.CSVHeight = int{height};
            break;
        }
    }

    for (const auto& [deployment_pos, version_bits_params] : opts.version_bits_parameters) {
        consensus.vDeployments[deployment_pos].nStartTime = version_bits_params.start_time;
        consensus.vDeployments[deployment_pos].nTimeout = version_bits_params.timeout;
        consensus.vDeployments[deployment_pos].min_activation_height = version_bits_params.min_activation_height;
    }
}

/**
 * SideBoost's target block interval, in seconds.  The consensus values that
 * depend on it are derived from it below, so changing it rescales them all;
 * a different interval is a different chain, though (genesis, test
 * networks, and the functional tests' BLOCK_SPACING).  See PLAN.md.
 */
static constexpr int64_t SIDEBOOST_BLOCK_SPACING{10};
/** Bitcoin's block interval, the base for the scaled values below. */
static constexpr int64_t BITCOIN_BLOCK_SPACING{10 * 60};
/** Blocks per difficulty window.  A number of blocks (the DAA's sample
    size), not a time: the window lasts this many block intervals.  */
static constexpr int64_t SIDEBOOST_DIFFICULTY_WINDOW{500};

/** A Bitcoin per-block limit scaled to a block interval, in proportion,
    rounded to the nearest multiple of WITNESS_SCALE_FACTOR, and never above
    Bitcoin's (the global constants size buffers and DoS limits).  */
static constexpr int64_t ScaleBlockLimit(int64_t bitcoin_limit, int64_t spacing)
{
    const int64_t unit{BITCOIN_BLOCK_SPACING * WITNESS_SCALE_FACTOR};
    const int64_t scaled{(2 * bitcoin_limit * spacing + unit) / (2 * unit) * WITNESS_SCALE_FACTOR};
    return std::min(bitcoin_limit, scaled);
}
/** Coinbase maturity: a quarter of Bitcoin's time (100 blocks).  */
static constexpr int CoinbaseMaturity(int64_t spacing)
{
    return static_cast<int>(COINBASE_MATURITY * BITCOIN_BLOCK_SPACING / spacing / 4);
}
/** Max future block time: 30 block intervals, so a window's apparent
    duration can't be materially stretched (6% of a 500-block window), and
    at most Bitcoin's 2 hours.  */
static constexpr int64_t MaxFutureBlockTime(int64_t spacing)
{
    return std::min(MAX_FUTURE_BLOCK_TIME, 30 * spacing);
}
/** Blocks a pruned node keeps at least: MIN_TIME_TO_KEEP's worth.  */
static constexpr int BlocksToKeep(int64_t spacing)
{
    return static_cast<int>(MIN_TIME_TO_KEEP / spacing);
}
static_assert(ScaleBlockLimit(MAX_BLOCK_WEIGHT, 10) == 66'668);
static_assert(ScaleBlockLimit(MAX_BLOCK_SIGOPS_COST, 10) == 1'332);
static_assert(ScaleBlockLimit(MAX_BLOCK_WEIGHT, BITCOIN_BLOCK_SPACING) == int64_t{MAX_BLOCK_WEIGHT});
static_assert(CoinbaseMaturity(10) == 1'500);
static_assert(MaxFutureBlockTime(10) == 5 * 60);
static_assert(BlocksToKeep(10) == 17'280);
static_assert(BlocksToKeep(BITCOIN_BLOCK_SPACING) == 288);
static_assert(BITCOIN_BLOCK_SPACING % SIDEBOOST_BLOCK_SPACING == 0, "BlockTimeScale() is a whole number of blocks");
static_assert(MaxFutureBlockTime(BITCOIN_BLOCK_SPACING) == MAX_FUTURE_BLOCK_TIME);
static_assert(MaxFutureBlockTime(SIDEBOOST_BLOCK_SPACING) < SIDEBOOST_DIFFICULTY_WINDOW * SIDEBOOST_BLOCK_SPACING);

/**
 * BIP300's voting periods for our own sidechains, for blocks `spacing`
 * seconds apart: the spec's (for Bitcoin's 10 minutes), in proportion to the
 * spacing, then 3× shorter for withdrawals (and the replacement of a used
 * slot, which the spec ties to them) and 2× shorter for activating an unused
 * slot.  At 10 s: 526,000 / 263,000 blocks (~61 / ~30 days), and 60,480 /
 * 54,450 (~7 / ~6.3 days).
 */
static constexpr Consensus::SidechainParams SidechainPeriods(int64_t spacing)
{
    const uint32_t blocks{static_cast<uint32_t>(BITCOIN_BLOCK_SPACING / spacing)};
    const uint32_t withdrawal_max_age{26'300 * blocks / 3};
    const uint32_t withdrawal_threshold{13'150 * blocks / 3};
    return {
        .withdrawal_bundle_max_age = withdrawal_max_age,
        .withdrawal_bundle_inclusion_threshold = withdrawal_threshold,
        .used_slot_proposal_max_age = withdrawal_max_age,
        .used_slot_activation_threshold = withdrawal_threshold,
        .unused_slot_proposal_max_age = 2'016 * blocks / 2,
        .unused_slot_activation_threshold = 1'815 * blocks / 2,
    };
}
static_assert(SidechainPeriods(10).withdrawal_bundle_max_age == 526'000);
static_assert(SidechainPeriods(10).withdrawal_bundle_inclusion_threshold == 263'000);
static_assert(SidechainPeriods(10).unused_slot_proposal_max_age == 60'480);
static_assert(SidechainPeriods(10).unused_slot_activation_threshold == 54'450);

/**
 * The script the dev tax is paid to, on every network that has one.
 *
 * TODO: a placeholder; replace it with the dev's before launch.  It pays to
 * a witness key hash (the first 20 bytes of SHA256("SideBoost dev tax
 * placeholder: nobody has its key")) whose key nobody knows, so until then
 * the tax is burned.  Mainnet address sb1qyvw3ywngdf3ggem3ln2yc58vc06trft7xhyjs2,
 * test networks tsb1qyvw3ywngdf3ggem3ln2yc58vc06trft7u3kmvp.
 */
static const std::vector<unsigned char> SIDEBOOST_DEV_TAX_SCRIPT{"0014231d123a686a62846771fcd44c50ecc3f4b1a57e"_hex_v_u8};

/**
 * Consensus rules shared by all SideBoost networks.  Mainnet uses them as is;
 * the test networks start from them and override only what testing needs, so
 * that they exercise the same rules as mainnet.  See PLAN.md for the rationale.
 */
static void SetSideBoostConsensus(Consensus::Params& consensus)
{
    consensus.signet_blocks = false;
    consensus.signet_challenge.clear();
    consensus.nSubsidyHalvingInterval = 210000;
    consensus.nInitialSubsidy = 0;
    /* 1,500 blocks at 10 s: ~4.2 hours, a quarter of Bitcoin's ~16.7.  */
    consensus.nCoinbaseMaturity = CoinbaseMaturity(SIDEBOOST_BLOCK_SPACING);
    /* Bitcoin's limits scaled to the block interval (1/60 at 10 s:
       66,668 WU and 1,332).  */
    consensus.nMaxBlockWeight = static_cast<unsigned int>(ScaleBlockLimit(MAX_BLOCK_WEIGHT, SIDEBOOST_BLOCK_SPACING));
    consensus.nMaxBlockSigOpsCost = ScaleBlockLimit(MAX_BLOCK_SIGOPS_COST, SIDEBOOST_BLOCK_SPACING);
    /* Deposits from ECX sidechain slot 83 (0x53, "S"), on each network
       from the ECX network of the same kind.  */
    consensus.deposit_slot = 83;
    /* A deposit is credited once 3 ECX blocks (~30 min) are on top of its
       block, which makes undoing it through an ECX reorg rare.  */
    consensus.nDepositMinDepth = 3;
    /* Every transaction pays at least 1 sat.  */
    consensus.min_transaction_fee = MIN_TRANSACTION_FEE;
    /* Every block pays a dev tax (consensus/devtax.h).  */
    consensus.dev_tax_script = SIDEBOOST_DEV_TAX_SCRIPT;
    /* Our own sidechains (bip300/).  */
    consensus.sidechains = SidechainPeriods(SIDEBOOST_BLOCK_SPACING);
    /* All buried deployments active from the start.  BIP34/65/66/CSV
       start at height 1 since the genesis block cannot satisfy them.  */
    consensus.BIP16Height = 0;
    consensus.BIP34Height = 1;
    consensus.BIP34Hash = uint256{};
    consensus.BIP65Height = 1;
    consensus.BIP66Height = 1;
    consensus.CSVHeight = 1;
    consensus.SegwitHeight = 0;
    consensus.MinBIP9WarningHeight = 0;
    consensus.powLimit = uint256{"000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    consensus.nPowTargetSpacing = SIDEBOOST_BLOCK_SPACING;
    consensus.nPowTargetTimespan = SIDEBOOST_DIFFICULTY_WINDOW * SIDEBOOST_BLOCK_SPACING; // ~83 minutes at 10 s
    /* Asymmetric retarget cap: difficulty rises at most 2x per window
       (limits how far a transient merge-miner can push it) but may fall
       up to 4x (fast recovery when hashrate leaves).  */
    consensus.nPowMaxDifficultyIncrease = 2;
    consensus.nPowMaxDifficultyDecrease = 4;
    consensus.nMaxFutureBlockTime = MaxFutureBlockTime(SIDEBOOST_BLOCK_SPACING); // 5 min at 10 s
    consensus.fPowAllowMinDifficultyBlocks = false;
    consensus.nMinDifficultySince = 0;
    consensus.enforce_BIP94 = false;
    consensus.fPowNoRetargeting = false;
    consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
    consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
    consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
    consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
    consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1815; // 90%
    consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

    // Deployment of Taproot (BIPs 340-342)
    consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
    consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
    consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
    consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay
    consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 1815; // 90%
    consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 2016;

    consensus.nMinimumChainWork = uint256{};
    consensus.defaultAssumeValid = uint256{};

    consensus.nAuxpowChainId = 0x5342; // "SB"
    consensus.nAuxpowStartHeight = 0;
    consensus.fStrictChainId = true;
    consensus.nLegacyBlocksBefore = 0;
}

/**
 * SideBoost main network.
 *
 * SideBoost is a BIP300 sidechain of ECX, merge-mined via AuxPoW: no block
 * subsidy (all coins enter through deposits), ~10 second blocks and a
 * proportionally smaller block size.  Parameters are otherwise kept as
 * neutral as possible.  See PLAN.md for the rationale.
 */
class CMainParams : public CChainParams {
public:
    CMainParams(const MainNetOptions& opts) {
        m_chain_type = ChainType::MAIN;
        SetSideBoostConsensus(consensus);
        ApplyDeploymentOptions(opts.dep_opts);

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0xfb;
        pchMessageStart[1] = 0xc0;
        pchMessageStart[2] = 0xb5;
        pchMessageStart[3] = 0xe3;
        nDefaultPort = 8350;
        nPruneAfterHeight = 100000;
        m_min_blocks_to_keep = BlocksToKeep(SIDEBOOST_BLOCK_SPACING);
        m_block_time_scale = BITCOIN_BLOCK_SPACING / SIDEBOOST_BLOCK_SPACING;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 1;

        genesis = CreateSideBoostGenesisBlock("SideBoost genesis 2026-10-26", 1792974307, 1434636, 0x1e00ffff, 1);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"0000009329242317161c8b0152052ee12e4086bfd2bdb6ae4c2473cc3186d533"});
        assert(genesis.hashMerkleRoot == uint256{"a5e87337eff5a33182d0b4d583a7a34909f4192625a9a3442cb597b92bdd3cf7"});

        // No seeds yet: connect with -addnode / -connect.
        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,63);  // 'S'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,125); // 's'
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,191); // PUBKEY_ADDRESS + 128
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "sb";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 0,
            .tx_count = 0,
            .dTxRate  = 0,
        };

        // Generated by headerssync-params.py on 2026-10-24, with
        // BLOCK_INTERVAL = 10 s, GENESIS_TIME = 2026-10-03 (an earlier
        // genesis's) and MINCHAINWORK_HEADERS = 3,153,600 (a year of
        // blocks): a placeholder, as low-work header sync only runs once
        // nMinimumChainWork is set. Regenerate when setting it.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 291,
            .redownload_buffer_size = 4244, // 4244/291 = ~14.6 commitments
        };
    }
};

/**
 * SideBoost testnet: public test network.
 *
 * Same rules as mainnet (no subsidy, 10 s blocks); only its identity differs.
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams(const TestNetOptions& opts) {
        m_chain_type = ChainType::TESTNET;
        SetSideBoostConsensus(consensus);
        ApplyDeploymentOptions(opts.dep_opts);

        pchMessageStart[0] = 0xfc;
        pchMessageStart[1] = 0xc1;
        pchMessageStart[2] = 0xb6;
        pchMessageStart[3] = 0xe4;
        nDefaultPort = 18350;
        nPruneAfterHeight = 1000;
        m_min_blocks_to_keep = BlocksToKeep(SIDEBOOST_BLOCK_SPACING);
        m_block_time_scale = BITCOIN_BLOCK_SPACING / SIDEBOOST_BLOCK_SPACING;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 1;

        genesis = CreateSideBoostGenesisBlock("SideBoost testnet genesis 2026-10-11", 1791761767, 12159491, 0x1e00ffff, 1);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"000000b71785f996c3d43f6a98c4c59b7ff128c5053b96bb9dbe5e570a9027a7"});
        assert(genesis.hashMerkleRoot == uint256{"b1f37b74073f02e09a35207ee72557b6d42a3829895a8fcd1df7fab95cae4c92"});

        // No seeds yet: connect with -addnode / -connect.
        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tsb";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 0,
            .tx_count = 0,
            .dTxRate  = 0,
        };

        // Same as mainnet
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 291,
            .redownload_buffer_size = 4244, // 4244/291 = ~14.6 commitments
        };
    }
};

/**
 * SideBoost testnet4: second public test network.
 *
 * Same rules as mainnet (no subsidy, 10 s blocks); only its identity differs.
 */
class CTestNet4Params : public CChainParams {
public:
    CTestNet4Params(const TestNetOptions& opts) {
        m_chain_type = ChainType::TESTNET4;
        SetSideBoostConsensus(consensus);
        ApplyDeploymentOptions(opts.dep_opts);

        pchMessageStart[0] = 0xfd;
        pchMessageStart[1] = 0xc2;
        pchMessageStart[2] = 0xb7;
        pchMessageStart[3] = 0xe5;
        nDefaultPort = 48350;
        nPruneAfterHeight = 1000;
        m_min_blocks_to_keep = BlocksToKeep(SIDEBOOST_BLOCK_SPACING);
        m_block_time_scale = BITCOIN_BLOCK_SPACING / SIDEBOOST_BLOCK_SPACING;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 1;

        genesis = CreateSideBoostGenesisBlock("SideBoost testnet4 genesis 2026-10-11", 1791761767, 23607739, 0x1e00ffff, 1);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"00000009f6ed86b40dc32169f0b826674abf8d784306aaf30896a4151387e5d0"});
        assert(genesis.hashMerkleRoot == uint256{"91d4a4663dcb3932cb6b65b4d1a0013f28af8abc689711f83f51df56c09121fe"});

        // No seeds yet: connect with -addnode / -connect.
        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tsb";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 0,
            .tx_count = 0,
            .dTxRate  = 0,
        };

        // Same as mainnet
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 291,
            .redownload_buffer_size = 4244, // 4244/291 = ~14.6 commitments
        };
    }
};

/**
 * Signet: test network with an additional consensus parameter (see BIP325).
 */
class SigNetParams : public CChainParams {
public:
    explicit SigNetParams(const SigNetOptions& options)
    {
        std::vector<uint8_t> bin;
        vFixedSeeds.clear();
        vSeeds.clear();

        if (!options.challenge) {
            /* SideBoost has no default signet: nodes require -signetchallenge
               (see AppInitParameterInteraction).  An unsatisfiable placeholder
               for code that builds signet params without one.  */
            bin = {OP_RETURN};
            //vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_signet), std::end(chainparams_seed_signet));
            //vSeeds.emplace_back("seed.signet.bitcoin.sprovoost.nl.");
            //vSeeds.emplace_back("seed.signet.achownodes.xyz."); // Ava Chow, only supports x1, x5, x9, x49, x809, x849, xd, x400, x404, x408, x448, xc08, xc48, x40c

            m_assumed_blockchain_size = 1;
            m_assumed_chain_state_size = 1;
            chainTxData = ChainTxData{
                .nTime    = 0,
                .tx_count = 0,
                .dTxRate  = 0,
            };
        } else {
            bin = *options.challenge;
            m_assumed_blockchain_size = 0;
            m_assumed_chain_state_size = 0;
            chainTxData = ChainTxData{
                0,
                0,
                0,
            };
            LogInfo("Signet with challenge %s", HexStr(bin));
        }

        if (options.seeds) {
            vSeeds = *options.seeds;
        }

        m_chain_type = ChainType::SIGNET;
        /* Same rules as mainnet (no subsidy, 10 s blocks); blocks are
           additionally signed by the challenge (BIP325).  */
        SetSideBoostConsensus(consensus);
        consensus.signet_blocks = true;
        consensus.signet_challenge.assign(bin.begin(), bin.end());
        ApplyDeploymentOptions(options.dep_opts);

        // message start is defined as the first 4 bytes of the sha256d of the block script
        HashWriter h{};
        h << consensus.signet_challenge;
        uint256 hash = h.GetHash();
        std::copy_n(hash.begin(), 4, pchMessageStart.begin());

        nDefaultPort = 38350;
        nPruneAfterHeight = 1000;
        m_min_blocks_to_keep = BlocksToKeep(SIDEBOOST_BLOCK_SPACING);
        m_block_time_scale = BITCOIN_BLOCK_SPACING / SIDEBOOST_BLOCK_SPACING;

        genesis = CreateSideBoostGenesisBlock("SideBoost signet genesis 2026-10-11", 1791761767, 8044771, 0x1e00ffff, 1);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"000000ca2649b892d67081832164e029bb18043477d34e07332bb724b5187405"});
        assert(genesis.hashMerkleRoot == uint256{"561c3c06ab810b118f70c2ce0cdb44c6f8fbf6507bb9dc2d667832bad548c29d"});

        m_assumeutxo_data = {};

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tsb";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        // Same as mainnet
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 291,
            .redownload_buffer_size = 4244, // 4244/291 = ~14.6 commitments
        };
    }
};

/**
 * Regression test: intended for private networks only. Has minimal difficulty to ensure that
 * blocks can be found instantly.
 */
class CRegTestParams : public CChainParams
{
public:
    explicit CRegTestParams(const RegTestOptions& opts)
    {
        m_chain_type = ChainType::REGTEST;
        /* Same rules as mainnet (block size, 10 s spacing, timestamps, ...),
           except for what testing needs, overridden below.  */
        SetSideBoostConsensus(consensus);

        /* Testing: mining pays coins (the only way to fund test wallets until
           deposits exist), with fast halvings and Bitcoin's 100-block
           maturity, as the test frameworks expect.  */
        consensus.nInitialSubsidy = 50 * COIN;
        consensus.nSubsidyHalvingInterval = 150;
        consensus.nCoinbaseMaturity = COINBASE_MATURITY;

        /* Testing: blocks are found instantly and difficulty never changes.  */
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = true;
        consensus.enforce_BIP94 = opts.enforce_bip94;
        /* Testing: deposits only when asked for (-depositslot), since they
           need a parent chain and its enforcer.  */
        consensus.deposit_slot = opts.deposit_slot;
        /* Testing: no dev tax unless asked for (-devtax), as Bitcoin's tests
           expect coinbases to claim the whole reward.  */
        consensus.dev_tax_script.clear();
        if (opts.dev_tax) consensus.dev_tax_script = SIDEBOOST_DEV_TAX_SCRIPT;
        /* Likewise no minimum fee unless asked for (-consensusminfee), as
           Bitcoin's tests have transactions without fees.  */
        consensus.min_transaction_fee = opts.min_transaction_fee ? MIN_TRANSACTION_FEE : 0;
        // Short enough to activate sidechains and pay out bundles in tests
        // (the enforcer's regtest values)
        consensus.sidechains = {
            .withdrawal_bundle_max_age = 10,
            .withdrawal_bundle_inclusion_threshold = 5,
            .used_slot_proposal_max_age = 10,
            .used_slot_activation_threshold = 5,
            .unused_slot_proposal_max_age = 10,
            .unused_slot_activation_threshold = 5,
        };

        /* Testing: short versionbits periods, so deployments can be
           exercised in a few hundred blocks.  */
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 108; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 144; // Faster than normal for regtest (144 instead of 2016)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 108; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 144;

        /* Testing: the identity below (genesis, chain ID, magic, address
           prefixes) stays Namecoin's, since the test frameworks hard-code it.
           Only the default ports differ, to avoid clashing with an ECX (or
           Bitcoin) regtest node on the same machine.  */
        consensus.nAuxpowChainId = 0x0001;

        pchMessageStart[0] = 0xfa;
        pchMessageStart[1] = 0xbf;
        pchMessageStart[2] = 0xb5;
        pchMessageStart[3] = 0xda;
        nDefaultPort = 18454;
        nPruneAfterHeight = opts.fastprune ? 100 : 1000;
        m_min_blocks_to_keep = BlocksToKeep(BITCOIN_BLOCK_SPACING); // as the tests expect
        m_block_time_scale = 1; // likewise
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        ApplyDeploymentOptions(opts.dep_opts);

        genesis = CreateTestnetGenesisBlock(1296688602, 2, 0x207fffff, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206"});
        assert(genesis.hashMerkleRoot == uint256{"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b"});

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();
        vSeeds.emplace_back("dummySeed.invalid.");

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        m_assumeutxo_data = {
            {   // For use by unit tests
                .height = 110,
                .hash_serialized = AssumeutxoHash{uint256{"86e9a1205b418b16dde3a18a78c730e30137e28466bda5dbf6b33ab8fc05447c"}},
                .m_chain_tx_count = 111,
                .blockhash = uint256{"34a7896b4bf4dd89c3568a457baf5d8d6f0674ab2e0a0fabb4134af265eb6310"},
            },
            {
                // For use by fuzz target src/test/fuzz/utxo_snapshot.cpp
                .height = 200,
                .hash_serialized = AssumeutxoHash{uint256{"17dcc016d188d16068907cdeb38b75691a118d43053b8cd6a25969419381d13a"}},
                .m_chain_tx_count = 201,
                .blockhash = uint256{"385901ccbd69dff6bbd00065d01fb8a9e464dede7cfe0372443884f9b1dcf6b9"},
            },
            {
                // For use by test/functional/feature_assumeutxo.py and test/functional/tool_bitcoin_chainstate.py
                .height = 299,
                .hash_serialized = AssumeutxoHash{uint256{"106b2c56233e378a824cf0d5ff2be42ed32c72f1605c9be288d00942908a40ac"}},
                .m_chain_tx_count = 334,
                .blockhash = uint256{"679788f89317d60dd751bf6d9829864f074a77be5bca9084237ac9be116c4e3f"},
            },
        };

        chainTxData = ChainTxData{
            .nTime = 0,
            .tx_count = 0,
            .dTxRate = 0.001, // Set a non-zero rate to make it testable
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "ncrt";

        // Copied from Testnet4.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 275,
            .redownload_buffer_size = 7017, // 7017/275 = ~25.5 commitments
        };
    }
};

std::unique_ptr<const CChainParams> CChainParams::SigNet(const SigNetOptions& options)
{
    return std::make_unique<const SigNetParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::RegTest(const RegTestOptions& options)
{
    return std::make_unique<const CRegTestParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::Main(const MainNetOptions& options)
{
    return std::make_unique<const CMainParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::TestNet(const TestNetOptions& options)
{
    return std::make_unique<const CTestNetParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::TestNet4(const TestNetOptions& options)
{
    return std::make_unique<const CTestNet4Params>(options);
}

std::vector<int> CChainParams::GetAvailableSnapshotHeights() const
{
    std::vector<int> heights;
    heights.reserve(m_assumeutxo_data.size());

    for (const auto& data : m_assumeutxo_data) {
        heights.emplace_back(data.height);
    }
    return heights;
}

std::optional<ChainType> GetNetworkForMagic(const MessageStartChars& message)
{
    const auto mainnet_msg = CChainParams::Main()->MessageStart();
    const auto testnet_msg = CChainParams::TestNet()->MessageStart();
    const auto testnet4_msg = CChainParams::TestNet4()->MessageStart();
    const auto regtest_msg = CChainParams::RegTest()->MessageStart();
    const auto signet_msg = CChainParams::SigNet()->MessageStart();

    if (std::ranges::equal(message, mainnet_msg)) {
        return ChainType::MAIN;
    } else if (std::ranges::equal(message, testnet_msg)) {
        return ChainType::TESTNET;
    } else if (std::ranges::equal(message, testnet4_msg)) {
        return ChainType::TESTNET4;
    } else if (std::ranges::equal(message, regtest_msg)) {
        return ChainType::REGTEST;
    } else if (std::ranges::equal(message, signet_msg)) {
        return ChainType::SIGNET;
    }
    return std::nullopt;
}
