#!/usr/bin/env bash
# Copyright (c) 2026 The SideBoost developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Mine SideBoost blocks at a throttled pace. Each cycle:
#
#   1. sleep 10*(1-SPEED) seconds;
#   2. mine at full speed for 10*SPEED seconds, or until a block is found;
#   3. start over.
#
# A new block from another node also starts a cycle over, during the sleep or
# the mining, since the work on the old tip would only make a stale block.
# `generatetoaddress` itself never notices a new tip, so this mines in short
# chunks of attempts and compares the tip between them. The chunk size adapts
# to your hash rate, to keep each chunk around a second, so a cycle can run
# up to about a second over.
#
# Usage: sideboost-miner.sh [-w WALLET] [-c CLI] SPEED [CLI OPTIONS...]
#   SPEED          0 to 1: the share of each 10 s cycle spent mining (1 never
#                  sleeps, 0 never mines)
#   -w WALLET      wallet to mine to (default: miner); loaded or created
#   -c CLI         sideboost-cli to use (default: $SIDEBOOST_CLI, else
#                  build/bin/sideboost-cli of this repo, else the PATH's)
#   CLI OPTIONS    passed to sideboost-cli, e.g. -datadir=DIR or -regtest
#
# Needs a running sideboostd. Stop it with Ctrl-C.

set -u

WALLET=miner
BUILT_CLI=$(dirname "$0")/../build/bin/sideboost-cli
if [ -x "$BUILT_CLI" ]; then DEFAULT_CLI=$BUILT_CLI; else DEFAULT_CLI=sideboost-cli; fi
CLI=${SIDEBOOST_CLI:-$DEFAULT_CLI}
while getopts "w:c:h" opt; do
    case $opt in
        w) WALLET=$OPTARG ;;
        c) CLI=$OPTARG ;;
        *) echo "Usage: $0 [-w WALLET] [-c CLI] SPEED [CLI OPTIONS...]" >&2; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

SPEED=${1:-}
[ -n "$SPEED" ] || { echo "Usage: $0 [-w WALLET] [-c CLI] SPEED [CLI OPTIONS...]" >&2; exit 1; }
shift
if ! awk -v s="$SPEED" 'BEGIN { exit !(s ~ /^([0-9]+\.?[0-9]*|\.[0-9]+)$/ && s >= 0 && s <= 1) }'; then
    echo "SPEED must be a number between 0 and 1" >&2
    exit 1
fi
PAUSE_MS=$(awk -v s="$SPEED" 'BEGIN { printf "%d", 10000 * (1 - s) }')
MINE_MS=$(awk -v s="$SPEED" 'BEGIN { printf "%d", 10000 * s }')

# Options go before the command, as sideboost-cli wants
OPTS=("$@")
cli() { "$CLI" "${OPTS[@]}" -rpcclienttimeout=0 "$@"; }
walletcli() { "$CLI" "${OPTS[@]}" -rpcclienttimeout=0 -rpcwallet="$WALLET" "$@"; }
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

if ! walletcli getwalletinfo >/dev/null 2>&1; then
    cli loadwallet "$WALLET" >/dev/null 2>&1 || cli createwallet "$WALLET" >/dev/null || exit 1
fi
ADDR=$(walletcli getnewaddress) || exit 1
echo "Mining to $ADDR with speed $SPEED (sleeping $((PAUSE_MS / 1000)).$(printf %03d $((PAUSE_MS % 1000))) s, then mining for up to $((MINE_MS / 1000)).$(printf %03d $((MINE_MS % 1000))) s, per cycle)"

tries=1000000
while true; do
    tip=$(cli getbestblockhash) || { sleep 5; continue; }

    # Sleep, watching for a new block
    end=$(( $(now_ms) + PAUSE_MS ))
    while (( $(now_ms) < end )); do
        sleep 0.25
        [ "$(cli getbestblockhash)" = "$tip" ] || continue 2
    done

    # Mine, in chunks, until the time is up or a block is found
    end=$(( $(now_ms) + MINE_MS ))
    while (( $(now_ms) < end )); do
        start=$(now_ms)
        found=$(walletcli generatetoaddress 1 "$ADDR" "$tries") || { sleep 5; continue 2; }
        if [[ $found == *\"* ]]; then
            echo "Found: $(echo "$found" | tr -d '[:space:]')"
            continue 2
        fi
        # A chunk repeats the same attempts if the block template is the same
        # as the last one's, so keep chunks over a second
        if (( $(now_ms) - start < 1000 )); then tries=$(( tries * 2 )); fi
        [ "$(cli getbestblockhash)" = "$tip" ] || continue 2
    done
done
