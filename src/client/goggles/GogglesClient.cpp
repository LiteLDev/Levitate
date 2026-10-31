#ifdef LV_CLIENT

// Client side of the goggles unit (U8):
// - the goggle overlay: while looking at a block the client asks the server for its Create
//   goggle / hover tooltip (`goggles.query`, every `overlayInterval` ticks and on every target
//   change) and draws it next to the crosshair like Create's GoggleOverlayRenderer (fade-in,
//   goggles icon, vanilla tooltip colours or the custom ones, `overlayOffsetX/Y`);
// - `/create overlay`: Create's GoggleConfigScreen (click / drag the sample to move the
//   overlay, Esc saves) and `/create overlay reset`;
// - item tooltips: Create's ItemDescription ("Hold [Shift] for Summary") and KineticStats
//   (stress impact / capacity) appended to the game's hover text of Levitate items.

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "ll/api/Config.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/safety/RedactableString.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/HitResult.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "core/goggles/Tooltip.h"
#include "feature/common/ValueBox.h"
#include "feature/goggles/GoggleInfo.h"
#include "feature/goggles/GogglesConfig.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/kinetics/KineticsConfig.h"
#include "feature/net/Net.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::goggles {

namespace {

namespace net = lv::feature::net;
namespace fmt = lv::core::goggles::fmt;
using feature::goggles::GogglesConfig;
using screen::Canvas;
using Json = nlohmann::json;

constexpr char const* kGogglesId = "levitate:goggles";
constexpr int         kKeyShift  = 16;
constexpr int         kKeyCtrl   = 17;
constexpr int         kKeyEscape = 27;

bool& configScreenOpen() {
    static bool open = false;
    return open;
}

GogglesConfig& cfg() { return config::mut<GogglesConfig>(); }

void saveConfig() {
    auto path = config::configDir() / "goggles.json";
    if (!ll::config::saveConfig(cfg(), path)) log().warn("goggles: could not save {}", path.string());
}

struct Input {
    bool shift = false, ctrl = false, leftDown = false;
};
Input& input() {
    static Input i;
    return i;
}

// ---- overlay state ---------------------------------------------------------------------------------

struct Overlay {
    std::optional<core::GlobalPos> target;   ///< block looked at
    std::optional<core::GlobalPos> shownFor; ///< block the lines belong to
    std::vector<std::string>       lines;
    std::string                    icon = kGogglesId;
    int                            cooldown   = 0;
    int                            hoverTicks = 0; ///< frames the panel has been shown (Create: per frame)
};
Overlay& overlay() {
    static Overlay o;
    return o;
}

/// Icon stacks per item id, for the current world only (each world has its own item registry).
std::unordered_map<std::string, ItemStack>& iconCache() {
    static auto* cache = new std::unordered_map<std::string, ItemStack>(); // never destroyed after the registry
    return *cache;
}

ItemStack const& iconStack(std::string const& id) {
    auto& cache = iconCache();
    auto  it    = cache.find(id);
    if (it == cache.end()) {
        ItemStack s;
        s.reinit(id, 1, 0);
        if (s.isNull()) {
            static ItemStack const empty;
            return empty; // not registered (yet): try again next frame
        }
        it = cache.emplace(id, std::move(s)).first;
    }
    return it->second;
}

bool lookingAtValueBox(LocalPlayer& player, HitResult const& hit) {
    auto&        region = player.getDimensionBlockSource();
    Block const& block  = region.getBlock(hit.mBlock);
    auto         id     = block.getTypeName();
    namespace c = lv::feature::common;
    if (!c::valuebox::boxesOf(id) && !c::filterslot::hasSlots(id)) return false;
    core::common::Vec local{hit.mPos.x - hit.mBlock.x, hit.mPos.y - hit.mBlock.y, hit.mPos.z - hit.mBlock.z};
    return c::valuebox::boxAt(block, hit.mFacing, local) || c::filterslot::slotAt(block, hit.mFacing, local);
}

int& pendingScreen() {
    static int ticks = 0;
    return ticks;
}

void openConfigScreen();

void pollTarget() {
    if (pendingScreen() > 0 && --pendingScreen() == 0) openConfigScreen();
    auto& o      = overlay();
    auto* player = currentPlayer();
    if (o.cooldown > 0) --o.cooldown;
    if (!player || !net::client::connected() || player->isSpectator()) {
        o.target.reset();
        return;
    }
    auto const& hit = player->getLevel().getHitResult();
    if (hit.mType != HitResultType::Tile || lookingAtValueBox(*player, hit)) {
        o.target.reset();
        return;
    }
    core::GlobalPos pos{static_cast<int>(player->getDimensionId()), hit.mBlock.x, hit.mBlock.y, hit.mBlock.z};
    bool            changed = o.target != pos;
    o.target                = pos;
    if (!changed && o.cooldown > 0) return;
    o.cooldown = std::max(1, cfg().overlayInterval);
    net::client::send("goggles.query", {{"pos", Json::array({pos.dim, pos.x, pos.y, pos.z})}});
}

// ---- drawing (Create's RemovedGuiUtils.drawHoveringText) --------------------------------------------

unsigned scaleAlpha(unsigned argb, float f) {
    auto a = static_cast<unsigned>(std::clamp(static_cast<float>(argb >> 24) * f, 0.f, 255.f));
    return (a << 24) | (argb & 0xFFFFFFu);
}

unsigned mix(unsigned a, unsigned b, float t) {
    auto ch = [&](int s) {
        float x = static_cast<float>((a >> s) & 0xFF), y = static_cast<float>((b >> s) & 0xFF);
        return static_cast<unsigned>(std::lround(x + (y - x) * t)) << s;
    };
    return ch(24) | ch(16) | ch(8) | ch(0);
}

void gradient(Canvas& c, float x, float y, float w, float h, unsigned top, unsigned bottom) {
    constexpr int kSteps = 8;
    for (int i = 0; i < kSteps; ++i) {
        float y0 = y + h * static_cast<float>(i) / kSteps, y1 = y + h * static_cast<float>(i + 1) / kSteps;
        c.fill({x, y0, w, y1 - y0}, mix(top, bottom, (static_cast<float>(i) + 0.5f) / kSteps));
    }
}

struct Colors {
    unsigned background, borderTop, borderBottom;
};

Colors overlayColors() {
    auto const& c = cfg();
    if (c.customColorsOverlay) {
        return {static_cast<unsigned>(c.customBackgroundOverlay), static_cast<unsigned>(c.customBorderTopOverlay), static_cast<unsigned>(c.customBorderBotOverlay)};
    }
    // BoxElement.COLOR_VANILLA_BACKGROUND (scaled to 75 % alpha) and COLOR_VANILLA_BORDER
    return {scaleAlpha(0xF0100010u, .75f), 0x505000FFu, 0x5028007Fu};
}

/// Bedrock's formatting colours (§0..§f and the material codes).
std::optional<unsigned> formatColor(char code) {
    switch (code) {
    case '0': return 0xFF000000u;
    case '1': return 0xFF0000AAu;
    case '2': return 0xFF00AA00u;
    case '3': return 0xFF00AAAAu;
    case '4': return 0xFFAA0000u;
    case '5': return 0xFFAA00AAu;
    case '6': return 0xFFFFAA00u;
    case '7': return 0xFFAAAAAAu;
    case '8': return 0xFF555555u;
    case '9': return 0xFF5555FFu;
    case 'a': return 0xFF55FF55u;
    case 'b': return 0xFF55FFFFu;
    case 'c': return 0xFFFF5555u;
    case 'd': return 0xFFFF55FFu;
    case 'e': return 0xFFFFFF55u;
    case 'f': return 0xFFFFFFFFu;
    case 'g': return 0xFFDDD605u;
    case 'h': return 0xFFE3D4D1u;
    case 'i': return 0xFFCECACAu;
    case 'j': return 0xFF443A3Bu;
    case 'm': return 0xFF971607u;
    case 'n': return 0xFFB4684Du;
    case 'p': return 0xFFDEB12Du;
    case 'q': return 0xFF47A036u;
    case 's': return 0xFF2CBAA8u;
    case 't': return 0xFF21497Bu;
    case 'u': return 0xFF9A5CC6u;
    case 'v': return 0xFFEB7114u;
    default: return std::nullopt;
    }
}

/// Draws text with formatting codes ("§7gray §fwhite"): the UI text call prints codes literally.
/// `§~` toggles Create's strikethrough (the server marks overstressed speed text with it).
void formattedText(Canvas& c, float x, float y, std::string const& text, unsigned base, float alpha) {
    static std::string const kSection = "§";
    unsigned                 color    = base;
    bool                     strike   = false;
    std::string              segment;
    auto flush = [&] {
        if (segment.empty()) return;
        c.text(x, y, segment, scaleAlpha(color, alpha));
        float w = c.textWidth(segment);
        // the line through the middle of the glyphs, with the text's shadow below it
        if (strike) {
            c.fill({x, y + 3.5f, w, 1}, scaleAlpha(color, alpha));
            c.fill({x + 1, y + 4.5f, w, 1}, scaleAlpha(0xFF000000u | ((color >> 2) & 0x3F3F3Fu), alpha));
        }
        x += w;
        segment.clear();
    };
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, kSection.size(), kSection) == 0 && i + kSection.size() < text.size()) {
            flush();
            char code = text[i + kSection.size()];
            if (code == 'r') color = base, strike = false;
            else if (code == '~') strike = !strike;
            else if (auto col = formatColor(code)) color = *col;
            i += kSection.size() + 1;
            continue;
        }
        segment += text[i++];
    }
    flush();
}

