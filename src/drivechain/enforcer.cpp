// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/enforcer.h>

#include <compat/compat.h>
#include <netaddress.h>
#include <netbase.h>
#include <streams.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/sock.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <thread>

namespace drivechain {

namespace {

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

ParentHeader ParseHeader(const UniValue& info)
{
    return {HashField(info, "blockHash"), HashField(info, "prevBlockHash"), static_cast<int>(U64Field(info, "height"))};
}

/** The events in a block: deposits and withdrawals (treasury events), and
 *  withdrawal bundle events.  */
void ParseEvents(const UniValue& block_info, const ParentHeader& header, ParentEvents& res)
{
    const UniValue& events{Field(block_info, "events")};
    if (events.isNull()) return;
    if (!events.isArray()) throw DepositSourceError("enforcer reply: bad events");
    for (const UniValue& event : events.getValues()) {
        if (const UniValue& deposit{Field(event, "deposit")}; !deposit.isNull()) {
            const UniValue& outpoint{Field(deposit, "outpoint")};
            const UniValue& output{Field(deposit, "output")};
            const auto address{TryParseHex<unsigned char>(Field(Field(output, "address"), "hex").getValStr())};
            const uint64_t value{U64Field(output, "valueSats")};
            if (!address || value > uint64_t(MAX_MONEY)) throw DepositSourceError("enforcer reply: bad deposit");
            const uint64_t sequence{U64Field(deposit, "sequenceNumber")};
            res.treasury.push_back({sequence, header.hash, header.height, Deposit{
                .sequence = sequence,
                .outpoint = COutPoint{Txid::FromUint256(HashField(outpoint, "txid")), static_cast<uint32_t>(U64Field(outpoint, "vout"))},
                .address = std::move(*address),
                .value = static_cast<CAmount>(value),
            }});
        } else if (const UniValue& withdrawal{Field(event, "withdrawalBundle")}; !withdrawal.isNull()) {
            // The M6ID in consensus (txid byte) order
            const auto m6id{TryParseHex<unsigned char>(Field(Field(withdrawal, "m6id"), "hex").getValStr())};
            if (!m6id || m6id->size() != uint256::size()) throw DepositSourceError("enforcer reply: bad m6id");
            BundleEvent bundle_event{.m6id = uint256{*m6id}, .block_hash = header.hash, .height = header.height};
            const UniValue& kind{Field(withdrawal, "event")};
            if (const UniValue& succeeded{Field(kind, "succeeded")}; !succeeded.isNull()) {
                // A treasury event too
                res.treasury.push_back({U64Field(succeeded, "sequenceNumber"), header.hash, header.height, std::nullopt});
                bundle_event.kind = BundleEvent::Kind::SUCCEEDED;
            } else if (!Field(kind, "failed").isNull()) {
                bundle_event.kind = BundleEvent::Kind::FAILED;
            } else if (!Field(kind, "submitted").isNull()) {
                bundle_event.kind = BundleEvent::Kind::SUBMITTED;
            } else {
                throw DepositSourceError("enforcer reply: bad withdrawal bundle event");
            }
            res.bundles.push_back(bundle_event);
        }
    }
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
        throw DepositSourceError(strprintf("enforcer: %s failed (HTTP %s): %s", method, status[1], Field(reply, "message").getValStr()));
    }
    return reply;
}

ParentHeader EnforcerClient::GetTip()
{
    const UniValue reply{Call("ValidatorService/GetChainTip", UniValue{UniValue::VOBJ})};
    return ParseHeader(Field(reply, "blockHeaderInfo"));
}

std::vector<ParentHeader> EnforcerClient::GetHeaders(const uint256& hash, uint32_t max_ancestors)
{
    UniValue request{UniValue::VOBJ};
    request.pushKV("blockHash", HashArg(hash));
    request.pushKV("maxAncestors", max_ancestors);
    const UniValue reply{Call("ValidatorService/GetBlockHeaderInfo", request)};
    std::vector<ParentHeader> res;
    const UniValue& infos{Field(reply, "headerInfos")};
    if (infos.isNull()) return res; // unknown block
    if (!infos.isArray()) throw DepositSourceError("enforcer reply: bad headerInfos");
    for (const UniValue& info : infos.getValues()) res.push_back(ParseHeader(info));
    return res;
}

ParentEvents EnforcerClient::GetEvents(uint8_t slot, const std::optional<uint256>& start, const uint256& end)
{
    UniValue request{UniValue::VOBJ};
    request.pushKV("sidechainId", slot);
    if (start) request.pushKV("startBlockHash", HashArg(*start));
    request.pushKV("endBlockHash", HashArg(end));
    const UniValue reply{Call("ValidatorService/GetTwoWayPegData", request)};
    ParentEvents res;
    const UniValue& blocks{Field(reply, "blocks")};
    if (blocks.isNull()) return res;
    if (!blocks.isArray()) throw DepositSourceError("enforcer reply: bad blocks");
    for (const UniValue& block : blocks.getValues()) {
        ParseEvents(Field(block, "blockInfo"), ParseHeader(Field(block, "blockHeaderInfo")), res);
    }
    return res;
}

void EnforcerClient::ProposeWithdrawalBundle(uint8_t slot, const CTransaction& blinded_m6)
{
    // A zero-input transaction, which the enforcer reads in Core's legacy
    // serialization (there is no witness)
    DataStream tx_bytes;
    tx_bytes << TX_NO_WITNESS(blinded_m6);
    UniValue request{UniValue::VOBJ};
    request.pushKV("sidechainId", slot);
    request.pushKV("transaction", EncodeBase64(tx_bytes));
    Call("BlockProducerService/ProposeWithdrawalBundle", request);
}

} // namespace drivechain
