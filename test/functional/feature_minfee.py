#!/usr/bin/env python3
# Copyright (c) 2026 The SideBoost developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test SideBoost's fee floor: 1 sat per transaction, and no dust limit.

Consensus requires every transaction to pay at least 1 sat
(MIN_TRANSACTION_FEE), on regtest with -consensusminfee. With the node's own defaults (the test framework
otherwise pins upstream Bitcoin's), that is also all relay requires: outputs
of 1 sat are standard, a 1-sat fee is relayed and mined however large the
transaction, and a replacement pays at least 1 sat more than what it replaces.
The wallet pays at least 1 sat/kvB, so never less than 1 sat.
"""

from decimal import Decimal

from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import COIN, CTxOut
from test_framework.script_util import key_to_p2wpkh_script
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet
from test_framework.wallet_util import generate_keypair


class MinFeeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # Regtest has the consensus minimum fee only when asked for
        self.extra_args = [["-consensusminfee"]] * self.num_nodes
        self.uses_wallet = None  # the wallet part only runs with a wallet

    def setup_nodes(self):
        self.add_nodes(self.num_nodes, self.extra_args)
        for node in self.nodes:
            node.pin_upstream_fees = False
        self.start_nodes()

    def run_test(self):
        node0, node1 = self.nodes
        wallet = MiniWallet(node0)
        self.generate(wallet, 101)

        self.log.info("Relay requires no feerate, replacements 2 sat/kvB more: at least 1 sat")
        info = node0.getmempoolinfo()
        assert_equal((info["minrelaytxfee"], info["incrementalrelayfee"]), (0, Decimal("0.00000002")))

        self.log.info("A transaction without a fee is invalid, by consensus")
        tx = wallet.create_self_transfer(fee_rate=0)["tx"]
        assert_raises_rpc_error(-26, "bad-txns-fee-too-low", node0.sendrawtransaction, tx.serialize().hex())
        tip = node0.getbestblockhash()
        block = create_block(int(tip, 16), create_coinbase(node0.getblockcount() + 1), txlist=[tx])
        add_witness_commitment(block)
        block.solve()
        assert_equal(node0.submitblock(block.serialize().hex()), "bad-txns-fee-too-low")

        self.log.info("A 1-sat fee, with a 1-sat output, is relayed and mined")
        tx = wallet.create_self_transfer(fee=Decimal("0.00000001"))["tx"]
        tx.vout[0].nValue -= 1
        tx.vout.append(CTxOut(1, key_to_p2wpkh_script(generate_keypair()[1])))
        txid = wallet.sendrawtransaction(from_node=node0, tx_hex=tx.serialize().hex())
        assert_equal(node0.getmempoolentry(txid)["fees"]["base"], Decimal("0.00000001"))
        self.sync_mempools()
        block = self.generate(node1, 1)[0]
        assert txid in node0.getblock(block)["tx"]

        self.log.info("A 1-sat fee is enough however large the transaction")
        tx = wallet.create_self_transfer(fee=Decimal("0.00000001"), target_vsize=5000)
        assert tx["tx"].get_vsize() >= 5000
        wallet.sendrawtransaction(from_node=node0, tx_hex=tx["hex"])
        self.generate(node0, 1)

        self.log.info("A replacement pays strictly more, if only 1 sat")
        utxo = wallet.get_utxo()
        original = wallet.create_self_transfer(utxo_to_spend=utxo, fee=Decimal("0.00000001"))
        node0.sendrawtransaction(original["hex"])
        same = wallet.create_self_transfer(utxo_to_spend=utxo, fee=Decimal("0.00000001"), sequence=0xfffffffd)
        assert same["txid"] != original["txid"]
        assert_raises_rpc_error(-26, "insufficient fee", node0.sendrawtransaction, same["hex"])
        more = wallet.create_self_transfer(utxo_to_spend=utxo, fee=Decimal("0.00000002"))
        node0.sendrawtransaction(more["hex"])
        assert_equal(node0.getrawmempool(), [more["txid"]])
        self.generate(node0, 1)

        if not self.is_wallet_compiled():
            return
        self.log.info("The wallet sends 1 sat for a 1-sat fee, and won't send without a fee")
        node0.createwallet("w")
        w = node0.get_wallet_rpc("w")
        self.generatetoaddress(node0, 101, w.getnewaddress())
        address = node1.get_deterministic_priv_key().address
        assert_raises_rpc_error(-6, "lower than the minimum fee rate setting", w.sendtoaddress, address, Decimal("0.00000001"), fee_rate=0)
        txid = w.sendtoaddress(address, Decimal("0.00000001"), fee_rate=Decimal("0.001"))
        details = w.gettransaction(txid)
        assert_equal(details["fee"], Decimal("-0.00000001"))
        decoded = w.decoderawtransaction(details["hex"])
        assert any(round(out["value"] * COIN) == 1 for out in decoded["vout"])
        # Bumped with a feerate of its choice, it pays as little as it likes
        bumped = w.bumpfee(txid, {"fee_rate": Decimal("0.02")})
        assert 2 <= round(bumped["fee"] * COIN) <= 10, bumped
        self.sync_mempools()
        block = self.generate(node1, 1)[0]
        assert bumped["txid"] in node0.getblock(block)["tx"]


if __name__ == '__main__':
    MinFeeTest(__file__).main()
