// Schematicannon (Create's SchematicannonBlockEntity / SchematicPrinter / LaunchedItem /
// MaterialChecklist): prints the deployed schematic in its slot block by block, one shot per
// `schematicannonDelay` ticks, each shot using one of the `schematicannonShotsPerGunpowder` shots
// a gunpowder gives, taking the blocks' items from the inventories on its six sides (any
// capability, a creative crate makes it free). Replace modes, "Skip missing Blocks", "Protect
// Block Entities", the checklist printer (book in, Material Checklist out), states and status
// messages as Create. Native screen for the client mod, form for vanilla players.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/actor/BlockActorType.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/Screens.h"
#include "feature/display/Clipboard.h"
#include "feature/net/Net.h"
#include "feature/schematics/Schematics.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::schematics {

namespace items   = common::items;
namespace screens = common::screens;
namespace caps    = common::items;

namespace {

constexpr char const* kScreen          = "schematics.cannon";
constexpr int         kNeighbourCheck  = 100;
constexpr int         kMaxAnchorDist   = 256;
constexpr char const* kDbKey           = "lv:schematics:cannons";
constexpr std::array<cs::Pos, 6> kDirs{{{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};

enum Slot { SchematicIn = 0, SchematicOut = 1, BookIn = 2, BookOut = 3, Gunpowder = 4 };

struct Partner {
    cs::Pos     pos;
    std::string snbt;
};

struct Flying {
    cs::Pos              target;
    int                  left = 0, total = 0;
    std::string          snbt; ///< "" = air (clearing)
    std::string          item;
    std::vector<Partner> partners;
    std::string          entity; ///< a launched entity's NBT (binary NBT as hex), "" for blocks
};

struct Requirement {
    bool        invalid = false;
    std::string item;  ///< "" = nothing needed (air)
    int         count = 0;
};

struct Cannon {
    GlobalPos                pos;
    std::array<ItemStack, 5> inv;
    std::string              state  = "STOPPED";
    std::string              status = "idle";
    int                      fuel = 0, replace = 2;
    bool                     skipMissing = true, replaceBE = false;
    int                      placed = 0, toPlace = 0;
    ItemStack                missing;
    float                    book = 0;
    bool                     dontUpdateChecklist = false;
    std::vector<Flying>      flying;

    // printer (not saved: Create loads the schematic again after a reload)
    bool                          printerLoaded = false, errored = false;
    std::shared_ptr<Loaded const> sch;
    SchematicData                 sd;
    cs::Pos                       bmin, bmax;
    cs::PrintCursor               cursor;
    std::optional<cs::Pos>        current; ///< offset from the anchor
    std::size_t                   entityIndex = 0; ///< next schematic entity (after the blocks)
    std::optional<cs::Pos>        aimSent;         ///< the target the client mods aim the barrel at
    std::vector<Block const*>     tblocks; ///< transformed block per palette index
    std::vector<char>             tdone;

    int                 neighbourCooldown = 0, printerCooldown = 0, lazy = 0;
    bool                positionNotLoaded = false, creative = false, blockSkipped = false;
    std::vector<int>    invDirs;
    cs::Checklist       checklist;
    bool                dirty = true, viewDirty = true;
};

std::map<GlobalPos, Cannon>               gCannons;
std::map<GlobalPos, std::set<std::int64_t>> gViewers;

void sendAim(Cannon& c, std::optional<cs::Pos> target);

// ---- persistence ------------------------------------------------------------------------------------

std::string toHex(std::string const& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 15]);
    }
    return out;
}

std::string fromHex(std::string const& hex) {
    auto nibble = [](char ch) { return ch >= 'a' ? ch - 'a' + 10 : ch - '0'; };
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    return out;
}

Json flyingJson(Flying const& f) {
    Json partners = Json::array();
    for (auto const& p : f.partners) partners.push_back({{"pos", {p.pos.x, p.pos.y, p.pos.z}}, {"snbt", p.snbt}});
    Json j{{"t", {f.target.x, f.target.y, f.target.z}}, {"left", f.left}, {"total", f.total}, {"snbt", f.snbt}, {"item", f.item}, {"partners", partners}};
    if (!f.entity.empty()) j["entity"] = f.entity;
    return j;
}

