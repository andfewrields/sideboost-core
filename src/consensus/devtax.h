// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_DEVTAX_H
#define BITCOIN_CONSENSUS_DEVTAX_H

#include <consensus/amount.h>

#include <algorithm>

/**
 * The dev tax: the part of a block's reward (subsidy plus fees, what its
 * coinbase may claim) that the coinbase must pay to the chain's dev tax
 * script (Consensus::Params::dev_tax_script), in one or more outputs; the
 * miner may claim the rest.
 *
 * 1% of the reward, rounded down, but at least DEV_TAX_FLOOR and at most
 * DEV_TAX_CEILING satoshis; nothing if the reward is DEV_TAX_EXEMPT_REWARD
 * satoshis or less (a block whose transactions pay a single sat of fees
 * leaves it to the miner).
 */
static constexpr CAmount DEV_TAX_EXEMPT_REWARD{1};
static constexpr CAmount DEV_TAX_FLOOR{1};
static constexpr CAmount DEV_TAX_CEILING{100};
/** The tax's share of the reward: 1 / DEV_TAX_DIVISOR.  */
static constexpr CAmount DEV_TAX_DIVISOR{100};

constexpr CAmount GetDevTax(CAmount block_reward)
{
    if (block_reward <= DEV_TAX_EXEMPT_REWARD) return 0;
    return std::min(std::max(DEV_TAX_FLOOR, block_reward / DEV_TAX_DIVISOR), DEV_TAX_CEILING);
}

static_assert(GetDevTax(0) == 0);
static_assert(GetDevTax(1) == 0);
static_assert(GetDevTax(2) == 1);
static_assert(GetDevTax(199) == 1);
static_assert(GetDevTax(4'250) == 42);
static_assert(GetDevTax(10'000) == 100);
static_assert(GetDevTax(50 * COIN) == 100);

#endif // BITCOIN_CONSENSUS_DEVTAX_H
