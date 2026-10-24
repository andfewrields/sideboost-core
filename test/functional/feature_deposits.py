#!/usr/bin/env python3
# Copyright (c) 2026 The SideBoost developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test deposits from the parent chain (BIP300 M5, see drivechain/deposit.h).

Runs a parent chain: an ECX regtest node and its bip300301_enforcer, given by
the environment variables ECX_BITCOIND and BIP300301_ENFORCER (the test is
skipped without them).  Activates slot 83 there, deposits into it, and checks
that SideBoost nodes credit the deposits, reject blocks that credit them
wrongly, and roll them back on a reorg of either chain.
"""

from decimal import Decimal

from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS
from test_framework.messages import COIN, SEQUENCE_FINAL, COutPoint, CTransaction, CTxIn, CTxOut, msg_block
from test_framework.p2p import P2PInterface
from test_framework.parentchain import ParentChainTest
from test_framework.test_framework import BitcoinTestFramework
from test_framework.script import CScript, OP_RETURN, OP_TRUE
from test_framework.util import assert_equal


class DepositsTest(ParentChainTest, BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2

    def deposits_in(self, node, blockhash):
        """The deposit transactions of a block (right after the coinbase)."""
        return [tx for tx in node.getblock(blockhash, 2)["tx"][1:] if tx["vin"][0].get("scriptSig", {}).get("hex", "").startswith("6a")]

    @staticmethod
    def deposit_tx(parent_txid, sequence, value, script):
        tx = CTransaction()
        tx.version = 2
        tx.vin = [CTxIn(COutPoint(int(parent_txid, 16), 0), CScript([OP_RETURN, sequence.to_bytes(8, "little")]), SEQUENCE_FINAL)]
        tx.vout = [CTxOut(value, script)]
        return tx

    def run_test(self):
        node0, node1 = self.nodes
        self.log.info("Activate slot 83 on the parent chain")
        self.activate_slot()

        self.log.info("A block credits a deposit once it has 3 confirmations")
        addr0 = node0.getnewaddress()
        txid0 = self.deposit(addr0, Decimal("1.5"))
        self.ecx_mine(3)
        assert_equal(self.deposits_in(node0, self.generate(node0, 1)[0]), [])
        self.ecx_mine()
        block0 = self.generate(node0, 1)[0]
        deposits = self.deposits_in(node0, block0)
        assert_equal(len(deposits), 1)
        assert_equal(deposits[0]["vin"][0]["txid"], txid0)
        assert_equal(deposits[0]["vout"][0]["value"], Decimal("1.5"))
        assert_equal(node0.getreceivedbyaddress(addr0), Decimal("1.5"))
        assert_equal(node1.getbestblockhash(), block0)  # node1 checked it too

        self.log.info("A block credits several deposits in order, a bad address burns")
        addr1 = node1.getnewaddress()
        txid1 = self.deposit(addr1, Decimal("0.25"))
        self.ecx_mine()
        txid2 = self.deposit("not-an-address", Decimal("0.5"))
        self.ecx_mine()
        txid3 = self.deposit(addr0, Decimal("0.1"))
        self.ecx_mine()
        self.confirm()
        block1 = self.generate(node0, 1)[0]
        deposits = self.deposits_in(node0, block1)
        assert_equal([d["vin"][0]["txid"] for d in deposits], [txid1, txid2, txid3])
        assert_equal(deposits[1]["vout"][0]["scriptPubKey"]["type"], "nulldata")
        assert_equal(node1.getreceivedbyaddress(addr1), Decimal("0.25"))
        assert_equal(node0.getreceivedbyaddress(addr0), Decimal("1.6"))

        self.log.info("Deposits never enter the mempool")
        assert_equal(node0.testmempoolaccept([deposits[0]["hex"]])[0]["reject-reason"], "deposit")

        self.log.info("Deposited coins can be spent right away")
        dep1 = deposits[0]
        raw = node1.createrawtransaction([{"txid": dep1["txid"], "vout": 0}], {node0.getnewaddress(): Decimal("0.2"), node1.getnewaddress(): Decimal("0.0499")})
        spend = node1.sendrawtransaction(node1.signrawtransactionwithwallet(raw)["hex"])
        self.generate(node1, 1)
        assert node1.gettransaction(spend)["confirmations"] == 1

        self.log.info("Blocks that credit deposits wrongly are rejected")
        addr4 = node0.getnewaddress()
        script4 = bytes.fromhex(node0.getaddressinfo(addr4)["scriptPubKey"])
        txid4 = self.deposit(addr4, Decimal("0.3"))
        self.ecx_mine()
        txid5 = self.deposit(addr4, Decimal("0.4"))
        self.ecx_mine()
        self.confirm()
        value4, value5 = int(Decimal("0.3") * COIN), int(Decimal("0.4") * COIN)
        good4 = self.deposit_tx(txid4, 4, value4, script4)
        good5 = self.deposit_tx(txid5, 5, value5, script4)
        _, res = self.submit(node0, [self.deposit_tx(txid4, 4, value4 + 1, script4)])
        assert_equal(res, "bad-deposit-mismatch")
        _, res = self.submit(node0, [self.deposit_tx(txid4, 4, value4, CScript([OP_TRUE]))])
        assert_equal(res, "bad-deposit-mismatch")
        _, res = self.submit(node0, [self.deposit_tx(txid3, 3, int(Decimal("0.1") * COIN), script4)])
        assert_equal(res, "bad-deposit-sequence")
        _, res = self.submit(node0, [good5])
        assert_equal(res, "bad-deposit-skipped")
        ordinary = CTransaction()
        ordinary.vin = [CTxIn(COutPoint(int(block0, 16), 0))]
        ordinary.vout = [CTxOut(1, CScript([OP_TRUE]))]
        _, res = self.submit(node0, [ordinary, good4])
        assert_equal(res, "bad-txns-deposit-position")

        self.log.info("A deposit our parent chain view doesn't know makes a block invalid, for as long as it doesn't")
        unknown = self.deposit_tx("11" * 32, 6, value4, script4)
        tip = node0.getbestblockhash()
        _, res = self.submit(node0, [good4, good5, unknown])
        assert_equal(res, "bad-deposit-unconfirmed")
        assert_equal(node0.getbestblockhash(), tip)

        self.log.info("Peers aren't punished for blocks invalid only in our view of the parent chain")
        peer = node0.add_p2p_connection(P2PInterface())
        unknown_block = self.make_block(node0, [good4, good5, self.deposit_tx("22" * 32, 6, value4, script4)])
        peer.send_and_ping(msg_block(unknown_block))
        assert_equal(self.status(node0, unknown_block.hash_hex), "invalid")
        # Nor for blocks on top of such blocks
        on_top = self.make_block(node0, [], prev=unknown_block.hash_hex)
        peer.send_and_ping(msg_block(on_top))
        assert_equal(node0.getbestblockhash(), tip)
        # Unlike for blocks invalid whatever the parent chain
        peer.send_without_ping(msg_block(self.make_block(node0, [ordinary, good4])))
        peer.wait_for_disconnect()

        self.log.info("Correct blocks are accepted, by both nodes")
        _, res = self.submit(node0, [good4, good5])
        assert_equal(res, None)
        self.sync_blocks()

        self.log.info("A reorg rolls deposits back, and they're credited again")
        node0.invalidateblock(block1)
        assert_equal(node0.getreceivedbyaddress(addr0), Decimal("1.5"))
        reorg = self.generate(node0, 1, sync_fun=self.no_op)[0]
        assert_equal([d["vin"][0]["txid"] for d in self.deposits_in(node0, reorg)], [txid1, txid2, txid3, txid4, txid5])
        # node1 reorgs to node0's longer chain
        self.generate(node0, 3, sync_fun=self.no_op)
        self.sync_blocks()
        assert_equal(node1.getreceivedbyaddress(addr1), Decimal("0.25"))
        assert_equal(node0.getreceivedbyaddress(addr0), Decimal("1.6"))

        self.log.info("So does one with too few confirmations; the node mines on, and takes the block once it has them")
        addr9 = node0.getnewaddress()
        txid9 = self.deposit(addr9, Decimal("0.09"))
        self.ecx_mine()
        tip = node0.getbestblockhash()
        early, res = self.submit(node0, [self.deposit_tx(txid9, 6, int(Decimal("0.09") * COIN), bytes.fromhex(node0.getaddressinfo(addr9)["scriptPubKey"]))])
        assert_equal(res, "bad-deposit-unconfirmed")
        assert_equal(self.status(node0, early.hash_hex), "invalid")
        assert_equal(node0.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)["previousblockhash"], tip)
        self.confirm()
        self.sync_parent_chain()
        self.wait_until(lambda: node0.getbestblockhash() == early.hash_hex)
        self.sync_blocks()
        assert_equal(node1.getbestblockhash(), early.hash_hex)

        self.log.info("The deposit state survives a restart")
        self.restart_node(0, extra_args=self.extra_args[0])
        self.connect_nodes(0, 1)
        txid6 = self.deposit(addr0, Decimal("0.01"))
        self.ecx_mine()
        self.confirm()
        block6 = self.generate(node0, 1)[0]
        assert_equal([d["vin"][0]["txid"] for d in self.deposits_in(node0, block6)], [txid6])

        self.log.info("A parent chain reorg that mines a deposit again changes nothing")
        addr7 = node0.getnewaddress()
        txid7 = self.deposit(addr7, Decimal("0.07"))
        parent7 = self.ecx_mine()
        self.confirm()
        block7 = self.generate(node0, 1)[0]
        assert_equal([d["vin"][0]["txid"] for d in self.deposits_in(node0, block7)], [txid7])
        self.ecx_reorg(parent7, [self.ecx.getrawtransaction(txid7)])
        self.sync_parent_chain()
        for node in self.nodes:
            assert_equal(node.getbestblockhash(), block7)
        assert_equal(node0.getreceivedbyaddress(addr7), Decimal("0.07"))

        self.log.info("A parent chain reorg that drops a deposit invalidates the block that credited it")
        addr8 = node0.getnewaddress()
        txid8 = self.deposit(addr8, Decimal("0.08"))
        parent8 = self.ecx_mine()
        self.confirm()
        block8 = self.generate(node0, 1)[0]
        assert_equal([d["vin"][0]["txid"] for d in self.deposits_in(node0, block8)], [txid8])
        after8 = self.generate(node0, 1)[0]
        new_branch = self.ecx_reorg(parent8, [])
        self.sync_parent_chain()
        for node in self.nodes:
            self.wait_until(lambda: node.getbestblockhash() == block7)
            assert_equal(self.status(node, after8), "invalid")
        assert_equal(node0.getreceivedbyaddress(addr8), 0)
        # New blocks don't credit it
        replacement = self.generate(node0, 1)[0]
        assert_equal(self.deposits_in(node0, replacement), [])
        # The reason the block is invalid survives a restart
        self.restart_node(1, extra_args=self.extra_args[1])
        self.connect_nodes(0, 1)
        assert_equal(self.status(node1, after8), "invalid")

        self.log.info("When the parent chain has the deposit again, the block is valid again")
        self.ecx.invalidateblock(new_branch)
        self.ecx.reconsiderblock(parent8)
        self.wait_for_enforcer()
        self.sync_parent_chain()
        for node in self.nodes:
            # block8's branch has more work than the replacement: back to it
            self.wait_until(lambda: node.getbestblockhash() == after8)
        assert_equal(node0.getreceivedbyaddress(addr8), Decimal("0.08"))

        self.log.info("Both nodes end with the same UTXO set, which node1's coinstatsindex tracked")
        assert_equal(node0.gettxoutsetinfo()["hash_serialized_3"], node1.gettxoutsetinfo()["hash_serialized_3"])
        self.wait_until(lambda: node1.getindexinfo()["coinstatsindex"]["synced"])
        utxos = node1.gettxoutsetinfo("muhash", use_index=False)
        indexed = node1.gettxoutsetinfo("muhash", use_index=True)
        for key in ("muhash", "txouts", "bogosize", "total_amount"):
            assert_equal(indexed[key], utxos[key])
        assert_equal(node1.gettxoutsetinfo("muhash", node1.getblockheader(block6)["height"])["block_info"]["deposits"], Decimal("0.01"))


if __name__ == '__main__':
    DepositsTest(__file__).main()
