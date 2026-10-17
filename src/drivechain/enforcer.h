// Copyright (c) 2026 The SideBoost developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_ENFORCER_H
#define BITCOIN_DRIVECHAIN_ENFORCER_H

#include <drivechain/deposit.h>

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

    BlockDeposits GetBlockDeposits(uint8_t slot, const uint256& block_hash, int min_depth) override;
    std::vector<Deposit> GetNewDeposits(uint8_t slot, uint64_t first_sequence, const uint256& hint, int min_depth) override;

private:
    struct Header {
        uint256 hash;
        uint256 prev;
        int height;
    };

    std::string m_host;
    uint16_t m_port;
    std::chrono::milliseconds m_timeout;

    /** Call `method` (e.g. "ValidatorService/GetChainTip").  Throws
     *  DepositSourceError.  */
    UniValue Call(const std::string& method, const UniValue& request) const;

    Header GetTip() const;
    /** The header of `hash` and up to `max_ancestors` of its ancestors,
     *  newest first.  Empty if the enforcer doesn't know `hash`.  */
    std::vector<Header> GetHeaders(const uint256& hash, uint32_t max_ancestors) const;
    /** Whether `header` is on the best chain ending in `tip`.  */
    bool IsInBestChain(const Header& header, const Header& tip) const;
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_ENFORCER_H
