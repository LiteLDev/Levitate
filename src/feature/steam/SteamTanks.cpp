// Steam unit: fluid tanks and creative fluid tanks (Create's FluidTankBlock / FluidTankBlockEntity /
// FluidTankItem / CreativeFluidTankBlockEntity): multiblock forming and splitting through
// core/steam/Tank, block states (connected textures, windows, light), the fluid capability,
// bucket / bottle exchange, the wrench and contraption storage.

#include <algorithm>
#include <cmath>
#include <random>

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/contraption/Contraption.h"
#include "feature/steam/SteamInternal.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::steam {

namespace blockdata = common::blockdata;
namespace fluids    = common::fluids;
namespace citems    = common::items;

namespace {

struct Pending {
    GlobalPos   pos;
    std::string prevId, newId;
};
std::vector<Pending>& pending() {
    static std::vector<Pending> p;
    return p;
}
std::map<GlobalPos, Json>& erased() {
    static std::map<GlobalPos, Json> e;
    return e;
}

int fluidLight(std::string const& fluid) { return fluid == "minecraft:lava" ? 15 : 0; }

/// The tanks of one kind (fluid tanks or creative tanks) of a dimension, read from and written to
/// BlockData, cached for one operation.
class WorldTanks : public cs::TankAccess {
public:
    WorldTanks(BlockSource& region, int dim, std::string id) : mRegion(region), mDim(dim), mId(std::move(id)) {}

    std::optional<cs::TankPart> get(cs::Pos3 const& p) override {
        if (auto it = mCache.find(p); it != mCache.end()) return it->second;
        GlobalPos                   gp = globalOf(mDim, p);
        std::optional<cs::TankPart> r;
        if (idAt(mRegion, gp) == mId) {
            auto const* doc = blockdata::get(gp);
            r               = parsePart(doc && doc->contains(kDataKey) ? &(*doc)[kDataKey] : nullptr, p);
        }
        mCache[p] = r;
        return r;
    }
    void set(cs::Pos3 const& p, cs::TankPart const& part) override {
        mCache[p]                                  = part;
        blockdata::edit(globalOf(mDim, p))[kDataKey] = partJson(part);
    }

private:
    BlockSource&                                    mRegion;
    int                                             mDim;
    std::string                                     mId;
    std::map<cs::Pos3, std::optional<cs::TankPart>> mCache;
};

bool contains(cs::Pos3 const& ctrl, int width, int height, cs::Pos3 const& p) {
    return p.x >= ctrl.x && p.x < ctrl.x + width && p.z >= ctrl.z && p.z < ctrl.z + width && p.y >= ctrl.y && p.y < ctrl.y + height;
}

std::string idOfItem(ItemStack const& s) { return citems::idOf(s); }

/// The filled container for `emptyId` holding `fluid` (bucket / bottle), with its amount.
std::optional<std::pair<std::string, int>> filledContainer(std::string const& emptyId, std::string const& fluid) {
    if (emptyId == "minecraft:glass_bottle") {
        if (fluid == "minecraft:water") return std::pair<std::string, int>{"minecraft:potion", 250};
        return std::nullopt;
    }
    if (emptyId != "minecraft:bucket") return std::nullopt;
    static std::map<std::string, std::string> const vanilla{
        {"minecraft:water",       "minecraft:water_bucket"      },
        {"minecraft:lava",        "minecraft:lava_bucket"       },
        {"minecraft:powder_snow", "minecraft:powder_snow_bucket"},
        {"minecraft:milk",        "minecraft:milk_bucket"       },
    };
    if (auto it = vanilla.find(fluid); it != vanilla.end()) return std::pair<std::string, int>{it->second, 1000};
    // units' buckets follow Create's naming: <namespace>:<fluid>_bucket
    std::string candidate = fluid + "_bucket";
    if (auto stack = citems::make(candidate)) {
        auto inItem = fluids::fluidInItem(*stack);
        if (inItem.fluid == fluid && inItem.amount == 1000) return std::pair<std::string, int>{candidate, 1000};
    }
    return std::nullopt;
}

std::string emptyContainerOf(std::string const& filledId) {
    if (filledId == "minecraft:potion") return "minecraft:glass_bottle";
    return "minecraft:bucket";
}

void consumeHeld(Player& player, ItemStack const& held, std::optional<ItemStack> const& result) {
    int count = citems::countOf(held);
    player.setSelectedItem(count > 1 ? citems::withCount(held, count - 1) : citems::empty());
    if (result) {
        if (count <= 1) player.setSelectedItem(*result);
        else giveOrDrop(player, *result);
    }
    player.sendInventory(false);
}

float exchangePitch(std::int64_t amountAfter) {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> d(0.f, 1.f);
    auto  perBlock = tankParams(false).capacityPerBlock;
    float pitch    = std::clamp(1.f - static_cast<float>(amountAfter) / static_cast<float>(perBlock * 16), 0.f, 1.f);
    pitch /= 1.5f;
    pitch += .5f;
    pitch += (d(rng) - .5f) / 4.f;
    return pitch;
}

} // namespace

