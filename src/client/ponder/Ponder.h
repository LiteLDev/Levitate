#pragma once

// Ponder on the client mod (U30, SPEC §3.6.30). Client build only.
//
// - Ponder.cpp:        scene library (mod dir `ponder/`), hold-W-over-an-item, `ponder.open`
//                      messages from the server (`/create ponder`), the module.
// - SceneRenderer.cpp: draws a scene state into a screen (blocks through the game's block
//                      tessellator, outlines, indicators, text bubbles).
// - PonderScreens.cpp: the scene player (PonderUI), the tag screen and the index.

#ifdef LV_CLIENT

#include <functional>
#include <string>
#include <vector>

#include "core/ponder/Scene.h"
#include "core/ponder/View.h"

namespace lv::client::screen {
class Canvas;
}

namespace lv::client::ponder {

namespace cp = core::ponder;

/// The scenes and tags shipped with the mod (loaded once, on first use).
[[nodiscard]] cp::Library const& library();

/// Opens the scene player for `component` (an item id), at its scene `scene` (empty: the first).
/// Returns false when the component has no scenes.
bool openComponent(std::string const& component, std::string const& scene = {});
/// Opens the Ponder index (categories and every entry).
void openIndex();
/// Opens a tag's screen (its associated entries).
bool openTag(std::string const& tag);

/// Localised display name of an item / block id ("" when the item does not exist).
[[nodiscard]] std::string itemName(std::string const& id);

// ---- input (Ponder.cpp) ----------------------------------------------------------------------

/// The left mouse button is held (tracked from the game's mouse events).
[[nodiscard]] bool leftMouseDown();
/// A key went down while a Ponder screen is open (Windows virtual-key code). Returns true when used.
bool ponderKey(int vk);
/// A Ponder screen is the current screen.
[[nodiscard]] bool ponderScreenOpen();
/// Goes back to the previous Ponder screen ("Think Back"); false when there is none.
bool thinkBack();
[[nodiscard]] bool canThinkBack();
/// Remembers how to reopen the current Ponder screen before navigating to another one.
void pushHistory(std::function<void()> reopen);
void clearHistory();
/// Runs `fn` before the next UI frame (screen changes must not happen inside a widget handler).
void deferUi(std::function<void()> fn);

// ---- rendering --------------------------------------------------------------------------------

struct DrawResult {
    std::string hoveredBlock; ///< identify mode: id of the block under the mouse
    int         drawnBlocks   = 0;
    int         missingBlocks = 0;
};

/// Draws `state` with `view`. `identify`: pick the block under (mx, my) and outline it.
DrawResult drawScene(
    screen::Canvas&       c,
    cp::Scene const&      scene,
    cp::SceneState const& state,
    cp::View const&       view,
    bool                  identify,
    float                 mx,
    float                 my
);

/// Draws the text bubbles of `state` (on top of everything).
void drawTexts(screen::Canvas& c, cp::Scene const& scene, cp::SceneState const& state, cp::View const& view, float screenW, bool slow);

/// Draws the icon of an item / block id in a size square at (x, y) (Levitate blocks from their model,
/// others with the game's item renderer). Returns false when the id does not exist.
bool drawIcon(screen::Canvas& c, std::string const& id, float x, float y, float size = 16);

/// Drops cached block lookups (a new world registers block states again).
void resetRenderCache();

} // namespace lv::client::ponder

#endif // LV_CLIENT
