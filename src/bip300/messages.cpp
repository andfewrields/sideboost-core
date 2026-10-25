// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bip300/messages.h>

#include <crypto/common.h>
#include <hash.h>
#include <util/overloaded.h>

#include <algorithm>

namespace bip300 {

namespace {

/** The single push following the OP_RETURN of `script`, if that's all it
 *  holds.  */
std::optional<std::vector<unsigned char>> OpReturnPayload(const CScript& script)
{
    CScript::const_iterator pc{script.begin()};
    opcodetype op;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, op) || op != OP_RETURN) return std::nullopt;
    if (!script.GetOp(pc, op, data) || op > OP_PUSHDATA4) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    return data;
}

bool HasTag(std::span<const unsigned char> data, const Tag& tag)
{
    return data.size() >= tag.size() && std::equal(tag.begin(), tag.end(), data.begin());
}

uint256 ReadHash(std::span<const unsigned char> data)
{
    return uint256{data.first(uint256::size())};
}

CScript OpReturn(const Tag& tag, std::initializer_list<std::span<const unsigned char>> parts)
{
    std::vector<unsigned char> data(tag.begin(), tag.end());
    for (const auto& part : parts) data.insert(data.end(), part.begin(), part.end());
    return CScript() << OP_RETURN << data;
}

std::optional<AckBundles> ParseAckBundles(std::span<const unsigned char> body)
{
    if (body.empty()) return std::nullopt;
    AckBundles res;
    const auto votes{body.subspan(1)};
    switch (body[0]) {
    case uint8_t(AckBundles::Version::REPEAT_PREVIOUS):
    case uint8_t(AckBundles::Version::LEADING_BY_50):
        if (!votes.empty()) return std::nullopt;
        res.version = AckBundles::Version{body[0]};
        return res;
    case uint8_t(AckBundles::Version::ONE_BYTE):
        res.version = AckBundles::Version::ONE_BYTE;
        for (const unsigned char vote : votes) {
            res.votes.push_back(vote == AckBundles::ABSTAIN_ONE_BYTE ? AckBundles::ABSTAIN :
                                vote == AckBundles::ALARM_ONE_BYTE   ? AckBundles::ALARM :
                                                                       vote);
        }
        return res;
    case uint8_t(AckBundles::Version::TWO_BYTES):
        if (votes.size() % 2) return std::nullopt;
        res.version = AckBundles::Version::TWO_BYTES;
        for (size_t i{0}; i < votes.size(); i += 2) res.votes.push_back(ReadLE16(votes.data() + i));
        return res;
    }
    return std::nullopt;
}

} // namespace

uint256 DescriptionHash(std::span<const unsigned char> description)
{
    return Hash(description);
}

std::optional<CoinbaseMessage> ParseCoinbaseMessage(const CScript& script)
{
    if (script.size() < 1 + 1 + Tag{}.size() || script[0] != OP_RETURN) return std::nullopt;
    const auto data{OpReturnPayload(script)};
    if (!data || data->size() < Tag{}.size()) return std::nullopt;
    const std::span<const unsigned char> body{std::span{*data}.subspan(Tag{}.size())};
    if (HasTag(*data, M1_TAG)) {
        if (body.empty()) return std::nullopt;
        return ProposeSidechain{body[0], {body.begin() + 1, body.end()}};
    }
    if (HasTag(*data, M2_TAG)) {
        if (body.size() != 1 + uint256::size()) return std::nullopt;
        return AckSidechain{body[0], ReadHash(body.subspan(1))};
    }
    if (HasTag(*data, M3_TAG)) {
        if (body.size() != 1 + uint256::size()) return std::nullopt;
        return ProposeBundle{body[0], ReadHash(body.subspan(1))};
    }
    if (HasTag(*data, M4_TAG)) {
        auto m4{ParseAckBundles(body)};
        if (!m4) return std::nullopt;
        return std::move(*m4);
    }
    if (HasTag(*data, M7_TAG)) {
        if (body.size() != 1 + uint256::size()) return std::nullopt;
        return BmmAccept{body[0], ReadHash(body.subspan(1))};
    }
    return std::nullopt;
}

AckBundles MakeAckBundles(std::vector<uint16_t> votes)
{
    const bool two_bytes{std::ranges::any_of(votes, [](uint16_t vote) {
        return vote != AckBundles::ABSTAIN && vote != AckBundles::ALARM && vote > AckBundles::MAX_ONE_BYTE_INDEX;
    })};
    return {.version = two_bytes ? AckBundles::Version::TWO_BYTES : AckBundles::Version::ONE_BYTE, .votes = std::move(votes)};
}

