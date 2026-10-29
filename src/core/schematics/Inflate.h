#pragma once

// DEFLATE decompression (RFC 1951) with GZIP (RFC 1952) and zlib (RFC 1950) framing, and a
// minimal GZIP writer (stored blocks), so Java `.nbt` structure files can be read and written
// without a zlib dependency. Pure C++.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lv::core::schematics {

/// Inflates a raw DEFLATE stream. nullopt on corrupt input or when the output would exceed `maxOutput`.
[[nodiscard]] std::optional<std::string> inflateRaw(std::string_view data, std::size_t maxOutput = 64u << 20, std::size_t* consumed = nullptr);
/// Decompresses GZIP data (checks the CRC32 and size). nullopt if not GZIP or corrupt.
[[nodiscard]] std::optional<std::string> gunzip(std::string_view data, std::size_t maxOutput = 64u << 20);
/// True if the data starts with the GZIP magic bytes.
[[nodiscard]] bool isGzip(std::string_view data);
/// GZIP-wraps `data` (stored DEFLATE blocks: valid GZIP, not compressed).
[[nodiscard]] std::string gzipStored(std::string_view data);
[[nodiscard]] std::uint32_t crc32(std::string_view data);

} // namespace lv::core::schematics
