#include "core/goggles/Tooltip.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace lv::core::goggles {

namespace {

/// Decodes one UTF-8 code point at `i` (advances `i`); invalid bytes decode as themselves.
std::uint32_t nextCodePoint(std::string_view s, std::size_t& i) {
    auto c = static_cast<unsigned char>(s[i]);
    int  n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (i + static_cast<std::size_t>(n) > s.size()) n = 1;
    std::uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3Fu);
    i += static_cast<std::size_t>(n);
    return cp;
}

bool isWide(std::uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3)
        || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60)
        || (cp >= 0xFFE0 && cp <= 0xFFE6);
}

int glyphWidth(std::uint32_t cp) {
    if (isWide(cp)) return 9;
    switch (cp) {
    case ' ':
    case 'I':
    case 't':
    case '[':
    case ']':
    case '"':
    case '(':
    case ')':
    case '{':
    case '}':
    case '*':
        return 4;
    case '!':
    case '.':
    case ',':
    case ':':
    case ';':
    case '|':
    case 'i':
    case '\'':
        return 2;
    case 'l':
    case '`':
        return 3;
    case 'f':
    case 'k':
    case '<':
    case '>':
        return 5;
    case '@':
    case '~':
        return 7;
    default:
        return 6;
    }
}

constexpr std::string_view kSection = "§";

} // namespace

std::string formatNumber(double value) {
    if (std::fabs(value) < 1e-5) value = 0; // Mth.equal(d, 0)
    bool        negative = value < 0;
    long long   hundredths = std::llround(std::fabs(value) * 100.0);
    long long   whole      = hundredths / 100;
    int         frac       = static_cast<int>(hundredths % 100);
    std::string digits     = std::to_string(whole);
    std::string grouped;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) grouped += ',';
        grouped += digits[i];
    }
    if (frac != 0) {
        char buf[4];
        std::snprintf(buf, sizeof buf, "%02d", frac);
        std::string f = buf;
        if (f.back() == '0') f.pop_back();
        grouped += "." + f;
    }
    if (negative && (whole != 0 || frac != 0)) grouped.insert(0, "-");
    return grouped;
}

std::string progressBar(int length, int filled) {
    std::string bar = " ";
    for (int i = 0; i < filled; ++i) bar += "█";
    for (int i = 0; i < length - filled; ++i) bar += "▒";
    return bar + " ";
}

std::string forGoggles(std::string_view text, int indent) { return std::string(static_cast<std::size_t>(4 + indent), ' ') + std::string(text); }

std::string stripFormatting(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text.substr(i, kSection.size()) == kSection) {
            i += kSection.size();
            if (i < text.size()) nextCodePoint(text, i);
            continue;
        }
        out += text[i++];
    }
    return out;
}

int textWidth(std::string_view text) {
    auto plain = stripFormatting(text);
    int  width = 0;
    for (std::size_t i = 0; i < plain.size();) {
        auto cp = nextCodePoint(plain, i);
        if (cp == '_') continue;
        width += glyphWidth(cp);
    }
    return width;
}

