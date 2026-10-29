#include "core/packages/Address.h"

#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lv::core::packages {

namespace {

enum class Kind { Literal, AnyChar, AnyRun, Class, Group, NotAhead, NotBehind };

struct Element;
using Sequence = std::vector<Element>;

struct Element {
    Kind                                  kind = Kind::Literal;
    char                                  ch   = 0;
    bool                                  negated = false;                ///< Class: [!...]
    std::vector<std::pair<char, char>>    ranges;                         ///< Class
    std::vector<Sequence>                 alternatives;                   ///< Group / NotAhead / NotBehind
};

bool classContains(Element const& e, char c) {
    bool in = false;
    for (auto const& [lo, hi] : e.ranges) {
        if (c >= lo && c <= hi) {
            in = true;
            break;
        }
    }
    return in != e.negated;
}

using Cont = std::function<bool(std::size_t)>;

/// Work limit of one match: player-typed globs like `*a*a*a*b` must not stall the server tick.
constexpr long kStepBudget = 200000;
thread_local long tSteps   = 0;

bool matchSeq(Sequence const& seq, std::size_t i, std::string_view text, std::size_t pos, Cont const& cont) {
    if (++tSteps > kStepBudget) return false;
    if (i == seq.size()) return cont(pos);
    auto const& e = seq[i];
    switch (e.kind) {
    case Kind::Literal:
        return pos < text.size() && text[pos] == e.ch && matchSeq(seq, i + 1, text, pos + 1, cont);
    case Kind::AnyChar:
        return pos < text.size() && matchSeq(seq, i + 1, text, pos + 1, cont);
    case Kind::Class:
        return pos < text.size() && classContains(e, text[pos]) && matchSeq(seq, i + 1, text, pos + 1, cont);
    case Kind::AnyRun:
        for (std::size_t k = pos; k <= text.size(); ++k)
            if (matchSeq(seq, i + 1, text, k, cont)) return true;
        return false;
    case Kind::Group:
        for (auto const& alt : e.alternatives) {
            if (matchSeq(alt, 0, text, pos, [&](std::size_t p) { return matchSeq(seq, i + 1, text, p, cont); })) return true;
        }
        return false;
    case Kind::NotAhead:
        for (auto const& alt : e.alternatives)
            if (matchSeq(alt, 0, text, pos, [](std::size_t) { return true; })) return false;
        return matchSeq(seq, i + 1, text, pos, cont);
    case Kind::NotBehind:
        for (auto const& alt : e.alternatives)
            for (std::size_t j = 0; j <= pos; ++j)
                if (matchSeq(alt, 0, text, j, [pos](std::size_t p) { return p == pos; })) return false;
        return matchSeq(seq, i + 1, text, pos, cont);
    }
    return false;
}

/// Parses `pattern`; false on a syntax error (Create then falls back to the regex "").
bool parse(std::string_view p, Sequence& out) {
    std::size_t i        = 0;
    bool        anchored = true;
    bool        inGroup  = false;
    Element     group;
    Sequence    current;
    auto        emit = [&](Element e) { (inGroup ? current : out).push_back(std::move(e)); };

    while (i < p.size()) {
        char c = p[i++];
        switch (c) {
        case '\\': {
            if (i >= p.size()) return false;
            emit({Kind::Literal, p[i++]});
            break;
        }
        case '[': {
            Element e{Kind::Class};
            if (i < p.size() && p[i] == '^') {
                e.ranges.push_back({'^', '^'});
                ++i;
            } else {
                if (i < p.size() && p[i] == '!') {
                    e.negated = true;
                    ++i;
                }
                if (i < p.size() && p[i] == '-') {
                    e.ranges.push_back({'-', '-'});
                    ++i;
                }
            }
            bool hasRangeStart = false; // as the JDK globs: the leading '^' / '-' never start a range
            char last          = 0;
            bool closed        = false;
            while (i < p.size()) {
                char d = p[i++];
                if (d == ']') {
                    closed = true;
                    break;
                }
                if (d == '\\' && i < p.size() && (p[i] == ']' || p[i] == '-' || p[i] == '\\')) {
                    d = p[i++];
                    e.ranges.push_back({d, d});
                    hasRangeStart = true;
                    last          = d;
                    continue;
                }
                if (d == '-') {
                    if (!hasRangeStart) return false;
                    if (i >= p.size()) return false;
                    char hi = p[i++];
                    if (hi == ']') {
                        e.ranges.push_back({'-', '-'});
                        closed = true;
                        break;
                    }
                    if (hi < last) return false;
                    e.ranges.back() = {last, hi};
                    hasRangeStart   = false;
                    continue;
                }
                e.ranges.push_back({d, d});
                hasRangeStart = true;
                last          = d;
            }
            if (!closed || e.ranges.empty()) return false;
            emit(std::move(e));
            break;
        }
        case '{': {
            if (inGroup) return false;
            inGroup = true;
            group   = Element{Kind::Group};
            if (i < p.size() && p[i] == '!') {
                group.kind = anchored ? Kind::NotAhead : Kind::NotBehind;
                ++i;
            }
            current.clear();
            break;
        }
        case '}': {
            if (!inGroup) {
                emit({Kind::Literal, '}'});
                break;
            }
            group.alternatives.push_back(std::move(current));
            current.clear();
            inGroup = false;
            if (group.kind == Kind::NotBehind) {
                // Java needs a bounded look-behind
                for (auto const& alt : group.alternatives)
                    for (auto const& e : alt)
                        if (e.kind == Kind::AnyRun) return false;
            }
            bool appendRun = group.kind == Kind::NotAhead && anchored;
            out.push_back(std::move(group));
            if (appendRun) {
                out.push_back({Kind::AnyRun});
                anchored = false;
            }
            break;
        }
        case ',': {
            if (inGroup) {
                group.alternatives.push_back(std::move(current));
                current.clear();
            } else {
                emit({Kind::Literal, ','});
            }
            break;
        }
        case '*':
            emit({Kind::AnyRun});
            if (!inGroup) anchored = false;
            break;
        case '?':
            emit({Kind::AnyChar});
            if (!inGroup) anchored = true;
            break;
        default: emit({Kind::Literal, c}); break;
        }
    }
    return !inGroup;
}

} // namespace

