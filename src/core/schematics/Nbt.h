#pragma once

// A small NBT tree with binary readers / writers for both byte orders (pure C++):
// Java (big endian, `.nbt` structure files inside GZIP) and Bedrock (little endian,
// `.mcstructure` files and the game's own CompoundTag binary form).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace lv::core::schematics::nbt {

enum class Type : std::uint8_t {
    End       = 0,
    Byte      = 1,
    Short     = 2,
    Int       = 3,
    Long      = 4,
    Float     = 5,
    Double    = 6,
    ByteArray = 7,
    String    = 8,
    List      = 9,
    Compound  = 10,
    IntArray  = 11,
    LongArray = 12,
};

enum class Endian { Big, Little };

struct Tag;

struct List {
    Type             elem = Type::End;
    std::vector<Tag> items;
};

/// Ordered key / value pairs (insertion order is kept for round trips).
struct Compound {
    std::vector<std::string> keys;
    std::vector<Tag>         values;

    [[nodiscard]] Tag const* find(std::string_view key) const;
    [[nodiscard]] Tag*       find(std::string_view key);
    /// The value of `key` (inserted as an empty tag when missing).
    Tag&                     operator[](std::string_view key);
    void                     set(std::string_view key, Tag value);
    bool                     erase(std::string_view key);
    [[nodiscard]] std::size_t size() const { return keys.size(); }
    [[nodiscard]] bool        empty() const { return keys.empty(); }
};

struct ByteArray {
    std::vector<std::int8_t> v;
};
struct IntArray {
    std::vector<std::int32_t> v;
};
struct LongArray {
    std::vector<std::int64_t> v;
};

struct Tag {
    using Value = std::variant<
        std::monostate,
        std::int8_t,
        std::int16_t,
        std::int32_t,
        std::int64_t,
        float,
        double,
        ByteArray,
        std::string,
        List,
        Compound,
        IntArray,
        LongArray>;
    Value v;

    Tag() = default;
    template <class T>
        requires std::is_constructible_v<Value, T&&>
    Tag(T&& value) : v(std::forward<T>(value)) {}

    [[nodiscard]] Type type() const { return static_cast<Type>(v.index()); }
    [[nodiscard]] bool isNumber() const;
    /// Any integral number (byte, short, int, long) or a float truncated; nullopt otherwise.
    [[nodiscard]] std::optional<std::int64_t> asInt() const;
    [[nodiscard]] std::string const*          asString() const { return std::get_if<std::string>(&v); }
    [[nodiscard]] Compound const*             asCompound() const { return std::get_if<Compound>(&v); }
    [[nodiscard]] Compound*                   asCompound() { return std::get_if<Compound>(&v); }
    [[nodiscard]] List const*                 asList() const { return std::get_if<List>(&v); }
    [[nodiscard]] List*                       asList() { return std::get_if<List>(&v); }

    static Tag compound() { return Tag(Compound{}); }
    static Tag list(Type elem) { return Tag(List{elem, {}}); }
};

[[nodiscard]] bool operator==(Tag const& a, Tag const& b);

struct ReadResult {
    std::string name; ///< root tag name
    Tag         root;
};

/// Reads one named root tag. Errors (truncation, bad types, too deep) give nullopt and `error`.
[[nodiscard]] std::optional<ReadResult> read(std::string_view data, Endian endian, std::string* error = nullptr);
/// Writes a named root tag.
[[nodiscard]] std::string write(Tag const& root, std::string_view name, Endian endian);

/// Compact text form for logs and tests (SNBT-like).
[[nodiscard]] std::string toString(Tag const& tag);

} // namespace lv::core::schematics::nbt
