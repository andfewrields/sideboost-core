// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/enforcer.h>

#include <compat/compat.h>
#include <netaddress.h>
#include <netbase.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/sock.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <thread>

namespace drivechain {

namespace {

/** The enforcer answered with an error (as opposed to not answering).  */
struct EnforcerReplyError : DepositSourceError {
    using DepositSourceError::DepositSourceError;
};

const UniValue& Field(const UniValue& obj, std::string_view key)
{
    if (!obj.isObject()) throw DepositSourceError("enforcer reply: expected an object");
    return obj.find_value(key);
}

uint256 HashField(const UniValue& obj, std::string_view key)
{
    const UniValue& hex{Field(Field(obj, key), "hex")};
    const auto hash{hex.isStr() ? uint256::FromHex(hex.get_str()) : std::nullopt};
    if (!hash) throw DepositSourceError(strprintf("enforcer reply: bad %s", key));
    return *hash;
}

/** A protobuf 64-bit integer, which JSON carries as a string.  Absent fields
 *  are 0.  */
uint64_t U64Field(const UniValue& obj, std::string_view key)
{
    const UniValue& val{Field(obj, key)};
    if (val.isNull()) return 0;
    const auto res{ToIntegral<uint64_t>(val.isStr() ? val.get_str() : val.getValStr())};
    if (!res) throw DepositSourceError(strprintf("enforcer reply: bad %s", key));
    return *res;
}

UniValue HashArg(const uint256& hash)
{
    UniValue res{UniValue::VOBJ};
    res.pushKV("hex", hash.GetHex());
    return res;
}

std::string Dechunk(std::string_view body)
{
    std::string res;
    while (true) {
        const size_t eol{body.find("\r\n")};
        if (eol == std::string_view::npos) throw DepositSourceError("enforcer reply: bad chunked encoding");
        const auto size{ToIntegral<size_t>(util::TrimStringView(body.substr(0, body.find_first_of(";\r"))), 16)};
        if (!size || body.size() < eol + 2 + *size) throw DepositSourceError("enforcer reply: bad chunked encoding");
        if (*size == 0) return res;
        res.append(body.substr(eol + 2, *size));
        body.remove_prefix(eol + 2 + *size);
        if (body.starts_with("\r\n")) body.remove_prefix(2);
    }
}

std::vector<Deposit> ParseDeposits(const UniValue& block_info, const uint256& block_hash)
{
    std::vector<Deposit> res;
    const UniValue& events{Field(block_info, "events")};
    if (events.isNull()) return res;
    if (!events.isArray()) throw DepositSourceError("enforcer reply: bad events");
    for (const UniValue& event : events.getValues()) {
        const UniValue& deposit{Field(event, "deposit")};
        if (deposit.isNull()) continue;
        const UniValue& outpoint{Field(deposit, "outpoint")};
        const UniValue& output{Field(deposit, "output")};
        const auto address{TryParseHex<unsigned char>(Field(Field(output, "address"), "hex").getValStr())};
        const uint64_t value{U64Field(output, "valueSats")};
        if (!address || value > uint64_t(MAX_MONEY)) throw DepositSourceError("enforcer reply: bad deposit");
        res.push_back(Deposit{
            .sequence = U64Field(deposit, "sequenceNumber"),
            .block_hash = block_hash,
            .outpoint = COutPoint{Txid::FromUint256(HashField(outpoint, "txid")), static_cast<uint32_t>(U64Field(outpoint, "vout"))},
            .address = std::move(*address),
            .value = static_cast<CAmount>(value),
        });
    }
    return res;
}

} // namespace

EnforcerClient::EnforcerClient(std::string host, uint16_t port, std::chrono::milliseconds timeout)
    : m_host{std::move(host)}, m_port{port}, m_timeout{timeout} {}

UniValue EnforcerClient::Call(const std::string& method, const UniValue& request) const
{
    const auto deadline{std::chrono::steady_clock::now() + m_timeout};
    const auto time_left{[&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    }};
    const std::vector<CService> services{Lookup(m_host, m_port, /*fAllowLookup=*/true, /*nMaxSolutions=*/1)};
    if (services.empty()) throw DepositSourceError(strprintf("cannot resolve enforcer host %s", m_host));
    const std::unique_ptr<Sock> sock{ConnectDirectly(services[0], /*manual_connection=*/true, m_timeout)};
    if (!sock) throw DepositSourceError(strprintf("cannot connect to the enforcer at %s:%d", m_host, m_port));

    const std::string body{request.write()};
    std::string out{strprintf("POST /cusf.mainchain.v1.%s HTTP/1.1\r\n"
                              "Host: %s:%d\r\n"
                              "Connection: close\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: %d\r\n\r\n%s",
                              method, m_host, m_port, body.size(), body)};
    std::string_view to_send{out};
    while (!to_send.empty()) {
        Sock::Event event{0};
        if (time_left().count() <= 0 || !sock->Wait(time_left(), Sock::SendEvent, &event)) throw DepositSourceError("enforcer: send timed out");
        if (!(event & Sock::SendEvent)) continue;
        const ssize_t sent{sock->Send(to_send.data(), to_send.size(), MSG_NOSIGNAL)};
        if (sent < 0) {
            if (IOErrorIsPermanent(WSAGetLastError())) throw DepositSourceError("enforcer: send failed");
            std::this_thread::yield();
            continue;
        }
        to_send.remove_prefix(sent);
    }

    std::string in;
    while (true) {
        Sock::Event event{0};
        if (time_left().count() <= 0 || !sock->Wait(time_left(), Sock::RecvEvent, &event)) throw DepositSourceError("enforcer: reply timed out");
        if (!(event & Sock::RecvEvent)) continue;
        char buf[4096];
        const ssize_t got{sock->Recv(buf, sizeof(buf), 0)};
        if (got == 0) break;
        if (got < 0) {
            if (IOErrorIsPermanent(WSAGetLastError())) throw DepositSourceError("enforcer: receive failed");
            std::this_thread::yield();
            continue;
        }
        in.append(buf, got);
    }

    const size_t header_end{in.find("\r\n\r\n")};
    if (header_end == std::string::npos) throw DepositSourceError("enforcer reply: no HTTP header");
    const std::string_view header{std::string_view{in}.substr(0, header_end)};
    std::string reply_body{in.substr(header_end + 4)};
    const std::vector<std::string> status{util::SplitString(header.substr(0, header.find("\r\n")), ' ')};
    if (status.size() < 2) throw DepositSourceError("enforcer reply: bad status line");
    if (ToLower(header).find("transfer-encoding: chunked") != std::string::npos) reply_body = Dechunk(reply_body);

    UniValue reply;
    if (!reply.read(reply_body)) throw DepositSourceError(strprintf("enforcer reply to %s is not JSON", method));
    if (status[1] != "200") {
        throw EnforcerReplyError(strprintf("enforcer: %s failed (HTTP %s): %s", method, status[1], Field(reply, "message").getValStr()));
    }
    return reply;
}

EnforcerClient::Header EnforcerClient::GetTip() const
{
    const UniValue reply{Call("ValidatorService/GetChainTip", UniValue{UniValue::VOBJ})};
    const UniValue& info{Field(reply, "blockHeaderInfo")};
    return {HashField(info, "blockHash"), HashField(info, "prevBlockHash"), static_cast<int>(U64Field(info, "height"))};
}

std::vector<EnforcerClient::Header> EnforcerClient::GetHeaders(const uint256& hash, uint32_t max_ancestors) const
{
    UniValue request{UniValue::VOBJ};
    request.pushKV("blockHash", HashArg(hash));
    request.pushKV("maxAncestors", max_ancestors);
    UniValue reply;
    try {
        reply = Call("ValidatorService/GetBlockHeaderInfo", request);
    } catch (const EnforcerReplyError&) {
        return {}; // unknown block
    }
    std::vector<Header> res;
    const UniValue& infos{Field(reply, "headerInfos")};
    if (infos.isNull()) return res;
    if (!infos.isArray()) throw DepositSourceError("enforcer reply: bad headerInfos");
    for (const UniValue& info : infos.getValues()) {
        res.push_back({HashField(info, "blockHash"), HashField(info, "prevBlockHash"), static_cast<int>(U64Field(info, "height"))});
    }
    return res;
}

bool EnforcerClient::IsInBestChain(const Header& header, const Header& tip) const
{
    if (header.height > tip.height) return false;
    Header cur{tip};
    while (cur.height > header.height) {
        const auto ancestors{GetHeaders(cur.hash, std::min(cur.height - header.height, 1000))};
        if (ancestors.size() < 2 || ancestors.back().height >= cur.height) throw DepositSourceError("enforcer: cannot walk the best chain");
        cur = ancestors.back();
    }
    return cur.hash == header.hash;
}

BlockDeposits EnforcerClient::GetBlockDeposits(uint8_t slot, const uint256& block_hash, int min_depth)
{
    using enum BlockDeposits::Status;
    const auto headers{GetHeaders(block_hash, 0)};
    if (headers.empty()) return {UNKNOWN, {}};
    const Header tip{GetTip()};
    if (!IsInBestChain(headers[0], tip)) return {NOT_IN_BEST_CHAIN, {}};
    if (tip.height - headers[0].height < min_depth) return {TOO_SHALLOW, {}};

    UniValue request{UniValue::VOBJ};
    request.pushKV("blockHash", HashArg(block_hash));
    request.pushKV("sidechainId", slot);
    const UniValue reply{Call("ValidatorService/GetBlockInfo", request)};
    const UniValue& infos{Field(reply, "infos")};
    if (!infos.isArray() || infos.empty()) throw DepositSourceError("enforcer: no info for a known block");
    return {OK, ParseDeposits(Field(infos[0], "blockInfo"), block_hash)};
}

std::vector<Deposit> EnforcerClient::GetNewDeposits(uint8_t slot, uint64_t first_sequence, const uint256& hint, int min_depth)
{
    Header end{GetTip()};
    if (end.height < min_depth) return {};
    if (min_depth > 0) {
        const auto ancestors{GetHeaders(end.hash, min_depth)};
        if (ancestors.empty() || ancestors.back().height != end.height - min_depth) throw DepositSourceError("enforcer: cannot walk the best chain");
        end = ancestors.back();
    }

    // Start from the parent of the hint, so later deposits in the hint's own
    // block are found too.
    std::optional<uint256> start;
    if (!hint.IsNull()) {
        if (const auto headers{GetHeaders(hint, 0)}; !headers.empty()) start = headers[0].prev;
    }

    UniValue reply;
    while (true) {
        UniValue request{UniValue::VOBJ};
        request.pushKV("sidechainId", slot);
        if (start) request.pushKV("startBlockHash", HashArg(*start));
        request.pushKV("endBlockHash", HashArg(end.hash));
        try {
            reply = Call("ValidatorService/GetTwoWayPegData", request);
            break;
        } catch (const EnforcerReplyError&) {
            // The hint left the best chain: search it all.
            if (!start) throw;
            start.reset();
        }
    }

    std::vector<Deposit> deposits;
    const UniValue& blocks{Field(reply, "blocks")};
    if (blocks.isArray()) {
        for (const UniValue& block : blocks.getValues()) {
            for (Deposit& deposit : ParseDeposits(Field(block, "blockInfo"), HashField(Field(block, "blockHeaderInfo"), "blockHash"))) {
                if (deposit.sequence >= first_sequence) deposits.push_back(std::move(deposit));
            }
        }
    }
    std::ranges::sort(deposits, {}, &Deposit::sequence);
    size_t contiguous{0};
    while (contiguous < deposits.size() && deposits[contiguous].sequence == first_sequence + contiguous) ++contiguous;
    deposits.resize(contiguous);
    return deposits;
}

} // namespace drivechain
