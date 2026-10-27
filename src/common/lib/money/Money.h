#pragma once
#include <cstdint>
#include <map>
#include <string_view>
#include <utility>

#include "common/api/protocol/Protocol.h"

namespace coop::money {

// Strict decimal input: no signs, exponents, suffixes or arithmetic overflow.
inline bool ParsePositive(std::string_view text, int32_t limit, int32_t& out, bool allowZero = false) {
    if (text.empty()) return false;
    int32_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9' || value > (limit - (c - '0')) / 10) return false;
        value = value * 10 + c - '0';
        if (value > limit) return false;
    }
    if (value == 0 && !allowZero) return false;
    out = value;
    return true;
}

// Kept for the lifetime of the game, including reconnects. A lost ACK must never
// cause a second debit/credit. BUSY is transient and is deliberately not cached.
class Receipts {
public:
    uint8_t Apply(uint64_t id, uint8_t action, int32_t amount, bool ready, int32_t& balance) {
        auto key = std::make_pair(id, action);
        auto it = m_results.find(key);
        if (it != m_results.end()) return it->second;
        if (!id || action > proto::MONEY_REFUND || amount <= 0) return proto::MONEY_REJECTED;
        if (!ready) {
            if (action == proto::MONEY_REFUND) return proto::MONEY_BUSY;
            m_results[key] = proto::MONEY_UNAVAILABLE;
            return proto::MONEY_UNAVAILABLE;
        }
        int64_t next = static_cast<int64_t>(balance) + (action == proto::MONEY_DEBIT ? -int64_t(amount) : amount);
        // A refund waits if the sender has since reached the game's money cap.
        if (action == proto::MONEY_REFUND && next > INT32_MAX) return proto::MONEY_BUSY;
        bool ok = action == proto::MONEY_DEBIT ? balance >= amount : next <= INT32_MAX;
        uint8_t result = ok ? proto::MONEY_OK : proto::MONEY_REJECTED;
        if (ok) balance = static_cast<int32_t>(next);
        m_results[key] = result;
        return result;
    }

private:
    std::map<std::pair<uint64_t, uint8_t>, uint8_t> m_results;
};

}  // namespace coop::money