cs::Pos vec3(Json const& j) {
    if (!j.is_array() || j.size() != 3) return {};
    return {j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
}

void save(Cannon& c) {
    if (!c.dirty) return;
    c.dirty = false;
    if (idAt(c.pos) != kCannon) return;
    Json inv = Json::array();
    for (auto const& s : c.inv) inv.push_back(items::toJson(s));
    Json flying = Json::array();
    for (auto const& f : c.flying) flying.push_back(flyingJson(f));
    data(c.pos) = Json{
        {"inv",         inv                       },
        {"state",       c.state                   },
        {"status",      c.status                  },
        {"fuel",        c.fuel                    },
        {"replace",     c.replace                 },
        {"skipMissing", c.skipMissing             },
        {"replaceBE",   c.replaceBE               },
        {"placed",      c.placed                  },
        {"toPlace",     c.toPlace                 },
        {"missing",     items::toJson(c.missing)  },
        {"book",        c.book                    },
        {"flying",      flying                    },
    };
}

Cannon& cannonAt(GlobalPos const& p) {
    auto it = gCannons.find(p);
    if (it != gCannons.end()) return it->second;
    Cannon c;
    c.pos = p;
    if (auto const* d = dataIf(p)) {
        if (auto inv = d->value("inv", Json::array()); inv.is_array())
            for (std::size_t i = 0; i < inv.size() && i < 5; ++i) c.inv[i] = items::fromJson(inv[i]);
        c.state       = d->value("state", std::string("STOPPED"));
        c.status      = d->value("status", std::string("idle"));
        c.fuel        = d->value("fuel", 0);
        c.replace     = std::clamp(d->value("replace", 2), 0, 3);
        c.skipMissing = d->value("skipMissing", true);
        c.replaceBE   = d->value("replaceBE", false);
        c.placed      = d->value("placed", 0);
        c.toPlace     = d->value("toPlace", 0);
        c.missing     = items::fromJson(d->value("missing", Json()));
        c.book        = d->value("book", 0.f);
        for (auto const& f : d->value("flying", Json::array())) {
            Flying fl;
            fl.target = vec3(f.value("t", Json()));
            fl.left   = f.value("left", 0);
            fl.total  = f.value("total", 0);
            fl.snbt   = f.value("snbt", std::string{});
            fl.item   = f.value("item", std::string{});
            for (auto const& p2 : f.value("partners", Json::array())) fl.partners.push_back({vec3(p2.value("pos", Json())), p2.value("snbt", std::string{})});
            fl.entity = f.value("entity", std::string{});
            c.flying.push_back(std::move(fl));
        }
    }
    c.dirty = false;
    return gCannons.emplace(p, std::move(c)).first->second;
}

void persistList() {
    Json list = Json::array();
    for (auto const& [p, _] : gCannons) list.push_back(posJson(p));
    game::db::save(kDbKey, list);
}

// ---- schematic access -------------------------------------------------------------------------------

Block const* schematicBlock(Cannon& c, cs::Pos offset) {
    if (!c.sch) return nullptr;
    auto const& s     = c.sch->schematic;
    auto        local = cs::unapply(c.sd.transform, offset);
    auto        idx   = s.at(local);
    if (idx < 0) return nullptr;
    auto i = static_cast<std::size_t>(idx);
    if (!c.tdone[i]) {
        c.tdone[i] = 1;
        auto st    = s.palette[i];
        cs::transformStates(st, c.sd.transform);
        c.tblocks[i] = c.sch->blocks[i] ? resolve(st) : nullptr;
    }
    return c.tblocks[i];
}

std::optional<std::int64_t> state(Block const& b, std::string_view key) {
    auto tag = stateOf(b);
    if (auto const* t = tag.states.find(key)) return t->asInt();
    return std::nullopt;
}

std::map<Block const*, Requirement> gReqCache;

Requirement requirementOf(Block const& b) {
    if (auto it = gReqCache.find(&b); it != gReqCache.end()) return it->second;
    Requirement r;
    std::string name = b.getTypeName();
    if (b.isAir()) {
        // nothing needed
    } else if (name == "minecraft:water" || name == "minecraft:flowing_water" || name == "minecraft:lava" || name == "minecraft:flowing_lava"
               || name == "minecraft:fire" || name == "minecraft:soul_fire" || name == "minecraft:portal" || name == "minecraft:end_portal"
               || name == "minecraft:piston_arm_collision" || name == "minecraft:sticky_piston_arm_collision" || name == "minecraft:moving_block"
               || name == "minecraft:bubble_column") {
        r.invalid = true;
    } else {
        auto inst = b.getBlockType().asItemInstance(b, nullptr);
        if (inst.isNull() || inst.getTypeName().empty() || inst.getTypeName() == "minecraft:air") {
            r.invalid = true;
        } else {
            r.item  = inst.getTypeName();
            r.count = 1;
            if (name.find("double_slab") != std::string::npos) r.count = 2;
            auto st = stateOf(b);
            auto n  = [&](std::string_view k) -> std::int64_t {
                auto const* t = st.states.find(k);
                return t ? t->asInt().value_or(0) : 0;
            };
            if (name == "minecraft:snow_layer") r.count = static_cast<int>(n("height")) + 1;
            if (name.ends_with("candle")) r.count = static_cast<int>(n("candles")) + 1;
            if (name == "minecraft:sea_pickle") r.count = static_cast<int>(n("cluster_count")) + 1;
        }
    }
    gReqCache[&b] = r;
    return r;
}

/// Create's shouldIgnoreBlockState: second halves are placed with their first half.
bool ignoredHalf(Block const& b) {
    auto upper = state(b, "upper_block_bit");
    if (upper && *upper == 1) return true;
    auto head = state(b, "head_piece_bit");
    if (head && *head == 1) return true;
    return false;
}

GlobalPos targetOf(Cannon const& c, cs::Pos offset) { return gp(c.pos.dim, c.sd.anchor + offset); }

/// Create's SchematicPrinter.shouldPlaceBlock + SchematicannonBlockEntity.shouldPlace.
bool shouldPlaceAt(Cannon& c, cs::Pos offset) {
    auto const* want = schematicBlock(c, offset);
    if (!want) return false;
    GlobalPos target = targetOf(c, offset);
    if (!loaded(target)) return false;
    auto const* cur = blockAt(target);
    if (!cur) return false;
    if (cur == want) return false;
    if (unbreakable(*cur)) return false;
    Block const* other = nullptr;
    if (auto upper = state(*want, "upper_block_bit"); upper && *upper == 0) other = blockAt({target.dim, target.x, target.y + 1, target.z});
    if (other && unbreakable(*other)) return false;

    double dx = target.x - c.pos.x, dy = target.y - c.pos.y, dz = target.z - c.pos.z;
    cs::PlaceQuery q;
    q.mode            = static_cast<cs::ReplaceMode>(c.replace);
    q.placingAir      = want->isAir();
    q.schematicSolid  = solid(*want);
    q.worldSolid      = solid(*cur);
    q.worldOtherSolid = other && solid(*other);
    q.worldHasData    = holdsData(target, *cur) || (other && other->getBlockType().getBlockEntityType() != BlockActorType::Undefined);
    q.protectData     = !c.replaceBE;
    q.nearCannon      = dx * dx + dy * dy + dz * dz < 4.0;
    q.ignored         = ignoredHalf(*want);
    return cs::shouldPlace(q);
}

// ---- inventories -------------------------------------------------------------------------------------

void findInventories(Cannon& c) {
    c.creative = false;
    c.invDirs.clear();
    auto* region = regionOf(c.pos.dim);
    if (!region) return;
    for (int d = 0; d < 6; ++d) {
        GlobalPos n{c.pos.dim, c.pos.x + kDirs[d].x, c.pos.y + kDirs[d].y, c.pos.z + kDirs[d].z};
        if (!loaded(n)) continue;
        if (idAt(n) == kCreativeCrate) c.creative = true;
        if (caps::has(*region, bp(n), d ^ 1)) c.invDirs.push_back(d);
    }
}

/// Create's grabItemsFromAttachedInventories (consume): enough across the inventories?
bool grabItems(Cannon& c, std::string const& item, int count, bool simulate) {
    if (c.creative) return true;
    auto* region = regionOf(c.pos.dim);
    if (!region) return false;
    auto pred  = [item](ItemStack const& s) { return items::idOf(s) == item; };
    int  found = 0;
    for (int d : c.invDirs) {
        GlobalPos n{c.pos.dim, c.pos.x + kDirs[d].x, c.pos.y + kDirs[d].y, c.pos.z + kDirs[d].z};
        found += items::countOf(caps::extract(*region, bp(n), d ^ 1, pred, count, true));
        if (found >= count) break;
    }
    if (found < count) return false;
    if (simulate) return true;
    int need = count;
    for (int d : c.invDirs) {
        if (need <= 0) break;
        GlobalPos n{c.pos.dim, c.pos.x + kDirs[d].x, c.pos.y + kDirs[d].y, c.pos.z + kDirs[d].z};
        need -= items::countOf(caps::extract(*region, bp(n), d ^ 1, pred, need, false));
    }
    return true;
}

// ---- printer -------------------------------------------------------------------------------------------

void resetPrinter(Cannon& c) {
    c.printerLoaded = false;
    c.errored       = false;
    c.sch.reset();
    c.current.reset();
    c.entityIndex = 0;
    sendAim(c, std::nullopt);
    c.missing = items::empty();
    c.placed  = 0;
    c.toPlace = 0;
    c.dirty = c.viewDirty = true;
}

void emptyOutSchematic(Cannon& c) {
    c.inv[SchematicIn] = items::empty();
    int  count         = items::countOf(c.inv[SchematicOut]);
    auto empty         = makeItem(kEmptySchematic, count + 1);
    if (empty) c.inv[SchematicOut] = *empty;
}

void updateChecklist(Cannon& c) {
    c.checklist = {};
    if (c.printerLoaded && !c.errored && c.sch) {
        c.toPlace   = c.placed;
        auto [lo, hi] = std::pair{c.bmin, c.bmax};
        for (int y = lo.y; y <= hi.y; ++y) {
            for (int z = lo.z; z <= hi.z; ++z) {
                for (int x = lo.x; x <= hi.x; ++x) {
                    cs::Pos off{x, y, z};
                    if (!loaded(targetOf(c, off))) {
                        c.checklist.blocksNotLoaded = true;
                        continue;
                    }
                    if (!shouldPlaceAt(c, off)) continue;
                    auto const* want = schematicBlock(c, off);
                    auto        req  = requirementOf(*want);
                    if (req.invalid || req.item.empty()) continue;
                    c.checklist.require(req.item, req.count);
                    ++c.toPlace;
                }
            }
        }
    }
    findInventories(c);
    if (auto* region = regionOf(c.pos.dim)) {
        for (int d : c.invDirs) {
            GlobalPos n{c.pos.dim, c.pos.x + kDirs[d].x, c.pos.y + kDirs[d].y, c.pos.z + kDirs[d].z};
            int       slots = caps::slotCount(*region, bp(n), d ^ 1);
            auto      h     = caps::find({*region, bp(n), d ^ 1});
            if (!h) continue;
            for (int s = 0; s < slots; ++s) {
                auto st = h->getSlot({*region, bp(n), d ^ 1}, s);
                if (!items::isEmpty(st)) c.checklist.collect(items::idOf(st), items::countOf(st));
            }
        }
    }
    c.dirty = c.viewDirty = true;
}

void initializePrinter(Cannon& c) {
    auto d = schematicData(c.inv[SchematicIn]);
    if (!d) {
        c.state  = "STOPPED";
        c.status = "schematicInvalid";
        c.dirty = c.viewDirty = true;
        return;
    }
    if (!d->deployed) {
        c.state  = "STOPPED";
        c.status = "schematicNotPlaced";
        c.dirty = c.viewDirty = true;
        return;
    }
    std::string err;
    auto        sch = loadSchematic(d->owner, d->file, &err);
    if (!sch || sch->schematic.solidCount() == 0) {
        // missing file: Create's "Schematic File Expired"; broken file: "Incompatible Blocks"
        c.state  = "STOPPED";
        c.status = (!sch && err != "file not found" && err != "invalid file name") ? "schematicErrored" : "schematicExpired";
        emptyOutSchematic(c);
        resetPrinter(c);
        log().info("schematicannon at {} {} {}: {} ({})", c.pos.x, c.pos.y, c.pos.z, c.status, d->file);
        return;
    }
    double ax = d->anchor.x - c.pos.x, ay = d->anchor.y - c.pos.y, az = d->anchor.z - c.pos.z;
    if (ax * ax + ay * ay + az * az >= double(kMaxAnchorDist) * kMaxAnchorDist) {
        c.state  = "STOPPED";
        c.status = "targetOutsideRange";
        resetPrinter(c);
        return;
    }
    c.sd            = *d;
    c.sch           = sch;
    c.printerLoaded = true;
    c.errored       = false;
    c.tblocks.assign(sch->schematic.palette.size(), nullptr);
    c.tdone.assign(sch->schematic.palette.size(), 0);
    auto b   = cs::bounds(d->transform, sch->schematic.size);
    c.bmin   = b[0];
    c.bmax   = b[1];
    c.cursor = cs::PrintCursor(c.bmin, c.bmax);
    c.current.reset();
    c.state  = "PAUSED";
    c.status = "ready";
    updateChecklist(c); // toPlace = placed + what is left
    c.dirty = c.viewDirty = true;
}

/// Brittle blocks are deferred (Create: gantry carriages, mechanical arms, BlockMovementChecks.isBrittle).
bool deferred(Cannon& c, cs::Pos offset) {
    auto const* b = schematicBlock(c, offset);
    if (!b) return false;
    std::string const& n = b->getTypeName();
    static constexpr std::array<std::string_view, 24> kBrittle{
        "torch", "lever", "button", "pressure_plate", "rail", "redstone_wire", "repeater", "comparator", "ladder", "sign", "banner",
        "carpet", "door", "lantern", "tripwire", "flower_pot", "bell", "vine", "candle", "levitate:gantry_carriage", "levitate:mechanical_arm",
        "skull", "_head", "snow_layer"
    };
    for (auto k : kBrittle) {
        if (n.find(k) == std::string::npos) continue;
        if (k == "door" && n.find("trapdoor") != std::string::npos) continue; // trapdoors hold themselves
        return true;
    }
    return false;
}

void sendLaunch(Cannon const& c, Flying const& f) {
    auto level = ll::service::getLevel();
    if (!level) return;
    Json msg{{"dim", c.pos.dim}, {"from", {c.pos.x, c.pos.y, c.pos.z}}, {"to", {f.target.x, f.target.y, f.target.z}}, {"ticks", f.total}, {"snbt", f.snbt}};
    level->forEachPlayer([&](Player& p) {
        if (!net::isCompanion(p) || static_cast<int>(p.getDimensionId()) != c.pos.dim) return true;
        auto   pos = p.getFeetPos();
        double dx = pos.x - c.pos.x, dz = pos.z - c.pos.z;
        if (dx * dx + dz * dz <= 128.0 * 128.0) net::send(p, "schematics.launch", msg);
        return true;
    });
}

/// Create's SchematicannonRenderer aims at the printer's current target: the client mods turn the
/// connector and pitch the barrel towards it ("schematics.cannon", null when idle).
void sendAim(Cannon& c, std::optional<cs::Pos> target) {
    if (c.aimSent == target) return;
    c.aimSent  = target;
    auto level = ll::service::getLevel();
    if (!level) return;
    Json msg{{"dim", c.pos.dim}, {"pos", {c.pos.x, c.pos.y, c.pos.z}}, {"target", target ? Json{target->x, target->y, target->z} : Json()}};
    level->forEachPlayer([&](Player& p) {
        if (!net::isCompanion(p) || static_cast<int>(p.getDimensionId()) != c.pos.dim) return true;
        auto   pos = p.getFeetPos();
        double dx = pos.x - c.pos.x, dz = pos.z - c.pos.z;
        if (dx * dx + dz * dz <= 128.0 * 128.0) net::send(p, "schematics.cannon", msg);
        return true;
    });
}

void launch(Cannon& c, cs::Pos offset, Block const& want, std::string const& item) {
    GlobalPos t = targetOf(c, offset);
    Flying    f;
    f.target = cp(t);
    f.total = f.left = cs::flightTicks(cp(c.pos), f.target);
    f.snbt           = want.isAir() ? std::string() : blockSnbt(want);
    f.item           = item;
    // second halves (doors, tall plants, beds) land with their first half
    if (auto upper = state(want, "upper_block_bit"); upper && *upper == 0) {
        cs::Pos up = offset + cs::Pos{0, 1, 0};
        if (auto const* b = schematicBlock(c, up); b && b->getTypeName() == want.getTypeName()) f.partners.push_back({f.target + cs::Pos{0, 1, 0}, blockSnbt(*b)});
    }
    if (auto head = state(want, "head_piece_bit"); head && *head == 0) {
        for (int d = 2; d < 6; ++d) {
            auto const* b = schematicBlock(c, offset + kDirs[d]);
            if (!b || b->getTypeName() != want.getTypeName()) continue;
            if (auto h = state(*b, "head_piece_bit"); h && *h == 1) {
                f.partners.push_back({f.target + kDirs[d], blockSnbt(*b)});
                break;
            }
        }
    }
    if (!want.isAir()) ++c.placed;
    sendLaunch(c, f);
    c.flying.push_back(std::move(f));
    // Create: GENERIC_EXPLODE at 0.1 volume, pitch 1.1
    sound(c.pos, "random.explode", 0.1f, 1.1f);
    particle(c.pos.dim, c.pos.x + 0.5, c.pos.y + 1.2, c.pos.z + 0.5, "minecraft:basic_smoke_particle");
}

/// Create's ItemRequirement.of(entity): armor stands need themselves and what they wear / hold;
/// other entities can't be printed (item frames are blocks on Bedrock).
std::optional<std::vector<std::pair<std::string, int>>> entityRequirement(cs::nbt::Tag const& tag) {
    auto const* c = tag.asCompound();
    if (!c) return std::nullopt;
    auto const* id = c->find("identifier");
    if (!id || !id->asString() || *id->asString() != "minecraft:armor_stand") return std::nullopt;
    std::vector<std::pair<std::string, int>> out{{"minecraft:armor_stand", 1}};
    for (auto const* key : {"Armor", "Mainhand", "Offhand"}) {
        auto const* l = c->find(key);
        if (!l || !l->asList()) continue;
        for (auto const& it : l->asList()->items) {
            auto const* ic = it.asCompound();
            if (!ic) continue;
            auto const* name  = ic->find("Name");
            auto const* count = ic->find("Count");
            if (!name || !name->asString() || name->asString()->empty() || *name->asString() == "minecraft:air") continue;
            out.emplace_back(*name->asString(), static_cast<int>(count ? count->asInt().value_or(1) : 1));
        }
    }
    return out;
}

/// Create's SchematicPrinter entity phase: launches the schematic's printable entities one per
/// shot once the blocks are done. Returns false when there is nothing left.
bool tickEntities(Cannon& c) {
    if (!c.sch || c.sch->schematic.format != "mcstructure") return false;
    auto const& tags = c.sch->schematic.entityTags;
    while (c.entityIndex < tags.size()) {
        auto const& tag = tags[c.entityIndex];
        auto        req = entityRequirement(tag);
        if (!req) {
            ++c.entityIndex;
            continue;
        }
        bool have = c.creative;
        if (!have) {
            have = true;
            for (auto const& [item, n] : *req) have = have && grabItems(c, item, n, true);
        }
        if (!have) {
            if (c.skipMissing) {
                c.status = "skipping";
                ++c.entityIndex;
                continue;
            }
            c.missing = makeItem(req->front().first, req->front().second).value_or(items::empty());
            c.state   = "PAUSED";
            c.status  = "missingBlock";
            c.dirty = c.viewDirty = true;
            return true;
        }
        if (!c.creative)
            for (auto const& [item, n] : *req) grabItems(c, item, n, false);
        // where it lands: the block of its saved position, transformed like the blocks
        auto const* pos = tag.asCompound()->find("Pos");
        cs::Pos     target = c.sd.anchor;
        if (pos && pos->asList() && pos->asList()->items.size() == 3) {
            auto const& v = pos->asList()->items;
            auto num = [](cs::nbt::Tag const& t) {
                if (auto const* f = std::get_if<float>(&t.v)) return static_cast<double>(*f);
                if (auto const* d = std::get_if<double>(&t.v)) return *d;
                return static_cast<double>(t.asInt().value_or(0));
            };
            auto const& o = c.sch->schematic.worldOrigin;
            cs::Pos local{static_cast<int>(std::floor(num(v[0]) - o.x)), static_cast<int>(std::floor(num(v[1]) - o.y)), static_cast<int>(std::floor(num(v[2]) - o.z))};
            target = c.sd.anchor + cs::apply(c.sd.transform, local);
        }
        sendAim(c, target);
        Flying f;
        f.target = target;
        f.total = f.left = cs::flightTicks(cp(c.pos), f.target);
        f.item           = req->front().first;
        f.entity         = toHex(cs::nbt::write(tag, "", cs::nbt::Endian::Little));
        ++c.entityIndex;
        c.state  = "RUNNING";
        c.status = "placing";
        sendLaunch(c, f);
        c.flying.push_back(std::move(f));
        sound(c.pos, "random.explode", 0.1f, 1.1f);
        particle(c.pos.dim, c.pos.x + 0.5, c.pos.y + 1.2, c.pos.z + 0.5, "minecraft:basic_smoke_particle");
        c.printerCooldown = config().schematicannonDelay;
        c.fuel -= 1;
        c.missing = items::empty();
        c.dirty = c.viewDirty = true;
        return true;
    }
    return false;
}

void finishedPrinting(Cannon& c) {
    c.inv[SchematicIn] = items::empty();
    emptyOutSchematic(c);
    c.state  = "STOPPED";
    c.status = "finished";
    log().info("schematicannon at {} {} {}: finished printing {} ({} blocks)", c.pos.x, c.pos.y, c.pos.z, c.sd.file, c.placed);
    resetPrinter(c);
    // Create: NOTE_BLOCK_BELL at volume 1, pitch 0.7
    sound(c.pos, "note.bell", 1.f, 0.7f);
}

void tickPrinter(Cannon& c) {
    auto& blueprint = c.inv[SchematicIn];
    c.blockSkipped  = false;
    if (items::isEmpty(blueprint) && c.status != "idle" && items::isEmpty(c.inv[SchematicOut])) {
        c.state  = "STOPPED";
        c.status = "idle";
        c.dirty = c.viewDirty = true;
        return;
    }
    if (c.state == "STOPPED") {
        if (c.printerLoaded) resetPrinter(c);
        return;
    }
    // (a creative crate leaves no fuel: still honour the pause)
    if (c.state == "PAUSED" && !c.positionNotLoaded && items::isEmpty(c.missing) && (c.fuel > 0 || c.creative)) return;
    if (!c.printerLoaded) {
        initializePrinter(c);
        return;
    }
    if (c.printerCooldown > 0) {
        --c.printerCooldown;
        return;
    }
    if (c.fuel <= 0 && !c.creative) {
        // refillFuelIfPossible ran this tick already
        c.state  = "PAUSED";
        c.status = "noGunpowder";
        c.dirty = c.viewDirty = true;
        return;
    }
    if (c.creative) {
        c.fuel = 0;
        if (!items::isEmpty(c.missing)) {
            c.missing = items::empty();
            c.state   = "RUNNING";
        }
    }
    if (items::isEmpty(c.missing) && !c.positionNotLoaded) {
        c.current = c.cursor.next([&](cs::Pos q) { return deferred(c, q); });
        if (!c.current) {
            if (tickEntities(c)) return;
            finishedPrinting(c);
            return;
        }
        c.dirty = c.viewDirty = true;
    }
    if (!c.current) return;
    GlobalPos target = targetOf(c, *c.current);
    sendAim(c, cp(target));
    if (!loaded(target)) {
        c.positionNotLoaded = true;
        c.status            = "targetNotLoaded";
        c.state             = "PAUSED";
        return;
    }
    if (c.positionNotLoaded) {
        c.positionNotLoaded = false;
        c.state             = "RUNNING";
    }
    auto const* want = schematicBlock(c, *c.current);
    Requirement req  = want ? requirementOf(*want) : Requirement{true};
    if (req.invalid || !shouldPlaceAt(c, *c.current)) {
        if (c.status != "searching") c.viewDirty = true;
        c.status       = "searching";
        c.blockSkipped = true;
        return;
    }
    if (!req.item.empty()) {
        if (!grabItems(c, req.item, req.count, true)) {
            if (c.skipMissing) {
                c.status       = "skipping";
                c.blockSkipped = true;
                if (!items::isEmpty(c.missing)) {
                    c.missing = items::empty();
                    c.state   = "RUNNING";
                }
                return;
            }
            c.missing = makeItem(req.item, req.count).value_or(items::empty());
            c.state   = "PAUSED";
            c.status  = "missingBlock";
            c.dirty = c.viewDirty = true;
            return;
        }
        grabItems(c, req.item, req.count, false);
    }
    c.state  = "RUNNING";
    c.status = want->isAir() ? "clearing" : "placing";
    launch(c, *c.current, *want, req.item);
    c.printerCooldown = config().schematicannonDelay;
    c.fuel -= 1;
    c.missing = items::empty();
    c.dirty = c.viewDirty = true;
}

/// Create's NBTProcessors in survival: of the block entity data only a sign's text is printed.
void signText(Cannon& c, GlobalPos const& t, Block const& b) {
    if (!c.sch || c.sch->schematic.format != "mcstructure" || !b.getTypeName().ends_with("sign")) return;
    auto const& s     = c.sch->schematic;
    cs::Pos     local = cs::unapply(c.sd.transform, cp(t) - c.sd.anchor);
    if (!s.contains(local)) return;
    auto it = s.blockEntities.find(s.index(local));
    if (it == s.blockEntities.end() || !it->second.asCompound()) return;
    cs::nbt::Compound safe;
    for (auto const* key : {"id", "FrontText", "BackText", "Text", "IsWaxed"})
        if (auto const* v = it->second.asCompound()->find(key)) safe.set(key, *v);
    placeBlockEntity(t, cs::nbt::Tag(std::move(safe)));
}

void landed(Cannon& c, Flying const& f) {
    GlobalPos t = gp(c.pos.dim, f.target);
    if (!loaded(t)) return;
    if (!f.entity.empty()) {
        if (auto tag = cs::nbt::read(fromHex(f.entity), cs::nbt::Endian::Little); tag && c.sch)
            if (placeEntity(c.pos.dim, tag->root, c.sch->schematic.worldOrigin, c.sd.transform, c.sd.anchor))
                log().info("schematicannon at {} {} {}: printed an entity at {} {} {}", c.pos.x, c.pos.y, c.pos.z, t.x, t.y, t.z);
        particle(t.dim, t.x + 0.5, t.y + 0.5, t.z + 0.5, "minecraft:basic_crit_particle");
        return;
    }
    if (f.snbt.empty()) {
        clearAt(t);
    } else if (auto const* b = blockFromSnbt(f.snbt)) {
        place(t, *b);
        signText(c, t, *b);
        for (auto const& p : f.partners)
            if (auto const* pb = blockFromSnbt(p.snbt)) place(gp(c.pos.dim, p.pos), *pb);
        particle(t.dim, t.x + 0.5, t.y + 0.5, t.z + 0.5, "minecraft:basic_crit_particle");
    }
}

void refillFuel(Cannon& c) {
    if (c.creative) return;
    int per = config().schematicannonShotsPerGunpowder;
    if (c.fuel > per) {
        c.fuel  = per;
        c.dirty = c.viewDirty = true;
        return;
    }
    if (c.fuel > 0) return;
    if (!items::isEmpty(c.inv[Gunpowder])) {
        c.inv[Gunpowder] = items::withCount(c.inv[Gunpowder], items::countOf(c.inv[Gunpowder]) - 1);
    } else {
        bool found   = false;
        auto* region = regionOf(c.pos.dim);
        for (int d : c.invDirs) {
            if (!region) break;
            GlobalPos n{c.pos.dim, c.pos.x + kDirs[d].x, c.pos.y + kDirs[d].y, c.pos.z + kDirs[d].z};
            auto got = caps::extract(*region, bp(n), d ^ 1, [](ItemStack const& s) { return items::idOf(s) == "minecraft:gunpowder"; }, 1, false);
            if (!items::isEmpty(got)) {
                found = true;
                break;
            }
        }
        if (!found) return;
    }
    c.fuel += per;
    if (c.status == "noGunpowder") {
        if (c.placed > 0) c.state = "RUNNING";
        c.status = "ready";
    }
    c.dirty = c.viewDirty = true;
}

ItemStack writtenChecklist(Cannon& c) {
    auto book = makeItem("minecraft:written_book");
    if (!book) return items::empty();
    auto entries = c.checklist.entries([](std::string const& id) {
        auto s = makeItem(id);
        return s ? items::displayName(*s) : id;
    });
    std::vector<std::string> names;
    for (auto const& e : entries) {
        auto s = makeItem(e.item);
        names.push_back(s ? items::displayName(*s) : e.item);
    }
    auto pages = cs::checklistPages(entries, names, c.checklist.blocksNotLoaded, std::string(translate("create.materialChecklist.blocksNotLoaded", "* Disclaimer *\n\nMaterial List may be inaccurate due to relevant chunks not being loaded.")));
    cs::nbt::Compound root;
    cs::nbt::List     list{cs::nbt::Type::Compound, {}};
    for (auto const& text : pages) {
        cs::nbt::Compound page;
        page.set("photoname", cs::nbt::Tag(std::string()));
        page.set("text", cs::nbt::Tag(text));
        list.items.emplace_back(std::move(page));
    }
    root.set("pages", cs::nbt::Tag(std::move(list)));
    root.set("author", cs::nbt::Tag(std::string("Schematicannon")));
    root.set("title", cs::nbt::Tag(std::string("§9") + std::string(translate("create.materialChecklist", "Material Checklist"))));
    root.set("generation", cs::nbt::Tag(std::int32_t(0)));
    root.set("xuid", cs::nbt::Tag(std::int64_t(0)));
    if (auto tag = gameTag(cs::nbt::Tag(std::move(root)))) book->setUserData(std::move(tag));
    return *book;
}

/// Create's MaterialChecklist.createWrittenClipboard: 7 entries per page, each with its item icon
/// and amount (missing ones first, then the completed ones checked off), read-only.
ItemStack writtenClipboard(Cannon& c) {
    auto nameOf = [](std::string const& id) {
        auto s = makeItem(id);
        return s ? items::displayName(*s) : id;
    };
    std::vector<std::vector<display::clipboard::Entry>> pages(1);
    if (c.checklist.blocksNotLoaded)
        pages.back().push_back({"§c" + std::string(translate("create.materialChecklist.blocksNotLoaded", "* Disclaimer *\n\nMaterial List may be inaccurate due to relevant chunks not being loaded.")), false});
    int written = 0;
    for (auto const& e : c.checklist.entries(nameOf)) {
        if (written == display::clipboard::kChecklistPageEntries) {
            written = 0;
            pages.emplace_back();
        }
        ++written;
        auto stack = makeItem(e.item);
        auto text  = std::string(e.complete ? "§2" : "") + nameOf(e.item) + " §0x" + std::to_string(e.amount) + "§7 | "
                  + std::to_string(e.amount / 64) + "▤ +" + std::to_string(e.amount % 64);
        pages.back().push_back({text, e.complete, stack ? items::toJson(*stack) : Json(), e.complete ? 0 : e.amount});
    }
    auto clip = display::clipboard::makeWritten(std::move(pages), std::string(translate("create.materialChecklist", "Material Checklist")));
    return clip ? *clip : items::empty();
}

void tickPaperPrinter(Cannon& c) {
    auto& paper = c.inv[BookIn];
    auto& out   = c.inv[BookOut];
    bool  full  = !items::isEmpty(out) && items::countOf(out) >= out.getMaxStackSize();
    if (c.errored) return;
    if (!c.printerLoaded) {
        if (!items::isEmpty(c.inv[SchematicIn])) initializePrinter(c);
        return;
    }
    if (items::isEmpty(paper) || full) {
        if (c.book != 0) c.viewDirty = true;
        c.book                = 0;
        c.dontUpdateChecklist = false;
        return;
    }
    if (c.book >= 1) {
        c.book = 0;
        if (!c.dontUpdateChecklist) updateChecklist(c);
        c.dontUpdateChecklist = true;
        bool clipboard       = items::idOf(paper) == kClipboard;
        paper                 = items::withCount(paper, items::countOf(paper) - 1);
        auto written          = clipboard ? writtenClipboard(c) : writtenChecklist(c);
        out                   = items::withCount(written, items::countOf(out) + 1);
        log().info("schematicannon at {} {} {}: printed a material checklist", c.pos.x, c.pos.y, c.pos.z);
        c.dirty = c.viewDirty = true;
        return;
    }
    c.book += 0.05f;
    c.viewDirty = true;
}

void tickCannon(Cannon& c) {
    if (c.state != "STOPPED" && c.neighbourCooldown-- <= 0) {
        c.neighbourCooldown = kNeighbourCheck;
        findInventories(c);
    }
    if (++c.lazy >= 30) {
        c.lazy = 0;
        findInventories(c);
    }
    // launched blocks
    for (std::size_t i = 0; i < c.flying.size();) {
        auto& f = c.flying[i];
        if (f.left > 0) {
            --f.left;
            ++i;
            continue;
        }
        landed(c, f);
        c.flying.erase(c.flying.begin() + static_cast<std::ptrdiff_t>(i));
        c.dirty = true;
    }
    if (!c.flying.empty()) c.dirty = true;
    tickPaperPrinter(c);
    refillFuel(c);
    int skips      = 1000;
    c.blockSkipped = true;
    while (c.blockSkipped && skips-- > 0) tickPrinter(c);
}

// ---- screen ----------------------------------------------------------------------------------------------

std::string statusText(Player& p, std::string const& status) {
    static std::map<std::string, std::string> const kFallback{
        {"clearing", "Clearing Blocks"},
        {"finished", "Finished"},
        {"idle", "Idle"},
        {"missingBlock", "Missing Item:"},
        {"noGunpowder", "Out of Gunpowder"},
        {"paused", "Paused"},
        {"placing", "Placing"},
        {"ready", "Ready"},
        {"running", "Running"},
        {"schematicErrored", "Incompatible Blocks"},
        {"schematicExpired", "Schematic File Expired"},
        {"schematicInvalid", "Schematic Invalid"},
        {"schematicNotPlaced", "Schematic not Positioned"},
        {"searching", "Searching"},
        {"skipping", "Skipping"},
        {"stopped", "Stopped"},
        {"targetNotLoaded", "Target is not loaded"},
        {"targetOutsideRange", "Target too far away"},
    };
    auto it = kFallback.find(status);
    return trFor(p, "create.schematicannon.status." + status, it == kFallback.end() ? status : it->second);
}

std::optional<GlobalPos> cannonOf(Player& player, Json const& ctx) {
    auto p = posFrom(ctx.value("pos", Json()));
    if (!p || idAt(*p) != kCannon || !inReach(player, *p)) return std::nullopt;
    return p;
}

Json cannonData(Player& player, Cannon& c) {
    Json inv = Json::array();
    for (auto const& s : c.inv) inv.push_back(items::toJson(s));
    int per = config().schematicannonShotsPerGunpowder;
    return Json{
        {"inv",         inv                                                        },
        {"state",       c.state                                                    },
        {"status",      c.status                                                   },
        {"statusText",  statusText(player, c.status)                               },
        {"missing",     items::toJson(c.missing)                                   },
        {"placed",      c.placed                                                   },
        {"toPlace",     c.toPlace                                                  },
        {"progress",    c.toPlace > 0 ? double(c.placed) / c.toPlace : 0.0         },
        {"fuel",        c.fuel                                                     },
        {"perGunpowder", c.creative ? 0 : per                                      },
        {"backup",      items::countOf(c.inv[Gunpowder])                           },
        {"replace",     c.replace                                                  },
        {"skipMissing", c.skipMissing                                              },
        {"replaceBE",   c.replaceBE                                                },
        {"book",        c.book                                                     },
    };
}

bool validFor(int slot, ItemStack const& s) {
    if (items::isEmpty(s)) return false;
    auto id = items::idOf(s);
    switch (slot) {
    case SchematicIn:
        return id == kSchematic;
    case BookIn:
        return id == "minecraft:book" || id == "minecraft:writable_book" || id == kClipboard;
    case Gunpowder:
        return id == "minecraft:gunpowder";
    default:
        return false;
    }
}

int slotFor(ItemStack const& s) {
    for (int slot : {SchematicIn, BookIn, Gunpowder})
        if (validFor(slot, s)) return slot;
    return -1;
}

/// Inserts into a cannon slot; returns the rest.
ItemStack insertInto(Cannon& c, int slot, ItemStack const& s, bool simulate) {
    if (!validFor(slot, s)) return s;
    auto& cur = c.inv[static_cast<std::size_t>(slot)];
    int   max = slot == SchematicIn ? 1 : s.getMaxStackSize();
    if (!items::isEmpty(cur) && !items::sameItemSameData(cur, s)) return s;
    int have = items::countOf(cur);
    int move = std::min(items::countOf(s), max - have);
    if (move <= 0) return s;
    if (!simulate) {
        cur     = items::withCount(s, have + move);
        c.dirty = c.viewDirty = true;
    }
    return items::withCount(s, items::countOf(s) - move);
}

void cannonForm(Player& player, Json const& ctx);

void doAction(Player* player, Cannon& c, Json const& action) {
    auto op = action.value("op", std::string{});
    if (op == "play") {
        c.state = "RUNNING";
        log().info("schematicannon at {} {} {}: started", c.pos.x, c.pos.y, c.pos.z);
    } else if (op == "pause") {
        c.state  = "PAUSED";
        c.status = "paused";
    } else if (op == "stop") {
        c.state  = "STOPPED";
        c.status = "stopped";
        log().info("schematicannon at {} {} {}: stopped", c.pos.x, c.pos.y, c.pos.z);
    } else if (op == "replace") {
        c.replace = std::clamp(action.value("mode", 2), 0, 3);
        updateChecklist(c);
    } else if (op == "settings") {
        int  mode      = std::clamp(action.value("mode", c.replace), 0, 3);
        bool replaceBE = action.value("replaceBE", c.replaceBE);
        bool changed   = mode != c.replace || replaceBE != c.replaceBE;
        c.replace      = mode;
        c.replaceBE    = replaceBE;
        c.skipMissing  = action.value("skipMissing", c.skipMissing);
        if (changed) updateChecklist(c);
    } else if (op == "skipMissing") {
        c.skipMissing = action.value("value", !c.skipMissing);
    } else if (op == "replaceBE") {
        c.replaceBE = action.value("value", !c.replaceBE);
        updateChecklist(c);
    } else if (op == "insert" && player) {
        int   slot = action.value("slot", -1);
        auto& inv  = player->getInventory();
        if (slot < 0 || slot >= inv.getContainerSize()) return;
        auto s      = inv.getItem(slot);
        int  target = slotFor(s);
        if (target < 0) return;
        auto rest  = insertInto(c, target, s, true);
        int  moved = items::countOf(s) - items::countOf(rest);
        if (moved <= 0) return;
        insertInto(c, target, takeFromInventory(*player, slot, moved), false);
    } else if (op == "take" && player) {
        int slot = std::clamp(action.value("slot", 0), 0, 4);
        auto s   = c.inv[static_cast<std::size_t>(slot)];
        if (items::isEmpty(s)) return;
        c.inv[static_cast<std::size_t>(slot)] = items::empty();
        give(*player, s);
    } else {
        return;
    }
    c.dirty = c.viewDirty = true;
    save(c);
}

void cannonForm(Player& player, Json const& ctx) {
    auto p = cannonOf(player, ctx);
    if (!p) return;
    auto& c = cannonAt(*p);
    trackCannon(*p);
    auto  pos = *p;
    std::string content = trFor(player, "create.gui.schematicannon.title", "Schematicannon") + "\n" + statusText(player, c.status);
    if (!items::isEmpty(c.missing)) content += " " + items::displayName(c.missing);
    content += "\n" + std::to_string(c.placed) + " / " + std::to_string(c.toPlace);
    int per = config().schematicannonShotsPerGunpowder;
    content += "\n" + trFor(player, "create.gui.schematicannon.shotsRemaining", "Shots left: {0}", c.fuel);
    content += "\n" + trFor(player, "create.gui.schematicannon.shotsRemainingWithBackup", "With backup: {0}", c.fuel + per * items::countOf(c.inv[Gunpowder]));
    static constexpr std::array<char const*, 5> kSlotKeys{"lv.schematics.cannon.slot.schematic", "lv.schematics.cannon.slot.empty", "lv.schematics.cannon.slot.book", "lv.schematics.cannon.slot.checklist", "lv.schematics.cannon.slot.gunpowder"};
    static constexpr std::array<char const*, 5> kSlotNames{"Schematic", "Used schematics", "Book", "Checklist", "Gunpowder"};
    for (int i = 0; i < 5; ++i) {
        auto const& s = c.inv[static_cast<std::size_t>(i)];
        content += "\n" + trFor(player, kSlotKeys[static_cast<std::size_t>(i)], kSlotNames[static_cast<std::size_t>(i)]) + ": "
                 + (items::isEmpty(s) ? trFor(player, "lv.common.form.empty", "(empty)") : items::displayName(s) + " x" + std::to_string(items::countOf(s)));
    }
    ll::form::SimpleForm form(trFor(player, "create.gui.schematicannon.title", "Schematicannon"), content);
    auto act = [pos](Json a) {
        return [pos, a](Player& pl) {
            if (idAt(pos) != kCannon || !inReach(pl, pos)) return;
            doAction(&pl, cannonAt(pos), a);
            cannonForm(pl, {{"pos", posJson(pos)}});
        };
    };
    form.appendButton(trFor(player, "lv.schematics.cannon.play", "Start printing"), act({{"op", "play"}}));
    form.appendButton(trFor(player, "lv.schematics.cannon.pause", "Pause"), act({{"op", "pause"}}));
    form.appendButton(trFor(player, "lv.schematics.cannon.stop", "Stop and reset"), act({{"op", "stop"}}));
    form.appendButton(trFor(player, "create.gui.schematicannon.showOptions", "Show Printer Settings"), [pos](Player& pl) {
        if (idAt(pos) != kCannon || !inReach(pl, pos)) return;
        auto&                    c2 = cannonAt(pos);
        ll::form::CustomForm     f(trFor(pl, "create.gui.schematicannon.showOptions", "Show Printer Settings"));
        std::vector<std::string> modes{
            trFor(pl, "create.gui.schematicannon.option.dontReplaceSolid", "Don't Replace Solid Blocks"),
            trFor(pl, "create.gui.schematicannon.option.replaceWithSolid", "Replace Solid with Solid"),
            trFor(pl, "create.gui.schematicannon.option.replaceWithAny", "Replace Solid with Any"),
            trFor(pl, "create.gui.schematicannon.option.replaceWithEmpty", "Replace Solid with Empty"),
        };
        f.appendDropdown("replace", trFor(pl, "lv.schematics.cannon.replace", "Replace mode"), modes, c2.replace);
        f.appendToggle("skip", trFor(pl, "create.gui.schematicannon.option.skipMissing", "Skip missing Blocks"), c2.skipMissing);
        f.appendToggle("protect", trFor(pl, "create.gui.schematicannon.option.skipBlockEntities", "Protect Block Entities"), !c2.replaceBE);
        f.sendTo(pl, [pos](Player& p2, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
            if (!r || idAt(pos) != kCannon || !inReach(p2, pos)) return;
            auto num = [&](std::string const& k) -> std::uint64_t {
                auto it = r->find(k);
                if (it == r->end()) return 0;
                auto const* u = std::get_if<uint64>(&it->second);
                return u ? *u : 0;
            };
            doAction(&p2, cannonAt(pos), {{"op", "settings"}, {"mode", static_cast<int>(num("replace"))}, {"skipMissing", num("skip") != 0}, {"replaceBE", num("protect") == 0}});
            cannonForm(p2, {{"pos", posJson(pos)}});
        });
    });
    // inserting held items and taking items out
    form.appendButton(trFor(player, "lv.schematics.cannon.insert_held", "Insert the held item"), [pos](Player& pl) {
        if (idAt(pos) != kCannon || !inReach(pl, pos)) return;
        doAction(&pl, cannonAt(pos), {{"op", "insert"}, {"slot", pl.getSelectedItemSlot()}});
        cannonForm(pl, {{"pos", posJson(pos)}});
    });
    for (int i = 0; i < 5; ++i) {
        if (items::isEmpty(c.inv[static_cast<std::size_t>(i)])) continue;
        form.appendButton(
            trFor(player, "lv.schematics.cannon.take", "Take out: {0}", items::displayName(c.inv[static_cast<std::size_t>(i)])),
            act({{"op", "take"}, {"slot", i}})
        );
    }
    form.sendTo(player);
}

} // namespace

