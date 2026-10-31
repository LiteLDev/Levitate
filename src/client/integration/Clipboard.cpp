// The system clipboard (client build only). Kept apart from game headers: <windows.h> defines
// macros (min, max, ERROR, ...) that clash with them.

#ifdef LV_CLIENT

#include "client/integration/Clipboard.h"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "user32.lib")

namespace lv::client::integration {

bool copyToClipboard(std::string_view utf8) {
    // CRLF line ends, as Windows text expects
    std::string text;
    text.reserve(utf8.size() + utf8.size() / 16);
    for (char c : utf8) {
        if (c == '\n') text += '\r';
        text += c;
    }
    int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide < 0) return false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (static_cast<SIZE_T>(wide) + 1) * sizeof(wchar_t));
    if (!mem) return false;
    auto* out = static_cast<wchar_t*>(GlobalLock(mem));
    if (!out) {
        GlobalFree(mem);
        return false;
    }
    if (wide > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out, wide);
    out[wide] = L'\0';
    GlobalUnlock(mem);
    if (!OpenClipboard(nullptr)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem); // on success the clipboard owns the memory
    return ok;
}

} // namespace lv::client::integration

#endif