float lineWidth(Canvas& c, std::string const& line) { return c.textWidth(core::goggles::stripFormatting(line)); }

float tooltipHeight(std::size_t lines) { return lines > 1 ? 8.f + 2.f + static_cast<float>(lines - 1) * 10.f : 8.f; }

void drawHoveringText(Canvas& c, std::vector<std::string> const& lines, float posX, float posY, Colors col, float alpha = 1) {
    auto [sw, sh] = screen::guiSize();
    float w       = 0;
    for (auto const& l : lines) w = std::max(w, lineWidth(c, l));
    float x = posX + 12;
    if (x + w + 4 > sw) x = posX - 16 - w;
    float h = tooltipHeight(lines.size());
    float y = posY - 12;
    if (y < 4) y = 4;
    else if (y + h + 4 > sh) y = sh - h - 4;

    c.fill({x - 3, y - 4, w + 6, 1}, col.background);
    c.fill({x - 3, y + h + 3, w + 6, 1}, col.background);
    c.fill({x - 3, y - 3, w + 6, h + 6}, col.background);
    c.fill({x - 4, y - 3, 1, h + 6}, col.background);
    c.fill({x + w + 3, y - 3, 1, h + 6}, col.background);
    gradient(c, x - 3, y - 2, 1, h + 4, col.borderTop, col.borderBottom);
    gradient(c, x + w + 2, y - 2, 1, h + 4, col.borderTop, col.borderBottom);
    c.fill({x - 3, y - 3, w + 6, 1}, col.borderTop);
    c.fill({x - 3, y + h + 2, w + 6, 1}, col.borderBottom);

    float ty = y;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].empty()) formattedText(c, x, ty - 1, lines[i], 0xFFFFFFFFu, alpha);
        if (i == 0) ty += 2;
        ty += 10;
    }
}

