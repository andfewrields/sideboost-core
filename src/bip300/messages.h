// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BIP300_MESSAGES_H
#define BITCOIN_BIP300_MESSAGES_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

/**
 * BIP300 and BIP301 for this chain's own sidechains: the messages.
 *
 * Miners put M1 to M4 and M7 in coinbase outputs, as BIP300/301 define them:
 * an OP_RETURN followed by a single push of a 4-byte tag and the body.  A
 * script that doesn't have exactly that form, or whose body is malformed, is
 * not a message.  The tags are the reference enforcer's (bip300301_enforcer),
 * whose M2 tag differs from the spec's.
 *
 *   M1 propose sidechain   <D5 E0 C4 AF> <slot> <description>
 *   M2 ACK proposal        <D6 E1 C5 DF> <slot> <sha256d(description)>
 *   M3 propose bundle      <D4 5A A9 43> <slot> <M6ID>
 *   M4 ACK bundles         <D7 7D 17 76> <version> [<votes>]
 *   M7 BMM accept          <D1 61 73 68> <slot> <sidechain block hash>
 *
 * Deposits (M5) are outputs of ordinary transactions; unlike BIP300's they
 * don't spend the previous treasury output, but pay into the slot's treasury
 * by consensus (see state.h):
 *
 *   M5 deposit             value: the deposit
 *                          OP_RETURN <D5 3B 05 9A> <slot> <sidechain address>
 *
 * A withdrawal bundle's payout (M6) is the sidechain's blinded M6, as
 * BIP300 defines it, spending the slot's treasury:
 *
 *   input:   prevout   (TreasuryTxid(), slot), empty scriptSig
 *   outputs: 0, OP_RETURN <total fee, 8 bytes BE>
 *            the sidechain's further outputs (its commitment, the payouts)
 *
 * Its M6ID is its txid without the input: the txid of the blinded M6.
 *
 * A BMM request (M8, BIP301) is an ordinary transaction whose first output
 * is
 *
 *   OP_RETURN <00 BF 00> <slot> <sidechain block hash> <previous block hash>
 *
 * valid only in a block on that previous block whose coinbase has the
 * matching M7.  Its fee is the bid.
 */
namespace bip300 {

using Tag = std::array<unsigned char, 4>;

inline constexpr Tag M1_TAG{0xD5, 0xE0, 0xC4, 0xAF};
inline constexpr Tag M2_TAG{0xD6, 0xE1, 0xC5, 0xDF};
inline constexpr Tag M3_TAG{0xD4, 0x5A, 0xA9, 0x43};
inline constexpr Tag M4_TAG{0xD7, 0x7D, 0x17, 0x76};
inline constexpr Tag M5_TAG{0xD5, 0x3B, 0x05, 0x9A};
inline constexpr Tag M7_TAG{0xD1, 0x61, 0x73, 0x68};
inline constexpr std::array<unsigned char, 3> M8_TAG{0x00, 0xBF, 0x00};

/** M1: propose that the sidechain `description` take `slot`.  */
struct ProposeSidechain {
    uint8_t slot{0};
    std::vector<unsigned char> description;

    bool operator==(const ProposeSidechain&) const = default;
};

/** The hash M2s ACK a proposal by: sha256d of its description.  */
uint256 DescriptionHash(std::span<const unsigned char> description);

/** M2: ACK the proposal for `slot` with this description hash.  */
struct AckSidechain {
    uint8_t slot{0};
    uint256 description_hash;

    bool operator==(const AckSidechain&) const = default;
};

/** M3: propose bundle `m6id` for `slot`.  */
struct ProposeBundle {
    uint8_t slot{0};
    uint256 m6id;

