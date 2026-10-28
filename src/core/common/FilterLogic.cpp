#include "core/common/FilterLogic.h"

namespace lv::core::common {

std::string_view attributeModeName(AttributeMode mode) {
    switch (mode) {
    case AttributeMode::WhitelistAll:
        return "whitelist_conj";
    case AttributeMode::Blacklist:
        return "blacklist";
    default:
        return "whitelist_disj";
    }
}

AttributeMode attributeModeFromName(std::string_view name, AttributeMode fallback) {
    if (name == "whitelist_disj") return AttributeMode::WhitelistAny;
    if (name == "whitelist_conj") return AttributeMode::WhitelistAll;
    if (name == "blacklist") return AttributeMode::Blacklist;
    return fallback;
}

bool combineAttributeTests(std::span<AttributeTest const> tests, AttributeMode mode) {
    for (auto const& t : tests) {
        bool matches = t.applies != t.inverted;
        if (matches) {
            if (mode == AttributeMode::Blacklist) return false;
            if (mode == AttributeMode::WhitelistAny) return true;
        } else if (mode == AttributeMode::WhitelistAll) {
            return false;
        }
    }
    // no early exit: deny-list and allow-all pass, allow-any fails
    return mode != AttributeMode::WhitelistAny;
}

bool combineListFilter(std::size_t entries, std::function<bool(std::size_t)> const& entryMatches, bool blacklist) {
    for (std::size_t i = 0; i < entries; ++i) {
        if (entryMatches(i)) return !blacklist;
    }
    return blacklist;
}

} // namespace lv::core::common
