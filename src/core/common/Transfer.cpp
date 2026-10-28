#include "core/common/Transfer.h"

#include <algorithm>
#include <charconv>

namespace lv::core::common {

std::vector<std::pair<int, int>> planInsert(std::span<SlotView const> slots, int amount) {
    std::vector<std::pair<int, int>> plan;
    if (amount <= 0) return plan;
    for (int pass = 0; pass < 2 && amount > 0; ++pass) {
        for (std::size_t i = 0; i < slots.size() && amount > 0; ++i) {
            auto const& s = slots[i];
            bool        use = pass == 0 ? (!s.empty && s.stackable) : s.empty;
            if (!use) continue;
            int room = std::max(0, s.maxCount - (s.empty ? 0 : s.count));
            int add  = std::min(room, amount);
            if (add <= 0) continue;
            plan.emplace_back(static_cast<int>(i), add);
            amount -= add;
        }
    }
    return plan;
}

int plannedTotal(std::vector<std::pair<int, int>> const& plan) {
    int n = 0;
    for (auto const& [slot, add] : plan) n += add;
    return n;
}

std::string blockDataChunkKey(int dim, int cx, int cz) {
    return "lv:common:bd:" + std::to_string(dim) + ":" + std::to_string(cx) + ":" + std::to_string(cz);
}

namespace {
bool parseInts(std::string_view s, char sep, int* out, int n) {
    for (int i = 0; i < n; ++i) {
        auto end = i + 1 < n ? s.find(sep) : s.size();
        if (end == std::string_view::npos) return false;
        auto part = s.substr(0, end);
        auto [p, ec] = std::from_chars(part.data(), part.data() + part.size(), out[i]);
        if (ec != std::errc{} || p != part.data() + part.size() || part.empty()) return false;
        s.remove_prefix(std::min(s.size(), end + 1));
    }
    return true;
}
} // namespace

bool parseChunkSuffix(std::string_view suffix, int& dim, int& cx, int& cz) {
    int v[3];
    if (!parseInts(suffix, ':', v, 3)) return false;
    dim = v[0], cx = v[1], cz = v[2];
    return true;
}

std::string posKey(int x, int y, int z) {
    return std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
}

bool parsePosKey(std::string_view key, int& x, int& y, int& z) {
    int v[3];
    if (!parseInts(key, ',', v, 3)) return false;
    x = v[0], y = v[1], z = v[2];
    return true;
}

} // namespace lv::core::common
