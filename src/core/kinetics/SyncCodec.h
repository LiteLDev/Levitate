#pragma once

// Text codec of the kinetic speed sync (server -> client mod), pure C++.
//
// The sync channel is a minimal stand-in for the companion protocol of U3 (see
// src/feature/kinetics/KineticsSync.cpp): the payload is the value string of a Bedrock
// ScriptMessagePacket. One message carries the speeds of the kinetic blocks of one chunk:
//
//   "<dim>;<R|D>;<cx>;<cz>|<x>,<y>,<z>,<speed>,<sourceFace>;..."
//
// R = the first message of a full sync of the dimension (the client drops what it knew about the
// dimension), D = a delta. `speed` is the effective speed in RPM (shortest round-trip decimal) or
// "-" when the block was removed; `sourceFace` is the face towards the block's source (0..5,
// Create's Direction order) or -1.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lv::core::kinetics::sync {

inline constexpr int kProtocolVersion = 1;

inline constexpr std::string_view kHelloId = "levitate:kinetics/hello"; ///< client -> server, value: protocol
inline constexpr std::string_view kAckId   = "levitate:kinetics/ack";   ///< server -> client, value: protocol
inline constexpr std::string_view kSyncId  = "levitate:kinetics/sync";  ///< server -> client, value: a batch

struct Entry {
    int   x = 0, y = 0, z = 0;
    float speed      = 0;
    int   sourceFace = -1;
    bool  removed    = false;
    friend bool operator==(Entry const&, Entry const&) = default;
};

struct Batch {
    int                dim   = 0;
    bool               reset = false;
    int                cx = 0, cz = 0;
    std::vector<Entry> entries;
    friend bool operator==(Batch const&, Batch const&) = default;
};

[[nodiscard]] std::string          encode(Batch const& batch);
[[nodiscard]] std::optional<Batch> decode(std::string_view text);

/// Groups entries of one dimension into one batch per chunk (16x16 columns), sorted by chunk.
/// The first batch gets `reset` when `reset` is set (an empty batch is emitted if there are no
/// entries, so the client still drops its old state).
[[nodiscard]] std::vector<Batch> byChunk(int dim, std::vector<Entry> entries, bool reset);

/// Floor division to a chunk coordinate.
[[nodiscard]] constexpr int chunkOf(int block) { return block >= 0 ? block / 16 : -((-block + 15) / 16); }

} // namespace lv::core::kinetics::sync
