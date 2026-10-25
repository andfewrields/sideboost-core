// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UNDO_H
#define BITCOIN_UNDO_H

#include <coins.h>
#include <compressor.h>
#include <consensus/consensus.h>
#include <primitives/transaction.h>
#include <serialize.h>

/** Formatter for undo information for a CTxIn
 *
 *  Contains the prevout's CTxOut being spent, and its metadata as well
 *  (coinbase or not, height). The serialization contains a dummy value of
 *  zero. This is compatible with older versions which expect to see
 *  the transaction version there.
 */
struct TxInUndoFormatter
{
    template<typename Stream>
    void Ser(Stream &s, const Coin& txout) {
        uint32_t nCode{(uint32_t{txout.nHeight} << 1) | uint32_t{txout.fCoinBase}};
        ::Serialize(s, VARINT(nCode));
        if (txout.nHeight > 0) {
            // Required to maintain compatibility with older undo format.
            ::Serialize(s, (unsigned char)0);
        }
        ::Serialize(s, Using<TxOutCompression>(txout.out));
    }

    template<typename Stream>
    void Unser(Stream &s, Coin& txout) {
        uint32_t nCode = 0;
        ::Unserialize(s, VARINT(nCode));
        txout.nHeight = nCode >> 1;
        txout.fCoinBase = nCode & 1;
        if (txout.nHeight > 0) {
            // Old versions stored the version number for the last spend of
            // a transaction's outputs. Non-final spends were indicated with
            // height = 0.
            unsigned int nVersionDummy;
            ::Unserialize(s, VARINT(nVersionDummy));
        }
        ::Unserialize(s, Using<TxOutCompression>(txout.out));
    }
};

/** Undo information for a CTransaction */
class CTxUndo
{
public:
    // undo information for all txins
    std::vector<Coin> vprevout;

    SERIALIZE_METHODS(CTxUndo, obj) { READWRITE(Using<VectorFormatter<TxInUndoFormatter>>(obj.vprevout)); }
};

/** A change a block made to a coin outside its transactions' inputs and
 *  outputs: consensus state the UTXO set holds (see bip300/state.h).  A spent
 *  coin stands for none. */
struct StateCoinChange
{
    COutPoint outpoint;
    Coin before;
    Coin after;

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        const uint8_t present = uint8_t{!before.IsSpent()} | uint8_t(uint8_t{!after.IsSpent()} << 1);
        ::Serialize(s, outpoint);
        ::Serialize(s, present);
        if (!before.IsSpent()) ::Serialize(s, Using<TxInUndoFormatter>(before));
        if (!after.IsSpent()) ::Serialize(s, Using<TxInUndoFormatter>(after));
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t present;
        ::Unserialize(s, outpoint);
        ::Unserialize(s, present);
        if (present & ~uint8_t{3}) throw std::ios_base::failure("unknown state coin change");
        before.Clear();
        after.Clear();
        if (present & 1) ::Unserialize(s, Using<TxInUndoFormatter>(before));
        if (present & 2) ::Unserialize(s, Using<TxInUndoFormatter>(after));
    }
};

/** Undo information for a CBlock */
class CBlockUndo
{
public:
    std::vector<CTxUndo> vtxundo; // for all but the coinbase
    /** State coins the block changed, in order. */
    std::vector<StateCoinChange> vstate;
    /** What happened to this chain's sidechains in the block, for RPCs
     *  (bip300::BlockEvents, serialized; empty if nothing did). */
    std::vector<unsigned char> sidechain_events;

    SERIALIZE_METHODS(CBlockUndo, obj) { READWRITE(obj.vtxundo, obj.vstate, obj.sidechain_events); }
};

#endif // BITCOIN_UNDO_H
