#pragma once

// Client mod side of the schematics unit (U29): native screens, the schematic tools, the ghost
// preview, the quill selection outline and the schematicannon's launched blocks. Internal.

#ifdef LV_CLIENT

#include <filesystem>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace lv::client::schematics {

using Json = nlohmann::json;

/// `<mod>/schematics/` (the player's local schematics, as Create's `schematics/` folder).
[[nodiscard]] std::filesystem::path localDir();
/// Local schematic files (`.nbt`, `.mcstructure`), sorted.
[[nodiscard]] std::vector<std::string> localFiles();
/// Starts uploading a local file to the table at `pos` ([dim, x, y, z]); false with a chat
/// message when the file is too large / not GZIP (Create's ClientSchematicLoader checks).
bool startUpload(std::string const& file, Json const& pos, int maxSizeKb, int packetSize);
/// The current upload's name ("" when none) and progress 0..1.
[[nodiscard]] std::pair<std::string, float> uploadState();

/// Text field focus (screens with text fields route keys to it).
struct TextFieldFocus;

} // namespace lv::client::schematics

#endif // LV_CLIENT
