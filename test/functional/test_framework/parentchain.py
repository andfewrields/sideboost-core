#!/usr/bin/env python3
# Copyright (c) 2026 The SideBoost developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A parent chain for tests of deposits and withdrawals (BIP300).

Runs an ECX regtest node and its bip300301_enforcer, given by the environment
variables ECX_BITCOIND and BIP300301_ENFORCER (tests are skipped without
them), and SideBoost nodes that take deposits from its slot 83.
"""

import json
import os
import subprocess
import urllib.request
from decimal import Decimal

from .authproxy import AuthServiceProxy, JSONRPCException
from .blocktools import create_block, create_coinbase
from .messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut
from .script import CScript, OP_RETURN, OP_TRUE
from .test_framework import SkipTest
from .util import assert_equal, p2p_port, rpc_port

SLOT = 83
OP_DRIVECHAIN = 0xb7  # ECX betanet builds: OP_NOP8
TREASURY = CScript(bytes([OP_DRIVECHAIN, 0x01, SLOT, OP_TRUE]))
ECX_FEE = 10_000


class ParentChainTest:
    """Mixed into a BitcoinTestFramework test, before it."""

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
        self.extra_args = [args, args + ["-coinstatsindex"]][:self.num_nodes]
        super().setup_nodes()

    def stop_nodes(self, wait=0):
        super().stop_nodes(wait)
        self.stop_parent_chain()

    @staticmethod
    def make_block(node, txs, prev=None):
        """A block with `txs` on `prev` (default: the tip)."""
        prev = prev or node.getbestblockhash()
        height = node.getblockheader(prev)["height"] + 1
        block = create_block(int(prev, 16), create_coinbase(height, nValue=0), txlist=txs)
        block.solve()
        return block

    def submit(self, node, txs):
        block = self.make_block(node, txs)
        return block, node.submitblock(block.serialize().hex())

    def confirm(self):
        """Mine the confirmations deposits need (3 blocks on top)."""
        self.ecx_mine(3)

    def ecx_reorg(self, first_replaced, txs):
        """Replace parent block `first_replaced` and its descendants by a
        longer branch whose first block holds `txs` (raw hex) and nothing
        else.  Return the new branch's first block."""
        height = self.ecx.getblockheader(first_replaced)["height"]
        length = self.ecx.getblockcount() - height + 2
        self.ecx.invalidateblock(first_replaced)
        address = self.ecx.getnewaddress()
        new_first = self.ecx.generateblock(address, txs)["hash"]
        for _ in range(length - 1):
            self.ecx.generateblock(address, [])
        self.wait_for_enforcer()
        return new_first

    def wait_for_enforcer(self):
        tip = self.ecx.getbestblockhash()
        self.wait_until(lambda: self.enforcer("ValidatorService/GetChainTip")["blockHeaderInfo"]["blockHash"]["hex"] == tip)

    def sync_parent_chain(self):
        """Have every node follow the parent chain to its current tip now."""
        message = f"Following the parent chain to tip {self.ecx.getbestblockhash()}".encode()
        for node in self.nodes:
            node.mockscheduler(6)
            self.wait_until(lambda: message in node.debug_log_path.read_bytes())

    def status(self, node, blockhash):
        return next(t["status"] for t in node.getchaintips() if t["hash"] == blockhash)

    def activate_slot(self):
        """Activate slot 83 on the parent chain."""
        self.ecx_mine(101)
        self.enforcer("BlockProducerService/SubmitSidechainProposal", {"sidechainId": SLOT, "declaration": {"v0": {
            "title": "SideBoost", "description": "SideBoost regtest",
            "hashId1": {"hex": "00" * 32}, "hashId2": {"hex": "00" * 20}}}})
        self.enforcer("BlockProducerService/SetAckAllProposals", {"policy": "ACK_ALL_PROPOSALS_POLICY_NEW_SLOTS"})
        self.ecx_mine(8)
        assert_equal([s["sidechainNumber"] for s in self.enforcer("ValidatorService/GetSidechains").get("sidechains", [])], [SLOT])