void trackCannon(GlobalPos const& p) {
    if (gCannons.contains(p)) return;
    cannonAt(p);
    persistList();
}

void openCannon(Player& player, GlobalPos const& pos) {
    trackCannon(pos);
    screens::open(player, kScreen, {{"pos", posJson(pos)}});
}

void tickCannons() {
    std::vector<GlobalPos> gone;
    for (auto& [p, c] : gCannons) {
        if (!loaded(p)) continue;
        if (idAt(p) != kCannon) {
            gone.push_back(p);
            continue;
        }
        tickCannon(c);
        save(c);
    }
    for (auto const& p : gone) {
        gCannons.erase(p);
        gViewers.erase(p);
    }
    if (!gone.empty()) persistList();
    // refresh open screens
    if (currentTick() % 4 == 0) {
        for (auto it = gViewers.begin(); it != gViewers.end(); ++it) {
            auto c = gCannons.find(it->first);
            if (c == gCannons.end() || !c->second.viewDirty) continue;
            c->second.viewDirty = false;
            for (auto uid : it->second)
                if (auto* pl = playerById(uid)) screens::refresh(*pl, kScreen, {{"pos", posJson(it->first)}});
        }
    }
}

void resetCannons() {
    gCannons.clear();
    gViewers.clear();
    gReqCache.clear();
}

