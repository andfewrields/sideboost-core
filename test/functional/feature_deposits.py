#!/usr/bin/env python3
# Copyright (c) 2026 The SideBoost developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test deposits from the parent chain (BIP300 M5, see drivechain/deposit.h).

Runs a parent chain: an ECX regtest node and its bip300301_enforcer, given by
the environment variables ECX_BITCOIND and BIP300301_ENFORCER (the test is
skipped without them).  Activates slot 83 there, deposits into it, and checks
that SideBoost nodes credit the deposits, reject blocks that credit them
wrongly, and roll them back on a reorg.
"""

import json
import os
import subprocess
import urllib.request
from decimal import Decimal

from test_framework.authproxy import AuthServiceProxy, JSONRPCException
from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import COIN, SEQUENCE_FINAL, COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript, OP_RETURN, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, assert_raises_rpc_error, p2p_port, rpc_port

SLOT = 83
OP_DRIVECHAIN = 0xb7  # ECX betanet builds: OP_NOP8
TREASURY = CScript(bytes([OP_DRIVECHAIN, 0x01, SLOT, OP_TRUE]))
ECX_FEE = 10_000


class DepositsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.ecx_bitcoind = os.getenv("ECX_BITCOIND")
        self.enforcer_bin = os.getenv("BIP300301_ENFORCER")
        if not self.ecx_bitcoind or not self.enforcer_bin:
            raise SkipTest("set ECX_BITCOIND and BIP300301_ENFORCER to run the parent chain")

    # Parent chain

    def start_parent_chain(self):
        self.ecx_rpc_port = rpc_port(8)
        zmq_port = p2p_port(9)
        self.enforcer_port = p2p_port(10)
        ecx_dir = os.path.join(self.options.tmpdir, "ecx")
        enforcer_dir = os.path.join(self.options.tmpdir, "enforcer")
        os.makedirs(ecx_dir)
        os.makedirs(enforcer_dir)
        self.ecx_proc = subprocess.Popen(
            [self.ecx_bitcoind, "-regtest", f"-datadir={ecx_dir}", f"-port={p2p_port(8)}", f"-rpcport={self.ecx_rpc_port}",
             "-rpcuser=u", "-rpcpassword=p", "-server", "-rest", "-txindex", "-listen=0", "-listenonion=0",
             "-fallbackfee=0.0001", f"-zmqpubsequence=tcp://127.0.0.1:{zmq_port}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ecx_url = f"http://u:p@127.0.0.1:{self.ecx_rpc_port}"
        self.wait_until(lambda: self.try_call(lambda: AuthServiceProxy(ecx_url).getblockcount()))
        AuthServiceProxy(ecx_url).createwallet("w")
        self.ecx = AuthServiceProxy(f"{ecx_url}/wallet/w")
        self.enforcer_log = open(os.path.join(self.options.tmpdir, "enforcer.log"), "w")
        self.enforcer_proc = subprocess.Popen(
            [self.enforcer_bin, f"--data-dir={enforcer_dir}",
             f"--node-rpc-addr=127.0.0.1:{self.ecx_rpc_port}", "--node-rpc-user=u", "--node-rpc-pass=p",
             f"--node-zmq-addr-sequence=tcp://127.0.0.1:{zmq_port}",
             "--network-magic=eca5b134", "--op-drivechain=nop8",
             f"--serve-grpc-addr=127.0.0.1:{self.enforcer_port}", f"--serve-rpc-addr=127.0.0.1:{p2p_port(11)}",
             "--enable-mempool", "--enable-block-template-server", f"--coinbase-recipient={self.ecx.getnewaddress()}"],
            stdout=self.enforcer_log, stderr=subprocess.STDOUT)
        self.wait_until(lambda: self.try_call(lambda: self.enforcer("ValidatorService/GetChainTip")))

    def stop_parent_chain(self):
        for proc in (getattr(self, "enforcer_proc", None), getattr(self, "ecx_proc", None)):
            if proc and proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=60)

    @staticmethod
    def try_call(fn):
        try:
            fn()
            return True
        except (OSError, JSONRPCException):
            return False

    def enforcer(self, method, request=None):
        req = urllib.request.Request(f"http://127.0.0.1:{self.enforcer_port}/cusf.mainchain.v1.{method}",
                                     json.dumps(request or {}).encode(), {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.load(resp)

    def ecx_mine(self, n=1):
        """Mine n parent blocks through the enforcer; return the last hash."""
        hashes = self.enforcer("MiningService/GenerateToAddress", {"blocks": n, "address": self.ecx.getnewaddress()})["blockHashes"]
        tip = hashes[-1]["hex"]
        self.wait_until(lambda: self.enforcer("ValidatorService/GetChainTip")["blockHeaderInfo"]["blockHash"]["hex"] == tip)
        return tip

    def deposit(self, address, amount):
        """Send a deposit (M5) of `amount` to `address`; return its parent txid.

        The enforcer only takes a deposit that spends the confirmed treasury
        output, so each must be mined before the next."""
        old_value = self.treasury[1] if self.treasury else 0
        value = int(amount * COIN)
        utxo = next(u for u in self.ecx.listunspent() if u["amount"] * COIN >= value + ECX_FEE)
        tx = CTransaction()
        prevtxs = []
        if self.treasury:
            tx.vin.append(CTxIn(COutPoint(int(self.treasury[0], 16), 0)))
            prevtxs.append({"txid": self.treasury[0], "vout": 0,
                            "scriptPubKey": TREASURY.hex(), "amount": Decimal(old_value) / COIN})
        tx.vin.append(CTxIn(COutPoint(int(utxo["txid"], 16), utxo["vout"])))
        tx.vout.append(CTxOut(old_value + value, TREASURY))
        tx.vout.append(CTxOut(0, CScript([OP_RETURN, address.encode()])))
        change = bytes.fromhex(self.ecx.getaddressinfo(self.ecx.getnewaddress())["scriptPubKey"])
        tx.vout.append(CTxOut(int(utxo["amount"] * COIN) - value - ECX_FEE, change))
        # The treasury input needs no signature
        signed = self.ecx.signrawtransactionwithwallet(tx.serialize().hex(), prevtxs)
        txid = self.ecx.sendrawtransaction(signed["hex"])
        self.treasury = (txid, old_value + value)
        return txid

    # SideBoost

    def setup_nodes(self):
        self.treasury = None
        self.start_parent_chain()
        args = [f"-depositslot={SLOT}", f"-enforcer=127.0.0.1:{self.enforcer_port}"]
        self.extra_args = [args, args + ["-coinstatsindex"]]
        super().setup_nodes()

    def stop_nodes(self, wait=0):
        super().stop_nodes(wait)
        self.stop_parent_chain()

    def deposits_in(self, node, blockhash):
        """The deposit transactions of a block (right after the coinbase)."""
        return [tx for tx in node.getblock(blockhash, 2)["tx"][1:] if tx["vin"][0].get("scriptSig", {}).get("hex", "").startswith("6a")]

    @staticmethod
    def deposit_tx(parent_txid, parent_block, sequence, value, script):
        tx = CTransaction()
        tx.version = 2
        tx.vin = [CTxIn(COutPoint(int(parent_txid, 16), 0),
                        CScript([OP_RETURN, bytes.fromhex(parent_block)[::-1], sequence.to_bytes(8, "little")]),
                        SEQUENCE_FINAL)]
        tx.vout = [CTxOut(value, script)]
        return tx

    def submit(self, node, txs):
        tip = node.getbestblockhash()
        height = node.getblockcount() + 1
        block = create_block(int(tip, 16), create_coinbase(height, nValue=0), txlist=txs)
        block.solve()
        return block, node.submitblock(block.serialize().hex())

    def run_test(self):
        node0, node1 = self.nodes
        self.log.info("Activate slot 83 on the parent chain")
        self.ecx_mine(101)
        self.enforcer("BlockProducerService/SubmitSidechainProposal", {"sidechainId": SLOT, "declaration": {"v0": {
            "title": "SideBoost", "description": "SideBoost regtest",
            "hashId1": {"hex": "00" * 32}, "hashId2": {"hex": "00" * 20}}}})
        self.enforcer("BlockProducerService/SetAckAllProposals", {"policy": "ACK_ALL_PROPOSALS_POLICY_NEW_SLOTS"})
        self.ecx_mine(8)
        assert_equal([s["sidechainNumber"] for s in self.enforcer("ValidatorService/GetSidechains").get("sidechains", [])], [SLOT])

        self.log.info("A block credits a deposit")
        addr0 = node0.getnewaddress()
        txid0 = self.deposit(addr0, Decimal("1.5"))
        parent0 = self.ecx_mine()
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
        parent4 = self.ecx_mine()
        txid5 = self.deposit(addr4, Decimal("0.4"))
        parent5 = self.ecx_mine()
        good4 = self.deposit_tx(txid4, parent4, 4, int(Decimal("0.3") * COIN), script4)
        good5 = self.deposit_tx(txid5, parent5, 5, int(Decimal("0.4") * COIN), script4)
        _, res = self.submit(node0, [self.deposit_tx(txid4, parent4, 4, int(Decimal("0.3") * COIN) + 1, script4)])
        assert_equal(res, "bad-deposit-mismatch")
        _, res = self.submit(node0, [self.deposit_tx(txid4, parent4, 4, int(Decimal("0.3") * COIN), CScript([OP_TRUE]))])
        assert_equal(res, "bad-deposit-mismatch")
        _, res = self.submit(node0, [good5])
        assert_equal(res, "bad-deposit-sequence")
        _, res = self.submit(node0, [self.deposit_tx(txid4, parent0, 4, int(Decimal("0.3") * COIN), script4)])
        assert_equal(res, "bad-deposit-unknown")
        ordinary = CTransaction()
        ordinary.vin = [CTxIn(COutPoint(int(block0, 16), 0))]
        ordinary.vout = [CTxOut(1, CScript([OP_TRUE]))]
        _, res = self.submit(node0, [ordinary, good4])
        assert_equal(res, "bad-txns-deposit-position")

        self.log.info("A deposit from a parent block we don't know waits instead of failing")
        unknown = self.deposit_tx(txid4, "11" * 32, 4, int(Decimal("0.3") * COIN), script4)
        tip = node0.getbestblockhash()
        assert_raises_rpc_error(-25, "not (yet) deep enough", self.submit, node0, [unknown])
        stuck = [t for t in node0.getchaintips() if t["status"] == "valid-headers"]
        assert_equal(len(stuck), 1)
        assert_equal(node0.getbestblockhash(), tip)
        node0.invalidateblock(stuck[0]["hash"])

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

        self.log.info("The deposit state survives a restart")
        self.restart_node(0, extra_args=self.extra_args[0])
        self.connect_nodes(0, 1)
        txid6 = self.deposit(addr0, Decimal("0.01"))
        self.ecx_mine()
        block6 = self.generate(node0, 1)[0]
        assert_equal([d["vin"][0]["txid"] for d in self.deposits_in(node0, block6)], [txid6])

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