void renderOverlay(MinecraftUIRenderContext& ctx) {
    auto& o = overlay();
    if (!o.target || !o.shownFor || *o.target != *o.shownFor || o.lines.empty()) {
        o.hoverTicks = 0;
        return;
    }
    ++o.hoverTicks;
    auto [sw, sh] = screen::guiSize();
    Canvas c(ctx);
    float  textW = 0;
    for (auto const& l : o.lines) textW = std::max(textW, lineWidth(c, l));
    float posX = sw / 2 + static_cast<float>(cfg().overlayOffsetX);
    float posY = sh / 2 + static_cast<float>(cfg().overlayOffsetY);
    posX       = std::min(posX, sw - textW - 20);
    posY       = std::min(posY, sh - tooltipHeight(o.lines.size()) - 20);

    float fade = std::clamp(static_cast<float>(o.hoverTicks) / 24.f, 0.f, 1.f);
    auto  col  = overlayColors();
    if (fade < 1) {
        float sign = cfg().overlayOffsetX + .5f >= 0 ? 1.f : -1.f;
        posX += static_cast<float>(std::pow(1 - fade, 3)) * sign * 8;
        col.background   = scaleAlpha(col.background, fade);
        col.borderTop    = scaleAlpha(col.borderTop, fade);
        col.borderBottom = scaleAlpha(col.borderBottom, fade);
    }
    drawHoveringText(c, o.lines, posX, posY, col, std::max(fade, 0.05f));
    c.item(iconStack(o.icon), posX + 10, posY - 16);
}

