#pragma once

// The system clipboard of the game client (client build only). `/create debuginfo` puts its
// report there, as Create's ServerDebugInfoPacket does on the Java client.

#ifdef LV_CLIENT

#include <string_view>

namespace lv::client::integration {

/// Replaces the clipboard's text with `utf8`. False when the clipboard is unavailable.
bool copyToClipboard(std::string_view utf8);

} // namespace lv::client::integration

#endif
