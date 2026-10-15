#!/usr/bin/env python3
# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test fastprune mode."""
from test_framework.messages import MAX_BLOCK_WEIGHT
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)
from test_framework.wallet import MiniWallet


class FeatureFastpruneTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-fastprune"]]

    def run_test(self):
        self.log.info("ensure that large blocks don't crash or freeze in -fastprune")
        wallet = MiniWallet(self.nodes[0])
        tx = wallet.create_self_transfer()['tx']
        # SideBoost: a block can't exceed the 64 KiB fastprune block file: the
        # 66,668 WU limit minus the non-witness part (~500 bytes, mostly the
        # AuxPoW proof, at 4 WU/byte) caps it at ~65,100 bytes.  Use the largest
        # block that fits instead.
        annex = b"\x50" + b"\xff" * (0x10000 - 1000)
        tx.wit.vtxinwit[0].scriptWitness.stack.append(annex)
        block_hash = self.generateblock(self.nodes[0], output="raw(55)", transactions=[tx.serialize().hex()])["hash"]
        assert_equal(self.nodes[0].getblockcount(), 201)
        assert_greater_than(self.nodes[0].getblock(block_hash)["weight"], MAX_BLOCK_WEIGHT - 100)


if __name__ == '__main__':
    FeatureFastpruneTest(__file__).main()