// ---- /create overlay: Create's GoggleConfigScreen ---------------------------------------------------

class OverlayConfigScreen : public screen::Screen {
public:
    OverlayConfigScreen() : Screen("", 0, 0) {
        std::string const sp = "    ";
        mTooltip             = {
            sp + ctr("create.gui.config.overlay1", "Hi :)"),
            sp + fmt::kGray + ctr("create.gui.config.overlay2", "This is a sample overlay"),
            "",
            sp + ctr("create.gui.config.overlay3", "Click or drag with your mouse"),
            sp + ctr("create.gui.config.overlay4", "to move this preview"),
            "",
            sp + fmt::kGray + ctr("create.gui.config.overlay5", "Press ESC to exit this screen"),
            sp + fmt::kGray + ctr("create.gui.config.overlay6", "and save the new position"),
            "",
            sp + ctr("create.gui.config.overlay7", "Run /create overlay reset"),
            sp + ctr("create.gui.config.overlay8", "to reset to the default position"),
        };
        mOffsetX = cfg().overlayOffsetX;
        mOffsetY = cfg().overlayOffsetY;
        configScreenOpen() = true;
    }

    // the panel has no size: its origin is the screen centre, so panel coordinates are offsets
    void renderExtra(Canvas& c, float mx, float my) override {
        if (input().leftDown) updateOffset(c, mx, my);
        auto [sw, sh] = screen::guiSize();
        c.setOrigin(0, 0);
        float posX = sw / 2 + static_cast<float>(mOffsetX), posY = sh / 2 + static_cast<float>(mOffsetY);
        drawHoveringText(c, mTooltip, posX, posY, {0xF0100010u, 0x505000FFu, 0x5028007Fu});
        c.item(iconStack(kGogglesId), posX + 10, posY - 16);
    }

    bool clickBackground(float mx, float my, screen::Button b, bool) override {
        if (b != screen::Button::Left) return false;
        mPendingClick = {mx, my};
        return true;
    }

    void tick() override {}