void loadCannons() {
    auto list = game::db::load(kDbKey);
    if (!list || !list->is_array()) return;
    for (auto const& j : *list)
        if (auto p = posFrom(j)) cannonAt(*p);
}

void cannonRemoved(GlobalPos const& p, Json const& doc) {
    if (doc.contains("inv"))
        for (auto const& s : doc["inv"]) dropAt(p, items::fromJson(s));
    gCannons.erase(p);
    gViewers.erase(p);
    persistList();
}

CannonView cannonView(GlobalPos const& p) {
    auto&      c = cannonAt(p);
    CannonView v;
    v.state   = c.state;
    v.status  = c.status;
    v.placed  = c.placed;
    v.toPlace = c.toPlace;
    v.fuel    = c.fuel;
    v.missing = items::idOf(c.missing);
    return v;
}

void cannonAction(Player* player, GlobalPos const& p, Json const& action) {
    trackCannon(p);
    doAction(player, cannonAt(p), action);
}

void cannonSetSlot(GlobalPos const& p, int slot, ItemStack const& stack) {
    trackCannon(p);
    auto& c                                = cannonAt(p);
    c.inv[static_cast<std::size_t>(std::clamp(slot, 0, 4))] = stack;
    c.dirty = c.viewDirty = true;
    save(c);
}