struct Glob::Impl {
    // (see Address.h)
    Sequence seq;
    bool     invalid = false;
};

Glob::Glob(std::string_view pattern) {
    auto impl = std::make_shared<Impl>();
    if (pattern.size() > kMaxAddressLength || !parse(pattern, impl->seq)) {
        impl->seq.clear();
        impl->invalid = true;
    }
    mImpl = std::move(impl);
}

bool Glob::matches(std::string_view text) const {
    if (mImpl->invalid) return text.empty();
    if (text.size() > kMaxAddressLength) return false;
    tSteps = 0;
    return matchSeq(mImpl->seq, 0, text, 0, [&](std::size_t p) { return p == text.size(); });
}

bool Glob::invalid() const { return mImpl->invalid; }

bool isBlank(std::string_view s) {
    for (char c : s)
        if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')) return false;
    return true;
}

bool matchAddress(std::string_view boxAddress, std::string_view address) {
    if (isBlank(address)) return isBlank(boxAddress);
    if (address == "*" || boxAddress == "*") return true;
    if (address == boxAddress) return true;
    // compiled globs are cached: ports and routing tables match the same filters every tick
    thread_local std::unordered_map<std::string, Glob> cache;
    auto compiled = [](std::string_view p) -> Glob const& {
        auto it = cache.find(std::string(p));
        if (it != cache.end()) return it->second;
        if (cache.size() > 512) cache.clear();
        return cache.emplace(std::string(p), Glob(p)).first->second;
    };
    return compiled(boxAddress).matches(address) || compiled(address).matches(boxAddress);
}

} // namespace lv::core::packages
