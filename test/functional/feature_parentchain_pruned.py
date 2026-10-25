#!/usr/bin/env python3
# Copyright (c) 2026 The SideBoost developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a pruned node that a parent chain reorg needs deleted blocks of.

Runs a parent chain as feature_deposits.py does.  A pruned node credits a
deposit, prunes the block that did, and then the parent chain drops the
deposit: following it would mean reading that block, so the node stops and
asks for a reindex, as a pruned Bitcoin node does on a reorg deeper than what
it kept.
"""

import re
from decimal import Decimal

from test_framework.parentchain import ParentChainTest
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

MIN_BLOCKS_TO_KEEP = 288  # regtest's


class ParentChainPrunedTest(ParentChainTest, BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.more_args = [["-prune=1", "-fastprune"]]

    def run_test(self):
        node = self.nodes[0]
        self.activate_slot()
        address = node.getnewaddress()
        self.deposit(address, Decimal(1))
        parent = self.ecx_mine()
        self.confirm()
        credited = self.generate(node, 1)[0]
        assert_equal(node.getreceivedbyaddress(address), 1)

        self.log.info("Prune the block that credited the deposit")
        self.generate(node, 1000)  # fills several of -fastprune's small block files
        height = node.getblockheader(credited)["height"]
        assert node.pruneblockchain(node.getblockcount() - MIN_BLOCKS_TO_KEEP) > height
        assert "pruneheight" in node.getblockchaininfo()

        self.log.info("A parent chain reorg dropping the deposit stops the node")
        self.ecx_reorg(parent, [])
        # The node follows the parent chain every 5 s, and stops by itself
        node.wait_until_stopped(expect_error=True, expected_stderr=re.compile(
            rf"Error: A fatal internal error occurred, see debug.log for details: Following the parent chain needs block {height}, "
            r"which this node can't read\. If it prunes, the block was deleted: restart with -reindex to download the chain again\."))


if __name__ == '__main__':
    ParentChainPrunedTest(__file__).main()
