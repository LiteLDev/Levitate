// Engineer's goggles overlay, server side (U8): builds Create's goggle / hover tooltip of the
// block a player looks at. Players with the client mod ask for it (`goggles.query`) and draw it
// next to the crosshair; vanilla players get it in the action bar.

#include "feature/goggles/GoggleInfo.h"

#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/network/packet/SetTitlePacket.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/HitResult.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/goggles/GogglesConfig.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/kinetics/KineticsConfig.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/Commands.h"
#include "mod/Config.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("goggles", lv::feature::goggles::GogglesConfig);

namespace lv::feature::goggles {

namespace fmt = core::goggles::fmt;
using Json    = nlohmann::json;

namespace {

constexpr char const* kGogglesId   = "levitate:goggles";
constexpr double      kMaxQueryRange = 12.0;

/// Create's AllBlocks `BlockStressValues.setGeneratorSpeed` calls (blocks of U7, by id).
std::unordered_map<std::string, GeneratorSpeed> presetGeneratorSpeeds() {
    std::unordered_map<std::string, GeneratorSpeed> m{
        {"levitate:hand_crank",          {32, false} },
        {"levitate:copper_valve_handle", {32, false} },
        {"levitate:water_wheel",         {8, false}  },
        {"levitate:large_water_wheel",   {4, false}  },
        {"levitate:creative_motor",      {256, true} },
    };
    for (auto const* c : {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray", "light_gray", "cyan",
                          "purple", "blue", "brown", "green", "red", "black"}) {
        m[std::string("levitate:") + c + "_valve_handle"] = {32, false};
    }
    return m;
}

struct Registry {
    std::unordered_map<std::string, std::vector<InfoProvider>> providers;
    std::vector<std::function<bool(Player const&)>>            wearing;
    std::unordered_map<std::string, SpeedLevel>                speedRequirement;
    std::unordered_set<std::string>                            hideImpact;
    std::unordered_map<std::string, GeneratorSpeed>            generatorSpeed = presetGeneratorSpeeds();
};
Registry& reg() {
    static Registry r;
    return r;
}

kinetics::KineticsConfig const& kcfg() { return config::get<kinetics::KineticsConfig>(); }

bool stressEnabled() { return !kcfg().disableStress; } // Create's StressImpact.isEnabled

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

char const* speedLevelName(SpeedLevel l) {
    switch (l) {
    case SpeedLevel::Slow:
        return "slow";
    case SpeedLevel::Medium:
        return "medium";
    case SpeedLevel::Fast:
        return "fast";
    default:
        return "none";
    }
}

/// English fallbacks of create.tooltip.speedRequirement.* / stressImpact.*.
char const* speedLevelEnglish(SpeedLevel l) {
    switch (l) {
    case SpeedLevel::Slow:
        return "Slow";
    case SpeedLevel::Medium:
        return "Moderate";
    case SpeedLevel::Fast:
        return "Fast";
    default:
        return "None";
    }
}

/// A readable name from an id when no translation is known ("levitate:fluid_tank" -> "Fluid Tank").
std::string prettyId(std::string_view id) {
    auto        colon = id.find(':');
    std::string path(colon == std::string_view::npos ? id : id.substr(colon + 1));
    bool        start = true;
    for (auto& c : path) {
        if (c == '_') {
            c     = ' ';
            start = true;
        } else if (start) {
            c     = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            start = false;
        }
    }
    return path;
}

std::string stackName(ItemStack const& stack) {
    auto name = common::items::displayName(stack);
    if (name.empty() || name.find(".name") != std::string::npos) return prettyId(common::items::idOf(stack));
    return name;
}

std::string blockName(BlockSource& region, BlockPos const& pos, Block const& block) {
    auto item = common::items::make(block.getTypeName());
    if (item) return stackName(*item);
    (void)region;
    (void)pos;
    return prettyId(block.getTypeName());
}

std::string fluidName(InfoContext const& ctx, std::string const& fluid) { return ctx.tr("fluid." + fluid, prettyId(fluid)); }

std::string heatName(InfoContext const& ctx, core::common::HeatLevel h) {
    auto name = std::string(core::common::heatLevelName(h));
    return ctx.tr("lv.goggles.heat." + lower(name), prettyId(name));
}

bool isLevitate(std::string_view id) { return id.starts_with("levitate:"); }

/// Items held by the block (Levitate blocks with an item capability).
bool itemTooltip(InfoContext const& ctx, Tooltip& t) {
    common::Access at{ctx.region, game::blockPos(ctx.pos), -1};
    auto           handler = common::items::find(at);
    if (!handler || !handler->slots || !handler->getSlot) return false;
    int                                     slots = handler->slots(at);
    std::vector<std::pair<std::string, int>> stacks; // name, count (merged by name)
    for (int i = 0; i < slots; ++i) {
        auto s = handler->getSlot(at, i);
        if (common::items::isEmpty(s)) continue;
        auto name = stackName(s);
        auto it   = std::find_if(stacks.begin(), stacks.end(), [&](auto const& p) { return p.first == name; });
        if (it == stacks.end()) stacks.emplace_back(name, common::items::countOf(s));
        else it->second += common::items::countOf(s);
    }
    if (stacks.empty()) return false;
    t.push_back(line(ctx.tr("lv.goggles.item_contents", "Contents:")));
    constexpr std::size_t kMaxShown = 8;
    for (std::size_t i = 0; i < stacks.size() && i < kMaxShown; ++i) {
        t.push_back(line(std::string(fmt::kGray) + stacks[i].first + fmt::kGreen + " x" + std::to_string(stacks[i].second), 1));
    }
    if (stacks.size() > kMaxShown) {
        t.push_back(line(std::string(fmt::kDarkGray) + ctx.tr("lv.goggles.more_items", "...and {} more", stacks.size() - kMaxShown), 1));
    }
    return true;
}

bool heatTooltip(InfoContext const& ctx, Tooltip& t) {
    auto h = common::heat::level(ctx.region, game::blockPos(ctx.pos));
    if (h == core::common::HeatLevel::None) return false;
    t.push_back(line(ctx.tr("lv.goggles.heat", "Heat") + ": " + fmt::kGold + heatName(ctx, h)));
    return true;
}

bool defaultGoggle(InfoContext const& ctx, Tooltip& t) {
    bool added = kineticGoggleTooltip(ctx, t);
    for (auto fn : {&fluidTooltip, &itemTooltip, &heatTooltip}) {
        Tooltip extra;
        if (!fn(ctx, extra)) continue;
        if (added) t.emplace_back();
        t.insert(t.end(), extra.begin(), extra.end());
        added = true;
    }
    return added;
}

void actionBar(Player& player, std::string const& text) {
    SetTitlePacket pkt(SetTitlePacketPayload::TitleType::Actionbar, text, std::nullopt);
    pkt.sendTo(player);
}

/// The action bar has no indentation: lines are trimmed (formatting codes kept).
std::string actionBarText(Tooltip const& lines) {
    std::string out;
    for (auto const& l : lines) {
        auto start = l.find_first_not_of(' ');
        if (!out.empty()) out += "\n";
        out += fmt::kReset;
        if (start != std::string::npos) out += l.substr(start);
    }
    return out;
}

Json posJson(core::GlobalPos const& p) { return Json::array({p.dim, p.x, p.y, p.z}); }

std::optional<core::GlobalPos> posFrom(Json const& j) {
    if (!j.is_array() || j.size() != 4) return std::nullopt;
    for (auto const& v : j)
        if (!v.is_number_integer()) return std::nullopt;
    return core::GlobalPos{j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
}

} // namespace

// ---- registry ------------------------------------------------------------------------------------

void registerBlock(std::string blockId, InfoProvider provider) { reg().providers[std::move(blockId)].push_back(std::move(provider)); }

void addWearingPredicate(std::function<bool(Player const&)> predicate) { reg().wearing.push_back(std::move(predicate)); }

bool isWearingGoggles(Player const& player) {
    auto const& head = player.getArmor(SharedTypes::Legacy::ArmorSlot::Head);
    if (!head.isNull() && head.getTypeName() == kGogglesId) return true;
    for (auto const& p : reg().wearing)
        if (p(player)) return true;
    return false;
}

void setSpeedRequirement(std::string blockId, SpeedLevel level) { reg().speedRequirement[std::move(blockId)] = level; }

SpeedLevel speedRequirementOf(std::string_view blockId) {
    auto it = reg().speedRequirement.find(std::string(blockId));
    return it == reg().speedRequirement.end() ? SpeedLevel::None : it->second;
}

void setHideStressImpact(std::string blockId, bool hide) {
    if (hide) reg().hideImpact.insert(std::move(blockId));
    else reg().hideImpact.erase(blockId);
}

bool hidesStressImpact(std::string_view blockId) { return reg().hideImpact.contains(std::string(blockId)); }

void setGeneratorSpeed(std::string blockId, double rpm, bool mayGenerateLess) {
    reg().generatorSpeed[std::move(blockId)] = {rpm, mayGenerateLess};
}

std::optional<GeneratorSpeed> generatorSpeedOf(std::string_view blockId) {
    auto it = reg().generatorSpeed.find(std::string(blockId));
    if (it == reg().generatorSpeed.end()) return std::nullopt;
    return it->second;
}

// ---- helpers -------------------------------------------------------------------------------------

void addHint(Tooltip& tooltip, std::string const& text) {
    for (auto const& l : core::goggles::cutText(text, fmt::kGray, fmt::kWhite)) tooltip.push_back(line(l));
}

std::string formattedSpeed(Player const& viewer, float speed, bool overstressed) {
    auto        level = kinetics::speedLevelOf(speed);
    char const* color = overstressed ? fmt::kDarkGray
                      : level == SpeedLevel::Slow   ? fmt::kGreen
                      : level == SpeedLevel::Medium ? fmt::kAqua
                      : level == SpeedLevel::Fast   ? fmt::kPurple
                                                    : fmt::kDarkGray;
    return std::string(color) + core::goggles::progressBar(3, static_cast<int>(level))
         + trFor(viewer, std::string("create.tooltip.speedRequirement.") + speedLevelName(level), speedLevelEnglish(level)) + " ("
         + number(std::fabs(speed)) + " " + trFor(viewer, "create.generic.unit.rpm", "RPM") + ") ";
}

std::string formattedStress(Player const& viewer, double percent) {
    int         level = percent > 1 ? 3 : percent > .75 ? 2 : percent > .5 ? 1 : 0; // StressImpact.of
    char const* names[]   = {"low", "medium", "high", "overstressed"};
    char const* english[] = {"Low", "Moderate", "High", "Overstressed"};
    char const* colors[]  = {fmt::kGreen, fmt::kYellow, fmt::kGold, fmt::kRed}; // relative colours
    return std::string(colors[level]) + core::goggles::progressBar(3, std::min(level + 1, 3))
         + trFor(viewer, std::string("create.tooltip.stressImpact.") + names[level], english[level]) + " ("
         + std::to_string(static_cast<int>(percent * 100)) + "%) ";
}

bool kineticGoggleTooltip(InfoContext const& ctx, Tooltip& t) {
    if (!stressEnabled()) return false;
    auto node = kinetics::nodeAt(ctx.pos);
    if (!node) return false;
    auto su    = ctx.tr("create.generic.unit.stress", "su");
    auto atCur = std::string(" ") + fmt::kDarkGray + ctx.tr("create.gui.goggles.at_current_speed", "at current speed");
    bool added = false;
    // KineticBlockEntity.addToGoggleTooltip
    if (std::fabs(node->impact) > 1e-5) {
        t.push_back(line(ctx.tr("create.gui.goggles.kinetic_stats", "Kinetic Stats:")));
        t.push_back(line(std::string(fmt::kGray) + ctx.tr("create.tooltip.stressImpact", "Kinetic Stress Impact:")));
        double total = node->impact * std::fabs(node->theoreticalSpeed);
        t.push_back(line(std::string(fmt::kAqua) + number(total) + su + atCur, 1));
        added = true;
    }
    // GeneratingKineticBlockEntity.addToGoggleTooltip
    if (node->generator && std::fabs(node->capacity) > 1e-5) {
        double base  = node->capacity;
        float  speed = node->theoreticalSpeed;
        if (speed != node->generatedSpeed && speed != 0) base *= node->generatedSpeed / speed;
        double total = std::fabs(base * speed);
        t.push_back(line(ctx.tr("create.gui.goggles.generator_stats", "Generator Stats:")));
        t.push_back(line(std::string(fmt::kGray) + ctx.tr("create.tooltip.capacityProvided", "Kinetic Stress Capacity:")));
        t.push_back(line(std::string(fmt::kAqua) + number(total) + su + atCur, 1));
        added = true;
    }
    return added;
}

bool kineticHoverTooltip(InfoContext const& ctx, Tooltip& t) {
    auto node = kinetics::nodeAt(ctx.pos);
    if (!node) return false;
    // KineticBlockEntity.addToTooltip
    if (node->overstressed && config::get<GogglesConfig>().enableOverstressedTooltip) {
        t.push_back(line(std::string(fmt::kGold) + ctx.tr("create.gui.stressometer.overstressed", "Overstressed")));
        addHint(
            t,
            ctx.tr(
                "create.gui.contraptions.network_overstressed",
                "It appears that this contraption is _overstressed_. Add more sources or _slow_ _down_ the components with a high _stress_ _impact_."
            )
        );
        return true;
    }
    auto  required = speedRequirementOf(ctx.block.getTypeName());
    float need     = required == SpeedLevel::Fast ? kcfg().fastSpeed : required == SpeedLevel::Medium ? kcfg().mediumSpeed : required == SpeedLevel::Slow ? 1.f : 0.f;
    float speed    = node->speed;
    if (speed != 0 && std::fabs(speed) < need) {
        t.push_back(line(std::string(fmt::kGold) + ctx.tr("create.tooltip.speedRequirement", "Speed Requirement:")));
        auto name = blockName(ctx.region, game::blockPos(ctx.pos), ctx.block);
        addHint(t, ctx.tr("create.gui.contraptions.not_fast_enough", "It appears that this {} is _not_ rotating with _enough_ _speed_.", name));
        return true;
    }
    return false;
}

bool fluidTooltip(InfoContext const& ctx, Tooltip& t) {
    common::Access at{ctx.region, game::blockPos(ctx.pos), -1};
    auto           handler = common::fluids::find(at);
    if (!handler || !handler->tanks) return false;
    auto tanks = handler->tanks(at);
    if (tanks.empty()) return false;
    auto mb = ctx.tr("create.generic.unit.millibuckets", "mB");
    t.push_back(line(ctx.tr("create.gui.goggles.fluid_container", "Fluid Container Info:")));
    bool empty = true;
    for (auto const& tank : tanks) {
        if (tank.contents.empty()) continue;
        t.push_back(line(std::string(fmt::kGray) + fluidName(ctx, tank.contents.fluid), 1));
        t.push_back(line(
            std::string(fmt::kGold) + number(tank.contents.amount) + mb + fmt::kGray + " / " + fmt::kDarkGray + number(tank.capacity) + mb,
            1
        ));
        empty = false;
    }
    if (tanks.size() > 1) {
        if (empty) t.pop_back();
        return true;
    }
    if (!empty) return true;
    t.push_back(line(
        std::string(fmt::kGray) + ctx.tr("create.gui.goggles.fluid_container.capacity", "Capacity: ") + fmt::kGold
            + number(tanks[0].capacity) + mb,
        1
    ));
    return true;
}

// ---- the overlay ---------------------------------------------------------------------------------

Overlay overlayFor(Player& player, core::GlobalPos const& target) {
    Overlay out;
    if (static_cast<int>(player.getDimensionId()) != target.dim) return out;
    auto& region = player.getDimensionBlockSource();
    auto  pos    = target;
    auto  bp     = game::blockPos(pos);
    if (!region.hasChunksAt(bp, 0, false)) return out;
    Block const* block = &region.getBlock(bp);

    auto providersOf = [](Block const& b) -> std::vector<InfoProvider> const* {
        auto it = reg().providers.find(b.getTypeName());
        return it == reg().providers.end() ? nullptr : &it->second;
    };
    bool wearing  = isWearingGoggles(player);
    bool sneaking = player.isSneaking();

    // Create's proxiedOverlayPosition
    if (auto const* list = providersOf(*block)) {
        InfoContext probe{player, region, pos, *block, sneaking, wearing};
        for (auto const& p : *list) {
            if (!p.proxy) continue;
            if (auto other = p.proxy(probe); other && other->dim == pos.dim && region.hasChunksAt(game::blockPos(*other), 0, false)) {
                pos   = *other;
                bp    = game::blockPos(pos);
                block = &region.getBlock(bp);
            }
            break;
        }
    }

    auto const* providers = providersOf(*block);
    auto        id        = block->getTypeName();
    bool        defaults  = isLevitate(id);
    if (providers)
        for (auto const& p : *providers)
            if (p.replaceDefaults) defaults = false;
    bool kinetic = kinetics::specOf(id) != nullptr;

    InfoContext ctx{player, region, pos, *block, sneaking, wearing};
    bool        hasGoggle = defaults, hasHover = defaults && kinetic;
    if (providers) {
        for (auto const& p : *providers) {
            hasGoggle |= static_cast<bool>(p.goggle);
            hasHover |= static_cast<bool>(p.hover);
            if (p.icon) out.icon = p.icon(ctx);
        }
    }

    Tooltip t;
    bool    goggleAdded = false, hoverAdded = false;
    if (hasGoggle && wearing) {
        if (defaults) goggleAdded |= defaultGoggle(ctx, t);
        if (providers)
            for (auto const& p : *providers)
                if (p.goggle) goggleAdded |= p.goggle(ctx, t);
    }
    if (hasHover) {
        if (!t.empty()) t.emplace_back();
        if (defaults && kinetic) hoverAdded |= kineticHoverTooltip(ctx, t);
        if (providers)
            for (auto const& p : *providers)
                if (p.hover) hoverAdded |= p.hover(ctx, t);
        if (goggleAdded && !hoverAdded && !t.empty() && t.back().empty()) t.pop_back();
    }
    if ((hasGoggle && !goggleAdded) && (hasHover && !hoverAdded)) return out;
    if (t.empty()) return out;
    out.lines = std::move(t);
    return out;
}

// ---- module: queries, action bar, command ----------------------------------------------------------

class OverlayModule {
public:
    void install() {
        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) { tick(); }));
        mListeners.push_back(bus.emplaceListener<ll::event::PlayerDisconnectEvent>([this](ll::event::PlayerDisconnectEvent& ev) {
            mShown.erase(ev.self().getOrCreateUniqueID().rawID);
        }));
        net::on("goggles.query", [this](Player& player, Json const& msg) {
            auto pos = posFrom(msg.value("pos", Json()));
            if (!pos) {
                net::send(player, "goggles.info", {{"pos", nullptr}, {"lines", Json::array()}});
                return;
            }
            auto const& p  = player.getPosition();
            double      dx = p.x - (pos->x + 0.5), dy = p.y - (pos->y + 0.5), dz = p.z - (pos->z + 0.5);
            Overlay     o;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) <= kMaxQueryRange) o = overlayFor(player, *pos);
            noteShown(player, *pos, o, "client mod");
            net::send(player, "goggles.info", {{"pos", posJson(*pos)}, {"lines", o.lines}, {"icon", o.icon}});
        });
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
        net::off("goggles.query");
        mShown.clear();
    }

