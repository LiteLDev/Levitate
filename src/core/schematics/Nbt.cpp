#include "core/schematics/Nbt.h"

#include <bit>
#include <cstring>
#include <sstream>

namespace lv::core::schematics::nbt {

// ---- Compound -----------------------------------------------------------------------------------

Tag const* Compound::find(std::string_view key) const {
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return &values[i];
    return nullptr;
}

Tag* Compound::find(std::string_view key) {
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return &values[i];
    return nullptr;
}

Tag& Compound::operator[](std::string_view key) {
    if (auto* t = find(key)) return *t;
    keys.emplace_back(key);
    values.emplace_back();
    return values.back();
}

void Compound::set(std::string_view key, Tag value) { (*this)[key] = std::move(value); }

bool Compound::erase(std::string_view key) {
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] != key) continue;
        keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

// ---- Tag ----------------------------------------------------------------------------------------

bool Tag::isNumber() const {
    auto t = type();
    return t >= Type::Byte && t <= Type::Double;
}

std::optional<std::int64_t> Tag::asInt() const {
    switch (type()) {
    case Type::Byte:
        return std::get<std::int8_t>(v);
    case Type::Short:
        return std::get<std::int16_t>(v);
    case Type::Int:
        return std::get<std::int32_t>(v);
    case Type::Long:
        return std::get<std::int64_t>(v);
    case Type::Float:
        return static_cast<std::int64_t>(std::get<float>(v));
    case Type::Double:
        return static_cast<std::int64_t>(std::get<double>(v));
    default:
        return std::nullopt;
    }
}

bool operator==(Tag const& a, Tag const& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
    case Type::List: {
        auto const& x = std::get<List>(a.v);
        auto const& y = std::get<List>(b.v);
        if (x.items.size() != y.items.size()) return false;
        if (!x.items.empty() && x.elem != y.elem) return false;
        for (std::size_t i = 0; i < x.items.size(); ++i)
            if (!(x.items[i] == y.items[i])) return false;
        return true;
    }
    case Type::Compound: {
        auto const& x = std::get<Compound>(a.v);
        auto const& y = std::get<Compound>(b.v);
        if (x.size() != y.size()) return false;
        for (std::size_t i = 0; i < x.keys.size(); ++i) {
            auto const* o = y.find(x.keys[i]);
            if (!o || !(x.values[i] == *o)) return false;
        }
        return true;
    }
    case Type::ByteArray:
        return std::get<ByteArray>(a.v).v == std::get<ByteArray>(b.v).v;
    case Type::IntArray:
        return std::get<IntArray>(a.v).v == std::get<IntArray>(b.v).v;
    case Type::LongArray:
        return std::get<LongArray>(a.v).v == std::get<LongArray>(b.v).v;
    case Type::Byte:
        return std::get<std::int8_t>(a.v) == std::get<std::int8_t>(b.v);
    case Type::Short:
        return std::get<std::int16_t>(a.v) == std::get<std::int16_t>(b.v);
    case Type::Int:
        return std::get<std::int32_t>(a.v) == std::get<std::int32_t>(b.v);
    case Type::Long:
        return std::get<std::int64_t>(a.v) == std::get<std::int64_t>(b.v);
    case Type::Float:
        return std::get<float>(a.v) == std::get<float>(b.v);
    case Type::Double:
        return std::get<double>(a.v) == std::get<double>(b.v);
    case Type::String:
        return std::get<std::string>(a.v) == std::get<std::string>(b.v);
    default:
        return true;
    }
}

// ---- reading ------------------------------------------------------------------------------------

namespace {

constexpr int kMaxDepth = 512;

class Reader {
public:
    Reader(std::string_view data, Endian endian) : mData(data), mEndian(endian) {}

    bool fail(std::string msg) {
        if (mError.empty()) mError = std::move(msg);
        return false;
    }
    [[nodiscard]] std::string const& error() const { return mError; }
    [[nodiscard]] std::size_t        remaining() const { return mData.size() - mPos; }

    template <class T>
    bool num(T& out) {
        if (remaining() < sizeof(T)) return fail("truncated data");
        using U = std::conditional_t<sizeof(T) == 1, std::uint8_t, std::conditional_t<sizeof(T) == 2, std::uint16_t, std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
        U u = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            auto b = static_cast<U>(static_cast<std::uint8_t>(mData[mPos + i]));
            if (mEndian == Endian::Little) u |= static_cast<U>(b << (8 * i));
            else u = static_cast<U>((u << 8) | b);
        }
        mPos += sizeof(T);
        std::memcpy(&out, &u, sizeof(T));
        return true;
    }