    void onClose() override {
        configScreenOpen() = false;
        cfg().overlayOffsetX = mOffsetX;
        cfg().overlayOffsetY = mOffsetY;
        saveConfig();
        log().info("goggles: overlay offset set to {}, {}", mOffsetX, mOffsetY);
    }

private:
    void updateOffset(Canvas& c, float mx, float my) {
        auto [sw, sh] = screen::guiSize();
        float w       = 0;
        for (auto const& l : mTooltip) w = std::max(w, lineWidth(c, l));
        float h  = 8 + static_cast<float>(mTooltip.size() - 1) * 10 + 2;
        mOffsetX = static_cast<int>(std::clamp(mx, -(sw / 2) - 5, std::max(-(sw / 2) - 5, (sw / 2) - w - 20)));
        mOffsetY = static_cast<int>(std::clamp(my, -(sh / 2) + 17, std::max(-(sh / 2) + 17, (sh / 2) - h + 5)));
    }

public:
    void applyPendingClick(Canvas& c) {
        if (!mPendingClick) return;
        updateOffset(c, mPendingClick->first, mPendingClick->second);
        mPendingClick.reset();
    }

private:
    std::vector<std::string>               mTooltip;
    int                                    mOffsetX = 0, mOffsetY = 0;
    std::optional<std::pair<float, float>> mPendingClick;
};

/// Applies a click of the config screen on the next frame (the clamp needs text widths).
class ClickApplier : public screen::Widget {
public:
    explicit ClickApplier(OverlayConfigScreen& s) : Widget({0, 0, 0, 0}), mScreen(s) {}
    void render(Canvas& c, float, float) override { mScreen.applyPendingClick(c); }

private:
    OverlayConfigScreen& mScreen;
};

void openConfigScreen() {
    auto s = std::make_unique<OverlayConfigScreen>();
    s->add<ClickApplier>(*s);
    screen::open(std::move(s));
}

// ---- item tooltips (Create's ItemDescription + KineticStats) ------------------------------------------

bool wearingGoggles() {
    auto* p = currentPlayer();
    if (!p) return false;
    auto const& head = p->getArmor(SharedTypes::Legacy::ArmorSlot::Head);
    return !head.isNull() && head.getTypeName() == kGogglesId;
}

/// Create's tooltip translation key: `item.create.<path>.tooltip` (or `block.create...`).
std::optional<std::string> descriptionKey(std::string const& id) {
    auto path = id.substr(id.find(':') + 1);
    // ItemDescription.useKey: the four cardboard armour pieces share item.create.cardboard_armor (U49)
    if (path == "cardboard_helmet" || path == "cardboard_chestplate" || path == "cardboard_leggings" || path == "cardboard_boots") path = "cardboard_armor";
    // ... and the dyed toolboxes, seats and postboxes share block.create.toolbox / seat / package_postbox (U59)
    else if (path.ends_with("_toolbox")) path = "toolbox";
    else if (path.ends_with("_seat")) path = "seat";
    else if (path.ends_with("_postbox")) path = "package_postbox";
    for (auto const* prefix : {"item.create.", "block.create."}) {
        auto key = std::string(prefix) + path + ".tooltip";
        if (!translateClient(key + ".summary", "").empty()) return key;
    }
    return std::nullopt;
}

std::vector<std::string> descriptionLines(std::string const& id) {
    auto key = descriptionKey(id);
    if (!key) return {};
    core::goggles::DescriptionText t;
    t.summary.push_back(translateClient(*key + ".summary", ""));
    for (int i = 1; i < 100; ++i) {
        auto cond = translateClient(*key + ".condition" + std::to_string(i), "");
        if (cond.empty()) break;
        t.behaviours.emplace_back(cond, translateClient(*key + ".behaviour" + std::to_string(i), ""));
    }
    for (int i = 1; i < 100; ++i) {
        auto control = translateClient(*key + ".control" + std::to_string(i), "");
        if (control.empty()) break;
        t.actions.emplace_back(control, translateClient(*key + ".action" + std::to_string(i), ""));
    }
    t.holdForDescription = translateClient("create.tooltip.holdForDescription", "Hold [{}] for Summary");
    t.holdForControls    = translateClient("create.tooltip.holdForControls", "Hold [{}] for Controls");
    t.keyShift           = translateClient("create.tooltip.keyShift", "Shift");
    t.keyCtrl            = translateClient("create.tooltip.keyCtrl", "Ctrl");
    return core::goggles::describeItem(t, input().shift, input().ctrl);
}