ItemStack cannonSlot(GlobalPos const& p, int slot) { return cannonAt(p).inv[static_cast<std::size_t>(std::clamp(slot, 0, 4))]; }

namespace detail {

void registerCannon() {
    screens::registerType({
        .id   = kScreen,
        .data = [](Player& p, Json const& ctx) -> std::optional<Json> {
            auto pos = cannonOf(p, ctx);
            if (!pos) return std::nullopt;
            trackCannon(*pos);
            gViewers[*pos].insert(uidOf(p));
            return cannonData(p, cannonAt(*pos));
        },
        .action =
            [](Player& p, Json const& ctx, Json const& action) {
                auto pos = cannonOf(p, ctx);
                if (!pos) return;
                doAction(&p, cannonAt(*pos), action);
            },
        .form = cannonForm,
    });
    // funnels, hoppers, arms: Create's SchematicannonInventory (schematic, paper and gunpowder in;
    // used schematics and checklists out)
    caps::registerBlock(
        std::string(kCannon),
        caps::Handler{
            .slots   = [](common::Access const&) { return 5; },
            .getSlot = [](common::Access const& a, int slot) -> ItemStack {
                GlobalPos p = game::globalPos(a.region, a.pos);
                return slot >= 0 && slot < 5 ? cannonAt(p).inv[static_cast<std::size_t>(slot)] : items::empty();
            },
            .insert = [](common::Access const& a, ItemStack const& stack, bool simulate) -> ItemStack {
                GlobalPos p    = game::globalPos(a.region, a.pos);
                int       slot = slotFor(stack);
                if (slot < 0) return stack;
                trackCannon(p);
                auto& c    = cannonAt(p);
                auto  rest = insertInto(c, slot, stack, simulate);
                if (!simulate) save(c);
                return rest;
            },
            .extract = [](common::Access const& a, caps::ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
                GlobalPos p = game::globalPos(a.region, a.pos);
                auto&     c = cannonAt(p);
                for (int slot : {SchematicOut, BookOut}) {
                    auto& s = c.inv[static_cast<std::size_t>(slot)];
                    if (items::isEmpty(s) || (filter && !filter(s))) continue;
                    int n   = std::min(amount, items::countOf(s));
                    auto out = items::withCount(s, n);
                    if (!simulate) {
                        s       = items::withCount(s, items::countOf(s) - n);
                        c.dirty = c.viewDirty = true;
                        save(c);
                    }
                    return out;
                }
                return items::empty();
            },
        }
    );
}

} // namespace detail

} // namespace lv::feature::schematics
