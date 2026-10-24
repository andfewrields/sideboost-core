#!/usr/bin/env python3
# Copyright (c) 2026 The SideBoost developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test withdrawals to the parent chain (BIP300 M3 to M6, see drivechain/withdrawal.h).

Runs a parent chain as feature_deposits.py does.  Withdraws coins deposited
from it, and checks that SideBoost nodes bundle the withdrawals, propose the
bundle to the enforcer, record its success once the parent chain pays it out,
bundle its withdrawals again when it fails, reject blocks that record
outcomes wrongly, follow a parent chain reorg that drops a payout, return
to the block that created a bundle the parent chain paid out, and (with
-bundlevotes) have the enforcer vote only for the bundle pending on their
chain.
"""

from decimal import Decimal

from test_framework.messages import BLOCK_SPACING, SEQUENCE_FINAL, COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.parentchain import ParentChainTest
from test_framework.test_framework import BitcoinTestFramework
from test_framework.script import CScript, OP_1, OP_2, OP_RETURN
from test_framework.util import assert_equal, assert_raises_rpc_error

PARENT_FEE = Decimal("0.0001")  # withdraw's default
BUNDLE_PROPOSAL_DEPTH = 20 * 60 // BLOCK_SPACING  # 20 minutes


class WithdrawalsTest(ParentChainTest, BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2

    def set_bundle_policy(self, policy):
        """Have the parent chain's block producer vote for bundles it knows
        ("KNOWN"), or abstain ("NONE")."""
        self.enforcer("BlockProducerService/SetWithdrawalBundlePolicy", {"policy": f"WITHDRAWAL_BUNDLE_POLICY_{policy}"})

    def bundle_acks(self):
        """The bundles the parent chain's block producer is told to vote for."""
        state = self.enforcer("BlockProducerService/GetBlockProducerState")
        return [bytes.fromhex(ack["m6id"]["hex"])[::-1].hex() for ack in state.get("explicitBundleAcks", [])]

    @staticmethod
    def outcome_tx(m6id, succeeded):
        tx = CTransaction()
        tx.version = 2
        tx.vin = [CTxIn(COutPoint(int(m6id, 16), 0), CScript([OP_RETURN, OP_1 if succeeded else OP_2]), SEQUENCE_FINAL)]
        tx.vout = [CTxOut(0, CScript([OP_RETURN]))]
        return tx

    @staticmethod
    def outcomes_in(node, blockhash):
        """The bundle outcomes a block records: (M6ID, succeeded)."""
        return [(tx["vin"][0]["txid"], tx["vin"][0]["scriptSig"]["hex"] == "6a51") for tx in node.getblock(blockhash, 2)["tx"][1:]
                if tx["vin"][0].get("scriptSig", {}).get("hex") in ("6a51", "6a52")]

    def wait_for_proposal(self, m6id):
        """Have the nodes propose the pending bundle `m6id` to the parent
        chain: not before BUNDLE_PROPOSAL_DEPTH blocks are on top of the block
        that created it."""
        node = self.nodes[0]
        message = f"Proposed withdrawal bundle {m6id} to the parent chain".encode()
        bundle = node.getwithdrawals()["bundle"]
        assert_equal(bundle["m6id"], m6id)
        self.generate(node, max(0, bundle["height"] + BUNDLE_PROPOSAL_DEPTH - 1 - node.getblockcount()))
        self.sync_parent_chain()
        assert message not in node.debug_log_path.read_bytes()
        self.generate(node, 1)
        self.sync_parent_chain()
        self.wait_until(lambda: message in node.debug_log_path.read_bytes())

    def mine_until_paid(self, address, amount):
        """Mine parent blocks until `address` (of the parent wallet) received
        `amount`; return the payout's parent block."""
        for _ in range(30):
            self.ecx_mine()
            received = [t for t in self.ecx.listtransactions("*", 1000) if t.get("address") == address and t["category"] == "receive"]
            if received:
                assert_equal(received[0]["amount"], amount)
                return received[0]["blockhash"]
        raise AssertionError("the bundle was not paid out")

    def record_outcome(self, m6id, succeeded):
        """Confirm the outcome on the parent chain, have node0 record it."""
        self.confirm()
        self.sync_parent_chain()
        block = self.generate(self.nodes[0], 1)[0]
        assert_equal(self.outcomes_in(self.nodes[0], block), [(m6id, succeeded)])
        return block

    def run_test(self):
        node0, node1 = self.nodes
        self.log.info("Activate slot 83 on the parent chain, fill its treasury")
        self.activate_slot()
        self.set_bundle_policy("KNOWN")
        self.deposit(node0.getnewaddress(), Decimal(10))
        self.ecx_mine()
        self.confirm()
        self.generate(node0, 1)
        assert_equal(node0.getwithdrawals() | {"checkpoint": None}, {"waiting": 0, "withdrawals": [], "checkpoint": None})

        self.log.info("A withdrawal is bundled by the block that requests it")
        ecx_addr1 = self.ecx.getnewaddress()
        txid1 = node0.withdraw(ecx_addr1, 1)
        request = next(out for out in node0.getrawtransaction(txid1, True)["vout"] if out["scriptPubKey"]["type"] == "nulldata")
        assert_equal(request["value"], 1 + PARENT_FEE)
        assert_equal(node0.getwithdrawals()["waiting"], 0)  # in the mempool
        block1 = self.generate(node0, 1)[0]
        withdrawals = node0.getwithdrawals()
        assert_equal(withdrawals["waiting"], 0)
        assert_equal(len(withdrawals["withdrawals"]), 1)
        w1 = withdrawals["withdrawals"][0]
        assert_equal((w1["position"], w1["txid"], w1["vout"], w1["address"], w1["amount"], w1["parent_fee"], w1["status"]),
                     (0, txid1, request["n"], ecx_addr1, 1, PARENT_FEE, "bundled"))
        bundle1 = withdrawals["bundle"]
        assert_equal((bundle1["blockhash"], bundle1["withdrawals"], bundle1["amount"], bundle1["parent_fee"]), (block1, 1, 1, PARENT_FEE))
        assert_equal(node1.getwithdrawals(), withdrawals)

        self.log.info("Disconnecting the block undoes it, connecting it again redoes it")
        node0.invalidateblock(block1)
        assert_equal(node0.getwithdrawals() | {"checkpoint": None}, {"waiting": 0, "withdrawals": [], "checkpoint": None})
        assert txid1 in node0.getrawmempool()
        node0.reconsiderblock(block1)
        assert_equal(node0.getwithdrawals(), withdrawals)

        self.log.info("Withdrawal requests are checked")
        assert_raises_rpc_error(-5, "Invalid parent chain address", node0.withdraw, "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", 1)  # mainnet's
        assert_raises_rpc_error(-5, "Invalid parent chain address", node0.withdraw, "not-an-address", 1)
        assert_raises_rpc_error(-8, "Amount is dust on the parent chain", node0.withdraw, ecx_addr1, Decimal("0.00000100"))

        self.log.info("A second withdrawal waits behind the pending bundle")
        ecx_addr2 = self.ecx.getnewaddress()
        txid2 = node0.withdraw(ecx_addr2, Decimal("0.5"), Decimal("0.001"))
        self.generate(node0, 1)
        withdrawals = node0.getwithdrawals()
        assert_equal(withdrawals["waiting"], 1)
        assert_equal([(w["txid"], w["status"]) for w in withdrawals["withdrawals"]], [(txid1, "bundled"), (txid2, "waiting")])
        assert_equal(withdrawals["bundle"]["m6id"], bundle1["m6id"])

        self.log.info("The bundle is proposed, voted through and paid out; its success is recorded")
        self.wait_for_proposal(bundle1["m6id"])
        self.mine_until_paid(ecx_addr1, 1)
        success1 = self.record_outcome(bundle1["m6id"], True)
        self.sync_blocks()
        # The waiting withdrawal makes the next bundle
        withdrawals = node1.getwithdrawals()
        bundle2 = withdrawals["bundle"]
        assert_equal(withdrawals["waiting"], 0)
        assert_equal([(w["txid"], w["status"]) for w in withdrawals["withdrawals"]], [(txid2, "bundled")])
        assert_equal((bundle2["blockhash"], bundle2["amount"]), (success1, Decimal("0.5")))
        self.wait_until(lambda: node1.getindexinfo()["coinstatsindex"]["synced"])
        assert_equal(node1.gettxoutsetinfo("muhash", node1.getblockheader(success1)["height"])["block_info"]["withdrawals"], 1 + PARENT_FEE)

        self.log.info("A bundle the parent chain's miners don't vote for fails; its withdrawals are bundled again")
        self.set_bundle_policy("NONE")
        self.wait_for_proposal(bundle2["m6id"])
        failed = False
        for _ in range(30):
            self.ecx_mine()
            two_way = self.enforcer("ValidatorService/GetTwoWayPegData", {"sidechainId": 83, "endBlockHash": {"hex": self.ecx.getbestblockhash()}})
            events = [e["withdrawalBundle"]["event"] for b in two_way.get("blocks", []) for e in b["blockInfo"].get("events", []) if "withdrawalBundle" in e]
            if any("failed" in e for e in events):
                failed = True
                break
        assert failed
        failure = self.record_outcome(bundle2["m6id"], False)
        withdrawals = node0.getwithdrawals()
        bundle3 = withdrawals["bundle"]
        assert bundle3["m6id"] != bundle2["m6id"]
        assert_equal((bundle3["blockhash"], bundle3["amount"]), (failure, Decimal("0.5")))
        assert_equal([(w["position"], w["txid"], w["status"]) for w in withdrawals["withdrawals"]], [(1, txid2, "bundled")])

        self.log.info("Blocks recording outcomes wrongly are rejected")
        _, res = self.submit(node0, [self.outcome_tx("11" * 32, True)])
        assert_equal(res, "bad-bundle-outcome-bundle")
        _, res = self.submit(node0, [self.outcome_tx(bundle2["m6id"], False)])
        assert_equal(res, "bad-bundle-outcome-bundle")
        tip = node0.getbestblockhash()
        assert_raises_rpc_error(-25, "not (yet) confirmed", self.submit, node0, [self.outcome_tx(bundle3["m6id"], True)])
        stuck = [t for t in node0.getchaintips() if t["status"] == "valid-headers"]
        assert_equal(len(stuck), 1)
        assert_equal(node0.getbestblockhash(), tip)
        node0.invalidateblock(stuck[0]["hash"])

        self.log.info("Voted for, the new bundle is paid out")
        self.set_bundle_policy("KNOWN")
        self.wait_for_proposal(bundle3["m6id"])
        paid_in = self.mine_until_paid(ecx_addr2, Decimal("0.5"))
        success3 = self.record_outcome(bundle3["m6id"], True)
        self.sync_blocks()
        assert_equal(node1.getwithdrawals() | {"checkpoint": None}, {"waiting": 0, "withdrawals": [], "checkpoint": None})

        self.log.info("A parent chain reorg that drops the payout invalidates the block that recorded it")
        new_branch = self.ecx_reorg(paid_in, [])
        self.sync_parent_chain()
        for node in self.nodes:
            self.wait_until(lambda: node.getbestblockhash() == node0.getblockheader(success3)["previousblockhash"])
            assert_equal(self.status(node, success3), "invalid")
            assert_equal(node.getwithdrawals()["bundle"]["m6id"], bundle3["m6id"])

        self.log.info("When the parent chain has the payout again, the block is valid again")
        self.ecx.invalidateblock(new_branch)
        self.ecx.reconsiderblock(paid_in)
        self.wait_for_enforcer()
        self.sync_parent_chain()
        for node in self.nodes:
            self.wait_until(lambda: node.getbestblockhash() == success3)
            assert_equal(node.getwithdrawals() | {"checkpoint": None}, {"waiting": 0, "withdrawals": [], "checkpoint": None})

        self.log.info("Valid chains contain the block the parent chain last paid out a bundle from")
        checkpoint = {"blockhash": bundle3["blockhash"], "height": node0.getblockheader(bundle3["blockhash"])["height"]}
        assert_equal(node0.getwithdrawals()["checkpoint"], checkpoint)
        ecx_addr3 = self.ecx.getnewaddress()
        txid3 = node0.withdraw(ecx_addr3, Decimal("0.25"))
        stale_block = self.generate(node0, 1)[0]
        stale = node0.getwithdrawals()["bundle"]
        self.wait_for_proposal(stale["m6id"])
        # node1 replaces the bundle's block by a longer branch, which bundles
        # the withdrawal again
        self.disconnect_nodes(0, 1)
        node1.invalidateblock(stale_block)
        node1.sendrawtransaction(node0.getrawtransaction(txid3, False, stale_block))
        branch = self.generate(node1, node0.getblockcount() - node1.getblockcount() + 5, sync_fun=self.no_op)
        node1.reconsiderblock(stale_block)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(node0.getbestblockhash(), branch[-1])
        replacement = node0.getwithdrawals()["bundle"]
        assert replacement["m6id"] != stale["m6id"]
        assert_equal([w["txid"] for w in node0.getwithdrawals()["withdrawals"]], [txid3])
        # The parent chain pays out the stale bundle: miners vote for it only,
        # once it's pending there (the enforcer drops other ACKs)
        self.set_bundle_policy("NONE")
        self.ecx_mine()
        m6id_bytes = bytes.fromhex(stale["m6id"])[::-1].hex()  # consensus byte order
        self.enforcer("BlockProducerService/SetWithdrawalBundleAck", {"sidechainNumber": 83, "m6id": {"hex": m6id_bytes}, "ack": True})
        self.mine_until_paid(ecx_addr3, Decimal("0.25"))
        self.confirm()
        self.sync_parent_chain()
        for node in self.nodes:
            self.wait_until(lambda: node.getblockheader(stale_block)["confirmations"] > 0)
            assert_equal(self.status(node, branch[-1]), "invalid")
            assert_equal(node.getwithdrawals()["checkpoint"], {"blockhash": stale_block, "height": node0.getblockheader(stale_block)["height"]})
            assert_equal(node.getwithdrawals()["bundle"]["m6id"], stale["m6id"])
        # Its payout is recorded there
        self.record_outcome(stale["m6id"], True)
        self.sync_blocks()
        # No block can take the stale block's place
        node1.invalidateblock(stale_block)
        assert_raises_rpc_error(-1, "bad-parent-checkpoint", self.generate, node1, 1, sync_fun=self.no_op)
        node1.reconsiderblock(stale_block)
        self.sync_blocks()

        self.log.info("With -bundlevotes, the enforcer votes only for the bundle pending on the node's chain")
        self.set_bundle_policy("KNOWN")
        self.restart_node(0, self.extra_args[0] + ["-bundlevotes"])
        self.connect_nodes(0, 1)
        ecx_addr4 = self.ecx.getnewaddress()
        txid4 = node0.withdraw(ecx_addr4, Decimal("0.125"))
        first_block = self.generate(node0, 1)[0]
        first = node0.getwithdrawals()["bundle"]
        self.wait_for_proposal(first["m6id"])
        self.ecx_mine()  # its M3: pending there, the ACK sticks
        node0.mockscheduler(6)
        self.wait_until(lambda: self.bundle_acks() == [first["m6id"]])
        assert_equal(self.enforcer("BlockProducerService/GetBlockProducerState")["withdrawalBundlePolicy"], "WITHDRAWAL_BUNDLE_POLICY_NONE")
        self.ecx_mine(2)  # votes for it
        # A reorg of this chain replaces the bundle's block, and the bundle
        self.disconnect_nodes(0, 1)
        node1.invalidateblock(first_block)
        node1.sendrawtransaction(node0.getrawtransaction(txid4, False, first_block))
        self.generate(node1, node0.getblockcount() - node1.getblockcount() + 5, sync_fun=self.no_op)
        node1.reconsiderblock(first_block)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        second = node0.getwithdrawals()["bundle"]
        assert second["m6id"] != first["m6id"]
        node0.mockscheduler(6)
        self.wait_until(lambda: self.bundle_acks() == [])  # the new one isn't pending there yet
        self.ecx_mine()
        node0.mockscheduler(6)
        self.wait_until(lambda: self.bundle_acks() == [second["m6id"]])
        # Only the new bundle is paid out, and once
        self.mine_until_paid(ecx_addr4, Decimal("0.125"))
        self.record_outcome(second["m6id"], True)
        self.sync_blocks()
        node0.mockscheduler(6)
        self.wait_until(lambda: self.bundle_acks() == [])
        self.ecx_mine(12)  # the old bundle expires
        assert_equal(len([t for t in self.ecx.listtransactions("*", 1000) if t.get("address") == ecx_addr4 and t["category"] == "receive"]), 1)

        self.log.info("Both nodes end with the same UTXO set, which node1's coinstatsindex tracked")
        assert_equal(node0.gettxoutsetinfo()["hash_serialized_3"], node1.gettxoutsetinfo()["hash_serialized_3"])
        self.wait_until(lambda: node1.getindexinfo()["coinstatsindex"]["synced"])
        utxos = node1.gettxoutsetinfo("muhash", use_index=False)
        indexed = node1.gettxoutsetinfo("muhash", use_index=True)
        for key in ("muhash", "txouts", "bogosize", "total_amount"):
            assert_equal(indexed[key], utxos[key])


if __name__ == '__main__':
    WithdrawalsTest(__file__).main()