// ---- documents -------------------------------------------------------------------------------------

cs::TankPart parsePart(Json const* doc, cs::Pos3 const& self) {
    cs::TankPart p;
    p.controller = self;
    if (!doc || !doc->is_object()) return p;
    try {
        if (auto it = doc->find("c"); it != doc->end() && it->is_array() && it->size() == 3)
            p.controller = {(*it)[0].get<int>(), (*it)[1].get<int>(), (*it)[2].get<int>()};
        p.width  = std::clamp(doc->value("w", 1), 1, 3);
        p.height = std::max(1, doc->value("h", 1));
        p.window = doc->value("win", true);
        p.fluid  = doc->value("f", std::string{});
        p.amount = doc->value("a", std::int64_t{0});
        if (p.amount <= 0) {
            p.amount = 0;
            p.fluid.clear();
        }
    } catch (...) {
        p = cs::TankPart{};
        p.controller = self;
    }
    return p;
}

Json partJson(cs::TankPart const& part) {
    Json j{
        {"c",   {part.controller.x, part.controller.y, part.controller.z}},
        {"w",   part.width                                               },
        {"h",   part.height                                              },
        {"win", part.window                                              },
    };
    if (!part.fluid.empty() && part.amount > 0) {
        j["f"] = part.fluid;
        j["a"] = part.amount;
    }
    return j;
}

std::optional<TankInfo> tankAt(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return std::nullopt;
    auto id = idAt(*region, pos);
    if (!isTankId(id)) return std::nullopt;
    WorldTanks world(*region, pos.dim, id);
    auto       part = world.get(pos3(pos));
    if (!part) return std::nullopt;
    auto ctrl = world.get(part->controller);
    if (!ctrl || !ctrl->isController(part->controller)) {
        ctrl          = part;
        ctrl->width   = 1;
        ctrl->height  = 1;
        part->controller = pos3(pos);
    }
    bool     creative = isCreativeTankId(id);
    auto     params   = tankParams(creative);
    TankInfo info;
    info.controller = globalOf(pos.dim, part->controller);
    info.blockId    = id;
    info.width      = ctrl->width;
    info.height     = ctrl->height;
    info.fluid      = ctrl->fluid;
    info.capacity   = cs::capacityOf(*ctrl, params);
    info.amount     = creative && !ctrl->fluid.empty() ? info.capacity : std::min(ctrl->amount, info.capacity);
    if (info.fluid.empty()) info.amount = 0;
    info.window   = ctrl->window;
    info.creative = creative;
    return info;
}

void setTankFluid(GlobalPos const& controller, std::string const& fluid, std::int64_t amount) {
    auto* region = regionOf(controller.dim);
    if (!region) return;
    auto id = idAt(*region, controller);
    if (!isTankId(id)) return;
    WorldTanks world(*region, controller.dim, id);
    auto       part = world.get(pos3(controller));
    if (!part || !part->isController(pos3(controller))) return;
    auto params   = tankParams(isCreativeTankId(id));
    auto capacity = cs::capacityOf(*part, params);
    int  oldLight = part->amount > 0 ? fluidLight(part->fluid) : 0;
    auto oldFill  = cs::fillState(part->amount, capacity);
    if (fluid.empty() || amount <= 0) {
        part->fluid.clear();
        part->amount = 0;
    } else {
        part->fluid  = fluid;
        part->amount = params.creative ? capacity : std::min(amount, capacity);
    }
    world.set(pos3(controller), *part);
    int newLight = part->amount > 0 ? fluidLight(part->fluid) : 0;
    if ((oldLight || newLight) && (oldLight != newLight || int(oldFill * part->height) != int(cs::fillState(part->amount, capacity) * part->height)))
        refreshStructure(controller);
    markTankForSync(controller);
}

