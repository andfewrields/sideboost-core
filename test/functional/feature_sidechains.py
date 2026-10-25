#!/usr/bin/env python3
# Copyright (c) 2026 The SideBoost developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test BIP300 and BIP301 for SideBoost's own sidechains (src/bip300/).

Regtest's periods: a sidechain activates with more than 5 ACKs within 10
blocks of its proposal, a bundle pays out with more than 5 votes within 10
blocks.  Node 0 builds the blocks with its block producer: it proposes a
sidechain, ACKs it, takes deposits, proposes a bundle it was given, votes it
through and pays it out, and takes the best BMM request.  Node 1 follows,
and checks every block.
"""

from decimal import Decimal

from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import COIN, CTransaction, CTxOut
from test_framework.script import CScript, OP_RETURN, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet

SLOT = 9
DESCRIPTION = bytes([0, 7]) + b"Thunder" + b"A Thunder sidechain"
M5_TAG = bytes([0xD5, 0x3B, 0x05, 0x9A])
M7_TAG = bytes([0xD1, 0x61, 0x73, 0x68])
M8_TAG = bytes([0x00, 0xBF, 0x00])


def deposit_script(slot, address):
    return CScript([OP_RETURN, M5_TAG + bytes([slot]) + address])


def bmm_accept_script(slot, side_hash):
    return CScript([OP_RETURN, M7_TAG + bytes([slot]) + side_hash])


def bmm_request_script(slot, side_hash, prev_block):
    return CScript([OP_RETURN, M8_TAG + bytes([slot]) + side_hash + bytes.fromhex(prev_block)[::-1]])


def blinded_m6(fee, payouts):
    """A sidechain's bundle: no inputs, the fee, a commitment, the payouts."""
    tx = CTransaction()
    tx.version = 2
    tx.vout = [CTxOut(0, CScript([OP_RETURN, fee.to_bytes(8, "big")])), CTxOut(0, CScript([OP_RETURN, bytes(32)]))]
    tx.vout += [CTxOut(amount, script) for amount, script in payouts]
    return tx


class SidechainsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

    def events(self, blockhash):
        return self.nodes[1].getsidechainevents(blockhash)

    def treasury(self, node):
        return node.getsidechains()[0]["treasury"]

    def run_test(self):
        node0, node1 = self.nodes
        wallet = MiniWallet(node0)
        self.generate(wallet, 110)

        self.log.info("Deposits need an active slot")
        tx = wallet.create_self_transfer()["tx"]
        tx.vout.append(CTxOut(1000, deposit_script(SLOT, b"addr")))
        tx.vout[0].nValue -= 1000
        assert_raises_rpc_error(-26, "bad-txns-sidechain-deposit-inactive", node0.sendrawtransaction, tx.serialize().hex())

        self.log.info("A proposal (M1), ACKed (M2) by the producer's default policy, activates")
        description_hash = node0.proposesidechain(SLOT, DESCRIPTION.hex())
        proposal_block = self.generate(node0, 1)[0]
        events = self.events(proposal_block)
        assert_equal(events["proposals"], [{"slot": SLOT, "description_hash": description_hash, "description": DESCRIPTION.hex(), "vout": events["proposals"][0]["vout"]}])
        proposals = node1.getsidechainproposals()
        assert_equal([(p["slot"], p["description_hash"], p["vote_count"], p["description"]) for p in proposals], [(SLOT, description_hash, 0, DESCRIPTION.hex())])
        assert_equal(node0.getsidechainproducer()["ack_policy"], "new_slots")
        activation = self.generate(node0, 6)[-1]
        assert_equal(node1.getsidechainproposals(), [])
        sidechain = node1.getsidechains()[0]
        assert_equal((sidechain["slot"], sidechain["description"], sidechain["vote_count"], sidechain["treasury"]), (SLOT, DESCRIPTION.hex(), 6, 0))
        assert_equal(self.events(activation)["activations"], [SLOT])
        # Activated: no longer proposed, from the next block on
        assert_equal(len(node0.getsidechainproducer()["proposals"]), 1)
        self.generate(node0, 1)
        assert_equal(node0.getsidechainproducer()["proposals"], [])

        self.log.info("Deposits (M5) go to the treasury, any number per block")
        deposits = []
        for amount in (Decimal("1"), Decimal("2.5")):
            sent = wallet.send_to(from_node=node0, scriptPubKey=deposit_script(SLOT, b"thunder-address"), amount=int(amount * COIN))
            deposits.append((sent["txid"], amount))
        block = self.generate(node0, 1)[0]
        assert_equal(self.treasury(node1), Decimal("3.5"))
        events = self.events(block)["deposits"]
        assert_equal(sorted((d["txid"], d["amount"]) for d in events), sorted(deposits))
        assert_equal([(d["slot"], d["sequence"], d["vout"], d["address"]) for d in events], [(SLOT, i, 1, b"thunder-address".hex()) for i in range(2)])
        # Malformed ones are invalid
        tx = wallet.create_self_transfer()["tx"]
        tx.vout.append(CTxOut(0, deposit_script(SLOT, b"x")))
        assert_raises_rpc_error(-26, "bad-txns-sidechain-deposit", node0.sendrawtransaction, tx.serialize().hex())

        self.log.info("A bundle (M3) the producer knows, voted (M4) over the threshold, pays out (M6) in that block")
        payout_script = wallet.get_output_script()
        bundle = blinded_m6(10_000, [(COIN, payout_script), (COIN // 2, payout_script)])
        m6id = node0.submitbundle(SLOT, bundle.serialize_without_witness().hex())
        assert_equal(m6id, bundle.txid_hex)
        submitted = self.generate(node0, 1)[0]
        assert_equal(self.events(submitted)["submitted"], [{"slot": SLOT, "m6id": m6id}])
        pending = node1.getpendingbundles()
        assert_equal([(b["slot"], b["m6id"]) for b in pending], [(SLOT, m6id)])
        # The producer upvoted it in the same block
        assert_equal(pending[0]["vote_count"], 2)
        paid = None
        for _ in range(5):
            paid = self.generate(node0, 1)[0]
            if self.events(paid)["succeeded"]:
                break
        succeeded = self.events(paid)["succeeded"]
        assert_equal([(s["slot"], s["m6id"], s["sequence"]) for s in succeeded], [(SLOT, m6id, 2)])
        payout = node1.getblock(paid, 2)["tx"][-1]
        assert_equal(payout["txid"], succeeded[0]["txid"])
        assert_equal(payout["vin"][0]["vout"], SLOT)
        assert_equal(node1.getpendingbundles(), [])
        assert_equal(self.treasury(node1), Decimal("3.5") - Decimal("1.5") - Decimal("0.0001"))
        assert_equal(node1.gettxout(payout["txid"], 2)["value"], 1)
        # The miner got the fee
        coinbase = node1.getblock(paid, 2)["tx"][0]
        fees = sum(tx["fee"] for tx in node1.getblock(paid, 2)["tx"][1:-1])
        assert_equal(sum(out["value"] for out in coinbase["vout"]), Decimal("50") + fees + Decimal("0.0001"))

        self.log.info("Undone and redone with its block")
        node1.invalidateblock(paid)
        assert_equal(self.treasury(node1), Decimal("3.5"))
        assert_equal([b["m6id"] for b in node1.getpendingbundles()], [m6id])
        assert node1.gettxout(payout["txid"], 2) is None
        node1.reconsiderblock(paid)
        assert_equal(node1.getbestblockhash(), node0.getbestblockhash())
        assert_equal(node1.getpendingbundles(), [])

        self.log.info("A bundle is proposed only once")
        # The producer forgets it once paid
        assert_equal(len(node0.getsidechainproducer()["bundles"]), 1)
        self.generate(node0, 1)
        assert_equal(node0.getsidechainproducer()["bundles"], [])
        node0.submitbundle(SLOT, bundle.serialize_without_witness().hex())
        self.generate(node0, 1)
        assert_equal(node1.getpendingbundles(), [])
        assert_equal(node0.getsidechainproducer()["bundles"], [])

        self.log.info("BMM requests (M8): the best bid per slot, with its M7")
        side_hash = bytes(range(32))
        tip = node0.getbestblockhash()
        requests = []
        for fee in (Decimal("0.0002"), Decimal("0.0005")):
            tx = wallet.create_self_transfer(fee=fee)["tx"]
            tx.vout.insert(0, CTxOut(0, bmm_request_script(SLOT, side_hash, tip)))
            requests.append(wallet.sendrawtransaction(from_node=node0, tx_hex=tx.serialize().hex()))
        stale = wallet.create_self_transfer()["tx"]
        stale.vout.insert(0, CTxOut(0, bmm_request_script(SLOT, side_hash, proposal_block)))
        assert_raises_rpc_error(-26, "bmm-request-stale", node0.sendrawtransaction, stale.serialize().hex())
        template = node0.getblocktemplate({"rules": ["segwit"]})
        assert_equal(template["sidechainmessages"], [bmm_accept_script(SLOT, side_hash).hex()])
        assert_equal([t["txid"] for t in template["transactions"]], [requests[1]])
        block = self.generate(node0, 1)[0]
        assert_equal(self.events(block)["bmm"], [{"slot": SLOT, "hash": side_hash.hex()}])
        assert requests[1] in node1.getblock(block)["tx"]
        # The other one can't be mined any more
        assert_equal(node0.getrawmempool(), [])

        self.log.info("A BMM request without its M7 is invalid")
        tx = wallet.create_self_transfer()["tx"]
        tx.vout.insert(0, CTxOut(0, bmm_request_script(SLOT, side_hash, node0.getbestblockhash())))
        height = node0.getblockcount() + 1
        block = create_block(int(node0.getbestblockhash(), 16), create_coinbase(height), txlist=[tx])
        add_witness_commitment(block)
        block.solve()
        assert_equal(node0.submitblock(block.serialize().hex()), "bad-txns-bmm-request-unaccepted")
        coinbase = create_coinbase(height)
        coinbase.vout.append(CTxOut(0, bmm_accept_script(SLOT, side_hash)))
        block = create_block(int(node0.getbestblockhash(), 16), coinbase, txlist=[tx])
        add_witness_commitment(block)
        block.solve()
        assert_equal(node0.submitblock(block.serialize().hex()), None)
        self.sync_all()

        self.log.info("The producer's settings survive a restart")
        node0.setsidechainpolicy(bundle_policy="none")
        node0.setbundleack(SLOT, m6id)
        self.restart_node(0)
        producer = node0.getsidechainproducer()
        assert_equal((producer["bundle_policy"], producer["bundle_acks"]), ("none", [{"slot": SLOT, "m6id": m6id}]))
        assert_raises_rpc_error(-8, "Unknown bundle_policy", node0.setsidechainpolicy, bundle_policy="most")


if __name__ == '__main__':
    SidechainsTest(__file__).main()