std::vector<std::string> kineticStats(std::string const& id) {
    std::vector<std::string> out;
    if (!feature::kinetics::specOf(id)) return out;
    auto const& k        = config::get<feature::kinetics::KineticsConfig>();
    if (k.disableStress) return out;
    bool        goggles  = wearingGoggles();
    double      impact   = feature::kinetics::stressImpactOf(id);
    double      capacity = feature::kinetics::stressCapacityOf(id);
    auto        rpm      = ctr("create.generic.unit.rpm", "RPM");
    char const* absolute[] = {fmt::kYellow, fmt::kGold, fmt::kRed}; // StressImpact LOW / MEDIUM / HIGH
    char const* names[]    = {"low", "medium", "high"};

    if (impact > 0 && !feature::goggles::hidesStressImpact(id)) {
        out.push_back(std::string(fmt::kGray) + ctr("create.tooltip.stressImpact", "Kinetic Stress Impact:"));
        int  level = impact >= k.highStressImpact ? 2 : impact >= k.mediumStressImpact ? 1 : 0;
        auto line  = std::string(absolute[level]) + core::goggles::progressBar(3, level + 1) + fmt::kWhite;
        char const* english[] = {"Low", "Moderate", "High"};
        if (goggles) line += core::goggles::formatNumber(impact) + "x " + rpm;
        else line += ctr(std::string("create.tooltip.stressImpact.") + names[level], english[level]);
        out.push_back(line);
    }
    if (capacity > 0) {
        out.push_back(std::string(fmt::kGray) + ctr("create.tooltip.capacityProvided", "Kinetic Stress Capacity:"));
        int  level    = capacity >= k.highCapacity ? 2 : capacity >= k.mediumCapacity ? 1 : 0;
        int  opposite = 2 - level;
        auto line     = std::string(absolute[opposite]) + core::goggles::progressBar(3, level + 1) + fmt::kWhite;
        char const* english[] = {"Small", "Medium", "Large"};
        if (goggles) {
            line += core::goggles::formatNumber(capacity) + "x " + rpm;
            out.push_back(line);
            if (auto gen = feature::goggles::generatorSpeedOf(id)) {
                auto amount = core::goggles::formatNumber(capacity * gen->rpm) + ctr("create.generic.unit.stress", "su");
                out.push_back(
                    std::string(fmt::kDarkGray) + " -> " + (gen->mayGenerateLess ? ctr("create.tooltip.up_to", "Up to {}", amount) : amount)
                );
            }
        } else {
            out.push_back(line + ctr(std::string("create.tooltip.capacityProvided.") + names[level], english[level]));
        }
    }
    return out;
}

struct ExtraLines {
    std::vector<std::string> insert, stats;
};

/// Built lines per (id, locale, Shift, Ctrl, goggles, tooltips on): the game asks for the hover
/// text often while an item is hovered.
std::unordered_map<std::string, ExtraLines>& hovertextCache() {
    static auto* cache = new std::unordered_map<std::string, ExtraLines>();
    return *cache;
}

ExtraLines const& extraLines(std::string const& id) {
    auto key = id + "|" + clientLocale() + "|" + (input().shift ? "s" : "") + (input().ctrl ? "c" : "") + (wearingGoggles() ? "g" : "")
             + (cfg().enableTooltips ? "t" : "");
    auto& cache = hovertextCache();
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    if (cache.size() > 512) cache.clear();
    ExtraLines e;
    if (cfg().enableTooltips) e.insert = descriptionLines(id);
    e.stats = kineticStats(id);
    return cache.emplace(key, std::move(e)).first->second;
}

