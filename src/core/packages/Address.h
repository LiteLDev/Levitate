#pragma once

// Package addresses (U25, SPEC §3.6.1): Create's `PackageItem.matchAddress` and the glob dialect
// it uses (catnip's `Glob.toRegexPattern`, a variant of the JDK's file-system globs).
//
// Glob syntax: `*` any run of characters, `?` one character, `[abc]` / `[a-z]` / `[!a-z]` a
// character class, `{a,b}` alternatives, `{!a,b}` "not followed by a or b" (at the start of the
// pattern or after a literal: anything that does not start with a / b), `\x` a literal x.
// Everything else is literal and case sensitive. A malformed glob (`[]`, a missing `]`, a
// nested or unclosed `{`, a trailing `\`) matches only the empty string, as Create's
// `toRegexPattern(glob, "")` fallback.

#include <memory>
#include <string>
#include <string_view>

namespace lv::core::packages {

/// Longer addresses / patterns never glob-match (they still match themselves exactly).
inline constexpr std::size_t kMaxAddressLength = 256;

/// A compiled glob (cheap to copy; immutable).
class Glob {
public:
    explicit Glob(std::string_view pattern);
    /// The whole `text` matches the pattern.
    [[nodiscard]] bool matches(std::string_view text) const;
    /// The pattern could not be parsed (matches only "").
    [[nodiscard]] bool invalid() const;

    struct Impl;

private:
    std::shared_ptr<Impl const> mImpl;
};

/// Create's String.isBlank(): empty or only whitespace.
[[nodiscard]] bool isBlank(std::string_view s);

/// Create's `PackageItem.matchAddress(boxAddress, address)`:
/// - a blank `address` (a port / filter) only matches blank box addresses;
/// - "*" on either side matches everything;
/// - equal strings match;
/// - otherwise either string, used as a glob, must match the other one.
[[nodiscard]] bool matchAddress(std::string_view boxAddress, std::string_view address);

} // namespace lv::core::packages
