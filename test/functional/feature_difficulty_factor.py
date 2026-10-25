#!/usr/bin/env python3
# Copyright (c) 2026 The SideBoost developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test transactions with a difficulty factor (primitives/transaction.h).

Bits 16 to 23 of a transaction's version are its difficulty factor k (0
meaning 1): a block including it must have a proof of work hash at most its
target divided by k.  The block producer picks the factor earning the most
fees per work, and the targets it gives miners are divided by it.
"""

from decimal import Decimal

from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import COIN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


def with_factor(factor):
    """Version 2 with `factor` (none for 1, the standard form)."""
    return 2 | (factor << 16) if factor > 1 else 2


class DifficultyFactorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.uses_wallet = None  # the wallet part only runs with a wallet

    def bits_target(self, node):
        return int(node.getblocktemplate({"rules": ["segwit"]})["bits"], 16)

    def target(self, node):
        bits = self.bits_target(node)
        return (bits & 0xffffff) << (8 * ((bits >> 24) - 3))

    def send(self, wallet, factor, fee):
        tx = wallet.create_self_transfer(fee=Decimal(fee) / COIN)["tx"]
        tx.version = with_factor(factor)
        return wallet.sendrawtransaction(from_node=self.nodes[0], tx_hex=tx.serialize().hex())

    def run_test(self):
        node0, node1 = self.nodes
        wallet = MiniWallet(node0)
        self.generate(wallet, 110)

        self.log.info("Transactions with a factor are relayed")
        txid = self.send(wallet, 4, 20_000)
        assert_equal(node0.getrawtransaction(txid, True)["version"], with_factor(4))
        self.sync_mempools()

        self.log.info("Mining targets are divided by the template's factor")
        template = node0.getblocktemplate({"rules": ["segwit"]})
        assert_equal(int(template["target"], 16), self.target(node0) // 4)
        aux = node0.createauxblock(wallet.get_address())
        assert_equal(int.from_bytes(bytes.fromhex(aux["_target"]), "little"), self.target(node0) // 4)

        self.log.info("With nothing better, the producer takes it, mining 4x harder")
        block = self.generate(node0, 1)[0]
        assert txid in node1.getblock(block)["tx"]

        self.log.info("It takes the factor earning the most fees per work")
        plain = self.send(wallet, 1, 10_000)
        hard = self.send(wallet, 4, 20_000)
        self.sync_mempools()
        assert_equal(int(node0.getblocktemplate({"rules": ["segwit"]})["target"], 16), self.target(node0))
        block = self.generate(node0, 1)[0]
        assert plain in node1.getblock(block)["tx"]
        assert_equal(node0.getrawmempool(), [hard])
        # Worth it now: (10,000 + 20,000 + 50,000) / 4 > 10,000
        plain = self.send(wallet, 1, 10_000)
        richer = self.send(wallet, 4, 50_000)
        block = self.generate(node0, 1)[0]
        assert all(t in node1.getblock(block)["tx"] for t in (plain, hard, richer))

        self.log.info("A block too weak for its factor is invalid")
        tx = wallet.create_self_transfer()["tx"]
        tx.version = with_factor(255)
        tip = node0.getbestblockhash()
        height = node0.getblockcount() + 1
        target = self.target(node0)
        block = create_block(int(tip, 16), create_coinbase(height), txlist=[tx])
        add_witness_commitment(block)
        while not (target // 255 < block.hash_int <= target):
            block.nNonce += 1
        assert_equal(node0.submitblock(block.serialize().hex()), "high-hash-factor")
        while block.hash_int > target // 255:
            block.nNonce += 1
        assert_equal(node0.submitblock(block.serialize().hex()), None)
        self.sync_blocks()

        if self.is_wallet_compiled():
            self.log.info("Wallet BMM requests can have a factor")
            node0.createwallet("w")
            w = node0.get_wallet_rpc("w")
            self.generatetoaddress(node0, 101, w.getnewaddress())
            request = w.createbmmrequest(9, "11" * 32, Decimal("0.001"), difficulty_factor=3)
            assert_equal(w.getrawtransaction(request["txid"], True)["version"], with_factor(3))
            block = self.generate(node0, 1)[0]
            assert request["txid"] in node1.getblock(block)["tx"]


if __name__ == '__main__':
    DifficultyFactorTest(__file__).main()