    bool str(std::string& out) {
        std::uint16_t len = 0;
        if (!num(len)) return false;
        if (remaining() < len) return fail("truncated string");
        out.assign(mData.substr(mPos, len));
        mPos += len;
        return true;
    }

    bool length(std::int32_t& n, std::size_t elemSize) {
        if (!num(n)) return false;
        if (n < 0) return fail("negative length");
        if (static_cast<std::size_t>(n) * elemSize > remaining()) return fail("length exceeds data");
        return true;
    }

    bool payload(Type type, Tag& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        switch (type) {
        case Type::Byte: {
            std::int8_t v{};
            if (!num(v)) return false;
            out.v = v;
            return true;
        }
        case Type::Short: {
            std::int16_t v{};
            if (!num(v)) return false;
            out.v = v;
            return true;
        }
        case Type::Int: {
            std::int32_t v{};
            if (!num(v)) return false;
            out.v = v;
            return true;
        }
        case Type::Long: {
            std::int64_t v{};
            if (!num(v)) return false;
            out.v = v;
            return true;
        }
        case Type::Float: {
            float v{};
            if (!num(v)) return false;
            out.v = v;
            return true;
        }
        case Type::Double: {
            double v{};
            if (!num(v)) return false;
            out.v = v;
            return true;
        }
        case Type::ByteArray: {
            std::int32_t n = 0;
            if (!length(n, 1)) return false;
            ByteArray a;
            a.v.resize(static_cast<std::size_t>(n));
            for (auto& b : a.v)
                if (!num(b)) return false;
            out.v = std::move(a);
            return true;
        }
        case Type::String: {
            std::string s;
            if (!str(s)) return false;
            out.v = std::move(s);
            return true;
        }
        case Type::List: {
            std::uint8_t elem = 0;
            if (!num(elem)) return false;
            if (elem > static_cast<std::uint8_t>(Type::LongArray)) return fail("bad list type");
            std::int32_t n = 0;
            if (!length(n, elem == 0 ? 0 : 1)) return false;
            List l;
            l.elem = static_cast<Type>(elem);
            if (l.elem == Type::End && n > 0) return fail("list of end tags");
            l.items.resize(static_cast<std::size_t>(n));
            for (auto& item : l.items)
                if (!payload(l.elem, item, depth + 1)) return false;
            out.v = std::move(l);
            return true;
        }
        case Type::Compound: {
            Compound c;
            for (;;) {
                std::uint8_t t = 0;
                if (!num(t)) return false;
                if (t == 0) break;
                if (t > static_cast<std::uint8_t>(Type::LongArray)) return fail("bad tag type");
                std::string name;
                if (!str(name)) return false;
                Tag value;
                if (!payload(static_cast<Type>(t), value, depth + 1)) return false;
                c.keys.push_back(std::move(name));
                c.values.push_back(std::move(value));
            }
            out.v = std::move(c);
            return true;
        }
        case Type::IntArray: {
            std::int32_t n = 0;
            if (!length(n, 4)) return false;
            IntArray a;
            a.v.resize(static_cast<std::size_t>(n));
            for (auto& x : a.v)
                if (!num(x)) return false;
            out.v = std::move(a);
            return true;
        }
        case Type::LongArray: {
            std::int32_t n = 0;
            if (!length(n, 8)) return false;
            LongArray a;
            a.v.resize(static_cast<std::size_t>(n));
            for (auto& x : a.v)
                if (!num(x)) return false;
            out.v = std::move(a);
            return true;
        }
        default:
            return fail("bad tag type");
        }
    }

private:
    std::string_view mData;
    std::size_t      mPos = 0;
    Endian           mEndian;
    std::string      mError;
};

class Writer {
public:
    explicit Writer(Endian endian) : mEndian(endian) {}
    std::string out;