CScript MakeCoinbaseMessageScript(const CoinbaseMessage& message)
{
    return std::visit(util::Overloaded{
        [](const ProposeSidechain& m1) {
            const unsigned char slot{m1.slot};
            return OpReturn(M1_TAG, {std::span{&slot, 1}, m1.description});
        },
        [](const AckSidechain& m2) {
            const unsigned char slot{m2.slot};
            return OpReturn(M2_TAG, {std::span{&slot, 1}, m2.description_hash});
        },
        [](const ProposeBundle& m3) {
            const unsigned char slot{m3.slot};
            return OpReturn(M3_TAG, {std::span{&slot, 1}, m3.m6id});
        },
        [](const AckBundles& m4) {
            std::vector<unsigned char> body{uint8_t(m4.version)};
            for (const uint16_t vote : m4.votes) {
                if (m4.version == AckBundles::Version::TWO_BYTES) {
                    body.resize(body.size() + 2);
                    WriteLE16(body.data() + body.size() - 2, vote);
                } else if (m4.version == AckBundles::Version::ONE_BYTE) {
                    body.push_back(vote == AckBundles::ABSTAIN ? AckBundles::ABSTAIN_ONE_BYTE :
                                   vote == AckBundles::ALARM   ? AckBundles::ALARM_ONE_BYTE :
                                                                 uint8_t(vote));
                }
            }
            return OpReturn(M4_TAG, {body});
        },
        [](const BmmAccept& m7) {
            const unsigned char slot{m7.slot};
            return OpReturn(M7_TAG, {std::span{&slot, 1}, m7.hash});
        },
    }, message);
}

CScript MakeDepositScript(uint8_t slot, std::span<const unsigned char> address)
{
    const unsigned char slot_byte{slot};
    return OpReturn(M5_TAG, {std::span{&slot_byte, 1}, address});
}

bool IsDepositScript(const CScript& script)
{
    if (script.empty() || script[0] != OP_RETURN) return false;
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype op;
    std::vector<unsigned char> data;
    return script.GetOp(pc, op, data) && op <= OP_PUSHDATA4 && HasTag(data, M5_TAG);
}

std::optional<Deposit> ParseDeposit(const CTxOut& out)
{
    if (!IsDepositScript(out.scriptPubKey) || out.nValue < 1 || !MoneyRange(out.nValue)) return std::nullopt;
    const auto data{OpReturnPayload(out.scriptPubKey)};
    if (!data || data->size() < M5_TAG.size() + 1) return std::nullopt;
    return Deposit{.slot = (*data)[M5_TAG.size()], .address = {data->begin() + M5_TAG.size() + 1, data->end()}};
}

CScript MakeBmmRequestScript(const BmmRequest& request)
{
    std::vector<unsigned char> data(M8_TAG.begin(), M8_TAG.end());
    data.push_back(request.slot);
    data.insert(data.end(), request.hash.begin(), request.hash.end());
    data.insert(data.end(), request.prev_block.begin(), request.prev_block.end());
    return CScript() << OP_RETURN << data;
}

std::optional<BmmRequest> ParseBmmRequest(const CTransaction& tx)
{
    // Exactly OP_RETURN, a direct push of 68 bytes, and those
    static constexpr size_t SIZE{M8_TAG.size() + 1 + 2 * uint256::size()};
    if (tx.vout.empty()) return std::nullopt;
    const CScript& script{tx.vout[0].scriptPubKey};
    if (script.size() != 2 + SIZE || script[0] != OP_RETURN || script[1] != SIZE) return std::nullopt;
    if (!std::equal(M8_TAG.begin(), M8_TAG.end(), script.begin() + 2)) return std::nullopt;
    const std::span<const unsigned char> body{std::span{script}.subspan(2 + M8_TAG.size())};
    return BmmRequest{.slot = body[0], .hash = ReadHash(body.subspan(1)), .prev_block = ReadHash(body.subspan(1 + uint256::size()))};
}

uint256 ComputeM6Id(const CTransaction& tx)
{
    CMutableTransaction blinded{tx};
    blinded.vin.clear();
    return blinded.GetHash().ToUint256();
}

std::optional<CAmount> ParseM6Fee(const CTransaction& m6)
{
    if (m6.vout.empty() || m6.vout[0].nValue != 0) return std::nullopt;
    const CScript& script{m6.vout[0].scriptPubKey};
    if (script.size() != 10 || script[0] != OP_RETURN || script[1] != 8) return std::nullopt;
    const uint64_t fee{ReadBE64(&script[2])};
    if (fee > uint64_t(MAX_MONEY)) return std::nullopt;
    return CAmount(fee);
}

std::optional<CAmount> M6Value(const CTransaction& blinded)
{
    if (!blinded.vin.empty()) return std::nullopt;
    const auto fee{ParseM6Fee(blinded)};
    if (!fee) return std::nullopt;
    CAmount value{*fee};
    for (const CTxOut& out : blinded.vout) {
        if (!MoneyRange(out.nValue)) return std::nullopt;
        value += out.nValue;
        if (!MoneyRange(value)) return std::nullopt;
    }
    return value;
}

CTransactionRef MakePayout(const CTransaction& blinded, uint8_t slot)
{
    CMutableTransaction tx{blinded};
    tx.vin.assign(1, CTxIn{COutPoint{SidechainTreasuryTxid(), slot}});
    return MakeTransactionRef(std::move(tx));
}

} // namespace bip300