void refreshStructure(GlobalPos const& controller) {
    auto* region = regionOf(controller.dim);
    if (!region) return;
    auto id = idAt(*region, controller);
    if (!isTankId(id)) return;
    WorldTanks world(*region, controller.dim, id);
    auto       c = pos3(controller);
    auto       ctrl = world.get(c);
    if (!ctrl || !ctrl->isController(c)) return;
    auto params   = tankParams(isCreativeTankId(id));
    auto capacity = cs::capacityOf(*ctrl, params);
    auto amount   = params.creative && !ctrl->fluid.empty() ? capacity : ctrl->amount;
    int  light    = fluidLight(ctrl->fluid);
    for (int y = 0; y < ctrl->height; ++y) {
        for (int x = 0; x < ctrl->width; ++x) {
            for (int z = 0; z < ctrl->width; ++z) {
                auto p    = c.offset(x, y, z);
                auto part = world.get(p);
                if (!part || !(part->controller == c)) continue;
                setStates(
                    *region,
                    globalOf(controller.dim, p),
                    {
                        {"levitate:top",    y == ctrl->height - 1 ? 1 : 0                                      },
                        {"levitate:bottom", y == 0 ? 1 : 0                                                     },
                        {"levitate:shape",  cs::windowShape(ctrl->window, ctrl->width, x, z)                   },
                        {"levitate:light",  cs::partLight(light, amount, capacity, ctrl->height, y, ctrl->window)},
                }
                );
            }
        }
    }
    markTankForSync(controller);
}

void setWindows(GlobalPos const& controller, bool window) {
    auto* region = regionOf(controller.dim);
    if (!region) return;
    auto id = idAt(*region, controller);
    if (!isTankId(id)) return;
    WorldTanks world(*region, controller.dim, id);
    auto       part = world.get(pos3(controller));
    if (!part || !part->isController(pos3(controller)) || part->window == window) return;
    part->window = window;
    world.set(pos3(controller), *part);
    refreshStructure(controller);
}

bool toggleWindows(GlobalPos const& pos) {
    auto info = tankAt(pos);
    if (!info) return false;
    if (boilerActive(info->controller)) return false;
    setWindows(info->controller, !info->window);
    log().info("steam: tank at {} {} {} windows {}", info->controller.x, info->controller.y, info->controller.z, info->window ? "closed" : "opened");
    return true;
}

// ---- world changes ---------------------------------------------------------------------------------

void queueChange(GlobalPos const& pos, std::string prevId, std::string newId) {
    pending().push_back({pos, std::move(prevId), std::move(newId)});
}

void stashErasedDoc(GlobalPos const& pos, Json const& doc) {
    if (doc.is_object() && doc.contains(kDataKey)) erased()[pos] = doc[kDataKey];
}

