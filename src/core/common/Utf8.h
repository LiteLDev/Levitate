#pragma once

// UTF-8 helpers for text typed by players (addresses, names, searches).

#include <cstddef>
#include <string>

namespace lv::core {

/// The first `chars` UTF-8 characters (code points) of `s`. Never splits a character: a cut one
/// would make the JSON sent back to a screen invalid. A truncated sequence at the end is dropped;
/// a stray byte counts as one character.
[[nodiscard]] std::string utf8Prefix(std::string const& s, std::size_t chars);

} // namespace lv::core