    bool operator==(const ProposeBundle&) const = default;
};

/** M4: votes on the pending bundles.  */
struct AckBundles {
    enum class Version : uint8_t {
        /** The previous block's votes again.  */
        REPEAT_PREVIOUS = 0,
        /** One vote per active slot, in slot order: a bundle's index (oldest
         *  first), ABSTAIN or ALARM.  */
        ONE_BYTE = 1,
        /** The same, in 2 bytes LE each; only when an index needs them.  */
        TWO_BYTES = 2,
        /** In each slot, upvote the bundle leading the others by
         *  LEADING_BY_MARGIN, if any.  */
        LEADING_BY_50 = 3,
    };
    static constexpr uint16_t ABSTAIN_ONE_BYTE{0xFF};
    static constexpr uint16_t ALARM_ONE_BYTE{0xFE};
    static constexpr uint16_t ABSTAIN{0xFFFF};
    static constexpr uint16_t ALARM{0xFFFE};
    /** The largest index a ONE_BYTE vote can hold.  */
    static constexpr uint16_t MAX_ONE_BYTE_INDEX{0xFD};
    static constexpr uint32_t LEADING_BY_MARGIN{50};

    Version version{Version::REPEAT_PREVIOUS};
    /** ONE_BYTE and TWO_BYTES: the votes, ABSTAIN and ALARM in their 2-byte
     *  form.  */
    std::vector<uint16_t> votes;

    bool operator==(const AckBundles&) const = default;
};

/** M7: the block commits to sidechain block `hash` in `slot`.  */
struct BmmAccept {
    uint8_t slot{0};
    uint256 hash;

    bool operator==(const BmmAccept&) const = default;
};

using CoinbaseMessage = std::variant<ProposeSidechain, AckSidechain, ProposeBundle, AckBundles, BmmAccept>;

/** The message `script` holds, if it is one.  */
std::optional<CoinbaseMessage> ParseCoinbaseMessage(const CScript& script);

/** The script of `message`.  An AckBundles' ONE_BYTE or TWO_BYTES form is
 *  picked by its votes (see MakeAckBundles).  */
CScript MakeCoinbaseMessageScript(const CoinbaseMessage& message);

/** The M4 casting `votes` (ABSTAIN, ALARM or an index, one per active slot):
 *  ONE_BYTE unless an index needs two.  */
AckBundles MakeAckBundles(std::vector<uint16_t> votes);

/** M5: a deposit of the output's value to `address` in `slot`.  */
struct Deposit {
    uint8_t slot{0};
    std::vector<unsigned char> address;

    bool operator==(const Deposit&) const = default;
};

CScript MakeDepositScript(uint8_t slot, std::span<const unsigned char> address);

/** Whether `script` claims to be a deposit: an OP_RETURN whose first push
 *  starts with M5_TAG.  Such an output must be a well-formed deposit
 *  (ParseDeposit), or its transaction is invalid.  */
bool IsDepositScript(const CScript& script);

/** The deposit `out` makes, or nullopt if it doesn't (well): it needs the
 *  form above and a value of at least 1 sat.  */
std::optional<Deposit> ParseDeposit(const CTxOut& out);

/** M8: a bid for the block's M7 to commit to `hash` in `slot`.  */
struct BmmRequest {
    uint8_t slot{0};
    uint256 hash;
    uint256 prev_block;

    bool operator==(const BmmRequest&) const = default;
};

CScript MakeBmmRequestScript(const BmmRequest& request);

/** The request `tx` makes, if its first output is one.  */
std::optional<BmmRequest> ParseBmmRequest(const CTransaction& tx);

/** The txid of `tx` without its inputs: a blinded M6's M6ID, or a payout's.  */
uint256 ComputeM6Id(const CTransaction& tx);

/** The total fee a (blinded) M6 pays the miner, from its first output, or
 *  nullopt if that isn't one: value 0, OP_RETURN <8 bytes BE>, within
 *  MoneyRange.  */
std::optional<CAmount> ParseM6Fee(const CTransaction& m6);

/** What a blinded M6 takes from the treasury: its outputs plus its fee, or
 *  nullopt if it isn't a blinded M6 (malformed fee output, or inputs).  */
std::optional<CAmount> M6Value(const CTransaction& blinded);

/** The payout spending `slot`'s treasury for blinded M6 `blinded`.  */
CTransactionRef MakePayout(const CTransaction& blinded, uint8_t slot);

} // namespace bip300

#endif // BITCOIN_BIP300_MESSAGES_H
