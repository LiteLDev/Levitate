#include "core/common/Utf8.h"

namespace lv::core {

std::string utf8Prefix(std::string const& s, std::size_t chars) {
    std::size_t i = 0, n = 0;
    while (i < s.size() && n < chars) {
        auto        c   = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        if (i + len > s.size()) break;
        i += len;
        ++n;
    }
    return s.substr(0, i);
}

} // namespace lv::core
