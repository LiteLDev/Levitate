#include "core/kinetics/SyncCodec.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <utility>

namespace lv::core::kinetics::sync {

namespace {

void appendInt(std::string& out, long long v) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, r.ptr);
}

void appendFloat(std::string& out, float v) {
    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, r.ptr);
}

template <class T>
bool parse(std::string_view s, T& out) {
    if (s.empty()) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> parts;
    std::size_t                   start = 0;
    while (true) {
        auto pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

} // namespace

std::string encode(Batch const& b) {
    std::string out;
    out.reserve(24 + b.entries.size() * 24);
    appendInt(out, b.dim);
    out += b.reset ? ";R;" : ";D;";
    appendInt(out, b.cx);
    out += ';';
    appendInt(out, b.cz);
    out += '|';
    bool first = true;
    for (auto const& e : b.entries) {
        if (!first) out += ';';
        first = false;
        appendInt(out, e.x);
        out += ',';
        appendInt(out, e.y);
        out += ',';
        appendInt(out, e.z);
        out += ',';
        if (e.removed) out += '-';
        else appendFloat(out, e.speed);
        out += ',';
        appendInt(out, e.removed ? -1 : e.sourceFace);
    }
    return out;
}

std::optional<Batch> decode(std::string_view text) {
    auto bar = text.find('|');
    if (bar == std::string_view::npos) return std::nullopt;
    auto head = split(text.substr(0, bar), ';');
    if (head.size() != 4) return std::nullopt;
    Batch b;
    if (!parse(head[0], b.dim)) return std::nullopt;
    if (head[1] == "R") b.reset = true;
    else if (head[1] != "D") return std::nullopt;
    if (!parse(head[2], b.cx) || !parse(head[3], b.cz)) return std::nullopt;
    auto body = text.substr(bar + 1);
    if (body.empty()) return b;
    for (auto item : split(body, ';')) {
        auto f = split(item, ',');
        if (f.size() != 5) return std::nullopt;
        Entry e;
        if (!parse(f[0], e.x) || !parse(f[1], e.y) || !parse(f[2], e.z)) return std::nullopt;
        if (f[3] == "-") {
            e.removed = true;
        } else if (!parse(f[3], e.speed)) {
            return std::nullopt;
        }
        if (!parse(f[4], e.sourceFace) || e.sourceFace < -1 || e.sourceFace > 5) return std::nullopt;
        b.entries.push_back(e);
    }
    return b;
}

std::vector<Batch> byChunk(int dim, std::vector<Entry> entries, bool reset) {
    std::map<std::pair<int, int>, std::vector<Entry>> chunks;
    for (auto& e : entries) chunks[{chunkOf(e.x), chunkOf(e.z)}].push_back(e);
    std::vector<Batch> out;
    if (reset && chunks.empty()) out.push_back(Batch{dim, true, 0, 0, {}});
    for (auto& [key, list] : chunks) {
        Batch b;
        b.dim     = dim;
        b.reset   = reset && out.empty();
        b.cx      = key.first;
        b.cz      = key.second;
        b.entries = std::move(list);
        out.push_back(std::move(b));
    }
    return out;
}

} // namespace lv::core::kinetics::sync