void processChanges() {
    if (pending().empty()) return;
    auto list = std::move(pending());
    pending().clear();

    std::vector<Pending>                                  removedTanks, addedTanks;
    std::set<GlobalPos>                                   touchedControllers;
    for (auto const& ch : list) {
        bool wasTank = isTankId(ch.prevId), isTank = isTankId(ch.newId);
        if (wasTank && ch.prevId != ch.newId) removedTanks.push_back(ch);
        if (isTank && ch.prevId != ch.newId) addedTanks.push_back(ch);
        if (ch.prevId == kEngine && ch.newId != kEngine) onEngineRemoved(ch.pos);
        if (ch.prevId == kWhistle && ch.newId != kWhistle) onWhistleRemoved(ch.pos);
        if (ch.prevId == kExtension && ch.newId != kExtension) onExtensionChanged(ch.pos, false);
        if (ch.prevId == kPoweredShaft && ch.newId != kPoweredShaft) onPoweredShaftRemoved(ch.pos);
    }

    // removed tank blocks: split their structures (Create's FluidTankBlock.onRemove -> splitMulti)
    for (auto const& r : removedTanks) {
        auto* region = regionOf(r.pos.dim);
        if (!region) continue;
        WorldTanks world(*region, r.pos.dim, r.prevId);
        auto       params = tankParams(isCreativeTankId(r.prevId));
        auto       self   = pos3(r.pos);
        std::optional<cs::TankPart> part;
        if (auto it = erased().find(r.pos); it != erased().end()) {
            part = parsePart(&it->second, self);
            erased().erase(it);
        } else {
            // no document (moved by a contraption or never formed): find the structure around it
            for (auto [dx, dy, dz] : {std::tuple{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}) {
                auto n = world.get(self.offset(dx, dy, dz));
                if (!n) continue;
                auto c = world.get(n->controller);
                if (c && c->isController(n->controller) && contains(n->controller, c->width, c->height, self)) {
                    part             = cs::TankPart{};
                    part->controller = n->controller;
                    part->width      = c->width;
                    part->height     = c->height;
                    break;
                }
            }
        }
        if (!part) continue;
        if (!part->isController(self)) {
            auto c = world.get(part->controller);
            if (!c || !c->isController(part->controller) || !contains(part->controller, c->width, c->height, self)) continue;
            part->width  = c->width;
            part->height = c->height;
        }
        GlobalPos oldCtrl = globalOf(r.pos.dim, part->controller);
        boilers().erase(oldCtrl);
        markTankRemovedForSync(oldCtrl);
        auto changed = cs::removePart(world, self, *part, std::nullopt, params);
        for (auto const& p : changed) {
            auto now = world.get(p);
            if (now) touchedControllers.insert(globalOf(r.pos.dim, now->controller));
            boilers().erase(globalOf(r.pos.dim, p));
        }
        log().info("steam: {} at {} {} {} removed", r.prevId, r.pos.x, r.pos.y, r.pos.z);
    }
    for (auto const& r : removedTanks) dropAttachmentsOf(r.pos);
    erased().clear();

    // placed tank blocks: each starts as its own 1x1 tank, then they connect (Create: formMulti
    // on the first tick)
    std::map<std::pair<int, std::string>, std::vector<cs::Pos3>> frontiers;
    for (auto const& a : addedTanks) {
        auto* region = regionOf(a.pos.dim);
        if (!region || idAt(*region, a.pos) != a.newId) continue;
        WorldTanks world(*region, a.pos.dim, a.newId);
        auto       self = pos3(a.pos);
        auto const* doc = blockdata::get(a.pos);
        auto        part = parsePart(doc && doc->contains(kDataKey) ? &(*doc)[kDataKey] : nullptr, self);
        part.controller = self;
        part.width      = 1;
        part.height     = 1;
        world.set(self, part);
        frontiers[{a.pos.dim, a.newId}].push_back(self);
        touchedControllers.insert(a.pos);
    }
    for (auto& [key, frontier] : frontiers) {
        auto* region = regionOf(key.first);
        if (!region) continue;
        WorldTanks world(*region, key.first, key.second);
        auto       changed = cs::formMulti(world, frontier, tankParams(isCreativeTankId(key.second)));
        for (auto const& p : changed) {
            boilers().erase(globalOf(key.first, p));
            if (auto now = world.get(p)) touchedControllers.insert(globalOf(key.first, now->controller));
        }
        for (auto const& p : frontier)
            if (auto now = world.get(p)) touchedControllers.insert(globalOf(key.first, now->controller));
    }
    for (auto const& a : addedTanks) {
        if (auto info = tankAt(a.pos))
            log().info("steam: {} placed at {} {} {} ({}x{}x{} tank at {} {} {})", a.newId, a.pos.x, a.pos.y, a.pos.z, info->width,
                       info->height, info->width, info->controller.x, info->controller.y, info->controller.z);
    }
    for (auto const& c : touchedControllers) {
        refreshStructure(c);
        markBoilerDirty(c);
    }

    // engines / whistles / extensions placed this tick (after the tanks they hang on)
    for (auto const& ch : list) {
        if (ch.newId == kEngine && ch.prevId != kEngine) onEngineAdded(ch.pos);
        if (ch.newId == kWhistle && ch.prevId != kWhistle) onWhistleAdded(ch.pos);
        if (ch.newId == kExtension && ch.prevId != kExtension) onExtensionChanged(ch.pos, true);
        if (ch.newId == kPoweredShaft && ch.prevId != kPoweredShaft) onPoweredShaftAdded(ch.pos);
    }
}

// ---- interactions ----------------------------------------------------------------------------------

bool useItemOnTank(Player& player, GlobalPos const& pos, ItemStack const& held) {
    auto info = tankAt(pos);
    if (!info || held.isNull()) return false;
    auto heldId        = idOfItem(held);
    bool creativePlayer = player.isCreative();
    auto access        = [&](auto&& fn) {
        auto* region = regionOf(pos.dim);
        return region ? fn(*region) : decltype(fn(*region)){};
    };

    // item -> tank (Create's FluidHelper.tryEmptyItemIntoBE: the whole content or nothing)
    auto inItem = fluids::fluidInItem(held);
    if (!inItem.empty()) {
        int accepted = access([&](BlockSource& r) { return fluids::fill(r, game::blockPos(pos), -1, inItem, true); });
        if (accepted != inItem.amount) return true; // Create: the click is used up, nothing happens
        if (info->creative && !boilerActive(info->controller)) setTankFluid(info->controller, inItem.fluid, info->capacity);
        (void)access([&](BlockSource& r) { return fluids::fill(r, game::blockPos(pos), -1, inItem, false); });
        if (!creativePlayer && !info->creative) consumeHeld(player, held, citems::make(emptyContainerOf(heldId)));
        auto after = tankAt(pos);
        playSound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, inItem.fluid == "minecraft:lava" ? "bucket.empty_lava" : "bucket.empty_water",
                  .5f, exchangePitch(after ? after->amount : 0));
        log().info("steam: {} emptied {} mB of {} into the tank at {} {} {}", player.getRealName(), inItem.amount, inItem.fluid, pos.x, pos.y, pos.z);
        return true;
    }

    // tank -> item (Create's FluidHelper.tryFillItemFromBE)
    if (heldId == "minecraft:bucket" || heldId == "minecraft:glass_bottle") {
        if (info->fluid.empty() || info->amount <= 0 || boilerActive(info->controller)) return true;
        auto filled = filledContainer(heldId, info->fluid);
        if (!filled) return true;
        fluids::FluidStack request{info->fluid, filled->second};
        auto got = access([&](BlockSource& r) { return fluids::drain(r, game::blockPos(pos), -1, request, true); });
        if (got.amount != filled->second) return true;
        (void)access([&](BlockSource& r) { return fluids::drain(r, game::blockPos(pos), -1, request, false); });
        if (info->creative) setTankFluid(info->controller, "", 0);
        if (!creativePlayer) {
            std::optional<ItemStack> out = citems::make(filled->first, 1, 0);
            consumeHeld(player, held, out);
        }
        auto after = tankAt(pos);
        playSound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, info->fluid == "minecraft:lava" ? "bucket.fill_lava" : "bucket.fill_water", .5f,
                  exchangePitch(after ? after->amount : 0));
        log().info("steam: {} filled {} from the tank at {} {} {}", player.getRealName(), filled->first, pos.x, pos.y, pos.z);
        return true;
    }
    return false;
}

