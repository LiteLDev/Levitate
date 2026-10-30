#pragma once

// Ponder scene scripts (U30, SPEC §3.6.30): the pure model behind the client's Ponder screens.
//
// A scene is data, one file per scene in `assets/ponder/scenes/<id>.json` (copied to
// `<mod>/ponder/scenes/` at build time). Its text lives in the lang files under Create's key
// names: `create.ponder.<id>.header` (the title) and `create.ponder.<id>.text_<n>`.
//
//     {
//       "id": "shaft",                       // = file name; Create's scene id
//       "size": [5, 5],                      // base plate (x, z); optional "height" (default 5)
//       "scale": 1.0,                        // optional view scale (Create's scaleSceneView)
//       "blocks": [                          // the initial world; every block starts hidden
//         {"at": [2, 1, 2], "id": "levitate:shaft", "states": {"levitate:axis": 0}},
//         {"area": [[0, 1, 2], [4, 1, 2]], "id": "levitate:shaft"}
//       ],
//       "timeline": [                        // run in order; only "idle" takes time
//         {"show": "base"},                  // the base plate (y = 0) fades in
//         {"idle": 10},
//         {"show": [[0, 1, 2], [4, 1, 2]], "from": "down"},
//         {"idle": 10},
//         {"rotate": [[0, 1, 2], [4, 1, 2]], "rpm": 16},
//         {"text": 1, "at": [2.5, 1.5, 2.5], "ticks": 60, "keyframe": true},
//         {"idle": 70}
//       ]
//     }
//
// Areas: `[x, y, z]` (one block), `[[x1, y1, z1], [x2, y2, z2]]` (a box, corners inclusive),
// `{"union": [area, ...]}`, or `"base"` (the base plate). Points (`at`, `to`, `origin`) are block
// coordinates as numbers (fractions allowed: [2.5, 1.5, 2.5] is the centre of block 2 1 2).
// States are integers, booleans or strings (string states / traits such as
// "minecraft:cardinal_direction": "north", used by the block model only).
// Directions: "down" (slides in moving down, the default), "up", "north", "south", "west",
// "east", "none". Colours (Create's PonderPalette): "white" (default), "black", "red", "green",
// "blue", "slow", "medium", "fast", "input", "output".
//
// Instructions:
//   {"idle": ticks}                                   wait
//   {"show": area, "from": dir, "as": "name"}         fade blocks in (15 ticks); "as" puts them in a
//                                                     movable section
//   {"hide": area, "to": dir}                         fade blocks out
//   {"set": area, "id": "...", "states": {...}}       place / replace blocks (visible at once)
//   {"destroy": area}                                 break blocks (they disappear)
//   {"modify": area, "states": {...}}                 change states of the blocks there
//   {"text": n, "at": point, "ticks": t, "color": c, "keyframe": bool, "near": bool}
//                                                     text bubble create.ponder.<id>.text_<n>,
//                                                     pointing at `at` ("near": placed next to it,
//                                                     default true; without "at": top of the scene)
//   {"outline": area, "color": c, "ticks": t}         highlight a box outline
//   {"input": point, "action": "right"|"left"|"scroll"|"none", "item": "id", "sneak": bool,
//    "ctrl": bool, "ticks": t, "keyframe": bool}      hand / item / key indicator at a point
//   {"redstone": area, "ticks": t}                    redstone indicator (red particles)
//   {"rotate": area, "rpm": s}                        kinetic speed of the blocks there (0 = stop)
//   {"move": "name", "by": [dx, dy, dz], "ticks": t}  slide a section (contraption movement)
//   {"turn": "name", "axis": "x"|"y"|"z", "degrees": d, "origin": point, "ticks": t}
//                                                     rotate a section about `origin`
//   {"item": "id", "at": point, "to": point, "ticks": t}
//                                                     an item entity (Create's createItemEntity) moving
//                                                     from `at` to `to`
//   {"entity": "minecraft:minecart", "at": point, "to": point, "ticks": t, "yaw": d, "yaw_to": d}
//                                                     an entity (mob, minecart, ...; Create's
//                                                     createEntity) moving from `at` to `to`, turned
//                                                     to `yaw` (degrees, 0 = facing south)
//   {"particles": "flame", "at": point, "ticks": t, "rate": n, "spread": [x, y, z], "motion": [x, y, z]}
//                                                     particles (Create's emitParticles): `rate` per
//                                                     tick (fractions allowed) around `at` (`spread`:
//                                                     half extents), with an extra motion in blocks
//                                                     per tick. Types: kParticleTypes
//   {"keyframe": true}                                a key frame of the timeline
//
// Everything here is game-independent and unit-tested (tests/core/PonderTest.cpp).

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace lv::core::ponder {

using Json = nlohmann::json;

struct Vec3i {
    int  x = 0, y = 0, z = 0;
    auto operator<=>(Vec3i const&) const = default;
};

struct Vec3f {
    float x = 0, y = 0, z = 0;
    bool  operator==(Vec3f const&) const = default;
};

struct Box {
    Vec3i                lo, hi; ///< inclusive corners, lo <= hi
    [[nodiscard]] bool   contains(Vec3i const& p) const;
};

struct Area {
    std::vector<Box>   boxes;
    bool               base = false; ///< the base plate (y = 0, inside the plate)
    [[nodiscard]] bool empty() const { return boxes.empty() && !base; }
};

enum class Dir { None, Down, Up, North, South, West, East };
enum class Palette { White, Black, Red, Green, Blue, Slow, Medium, Fast, Input, Output };

/// ARGB colour of a palette entry (Create's PonderPalette).
[[nodiscard]] unsigned paletteColour(Palette p);
/// The unit offset a block slides in from (`Dir::Down`: from above, i.e. +y).
[[nodiscard]] Vec3f slideOffset(Dir d);

using States = std::map<std::string, int>;

using StringStates = std::map<std::string, std::string>;

struct BlockDef {
    std::string  id;
    States       states;
    StringStates strings; ///< string states / block traits (e.g. "minecraft:block_face": "north")
    bool         operator==(BlockDef const&) const = default;
};

enum class Op { Idle, Show, Hide, Set, Destroy, Modify, Text, Outline, Input, Redstone, Rotate, Move, Turn, Item, Keyframe, Entity, Particles };

/// Particle types of the `particles` instruction (U46).
inline constexpr char const* kParticleTypes[] = {"flame", "soul_flame", "smoke", "large_smoke", "cloud",
                                                 "splash", "bubble", "snowflake", "heart", "crit"};

struct Instruction {
    Op          op    = Op::Idle;
    int         ticks = 0;
    Area        area;
    Dir         dir = Dir::Down;
    std::string section;
    BlockDef    block;   ///< set
    States      states;  ///< modify
    StringStates strings; ///< modify
    int         text = 0;
    bool        hasPoint = false;
    Vec3f       point;   ///< text / input / item start / turn origin
    Vec3f       to;      ///< item end
    Vec3f       by;      ///< move
    Palette     color    = Palette::White;
    bool        keyframe = false;
    bool        near     = true;
    std::string action;  ///< input
    std::string item;    ///< input / item
    bool        sneak = false, ctrl = false;
    float       rpm     = 0;
    char        axis    = 'y';
    float       degrees = 0;
    std::string entity;            ///< entity: type id
    float       yaw = 0, yawTo = 0; ///< entity: degrees
    std::string particle;          ///< particles: type
    float       rate = 1;          ///< particles per tick
    Vec3f       spread{0.25f, 0.25f, 0.25f};
    Vec3f       motion;
};

struct Scene {
    std::string                 id;
    int                         sizeX = 5, sizeZ = 5, height = 5;
    float                       scale = 1.0f;
    std::map<Vec3i, BlockDef>   blocks; ///< initial world (without the base plate)
    std::vector<Instruction>    timeline;

    /// Total length in ticks (sum of idles).
    [[nodiscard]] int length() const;
    /// Ticks of the key frames, ascending, without duplicates.
    [[nodiscard]] std::vector<int> keyframes() const;
    /// Text numbers used by the timeline, ascending.
    [[nodiscard]] std::vector<int> textNumbers() const;
    /// Every block id used (initial world and "set"), sorted.
    [[nodiscard]] std::vector<std::string> blockIds() const;
    /// Lang keys: `create.ponder.<id>.header`, `create.ponder.<id>.text_<n>`.
    [[nodiscard]] std::string headerKey() const { return "create.ponder." + id + ".header"; }
    [[nodiscard]] std::string textKey(int n) const { return "create.ponder." + id + ".text_" + std::to_string(n); }
};

struct ParseResult {
    std::optional<Scene>     scene;
    std::vector<std::string> errors;
};

/// Parses and validates a scene file. `scene` is set when there are no errors.
[[nodiscard]] ParseResult parseScene(Json const& j);

// ---- evaluation ----------------------------------------------------------------------------

/// Ticks a block takes to fade in / out.
inline constexpr float kFadeTicks = 15.0f;

struct BlockView {
    Vec3i       pos;
    BlockDef    block;
    float       alpha = 1;  ///< 0..1 (fading)
    Vec3f       offset;     ///< slide offset + section movement, in blocks
    std::string section;
    float       angle = 0;  ///< kinetic rotation angle (degrees) at this time
    float       rpm   = 0;
    bool        base  = false;
};

struct TextView {
    int     text = 0;
    bool    hasPoint = false;
    Vec3f   point;
    Palette color = Palette::White;
    bool    near  = true;
    float   age   = 0;    ///< ticks since shown
    float   left  = 0;    ///< ticks until it disappears
    int     index = 0;    ///< order among the visible texts
};

struct OutlineView {
    Area    area;
    Palette color = Palette::White;
    float   age = 0, left = 0;
};

struct InputView {
    Vec3f       point;
    std::string action, item;
    bool        sneak = false, ctrl = false;
    float       age = 0, left = 0;
};

struct ItemView {
    std::string id;
    Vec3f       pos;
};

struct EntityView {
    std::string id;           ///< entity type
    int         instance = 0; ///< index of its instruction in the timeline
    Vec3f       pos;
    float       yaw = 0;
    float       age = 0, left = 0;
};

struct ParticleView {
    std::string type;
    int         instance = 0; ///< index of its instruction (seeds the particle positions)
    Vec3f       point, spread, motion;
    float       rate = 1;
    float       age = 0, left = 0;
};

struct RedstoneView {
    Area  area;
    float age = 0, left = 0;
};

struct SectionTurn {
    char  axis    = 'y';
    float degrees = 0;
    Vec3f origin;
};

struct SectionView {
    Vec3f                    offset;
    std::vector<SectionTurn> turns; ///< applied in order, each about its origin (moved by `offset`)
};

struct SceneState {
    float                              time = 0;
    std::vector<BlockView>             blocks; ///< visible blocks (alpha > 0), sorted by position
    std::vector<TextView>              texts;
    std::vector<OutlineView>           outlines;
    std::vector<InputView>             inputs;
    std::vector<ItemView>              items;
    std::vector<EntityView>            entities;
    std::vector<ParticleView>          particles;
    std::vector<RedstoneView>          redstone;
    std::map<std::string, SectionView> sections;
};

/// The scene as it looks `time` ticks after it started (fractions interpolate animations).
/// Replays the timeline from the start: any time can be shown (timeline scrubbing, key frames).
[[nodiscard]] SceneState evaluate(Scene const& scene, float time);

// ---- library (index, tags) -------------------------------------------------------------------

struct Tag {
    std::string              id;    ///< create.ponder.tag.<id> / .<id>.description
    std::string              icon;  ///< item id of the tag's icon
    std::vector<std::string> items; ///< associated entries, in order
    bool                     index = true; ///< listed on the Ponder index screen
};

/// The scenes and tags (`assets/ponder/index.json`):
///     {"components": [{"id": "levitate:cogwheel", "scenes": ["cogwheel", "cog_speedup", ...]}, ...],
///      "tags": [{"id": "kinetic_relays", "icon": "levitate:cogwheel", "index": true, "items": [...]}, ...]}
/// Components are in Create's registration order (the index order); each lists its scenes in order.
class Library {
public:
    struct Component {
        std::string              id;
        std::vector<std::string> scenes;
    };

    /// Adds a parsed scene.
    void add(Scene scene);
    /// Parses the index. Returns errors.
    std::vector<std::string> setIndex(Json const& j);
    /// "component -> scene" pairs of the index whose scene was not added.
    [[nodiscard]] std::vector<std::string> missingScenes() const;

    /// The component has at least one loaded scene.
    [[nodiscard]] bool                      has(std::string const& component) const;
    /// Loaded scenes of a component, in order.
    [[nodiscard]] std::vector<Scene const*> scenesOf(std::string const& component) const;
    [[nodiscard]] Scene const*              scene(std::string const& id) const;
    [[nodiscard]] Component const*          component(std::string const& id) const;
    [[nodiscard]] std::vector<Tag> const&   tags() const { return mTags; }
    /// Tags whose items contain `component`.
    [[nodiscard]] std::vector<Tag const*>   tagsOf(std::string const& component) const;
    [[nodiscard]] Tag const*                tag(std::string const& id) const;
    /// Every component with loaded scenes, in index order.
    [[nodiscard]] std::vector<std::string>  components() const;
    [[nodiscard]] std::vector<Component> const& allComponents() const { return mComponents; }
    [[nodiscard]] std::size_t               sceneCount() const { return mScenes.size(); }
    [[nodiscard]] std::map<std::string, Scene> const& scenes() const { return mScenes; }

private:
    std::map<std::string, Scene> mScenes;
    std::vector<Component>       mComponents;
    std::vector<Tag>             mTags;
};

/// Normalises a user-typed component id: "shaft" -> "levitate:shaft"; ids with a namespace are kept.
[[nodiscard]] std::string normaliseId(std::string const& id);

/// Create's hold-to-ponder progress, once per tick: held, `p = min(1, p + max(0.25, p) * 0.25)`
/// (Ponder opens when it reaches 1); released, `p = max(0, p - 0.05)`.
[[nodiscard]] float holdProgressStep(float progress, bool held);

/// Greedy word wrap for text bubbles: splits `text` into lines no wider than `maxWidth` using
/// `width(s)`. Breaks at spaces; CJK text (no spaces) breaks between characters (UTF-8 aware).
template <class WidthFn>
std::vector<std::string> wrapText(std::string const& text, float maxWidth, WidthFn&& width);

/// UTF-8 code point boundaries of `s` (byte offsets, including 0 and s.size()).
[[nodiscard]] std::vector<std::size_t> utf8Boundaries(std::string const& s);

template <class WidthFn>
std::vector<std::string> wrapText(std::string const& text, float maxWidth, WidthFn&& width) {
    std::vector<std::string> lines;
    std::size_t              start = 0;
    while (start <= text.size()) {
        auto nl  = text.find('\n', start);
        auto end = nl == std::string::npos ? text.size() : nl;
        std::string para = text.substr(start, end - start);
        // break the paragraph
        std::string line;
        auto        bounds = utf8Boundaries(para);
        std::size_t lineStart = 0, lastSpace = std::string::npos;
        for (std::size_t i = 1; i < bounds.size(); ++i) {
            std::size_t cut = bounds[i];
            if (para[bounds[i - 1]] == ' ') lastSpace = bounds[i - 1];
            if (width(para.substr(lineStart, cut - lineStart)) > maxWidth && cut - lineStart > 1) {
                std::size_t brk = bounds[i - 1];
                if (lastSpace != std::string::npos && lastSpace > lineStart) brk = lastSpace;
                if (brk <= lineStart) brk = bounds[i - 1];
                lines.push_back(para.substr(lineStart, brk - lineStart));
                lineStart = brk;
                while (lineStart < para.size() && para[lineStart] == ' ') ++lineStart;
                lastSpace = std::string::npos;
            }
        }
        lines.push_back(para.substr(lineStart));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

} // namespace lv::core::ponder
