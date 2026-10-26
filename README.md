SideBoost Core
==============

> **⚠️ Experimental.** Both SideBoost's design and this code are experimental
> and have seen little real-world use. They may have bugs that lose coins,
> and the rules may still change. Don't put more money into SideBoost than
> you are ready to lose.

What is SideBoost?
------------------

SideBoost is a drivechain (BIP300 sidechain) of eCash (ECX), meant to enable
fast drivechains.

* **Pegged to ECX.** SideBoost has no coin of its own and no block subsidy:
  its coins are ECX, deposited into its sidechain slot on ECX (slot 83) and
  withdrawn through BIP300 withdrawal bundles that ECX miners vote on.
* **10-second blocks**, with Bitcoin's throughput per unit of time (block
  weight and sigops scaled to the interval).
* **Its own proof of work, with AuxPoW**: blocks can be merge-mined by ECX
  miners, or mined on their own. SideBoost is a fork of
  [Namecoin Core](https://github.com/namecoin/namecoin-core), the
  longest-running merge-mined chain, without its name system.
* **A home for further drivechains.** SideBoost enforces BIP300 and BIP301
  (blind merged mining) itself, so other drivechains can be activated on top
  of it, with deposits, withdrawals and BMM, at SideBoost's speed.

How it works, briefly
---------------------

| | |
|---|---|
| Deposits from ECX | A SideBoost node learns about deposits to slot 83 from a BIP300 enforcer (`-enforcer=host:port`): the full [bip300301_enforcer](https://github.com/LayerTwo-Labs/bip300301_enforcer) with an ECX node, or a light enforcer that follows ECX without one. Blocks credit them once 3 ECX blocks are on top |
| Withdrawals to ECX | The `withdraw` RPC queues a withdrawal; nodes bundle queued withdrawals and propose each bundle to ECX's miners, who vote on it over ECX's BIP300 period |
| Drivechains on top | Sidechains propose and activate in SideBoost's slots, take deposits and pay out withdrawal bundles voted by SideBoost's miners, and are blind-merge-mined in SideBoost blocks (see `getsidechains`, `depositsidechain`, `createbmmrequest`) |

The executables are `sideboostd`, `sideboost-qt`, `sideboost-cli`,
`sideboost-tx`, `sideboost-util` and `sideboost-wallet`; the data directory is
`~/.sideboost`.

Running a node
--------------

A node needs an enforcer to learn about ECX. The simplest is the
[light-enforcer](https://github.com/andfewrields/light-enforcer), which
follows ECX without an ECX node (see its README for building it). In two
terminals:

```bash
# 1. the enforcer, ready when it serves a tip (a few minutes at first)
light-enforcer --assume-slot-active

# 2. the node
sideboostd -printtoconsole -enforcer=127.0.0.1:50051
```

**Beta testing: `--assume-slot-active`.** Slot 83 isn't active on ECX yet.
With this option the light enforcer reports the slot's deposits as if it
were, so SideBoost can be tried with real ECX deposits. It's off by default
and only for testing:

* Every SideBoost node on the network must use it. A node without it sees
  none of these deposits, and stops at the first block that credits one.
* The coins can't come back. Before activation the slot's treasury output is
  anyone-can-spend on ECX and withdrawals never pay out, so deposit only tiny
  amounts. Nothing deposited before activation is in the treasury afterwards.
* The genesis block will be re-mined before the real launch, so a beta chain
  won't carry over.

**Mining.** To mine on a node of your own, with a wallet that receives the
fees:

```bash
contrib/sideboost-miner.sh 1      # full speed
contrib/sideboost-miner.sh 0.5    # in each 10 s cycle: sleep 5 s, mine up to 5 s
```

The argument is the speed, 0 to 1. Each cycle the script sleeps
`10 * (1 - speed)` seconds, then mines at full speed for `10 * speed`
seconds or until it finds a block, and starts over. A block from another node
also starts the cycle over (`generatetoaddress` alone would keep mining a
stale block). Options: `-w WALLET`, `-c path/to/sideboost-cli`, and further
`sideboost-cli` options after the speed.

License
-------

SideBoost Core is released under the terms of the MIT license. See
[COPYING](COPYING) for more information or see
https://opensource.org/license/MIT. It builds on Namecoin Core and Bitcoin
Core, whose copyright notices are kept.

Building
--------

See the `doc/build-*.md` files for instructions; on Linux, for example:

```bash
cmake -B build
cmake --build build
```

Testing
-------

Unit tests can be run (assuming they weren't disabled when generating the
build system) with `ctest --test-dir build`, or `build/bin/test_sideboost`.
Further details on running and extending unit tests can be found in
[/src/test/README.md](/src/test/README.md).

There are also [regression and integration tests](/test), written in Python.
They can be run (if the [test dependencies](/test) are installed) with
`build/test/functional/test_runner.py` (assuming `build` is your build
directory). The tests of deposits and withdrawals need an ECX node and a
`bip300301_enforcer`, found through the `ECX_BITCOIND` and
`BIP300301_ENFORCER` environment variables.