std::vector<std::string> cutText(std::string_view text, std::string_view primary, std::string_view highlight, int indent, int maxWidth) {
    // words: a word ends after its trailing spaces; wide (CJK) characters are words of their own
    std::vector<std::string> words;
    std::string              word;
    bool                     inSpaces = false;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t start = i;
        auto        cp    = nextCodePoint(text, i);
        auto        chunk = text.substr(start, i - start);
        if (isWide(cp)) {
            if (!word.empty()) words.push_back(std::move(word));
            word.clear();
            words.emplace_back(chunk);
            inSpaces = false;
            continue;
        }
        if (cp == ' ') {
            word += chunk;
            inSpaces = true;
            continue;
        }
        if (inSpaces) {
            words.push_back(std::move(word));
            word.clear();
            inSpaces = false;
        }
        word += chunk;
    }
    if (!word.empty()) words.push_back(std::move(word));

    // hard wrap
    std::vector<std::string> lines;
    std::string              current;
    int                      width = 0;
    for (auto const& w : words) {
        int newWidth = textWidth(w);
        if (width + newWidth > maxWidth) {
            if (width > 0) {
                lines.push_back(std::move(current));
                current.clear();
                width = 0;
            } else {
                lines.push_back(w);
                continue;
            }
        }
        current += w;
        width += newWidth;
    }
    if (width > 0) lines.push_back(std::move(current));

    // colours: parts between underscores are highlighted; the state carries over line ends
    std::vector<std::string> out;
    bool                     highlighted = false;
    for (auto const& line : lines) {
        std::string formatted = std::string(primary) + std::string(static_cast<std::size_t>(indent), ' ');
        std::size_t start     = 0;
        while (true) {
            auto end  = line.find('_', start);
            auto part = std::string_view(line).substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!part.empty()) formatted += std::string(highlighted ? highlight : primary) + std::string(part);
            highlighted = !highlighted;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        highlighted = !highlighted;
        out.push_back(std::move(formatted));
    }
    return out;
}

std::vector<std::string> describeItem(DescriptionText const& text, bool shift, bool ctrl) {
    if (text.summary.empty()) return {};
    std::vector<std::string> lines, onShift, onCtrl;

    for (auto const& s : text.summary) {
        auto cut = cutText(s, fmt::kCreatePrimary, fmt::kCreateHighlight);
        onShift.insert(onShift.end(), cut.begin(), cut.end());
    }
    if (!text.behaviours.empty()) onShift.emplace_back();
    for (auto const& [condition, behaviour] : text.behaviours) {
        onShift.push_back(std::string(fmt::kGray) + condition);
        auto cut = cutText(behaviour, fmt::kCreatePrimary, fmt::kCreateHighlight, 1);
        onShift.insert(onShift.end(), cut.begin(), cut.end());
    }
    for (auto const& [control, action] : text.actions) {
        onCtrl.push_back(std::string(fmt::kGray) + control);
        auto cut = cutText(action, fmt::kCreatePrimary, fmt::kCreateHighlight, 1);
        onCtrl.insert(onCtrl.end(), cut.begin(), cut.end());
    }

    bool hasDescription = !onShift.empty();
    bool hasControls    = !onCtrl.empty();

    auto split = [](std::string const& pattern) -> std::pair<std::string, std::string> {
        for (std::string_view ph : {std::string_view("{0}"), std::string_view("{}")}) {
            auto at = pattern.find(ph);
            if (at != std::string::npos) return {pattern.substr(0, at), pattern.substr(at + ph.size())};
        }
        return {pattern, ""};
    };
    auto [desc0, desc1] = split(text.holdForDescription);
    auto [ctrl0, ctrl1] = split(text.holdForControls);

    for (auto* list : {&lines, &onShift, &onCtrl}) {
        bool isShift = list == &onShift;
        bool isCtrl  = list == &onCtrl;
        if (hasControls) {
            list->insert(
                list->begin(),
                std::string(fmt::kDarkGray) + ctrl0 + (isCtrl ? fmt::kWhite : fmt::kGray) + text.keyCtrl + fmt::kDarkGray + ctrl1
            );
        }
        if (hasDescription) {
            list->insert(
                list->begin(),
                std::string(fmt::kDarkGray) + desc0 + (isShift ? fmt::kWhite : fmt::kGray) + text.keyShift + fmt::kDarkGray + desc1
            );
        }
        if (isShift || isCtrl) {
            auto at = static_cast<std::ptrdiff_t>(hasDescription && hasControls ? 2 : 1);
            if (at <= static_cast<std::ptrdiff_t>(list->size())) list->insert(list->begin() + at, std::string{});
        }
    }
    if (!hasDescription) {
        onCtrl.clear();
        onShift.insert(onShift.end(), lines.begin(), lines.end());
    }
    if (!hasControls) onCtrl = lines;

    if (shift) return onShift;
    if (ctrl) return onCtrl;
    return lines;
}

} // namespace lv::core::goggles