private:
    struct Shown {
        std::optional<core::GlobalPos> pos;
        std::string                    text;
        std::uint64_t                  sentAt = 0;
    };

    /// One log line per player and newly shown block (the e2e scenarios wait for it).
    void noteShown(Player& player, core::GlobalPos const& pos, Overlay const& o, char const* how) {
        auto& s = mShown[player.getOrCreateUniqueID().rawID];
        if (o.lines.empty()) {
            s.pos.reset();
            return;
        }
        if (s.pos == pos) return;
        s.pos = pos;
        log().info("goggles: overlay for {} at {} ({} lines, {})", player.getRealName(), pos.key(), o.lines.size(), how);
    }

    void tick() {
        ++mTick;
        auto const& cfg = config::get<GogglesConfig>();
        if (!cfg.vanillaOverlay || mTick % static_cast<std::uint64_t>(std::max(1, cfg.overlayInterval)) != 0) return;
        auto level = ll::service::getLevel();
        if (!level) return;
        level->forEachPlayer([this](Player& player) {
            if (net::isCompanion(player) || player.isSimulatedPlayer()) return true;
            vanillaOverlay(player);
            return true;
        });
    }

    void vanillaOverlay(Player& player) {
        auto&   s   = mShown[player.getOrCreateUniqueID().rawID];
        auto    hit = player.traceRay(5.5f, false, true);
        Overlay o;
        core::GlobalPos pos{};
        if (hit.mType == HitResultType::Tile) {
            pos = game::globalPos(player.getDimensionBlockSource(), hit.mBlock);
            o   = overlayFor(player, pos);
        }
        if (o.lines.empty()) {
            if (!s.text.empty()) actionBar(player, " "); // clear the stale text
            s.text.clear();
            s.pos.reset();
            return;
        }
        auto text = actionBarText(o.lines);
        if (text != s.text || mTick - s.sentAt >= 20) {
            actionBar(player, text);
            s.sentAt = mTick;
        }
        s.text = text;
        noteShown(player, pos, o, "action bar");
    }

    std::vector<ll::event::ListenerPtr>       mListeners;
    std::unordered_map<std::int64_t, Shown>   mShown;
    std::uint64_t                             mTick = 0;
};

} // namespace lv::feature::goggles