bool tryMultiPlace(Player& player, GlobalPos const& clicked, int face, ItemStack const& held) {
    if (player.isSneaking() || (face != 0 && face != 1)) return false;
    auto heldId = idOfItem(held);
    if (!isTankId(heldId)) return false;
    auto* region = regionOf(clicked.dim);
    if (!region || idAt(*region, clicked) != heldId) return false;
    auto info = tankAt(clicked);
    if (!info || info->width == 1) return false;
    int       startY = face == 0 ? info->controller.y - 1 : info->controller.y + info->height;
    GlobalPos target = offset(clicked, face == 0 ? cs::Dir::Down : cs::Dir::Up);
    if (target.y != startY) return false;
    int toPlace = 0;
    for (int x = 0; x < info->width; ++x) {
        for (int z = 0; z < info->width; ++z) {
            GlobalPos p{clicked.dim, info->controller.x + x, startY, info->controller.z + z};
            if (!loaded(*region, p)) return false;
            if (isTankId(idAt(*region, p))) continue;
            if (!game::replaceable(*region, game::blockPos(p))) return false;
            ++toPlace;
        }
    }
    if (toPlace == 0) return false;
    if (!player.isCreative() && citems::countOf(held) < toPlace) return false;
    auto const* block = game::blockWith(heldId);
    if (!block) return false;
    for (int x = 0; x < info->width; ++x) {
        for (int z = 0; z < info->width; ++z) {
            GlobalPos p{clicked.dim, info->controller.x + x, startY, info->controller.z + z};
            if (isTankId(idAt(*region, p))) continue;
            game::setBlock(*region, game::blockPos(p), *block);
        }
    }
    if (!player.isCreative()) {
        int left = citems::countOf(held) - toPlace;
        player.setSelectedItem(left > 0 ? citems::withCount(held, left) : citems::empty());
        player.sendInventory(false);
    }
    playSound(clicked.dim, target.x + .5, target.y + .5, target.z + .5, "dig.copper", 1.f, .8f);
    log().info("steam: {} placed a layer of {} tanks at y {}", player.getRealName(), toPlace, startY);
    return true;
}