void extendHovertext(std::string& text, std::string const& id) {
    auto const& extra  = extraLines(id);
    auto const& insert = extra.insert;
    auto const& stats  = extra.stats;
    if (insert.empty() && stats.empty()) return;
    auto        nl   = text.find('\n');
    std::string head = nl == std::string::npos ? text : text.substr(0, nl);
    std::string rest = nl == std::string::npos ? std::string{} : text.substr(nl);
    std::string out  = head;
    for (auto const& l : insert) out += "\n" + std::string(fmt::kReset) + l;
    out += rest;
    if (!stats.empty()) {
        out += "\n";
        for (auto const& l : stats) out += "\n" + std::string(fmt::kReset) + l;
    }
    text = std::move(out);
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    GogglesHovertextHook,
    ll::memory::HookPriority::Normal,
    ItemStackBase,
    &ItemStackBase::getFormattedHovertext,
    Bedrock::Safety::RedactableString,
    Level& level,
    bool   showCategory
) {
    auto r = origin(level, showCategory);
    if (isNull()) return r;
    auto id = getTypeName();
    if (!id.starts_with("levitate:")) return r;
    extendHovertext(r.mUnredactedString, id);
    if (r.mRedactedString) extendHovertext(*r.mRedactedString, id);
    return r;
}

class GogglesClientModule {
public:
    // Client input / render listeners are added at mod load (see ValueBoxClient.cpp).
    void install() {}
    void load() {
        net::client::on("goggles.info", [](Json const& msg) {
            auto& o   = overlay();
            auto  pos = msg.value("pos", Json());
            if (!pos.is_array() || pos.size() != 4) return;
            core::GlobalPos p{pos[0].get<int>(), pos[1].get<int>(), pos[2].get<int>(), pos[3].get<int>()};
            std::vector<std::string> lines;
            for (auto const& l : msg.value("lines", Json::array())) lines.push_back(l.get<std::string>());
            if (o.shownFor != p || lines.empty()) o.hoverTicks = 0;
            o.shownFor = p;
            o.lines    = std::move(lines);
            o.icon     = msg.value("icon", std::string(kGogglesId));
        });
        net::client::on("goggles.overlay", [](Json const& msg) {
            auto action = msg.value("action", std::string{});
            if (action == "reset") {
                // Create's OverlayConfigCommand: both offsets go to 0
                cfg().overlayOffsetX = 0;
                cfg().overlayOffsetY = 0;
                saveConfig();
                log().info("goggles: overlay offset reset");
            } else if (action == "screen") {
                // after the chat screen that ran the command has closed (the screen framework hosts
                // its screen above the scene that is on top when it opens)
                pendingScreen() = 10;
            }
        });
        // the server's stress tables (Create syncs its stress config): item tooltips use them
        net::client::on("kinetics.stress", [](Json const& msg) {
            feature::kinetics::setSyncedStressTables(msg);
            hovertextCache().clear();
            log().info("goggles client: stress tables from the server ({} impacts)", msg.value("impact", Json::object()).size());
        });
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&) { pollTarget(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            overlay() = Overlay{};
            iconCache().clear();
            hovertextCache().clear();
            feature::kinetics::clearSyncedStressTables();
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::KeyInputEvent>([](ll::event::KeyInputEvent& ev) {
            if (ev.keyCode() == kKeyShift) input().shift = ev.isDown();
            if (ev.keyCode() == kKeyCtrl) input().ctrl = ev.isDown();
            // Esc saves and closes the position screen (Create's GoggleConfigScreen.removed)
            if (ev.keyCode() == kKeyEscape && ev.isDown() && configScreenOpen() && screen::current()) screen::close();
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::MouseInputEvent>([](ll::event::MouseInputEvent& ev) {
            if (ev.actionButtonId() == 1) input().leftDown = ev.buttonData() == 1;
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& ev) {
            if (screen::current() || ev.screenView().getScreenName() != "hud_screen") return;
            renderOverlay(ev.uiRenderContext());
        }));
        GogglesHovertextHook::hook();
    }
    void uninstall() {
        GogglesHovertextHook::unhook();
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};

} // namespace lv::client::goggles

LV_REGISTER_MODULE(lv::client::goggles::GogglesClientModule);

#endif // LV_CLIENT