LV_REGISTER_MODULE(lv::feature::goggles::OverlayModule);

// `/create overlay` (Create's OverlayConfigCommand): opens the overlay position screen of the
// client mod; `/create overlay reset` puts the overlay back to its default position.
LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    namespace net = lv::feature::net;
    cmd.overload().text("overlay").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = lv::commands::playerOf(origin);
        if (!player || !net::isCompanion(*player)) {
            return output.error(lv::tr("lv.goggles.overlay.needs_client", "The overlay position can only be changed with the Levitate client mod."));
        }
        net::send(*player, "goggles.overlay", {{"action", "screen"}});
        lv::log().info("goggles: overlay screen opened for {}", player->getRealName());
        output.success(lv::trFor(*player, "lv.goggles.overlay.opened", "Opened the overlay position screen."));
    });
    cmd.overload().text("overlay").text("reset").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = lv::commands::playerOf(origin);
        if (player && net::isCompanion(*player)) net::send(*player, "goggles.overlay", {{"action", "reset"}});
        lv::log().info("goggles: overlay reset for {}", player ? player->getRealName() : std::string("console"));
        output.success(
            player ? lv::trFor(*player, "lv.goggles.overlay.reset", "Create Goggle Overlay has been reset to default position")
                   : lv::tr("lv.goggles.overlay.reset", "Create Goggle Overlay has been reset to default position")
        );
    });
});