// ---- capabilities ----------------------------------------------------------------------------------

void registerTankCapabilities() {
    fluids::Handler h;
    h.tanks = [](common::Access const& a) -> std::vector<fluids::Tank> {
        auto info = tankAt(game::globalPos(a.region, a.pos));
        if (!info) return {};
        if (boilerActive(info->controller)) return {fluids::Tank{{}, static_cast<int>(info->capacity)}};
        return {fluids::Tank{{info->fluid, static_cast<int>(info->amount)}, static_cast<int>(std::min<std::int64_t>(info->capacity, INT32_MAX))}};
    };
    h.fill = [](common::Access const& a, fluids::FluidStack const& s, bool simulate) -> int {
        if (s.empty()) return 0;
        auto info = tankAt(game::globalPos(a.region, a.pos));
        if (!info) return 0;
        if (boilerActive(info->controller)) {
            // Create's BoilerFluidHandler: takes any amount of water and turns it into steam
            if (s.fluid != "minecraft:water") return 0;
            if (!simulate) feedBoiler(info->controller, s.amount);
            return s.amount;
        }
        if (info->creative) return s.amount; // creative tanks void what they are given
        if (!info->fluid.empty() && info->amount > 0 && info->fluid != s.fluid) return 0;
        auto space    = std::max<std::int64_t>(0, info->capacity - info->amount);
        int  accepted = static_cast<int>(std::min<std::int64_t>(space, s.amount));
        if (!simulate && accepted > 0) setTankFluid(info->controller, s.fluid, info->amount + accepted);
        return accepted;
    };
    h.drain = [](common::Access const& a, fluids::FluidStack const& req, bool simulate) -> fluids::FluidStack {
        auto info = tankAt(game::globalPos(a.region, a.pos));
        if (!info || boilerActive(info->controller) || info->fluid.empty() || info->amount <= 0 || req.amount <= 0) return {};
        if (!req.fluid.empty() && req.fluid != info->fluid) return {};
        int n = static_cast<int>(std::min<std::int64_t>(req.amount, info->amount));
        if (!simulate && !info->creative) setTankFluid(info->controller, info->fluid, info->amount - n);
        return {info->fluid, n};
    };
    fluids::registerBlock(kTank, h);
    fluids::registerBlock(kCreativeTank, h);

    // tanks on contraptions keep their contents (Create's mounted fluid storage)
    contraption::registerDataCarrier({
        "steam:tank",
        [](BlockSource& region, GlobalPos const& gp) -> std::optional<Json> {
            if (!isTankId(idAt(region, gp))) return std::nullopt;
            auto const* doc = blockdata::get(gp);
            if (!doc || !doc->contains(kDataKey)) return std::nullopt;
            Json data = (*doc)[kDataKey];
            auto self = pos3(gp);
            auto part = parsePart(&data, self);
            data["ctrl"] = part.isController(self);
            blockdata::edit(gp).erase(kDataKey);
            return data;
        },
        [](BlockSource&, GlobalPos const& gp, Json const& data) {
            Json copy = data;
            copy.erase("ctrl");
            blockdata::edit(gp)[kDataKey] = copy;
        },
        nullptr,
    });
    contraption::FluidStorageAdapter adapter{
        [](Json const& carried) -> std::optional<contraption::FluidTank> {
            auto it = carried.find("steam:tank");
            if (it == carried.end() || !it->value("ctrl", false)) return std::nullopt;
            auto part = parsePart(&*it, {});
            bool creative = false;
            auto cap  = cs::capacityOf(part, tankParams(creative));
            return contraption::FluidTank{part.fluid, static_cast<int>(std::min<std::int64_t>(part.amount, cap)), static_cast<int>(cap)};
        },
        [](Json& carried, contraption::FluidTank const& tank) {
            auto it = carried.find("steam:tank");
            if (it == carried.end()) return;
            if (tank.fluid.empty() || tank.amount <= 0) {
                it->erase("f");
                it->erase("a");
            } else {
                (*it)["f"] = tank.fluid;
                (*it)["a"] = tank.amount;
            }
        },
    };
    contraption::registerFluidStorage(kTank, adapter);
}

} // namespace lv::feature::steam
