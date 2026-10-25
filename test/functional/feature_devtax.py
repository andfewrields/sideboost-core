#!/usr/bin/env python3
# Copyright (c) 2026 The SideBoost developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the dev tax (consensus/devtax.h).

Regtest, with -devtax as on the other networks: every coinbase pays 1% of the
block's reward (subsidy plus fees) to the dev tax script, at least 1 and at
most 100 satoshis, but nothing on a reward of 1 satoshi, and blocks paying
less are invalid.
"""

from test_framework.blocktools import REGTEST_RETARGET_PERIOD, create_block, create_coinbase
from test_framework.messages import BLOCK_SPACING, COIN, CTxOut
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet

DEV_TAX_SCRIPT = "0014231d123a686a62846771fcd44c50ecc3f4b1a57e"


def dev_tax(reward):
    if reward <= 1:
        return 0
    return min(max(1, reward // 100), 100)


class DevTaxTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-devtax"]]

    def mine_to(self, height):
        """Mine up to `height`, one block interval of mock time apart, so
        timestamps stay within the max future block time."""
        node = self.nodes[0]
        while node.getblockcount() < height:
            n = min(100, height - node.getblockcount())
            self.mocktime = max(self.mocktime, node.getblockheader(node.getbestblockhash())["time"]) + n * BLOCK_SPACING
            node.setmocktime(self.mocktime)
            tip = self.generate(node, n)[-1]
        return tip

    def subsidy(self, height):
        return (50 * COIN) >> (height // REGTEST_RETARGET_PERIOD)

    def check_coinbase(self, blockhash, reward):
        """The block's coinbase pays the dev tax on `reward` right after the
        miner's output, and the miner the rest."""
        vout = self.nodes[0].getblock(blockhash, 2)["tx"][0]["vout"]
        tax = dev_tax(reward)
        assert_equal(round(vout[0]["value"] * COIN), reward - tax)
        if tax:
            assert_equal((vout[1]["scriptPubKey"]["hex"], round(vout[1]["value"] * COIN)), (DEV_TAX_SCRIPT, tax))
        else:
            assert all(out["scriptPubKey"]["hex"] != DEV_TAX_SCRIPT for out in vout)

    def submit(self, outputs):
        """Submit a block on the tip whose coinbase has `outputs`, as
        (amount, script) pairs."""
        node = self.nodes[0]
        height = node.getblockcount() + 1
        coinbase = create_coinbase(height)
        coinbase.vout = [CTxOut(amount, script) for amount, script in outputs]
        tip = node.getbestblockhash()
        block = create_block(int(tip, 16), coinbase, ntime=node.getblockheader(tip)["time"] + 1)
        block.solve()
        return node.submitblock(block.serialize().hex())

    def run_test(self):
        node = self.nodes[0]
        wallet = MiniWallet(node)
        dev = CScript(bytes.fromhex(DEV_TAX_SCRIPT))
        self.mocktime = node.getblockheader(node.getbestblockhash())["time"]

        self.log.info("Blocks pay the dev tax, at most 100 sats")
        tip = self.generate(wallet, 101)[-1]
        self.check_coinbase(tip, 50 * COIN)
        template = node.getblocktemplate({"rules": ["segwit"]})
        assert_equal(template["devtax"], {"script": DEV_TAX_SCRIPT, "amount": 100})
        assert_equal(template["coinbasevalue"], 50 * COIN - 100)

        self.log.info("Blocks that don't pay it are invalid")
        reward = 50 * COIN
        assert_equal(self.submit([(reward, CScript([OP_TRUE]))]), "bad-cb-devtax")
        assert_equal(self.submit([(reward - 99, CScript([OP_TRUE])), (99, dev)]), "bad-cb-devtax")
        # Paid in several outputs, anywhere in the coinbase
        assert_equal(self.submit([(50, dev), (reward - 100, CScript([OP_TRUE])), (50, dev)]), None)

        self.log.info("generateblock claims the fees of its transactions, and pays the tax on them")
        tx = wallet.create_self_transfer()
        fee = round(tx["fee"] * COIN)
        block = self.generateblock(node, wallet.get_address(), [tx["hex"]])["hash"]
        self.check_coinbase(block, self.subsidy(node.getblockcount()) + fee)

        self.log.info("Below 10,000 sats, it's 1%, and at least 1 sat")
        for halvings, tax in ((19, 95), (26, 1)):
            height = halvings * REGTEST_RETARGET_PERIOD
            tip = self.mine_to(height)
            assert_equal(dev_tax(self.subsidy(height)), tax)
            self.check_coinbase(tip, self.subsidy(height))

        self.log.info("On a reward of 1 sat there is none: it goes to the miner")
        height = 32 * REGTEST_RETARGET_PERIOD
        tip = self.mine_to(height)
        assert_equal(self.subsidy(height), 1)
        self.check_coinbase(tip, 1)
        assert_equal(self.submit([(1, CScript([OP_TRUE]))]), None)

        self.log.info("Without a reward there is none")
        height = 33 * REGTEST_RETARGET_PERIOD
        tip = self.mine_to(height)
        assert_equal(self.subsidy(height), 0)
        self.check_coinbase(tip, 0)
        assert "devtax" not in node.getblocktemplate({"rules": ["segwit"]})
        # Fees alone are taxed
        tx = wallet.send_self_transfer(from_node=node)
        tip = self.generate(node, 1)[0]
        self.check_coinbase(tip, round(tx["fee"] * COIN))


if __name__ == '__main__':
    DevTaxTest(__file__).main()