    template <class T>
    void num(T value) {
        using U = std::conditional_t<sizeof(T) == 1, std::uint8_t, std::conditional_t<sizeof(T) == 2, std::uint16_t, std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
        U u = 0;
        std::memcpy(&u, &value, sizeof(T));
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            std::size_t shift = mEndian == Endian::Little ? 8 * i : 8 * (sizeof(T) - 1 - i);
            out.push_back(static_cast<char>((u >> shift) & 0xFF));
        }
    }
    void str(std::string_view s) {
        auto n = static_cast<std::uint16_t>(std::min<std::size_t>(s.size(), 0xFFFF));
        num(n);
        out.append(s.substr(0, n));
    }
    void payload(Tag const& t) {
        switch (t.type()) {
        case Type::Byte:
            num(std::get<std::int8_t>(t.v));
            break;
        case Type::Short:
            num(std::get<std::int16_t>(t.v));
            break;
        case Type::Int:
            num(std::get<std::int32_t>(t.v));
            break;
        case Type::Long:
            num(std::get<std::int64_t>(t.v));
            break;
        case Type::Float:
            num(std::get<float>(t.v));
            break;
        case Type::Double:
            num(std::get<double>(t.v));
            break;
        case Type::ByteArray: {
            auto const& a = std::get<ByteArray>(t.v).v;
            num(static_cast<std::int32_t>(a.size()));
            for (auto b : a) num(b);
            break;
        }
        case Type::String:
            str(std::get<std::string>(t.v));
            break;
        case Type::List: {
            auto const& l    = std::get<List>(t.v);
            Type        elem = l.items.empty() ? l.elem : l.items.front().type();
            num(static_cast<std::uint8_t>(elem));
            num(static_cast<std::int32_t>(l.items.size()));
            for (auto const& item : l.items) payload(item);
            break;
        }
        case Type::Compound: {
            auto const& c = std::get<Compound>(t.v);
            for (std::size_t i = 0; i < c.keys.size(); ++i) {
                if (c.values[i].type() == Type::End) continue;
                num(static_cast<std::uint8_t>(c.values[i].type()));
                str(c.keys[i]);
                payload(c.values[i]);
            }
            num(std::uint8_t{0});
            break;
        }
        case Type::IntArray: {
            auto const& a = std::get<IntArray>(t.v).v;
            num(static_cast<std::int32_t>(a.size()));
            for (auto x : a) num(x);
            break;
        }
        case Type::LongArray: {
            auto const& a = std::get<LongArray>(t.v).v;
            num(static_cast<std::int32_t>(a.size()));
            for (auto x : a) num(x);
            break;
        }
        default:
            break;
        }
    }

private:
    Endian mEndian;
};

void appendString(std::ostringstream& os, Tag const& t) {
    switch (t.type()) {
    case Type::End:
        os << "end";
        break;
    case Type::Byte:
        os << int(std::get<std::int8_t>(t.v)) << 'b';
        break;
    case Type::Short:
        os << std::get<std::int16_t>(t.v) << 's';
        break;
    case Type::Int:
        os << std::get<std::int32_t>(t.v);
        break;
    case Type::Long:
        os << std::get<std::int64_t>(t.v) << 'L';
        break;
    case Type::Float:
        os << std::get<float>(t.v) << 'f';
        break;
    case Type::Double:
        os << std::get<double>(t.v) << 'd';
        break;
    case Type::String:
        os << '"' << std::get<std::string>(t.v) << '"';
        break;
    case Type::ByteArray:
        os << "[B;" << std::get<ByteArray>(t.v).v.size() << ']';
        break;
    case Type::IntArray:
        os << "[I;" << std::get<IntArray>(t.v).v.size() << ']';
        break;
    case Type::LongArray:
        os << "[L;" << std::get<LongArray>(t.v).v.size() << ']';
        break;
    case Type::List: {
        os << '[';
        auto const& l = std::get<List>(t.v);
        for (std::size_t i = 0; i < l.items.size(); ++i) {
            if (i) os << ',';
            appendString(os, l.items[i]);
        }
        os << ']';
        break;
    }
    case Type::Compound: {
        os << '{';
        auto const& c = std::get<Compound>(t.v);
        for (std::size_t i = 0; i < c.keys.size(); ++i) {
            if (i) os << ',';
            os << c.keys[i] << ':';
            appendString(os, c.values[i]);
        }
        os << '}';
        break;
    }
    }
}

} // namespace

std::optional<ReadResult> read(std::string_view data, Endian endian, std::string* error) {
    Reader       r(data, endian);
    std::uint8_t type = 0;
    ReadResult   out;
    bool         ok = r.num(type);
    if (ok && type == 0) ok = r.fail("empty root");
    if (ok && type > static_cast<std::uint8_t>(Type::LongArray)) ok = r.fail("bad root type");
    ok = ok && r.str(out.name) && r.payload(static_cast<Type>(type), out.root, 0);
    if (!ok) {
        if (error) *error = r.error();
        return std::nullopt;
    }
    return out;
}

std::string write(Tag const& root, std::string_view name, Endian endian) {
    Writer w(endian);
    w.num(static_cast<std::uint8_t>(root.type()));
    w.str(name);
    w.payload(root);
    return std::move(w.out);
}

std::string toString(Tag const& tag) {
    std::ostringstream os;
    appendString(os, tag);
    return os.str();
}

} // namespace lv::core::schematics::nbt
