// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_ENFORCER_H
#define BITCOIN_DRIVECHAIN_ENFORCER_H

#include <drivechain/parentchain.h>

#include <chrono>
#include <cstdint>
#include <string>

class UniValue;

namespace drivechain {

/** The enforcer's default gRPC port.  */
static constexpr uint16_t DEFAULT_ENFORCER_PORT{50051};
/** How long to wait for an answer from the enforcer.  */
static constexpr std::chrono::milliseconds ENFORCER_TIMEOUT{std::chrono::seconds{10}};
/** How often to retry connecting blocks whose deposits couldn't be checked.  */
static constexpr std::chrono::milliseconds DEPOSIT_RETRY_INTERVAL{std::chrono::seconds{5}};

/** A DepositSource that asks the parent chain's bip300301_enforcer, through
 *  the JSON form of its Connect (gRPC) API over plain HTTP.  */
class EnforcerClient final : public DepositSource
{
public:
    /** `host:port` of the enforcer's gRPC server.  */
    EnforcerClient(std::string host, uint16_t port, std::chrono::milliseconds timeout);

    ParentHeader GetTip() override;
    std::vector<ParentHeader> GetHeaders(const uint256& hash, uint32_t max_ancestors) override;
    ParentEvents GetEvents(uint8_t slot, const std::optional<uint256>& start, const uint256& end) override;
    void ProposeWithdrawalBundle(uint8_t slot, const CTransaction& blinded_m6) override;

private:
    std::string m_host;
    uint16_t m_port;
    std::chrono::milliseconds m_timeout;

    /** Call `method` (e.g. "ValidatorService/GetChainTip").  Throws
     *  DepositSourceError.  */
    UniValue Call(const std::string& method, const UniValue& request) const;
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_ENFORCER_H
