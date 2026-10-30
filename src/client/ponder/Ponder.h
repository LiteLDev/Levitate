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

#include "glm/mat4x4.hpp"

class BaseActorRenderContext;
class Block;
class MinecraftUIRenderContext;
class ScreenContext;
class Tessellator;
namespace mce {
class MaterialPtr;
class Mesh;
} // namespace mce

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

/// The Ponder key (Create's rebindable key mapping, `config/ponder.json` ponderKey) and its name.
[[nodiscard]] int         ponderKeyCode();
[[nodiscard]] std::string keyName(int vk);
/// The next key pressed becomes the Ponder key (Esc cancels); `done(vk)` runs after (0: cancelled).
void rebindPonderKey(std::function<void(int)> done);
[[nodiscard]] bool rebinding();
/// Stops waiting for a new Ponder key (the screen that asked closed).
void cancelRebind();

// ---- rendering --------------------------------------------------------------------------------

struct DrawResult {
    std::string hoveredBlock; ///< identify mode: id of the block under the mouse
    int         drawnBlocks   = 0;
    int         missingBlocks = 0;
    int         drawnEntities = 0; ///< (U46) entities drawn by the game's actor renderer
    int         itemEntities  = 0; ///< (U46) item entities drawn by the game's actor renderer
    int         particles     = 0; ///< (U46) particle sprites drawn
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

// ---- entities and particles (SceneEntities.cpp, U46) --------------------------------------------

/// Draws the scene's entities with the game's actor renderer (`camera`: scene -> UI world matrix).
/// Returns how many were drawn.
int drawSceneEntities(ScreenContext& sc, BaseActorRenderContext& rc, glm::mat4 const& camera, cp::SceneState const& st);
/// Draws an item entity showing `item` at `pos` (scene coordinates); false when it can't be drawn.
bool drawSceneItemEntity(ScreenContext& sc, BaseActorRenderContext& rc, glm::mat4 const& camera, std::string const& item, cp::Vec3f const& pos, float time);
/// Draws the scene's particles (billboards facing `view`) with `material`. Returns the count.
int drawSceneParticles(
    MinecraftUIRenderContext& ui,
    ScreenContext&            sc,
    glm::mat4 const&          camera,
    mce::MaterialPtr const&   material,
    cp::SceneState const&     st,
    cp::View const&           view
);
/// Drops the puppet actors the entities are drawn with (scene player closed, level left).
void releaseScenePuppets();

/// Appends the model of `block` (`def`: its id and states) to a begun tessellator at (x, y, z),
/// as Ponder draws it: Levitate blocks from the pack's geometry, other blocks with the game's block
/// tessellator (U29's schematic ghost uses it). Returns false when it can't be drawn.
bool appendBlock(Tessellator& tess, cp::BlockDef const& def, Block const& block, float x, float y, float z);
/// A cached mesh (block units, 0..1) of a Levitate block's model without the bones `hideBones`
/// (U29's schematicannon draws its connector and barrel with it); nullptr for other blocks.
/// Dropped with the level.
mce::Mesh* modelPartMesh(Tessellator& tess, cp::BlockDef const& def, std::vector<std::string> const& hideBones);

} // namespace lv::client::ponder

#endif // LV_CLIENT
