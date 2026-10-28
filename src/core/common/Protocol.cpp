#include "core/common/Protocol.h"

namespace lv::core::common {

std::string encodePayload(nlohmann::json const& payload) {
    if (payload.is_null()) return "{}";
    return payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::optional<nlohmann::json> decodePayload(std::string_view text) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

bool validChannel(std::string_view channel) {
    if (channel.empty() || channel.size() > 64) return false;
    for (char c : channel) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
        if (!ok) return false;
    }
    return channel.front() != '.' && channel.back() != '.';
}

} // namespace lv::core::common
