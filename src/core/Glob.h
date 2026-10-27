#pragma once

#include <string_view>

namespace lv::core {

/// Shell-style match: `*` = any run of characters, `?` = one character, everything else literal.
[[nodiscard]] bool globMatch(std::string_view pattern, std::string_view text);

} // namespace lv::core
