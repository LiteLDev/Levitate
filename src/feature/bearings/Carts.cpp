// Cart contraptions (Create's CartAssemblerBlock(Entity), ControllerRailBlock, MountedContraption,
// OrientedContraptionEntity, CouplingHandler / CouplingPhysics, MinecartContraptionItem).
//
// Bedrock minecarts only ride vanilla rail blocks. The cart assembler and the controller rail are
// our own blocks, so carts on them are guided by this unit: held on the rail line at rail height
// and moved along the rail at the speed the rail gives them (SPEC §8).

#include <algorithm>
#include <cmath>
#include <numbers>
#include <map>
#include <set>
#include <unordered_map>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/entity/components_json_legacy/ContainerComponent.h"
#include "mc/world/Container.h"
#include "mc/world/inventory/FillingContainer.h"

#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/phys/AABB.h"

#include "feature/advancements/Advancements.h"
#include "feature/bearings/Bearings.h"
#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/BearingsExt.h"
#include "feature/contraption/Contraption.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include <random>
#include "ll/api/memory/Hook.h"
#include "mc/world/inventory/simulation/validation/ShulkerBoxContainerValidation.h"
#include "mc/world/item/ItemStackBase.h"
#include "mc/world/level/block/DispenserBlock.h"
#include "mc/world/level/block/actor/ShulkerBoxBlockActor.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mod/Module.h"

namespace lv::feature::bearings {

namespace ct = ::lv::feature::contraption;
namespace bd = ::lv::feature::common::blockdata;
namespace vb = ::lv::feature::common::valuebox;
namespace it = ::lv::feature::common::items;

namespace {

constexpr char   kDbKey[]          = "lv:bearings:carts";
constexpr char   kItemData[]       = "lv:minecart_contraption";
constexpr double kMaxCartSpeed     = 0.4;  ///< blocks per tick on a rail (vanilla)
constexpr int    kAssemblyCooldown = 8;    ///< Create's CartAssemblerBlockEntity cooldown (ticks)
constexpr std::size_t kMaxItemData = 512 * 1024; ///< largest contraption an item may carry (bytes of JSON)

/// A contraption riding a minecart (Create's OrientedContraptionEntity on its cart).
struct CartRecord {
    std::int64_t cart    = 0;
    ct::Id       id      = 0;
    int          dim     = 0;
    double       initial = 0; ///< heading of the structure when it was assembled (degrees)
    double       heading = 0;
    int          mode    = 0; ///< CartMovementMode
    std::int64_t coupled = 0; ///< the other cart of a two-cart contraption
    double       baseY   = 0; ///< anchor block y - cart y
    cc::Vec3d    last;        ///< cart position last tick
    int          missing = 0; ///< ticks the cart couldn't be found while its chunk was loaded
    bool         pending = false; ///< placed at `placeAt` on the next tick
    cc::Pos      placeAt;
    double       placeHeading = 0;
};

struct Coupling {
    std::int64_t a = 0, b = 0;
    double       length      = 2;
    bool         contraption = false; ///< made by a two-cart contraption (no item to drop)
    int          dim         = 0;
    cc::Vec3d    lastA, lastB;        ///< positions last seen
    int          missing     = 0;     ///< ticks one cart was missing in a loaded chunk
};

/// A cart counts as broken after this many ticks missing while its chunk is loaded (entities of a
/// freshly loaded chunk may appear a little after the chunk).
constexpr int kMissingTicks = 40;

struct State {
    std::map<std::int64_t, CartRecord>                  carts; // by cart
    std::vector<Coupling>                               couplings;
    std::unordered_map<std::int64_t, std::int64_t>      selection;   // player -> selected cart
    std::unordered_map<std::int64_t, double>            railSpeed;   // cart -> speed along our rail
    std::unordered_map<std::int64_t, std::uint64_t>     railSeen;
    std::map<GP, std::uint64_t>                         cooldown;    // assembler -> last action tick
    std::map<GP, int>                                   railTypes;   // assembler -> rail type (drops)
    std::set<GP>                                        creativeBroken;
    std::map<GP, common::redstone::WatchId>             railWatches; // assembler / controller rail -> redstone watch
    double                                              railY = 0.35; ///< cart y above its rail block (measured on BDS 1.26.51, calibrated)
    bool                                                dirty = false;
    bool                                                calibrated = false;
};

State& state() {
    static State s;
    return s;
}

Json vec(cc::Vec3d const& v) { return Json::array({v.x, v.y, v.z}); }
cc::Vec3d vecOf(Json const& j) {
    if (!j.is_array() || j.size() < 3) return {};
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}
cc::Vec3d ccv(Vec3 const& v) { return {v.x, v.y, v.z}; }
Vec3      mcv(cc::Vec3d const& v) { return Vec3{float(v.x), float(v.y), float(v.z)}; }

bool isMinecart(Actor const& a) {
    switch (a.getEntityTypeId()) {
    case ActorType::MinecartRideable:
    case ActorType::MinecartChest:
    case ActorType::MinecartHopper:
    case ActorType::MinecartTNT:
    case ActorType::MinecartCommandBlock:
    case ActorType::MinecartFurnace:
        return true;
    default:
        return false;
    }
}

/// Create's CartAssemblerBlock.canAssembleTo: rideable, chest (and furnace) minecarts.
bool canAssembleTo(Actor const& a) {
    auto t = a.getEntityTypeId();
    return t == ActorType::MinecartRideable || t == ActorType::MinecartChest || t == ActorType::MinecartFurnace;
}

cc::Vec3d posDelta(Actor& a) { return ccv(a.mBuiltInComponents->mStateVectorComponent->mPosDelta); }
void      setPosDelta(Actor& a, cc::Vec3d const& v) { a.mBuiltInComponents->mStateVectorComponent->mPosDelta = mcv(v); }

/// Axis of a straight rail block (x: east-west), nullopt for curves / no rail.
std::optional<bool> railAxisX(BlockSource& region, BlockPos const& p) {
    if (!region.hasChunksAt(p, 0, false)) return std::nullopt;
    auto const&        block = region.getBlock(p);
    std::string const& id    = block.getTypeName();
    if (id == kCartAssembler || id == kControllerRail) return game::intState(block, "levitate:rail_axis").value_or(0) == 1;
    if (id == "minecraft:rail" || id == "minecraft:golden_rail" || id == "minecraft:detector_rail" || id == "minecraft:activator_rail") {
        int d = game::intState(block, "rail_direction").value_or(0);
        if (d == 1 || d == 2 || d == 3) return true;
        if (d == 0 || d == 4 || d == 5) return false;
    }
    return std::nullopt;
}

bool isVanillaRail(std::string const& id) {
    return id == "minecraft:rail" || id == "minecraft:golden_rail" || id == "minecraft:detector_rail" || id == "minecraft:activator_rail";
}

/// Learns how high a cart rides above its rail block from carts on flat vanilla rails.
void calibrate(BlockSource& region, Actor& cart) {
    auto p  = cart.getPosition();
    auto bp = game::blockPosOf(p);
    for (int dy : {0, -1}) {
        BlockPos q{bp.x, bp.y + dy, bp.z};
        if (!region.hasChunksAt(q, 0, false)) return;
        auto const& b = region.getBlock(q);
        if (!isVanillaRail(b.getTypeName())) continue;
        int d = game::intState(b, "rail_direction").value_or(0);
        if (d != 0 && d != 1) return;
        double off = p.y - q.y;
        // a cart resting on (or rolling along) a flat rail; jumps don't count
        if (std::abs(cart.getPosDelta().y) > 0.05) return;
        // the height is a constant of the game: learnt once per world
        if (!state().calibrated && off >= 0.3 && off < 0.45) {
            state().calibrated = true;
            if (std::abs(off - state().railY) > 1e-3) log().info("minecart rail height calibrated: {:.4f}", off);
            state().railY = off;
        }
        return;
    }
}

std::vector<Actor*> cartsAt(BlockSource& region, GP const& pos) {
    AABB box{Vec3{float(pos.x), float(pos.y) - 0.5f, float(pos.z)}, Vec3{float(pos.x + 1), float(pos.y + 1.5), float(pos.z + 1)}};
    std::vector<Actor*> out;
    for (auto* a : region.getEntities(box, 0.0f)) {
        if (!a || !a->isAlive() || !isMinecart(*a)) continue;
        auto p = a->getPosition();
        if (std::floor(p.x) != pos.x || std::floor(p.z) != pos.z) continue;
        if (p.y < pos.y - 0.5 || p.y >= pos.y + 1) continue;
        out.push_back(a);
    }
    return out;
}

Actor* spawnMinecart(BlockSource& region, cc::Vec3d const& at, std::string const& id, float yaw) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto owner = level->getActorFactory().createSpawnedActor(ActorDefinitionIdentifier(id), nullptr, mcv(at), Vec2{0, yaw});
    if (!owner) return nullptr;
    return level->addEntity(region, std::move(owner));
}

cc::Dir horizontalDirection(Block const& assembler) {
    bool axisX     = game::intState(assembler, "levitate:rail_axis").value_or(0) == 1;
    bool backwards = game::intState(assembler, "levitate:backwards").value_or(0) != 0;
    return cb::railDirection(axisX, backwards);
}

int cartMode(GP const& pos) {
    auto const* d = bd::get(pos);
    if (!d) return 0;
    auto c = d->find("cart");
    return c == d->end() ? 0 : c->value("mode", 0);
}

bool hasPassengers(Actor& cart) { return cart.hasPassenger() || state().carts.contains(uidOf(cart)); }

int couplingCount(std::int64_t cart) {
    int n = 0;
    for (auto const& c : state().couplings)
        if (c.a == cart || c.b == cart) ++n;
    return n;
}

bool connected(std::int64_t from, std::int64_t to) {
    std::set<std::int64_t>  seen{from};
    std::vector<std::int64_t> todo{from};
    while (!todo.empty()) {
        auto cur = todo.back();
        todo.pop_back();
        if (cur == to) return true;
        for (auto const& c : state().couplings) {
            std::int64_t next = c.a == cur ? c.b : c.b == cur ? c.a : 0;
            if (next && seen.insert(next).second) todo.push_back(next);
        }
    }
    return false;
}

void dropCoupling(BlockSource& region, cc::Vec3d const& at) {
    auto level = ll::service::getLevel();
    auto item  = it::make(kMinecartCoupling);
    if (level && item) level->getSpawner().spawnItem(region, *item, nullptr, mcv(at), 10);
}

/// Local extent of a contraption along x (Create uses the entity box's x size).
double widthOf(ct::Contraption const& c) {
    int lo = 0, hi = 0;
    bool first = true;
    for (auto const& b : c.blocks()) {
        lo    = first ? b.local.x : std::min(lo, b.local.x);
        hi    = first ? b.local.x : std::max(hi, b.local.x);
        first = false;
    }
    return first ? 1.0 : double(hi - lo + 1);
}

/// Sets the contraption so its anchor follows the cart (pivot at the cart, turned by `angle`).
void follow(ct::Contraption& c, cc::Vec3d const& cart, double baseY, double angle, std::pair<cc::Axis, double> tilt = {cc::Axis::X, 0}) {
    auto t     = c.transform();
    t.axis     = cc::Axis::Y;
    t.pivot    = {0.5, 0, 0.5};
    t.angle    = angle;
    t.tiltAxis = tilt.first;
    t.tilt     = tilt.second;
    t.offset = cc::Vec3d{cart.x - 0.5, cart.y + baseY, cart.z - 0.5} - t.origin;
    c.setTransform(t);
}

Json recordJson(CartRecord const& r) {
    return {
        {"cart",    r.cart   },
        {"id",      r.id     },
        {"dim",     r.dim    },
        {"initial", r.initial},
        {"heading", r.heading},
        {"mode",    r.mode   },
        {"coupled", r.coupled},
        {"baseY",   r.baseY  },
        {"last",    vec(r.last)},
    };
}

CartRecord recordOf(Json const& j) {
    CartRecord r;
    r.cart    = j.value("cart", std::int64_t{0});
    r.id      = j.value("id", ct::Id{0});
    r.dim     = j.value("dim", 0);
    r.initial = j.value("initial", 0.0);
    r.heading = j.value("heading", 0.0);
    r.mode    = j.value("mode", 0);
    r.coupled = j.value("coupled", std::int64_t{0});
    r.baseY   = j.value("baseY", 0.0);
    r.last    = vecOf(j.value("last", Json::array()));
    return r;
}

// ---- assembly / disassembly ------------------------------------------------------------------

void reportAssembler(GP const& pos, std::string const& text) {
    bd::edit(pos)["cart"]["error"] = text;
    log().info("cart assembler at {} could not assemble: {}", pos.key(), text);
}

void assembleOn(BlockSource& region, GP const& pos, Actor& cart) {
    if (hasPassengers(cart) || !canAssembleTo(cart)) return;
    GP above{pos.dim, pos.x, pos.y + 1, pos.z};
    if (game::replaceable(region, bp(above))) return; // only the anchor: nothing to carry (Create: no exception)
    auto const& block = region.getBlock(bp(pos));

    // a second cart assembler under the structure with a free cart: a two-cart contraption
    ct::AssembleOptions o;
    o.anchor = pos;
    o.start  = {ccp(above)};
    o.axis   = cc::Axis::Y;
    o.pivot  = {0.5, 0, 0.5};
    o.kind   = "cart_assembler";
    auto r   = ct::assemble(o);
    if (!r.ok()) {
        reportAssembler(pos, r.errorText);
        return;
    }
    auto* c = ct::find(*r.id);
    if (!c) return;
    Actor* other = nullptr;
    for (auto const& b : c->blocks()) {
        GP under{pos.dim, pos.x + b.local.x, pos.y + b.local.y - 1, pos.z + b.local.z};
        if (under == pos || idAt(region, under) != kCartAssembler) continue;
        for (auto* a : cartsAt(region, under)) {
            if (a == &cart || !canAssembleTo(*a) || hasPassengers(*a)) continue;
            other = a;
            break;
        }
        if (other) break;
    }
    cc::Vec3d here = ccv(cart.getPosition());
    double    initial = cb::headingOf(horizontalDirection(block));
    if (other) {
        // Create: the carts are coupled through the contraption (fails below 2 blocks)
        cc::Vec3d centre{pos.x + .5, here.y, pos.z + .5};
        cart.teleportTo(mcv(centre), false, 0, 0, false);
        here = centre;
        auto err = coupleCarts(nullptr, uidOf(cart), uidOf(*other));
        if (!err.empty()) {
            ct::disassemble(*r.id); // Create checks before lifting: put it back unchanged
            reportAssembler(pos, tr(err, err));
            return;
        }
        auto there = ccv(other->getPosition());
        initial    = cb::headingOf(there.x - here.x, there.z - here.z);
    }
    CartRecord rec;
    rec.cart    = uidOf(cart);
    rec.id      = *r.id;
    rec.dim     = pos.dim;
    rec.initial = rec.heading = initial;
    rec.mode    = cartMode(pos);
    rec.coupled = other ? uidOf(*other) : 0;
    rec.baseY   = pos.y - here.y;
    rec.last    = here;
    state().carts[rec.cart] = rec;
    state().dirty           = true;
    if (auto& doc = bd::edit(pos)["cart"]; doc.is_object()) doc.erase("error");
    log().info("cart assembler at {} assembled contraption #{} on minecart {}{}", pos.key(), rec.id, rec.cart, other ? " (coupled)" : "");
}

/// Create's CartAssemblerBlockEntity#disassemble: the structure is placed at the assembler, turned
/// to the assembler's direction.
void disassembleOn(BlockSource& region, GP const& pos, Actor& cart) {
    auto found = state().carts.find(uidOf(cart));
    if (found == state().carts.end() || found->second.pending) return;
    auto& rec = found->second;
    if (rec.coupled) {
        // both carts must be on assemblers that disassemble
        auto* other = actorById(rec.coupled);
        if (!other) return;
        GP op = game::globalPos(pos.dim, game::blockPosOf(other->getPosition()));
        if (idAt(region, op) != kCartAssembler) return;
        auto const& ob = region.getBlock(bp(op));
        auto        t  = static_cast<cb::RailType>(game::intState(ob, "levitate:rail_type").value_or(0));
        if (!cb::shouldDisassemble(cb::actionFor(t, game::intState(ob, "levitate:powered").value_or(0) != 0, true))) return;
        std::erase_if(state().couplings, [&](Coupling const& c) {
            return (c.a == rec.cart && c.b == rec.coupled) || (c.b == rec.cart && c.a == rec.coupled);
        });
    }
    rec.pending      = true;
    rec.placeAt      = ccp(pos);
    rec.placeHeading = cb::headingOf(horizontalDirection(region.getBlock(bp(pos))));
    if (rec.coupled) rec.placeHeading = rec.heading; // Create keeps the coupled orientation
    if (auto* c = ct::find(rec.id)) {
        auto t   = c->transform();
        t.angle  = rec.placeHeading - rec.initial;
        t.offset = cc::Vec3d::of(rec.placeAt) - t.origin;
        c->setTransform(t);
    }
    state().dirty = true;
}

void finishPending() {
    for (auto i = state().carts.begin(); i != state().carts.end();) {
        auto& rec = i->second;
        if (!rec.pending) {
            ++i;
            continue;
        }
        if (auto* c = ct::find(rec.id)) {
            disassembleSound(rec.dim, cc::Vec3d::of(rec.placeAt) + cc::Vec3d{.5, .5, .5});
            ct::disassemble(rec.id);
        }
        log().info("cart contraption #{} disassembled at {} {} {}", rec.id, rec.placeAt.x, rec.placeAt.y, rec.placeAt.z);
        i             = state().carts.erase(i);
        state().dirty = true;
    }
}

// ---- guiding carts over our rails --------------------------------------------------------------

void guide(BlockSource& region, GP const& pos, Actor& cart, double speed, bool axisX) {
    cc::Vec3d v = axisX ? cc::Vec3d{speed, 0, 0} : cc::Vec3d{0, 0, speed};
    setPosDelta(cart, v);
    cc::Vec3d p = ccv(cart.getPosition());
    cc::Vec3d target{axisX ? p.x : pos.x + .5, pos.y + state().railY, axisX ? pos.z + .5 : p.z};
    if ((target - p).lengthSqr() > 0.06 * 0.06) cart.teleportTo(mcv(target), false, 0, 0, true);
    (void)region;
}

// ---- couplings drawn by the client mod (Create's CouplingRenderer) ------------------------------

/// The coupled cart pairs as companions get them: [[a, b], ...] (ActorUniqueIDs).
Json couplingsMessage() {
    Json list = Json::array();
    for (auto const& c : state().couplings) list.push_back(Json::array({c.a, c.b}));
    return {{"couplings", std::move(list)}};
}

/// Sends the couplings to the companions when they changed (and the full list to joining ones).
/// The couplings message last sent to the companions (reset with the world).
Json& lastCouplings() {
    static Json last;
    return last;
}

void syncCouplings() {
    static bool joined = false; // join listeners can't be removed: registered once
    if (!joined) {
        joined = true;
        feature::net::onCompanionJoined([](Player& p) { feature::net::send(p, "bearings.couplings", couplingsMessage()); });
    }
    auto& last = lastCouplings();

    Json        msg = couplingsMessage();
    if (msg == last) return;
    last       = msg;
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        feature::net::send(p, "bearings.couplings", msg);
        return true;
    });
}
} // namespace

// ---- rails ------------------------------------------------------------------------------------

void tickRail(BlockSource& region, GP const& pos, Kind kind) {
    auto&       st    = state();
    auto const& block = region.getBlock(bp(pos));
    bool        axisX = game::intState(block, "levitate:rail_axis").value_or(0) == 1;

    // redstone: Create's neighborChanged (common::redstone watch, set up once per rail)
    if (!st.railWatches.contains(pos)) {
        st.railWatches[pos] = common::redstone::watch(pos, [](GP const& at, int signal) {
            auto* r = regionOf(at.dim);
            if (!r || !loaded(*r, at)) return;
            auto const& b  = r->getBlock(bp(at));
            auto const& id = b.getTypeName();
            if (id == kCartAssembler) {
                int powered = signal > 0 ? 1 : 0;
                if (game::intState(b, "levitate:powered").value_or(0) != powered) setStates(*r, at, {{"levitate:powered", powered}});
            } else if (id == kControllerRail) {
                if (game::intState(b, "levitate:power").value_or(0) != signal) setStates(*r, at, {{"levitate:power", signal}});
            }
        });
    }
    auto const& now = region.getBlock(bp(pos));
    if (kind == Kind::CartAssembler) st.railTypes[pos] = game::intState(now, "levitate:rail_type").value_or(0);

    for (auto* cart : cartsAt(region, pos)) {
        auto      uid    = uidOf(*cart);
        cc::Vec3d motion = posDelta(*cart);
        double    along  = axisX ? motion.x : motion.z;
        // the cart is off vanilla rails here: keep the speed we gave it (no off-rail friction),
        // unless something pushed it harder or the other way
        if (auto s = st.railSpeed.find(uid); s != st.railSpeed.end() && st.railSeen[uid] + 2 >= currentTick()) {
            if ((s->second > 0) == (along > 0) && std::abs(along) < std::abs(s->second)) along = s->second;
        }
        double    speed  = along;
        cc::Vec3d centre{pos.x + .5, pos.y + st.railY, pos.z + .5};
        cc::Vec3d offset = centre - ccv(cart->getPosition());

        if (kind == Kind::ControllerRail) {
            bool      backwards = game::intState(now, "levitate:backwards").value_or(0) != 0;
            int       power     = game::intState(now, "levitate:power").value_or(0);
            cc::Vec3d m         = axisX ? cc::Vec3d{along, 0, 0} : cc::Vec3d{0, 0, along};
            auto      v         = cb::controllerVelocity(m, cb::railDirection(axisX, backwards), power, kMaxCartSpeed);
            auto      b         = cb::brakeVelocity(offset);
            speed               = v ? (axisX ? v->x : v->z) : (axisX ? b.x : b.z);
        } else {
            auto type    = static_cast<cb::RailType>(game::intState(now, "levitate:rail_type").value_or(0));
            bool powered = game::intState(now, "levitate:powered").value_or(0) != 0;
            auto action  = cb::actionFor(type, powered, hasPassengers(*cart));
            auto& last   = st.cooldown[pos];
            if (currentTick() >= last + kAssemblyCooldown && (cb::shouldAssemble(action) || cb::shouldDisassemble(action))) {
                last = currentTick();
                if (cb::shouldAssemble(action)) assembleOn(region, pos, *cart);
                if (cb::shouldDisassemble(action)) disassembleOn(region, pos, *cart);
            }
            if (action == cb::CartAction::AssembleAccelerate && std::abs(along) > 1.0 / 128) {
                double dir = along > 0 ? 1 : -1;
                // a redstone conductor at one end sends the cart the other way
                for (int s : {-1, 1}) {
                    GP n = axisX ? GP{pos.dim, pos.x + s, pos.y, pos.z} : GP{pos.dim, pos.x, pos.y, pos.z + s};
                    if (loaded(region, n) && region.getBlock(bp(n)).getBlockType().mSolid) dir = -s;
                }
                speed = dir * kMaxCartSpeed;
            } else if (action == cb::CartAction::AssembleAccelerateDirectional) {
                auto d = cc::offset(horizontalDirection(now));
                speed  = (axisX ? d.x : d.z) * kMaxCartSpeed;
            } else if (action == cb::CartAction::DisassembleBrake) {
                auto b = cb::brakeVelocity(offset);
                speed  = axisX ? b.x : b.z;
            }
        }
        st.railSpeed[uid] = speed;
        st.railSeen[uid]  = currentTick();
        guide(region, pos, *cart, speed, axisX);
    }
}

void railRemoved(GP const& pos, Kind kind) {
    auto& st = state();
    if (kind == Kind::CartAssembler) {
        auto t = st.railTypes.find(pos);
        if (t != st.railTypes.end()) {
            // Create's cart assembler drops the rail it was placed on
            if (!st.creativeBroken.contains(pos)) {
                static char const* rails[]{"minecraft:rail", "minecraft:golden_rail", "minecraft:detector_rail", "minecraft:activator_rail", "levitate:controller_rail"};
                auto* region = regionOf(pos.dim);
                auto  item   = it::make(rails[std::clamp(t->second, 0, 4)]);
                auto  level  = ll::service::getLevel();
                if (region && item && level) level->getSpawner().spawnItem(*region, *item, nullptr, Vec3{pos.x + .5f, pos.y + .5f, pos.z + .5f}, 10);
            }
            st.railTypes.erase(t);
        }
    }
    st.creativeBroken.erase(pos);
    st.cooldown.erase(pos);
    if (auto w = st.railWatches.find(pos); w != st.railWatches.end()) {
        common::redstone::unwatch(w->second);
        st.railWatches.erase(w);
    }
}

Actor* spawnCart(BlockSource& region, cc::Vec3d const& at, std::string const& entityId) { return spawnMinecart(region, at, entityId, 0); }

double cartRailY() { return state().railY; }

void markCreativeBreak(GP const& pos) { state().creativeBroken.insert(pos); }

// ---- carts each tick --------------------------------------------------------------------------

void tickCarts() {
    auto& st = state();
    finishPending();
    // forget rail speeds of carts that left our rails
    for (auto i = st.railSeen.begin(); i != st.railSeen.end();) {
        if (i->second + 2 < currentTick()) {
            st.railSpeed.erase(i->first);
            i = st.railSeen.erase(i);
        } else {
            ++i;
        }
    }
    std::vector<std::int64_t> gone;
    for (auto& [uid, rec] : st.carts) {
        if (rec.pending) continue;
        auto* region = regionOf(rec.dim);
        if (!region) continue;
        auto* c    = ct::find(rec.id);
        auto* cart = actorById(uid);
        if (!c) {
            gone.push_back(uid);
            continue;
        }
        if (!cart) {
            // the cart broke (its chunk is loaded but it is gone): Create's contraption dismounts and is placed
            if (region->hasChunksAt(game::blockPosOf(mcv(rec.last)), 0, false) && ++rec.missing >= kMissingTicks) {
                log().info("minecart {} is gone: cart contraption #{} disassembled", uid, rec.id);
                ct::disassemble(rec.id);
                gone.push_back(uid);
            }
            continue;
        }
        rec.missing = 0;
        // a two-cart contraption whose coupling is gone rides on its first cart alone
        if (rec.coupled && !couplingLength(rec.cart, rec.coupled)) rec.coupled = 0;
        calibrate(*region, *cart);
        cc::Vec3d p    = ccv(cart->getPosition());
        cc::Vec3d diff = p - rec.last;
        rec.last       = p;
        double headingBefore = rec.heading;
        std::pair<cc::Axis, double> tilt{cc::Axis::X, 0};
        if (rec.coupled) {
            if (auto* other = actorById(rec.coupled)) {
                auto o      = ccv(other->getPosition());
                rec.heading = cb::headingOf(o.x - p.x, o.z - p.z);
                // OrientedContraptionEntity#updateOrientation: the pitch follows the height
                // difference of the two carts, about the local axis across the cart line
                double rad = rec.initial * std::numbers::pi / 180.0;
                int    ax  = static_cast<int>(std::lround(std::cos(rad)));
                int    az  = static_cast<int>(std::lround(-std::sin(rad)));
                if (ax != 0 || az != 0) tilt = cc::pitchTilt(ax, az, o - p);
            }
        } else if (static_cast<cb::CartMovementMode>(rec.mode) != cb::CartMovementMode::RotationLocked) {
            cc::Vec3d motion = posDelta(*cart);
            motion.y         = 0;
            if (motion.lengthSqr() < 1e-8) motion = {diff.x, 0, diff.z};
            if (auto axisX = railAxisX(*region, game::blockPosOf(cart->getPosition()))) {
                // follow the rail (Create projects the motion onto the rail vector)
                motion = *axisX ? cc::Vec3d{motion.x, 0, 0} : cc::Vec3d{0, 0, motion.z};
            }
            double len = motion.length();
            if (len > 1e-4) {
                double target   = cb::headingOf(motion.x, motion.z);
                double hint     = cb::shortestAngleDiff(rec.heading, cb::headingOf(diff.x, diff.z));
                double approach = cb::shortestAngleDiff(rec.heading, target, hint);
                double max      = cb::maxYawApproach(len, widthOf(*c));
                rec.heading += std::clamp(approach, -max, max);
                if (std::abs(cb::shortestAngleDiff(rec.heading, target)) < 1) rec.heading = target;
            }
        }
        // Create's OrientedContraptionEntity#isActorActive: "Pause actors while rotating"
        bool rotating = std::abs(cb::shortestAngleDiff(headingBefore, rec.heading)) > 1e-3;
        bool paused   = static_cast<cb::CartMovementMode>(rec.mode) == cb::CartMovementMode::RotatePaused && rotating;
        if (paused != c->actorsPaused()) {
            c->setActorsPaused(paused);
            log().info("cart contraption #{}: actors {} while rotating", rec.id, paused ? "paused" : "resumed");
        }
        follow(*c, p, rec.baseY, rec.heading - rec.initial, tilt);
    }
    for (auto uid : gone) st.carts.erase(uid);
    if (!gone.empty()) st.dirty = true;

    // couplings (Create's CouplingPhysics soft step: keep the coupling length)
    std::vector<std::size_t> broken;
    for (std::size_t i = 0; i < st.couplings.size(); ++i) {
        auto& cp = st.couplings[i];
        auto* a  = actorById(cp.a);
        auto* b  = actorById(cp.b);
        if (a) cp.lastA = ccv(a->getPosition()), cp.dim = static_cast<int>(a->getDimensionId());
        if (b) cp.lastB = ccv(b->getPosition());
        if (!a || !b) {
            // a cart broke (missing although its chunk is loaded): the coupling drops
            auto*     region = regionOf(cp.dim);
            cc::Vec3d where  = a ? cp.lastB : cp.lastA;
            bool      inside = region && region->hasChunksAt(game::blockPosOf(mcv(where)), 0, false);
            if ((a || b) && inside && ++cp.missing >= kMissingTicks) {
                Actor* left = a ? a : b;
                if (!cp.contraption) dropCoupling(*region, ccv(left->getPosition()));
                broken.push_back(i);
            }
            continue;
        }
        cp.missing = 0;
        cc::Vec3d pa = ccv(a->getPosition()), pb = ccv(b->getPosition());
        cc::Vec3d va = posDelta(*a), vb = posDelta(*b);
        auto      clampLen = [](cc::Vec3d v, double max) {
            double l = v.length();
            return l > max ? v * (max / l) : v;
        };
        va = clampLen(va, 1);
        vb = clampLen(vb, 1);
        cc::Vec3d na = pa + va, nb = pb + vb;
        double    stress = cp.length - (na - nb).length();
        if (std::abs(stress) < 1e-3) continue;
        auto correct = [&](Actor& cart, cc::Vec3d const& self, cc::Vec3d const& other, cc::Vec3d v) {
            cc::Vec3d off = self - other;
            double    l   = off.length();
            if (l < 1e-6) return v;
            cc::Vec3d corr = off * (std::clamp(stress, -1.75, 1.75) / l);
            if (auto axisX = railAxisX(*regionOf(static_cast<int>(cart.getDimensionId())), game::blockPosOf(cart.getPosition()))) {
                corr = *axisX ? cc::Vec3d{corr.x, 0, 0} : cc::Vec3d{0, 0, corr.z};
            }
            return clampLen(v + clampLen(corr, kMaxCartSpeed), kMaxCartSpeed);
        };
        cc::Vec3d newA = correct(*a, na, nb, va);
        cc::Vec3d newB = correct(*b, nb, na, vb);
        setPosDelta(*a, newA);
        setPosDelta(*b, newB);
        if (st.railSpeed.contains(cp.a)) st.railSpeed[cp.a] = std::abs(newA.x) > std::abs(newA.z) ? newA.x : newA.z;
        if (st.railSpeed.contains(cp.b)) st.railSpeed[cp.b] = std::abs(newB.x) > std::abs(newB.z) ? newB.x : newB.z;
    }
    for (auto i = broken.rbegin(); i != broken.rend(); ++i) st.couplings.erase(st.couplings.begin() + static_cast<long long>(*i));
    if (!broken.empty()) st.dirty = true;
    if (st.dirty && currentTick() % 20 == 0) saveCarts();
    syncCouplings();
}

// ---- couplings --------------------------------------------------------------------------------

std::string coupleCarts(Player* player, std::int64_t a, std::int64_t b) {
    auto* ca = actorById(a);
    auto* cb_ = actorById(b);
    if (!ca || !cb_ || !isMinecart(*ca) || !isMinecart(*cb_)) return "create.minecart_coupling.unloaded";
    if (a == b) return "create.minecart_coupling.no_loops";
    int distance = static_cast<int>((ccv(ca->getPosition()) - ccv(cb_->getPosition())).length());
    bool contraption = player == nullptr;
    if (distance < 2) {
        if (contraption) return "lv.bearings.coupling_too_close"; // Create: no two-cart contraptions below 2 blocks
        distance = 2;
    }
    if (distance > config::get<Config>().maxCartCouplingLength) return "create.minecart_coupling.too_far";
    if (couplingCount(a) >= 2 || couplingCount(b) >= 2) return "create.minecart_coupling.two_couplings_max";
    if (connected(a, b)) return "create.minecart_coupling.no_loops";
    state().couplings.push_back({a, b, double(distance), contraption});
    state().dirty = true;
    log().info("minecarts {} and {} coupled ({} blocks)", a, b, distance);
    return {};
}

std::optional<double> couplingLength(std::int64_t a, std::int64_t b) {
    for (auto const& c : state().couplings)
        if ((c.a == a && c.b == b) || (c.a == b && c.b == a)) return c.length;
    return std::nullopt;
}

std::optional<std::uint64_t> contraptionOfCart(std::int64_t cartUid) {
    auto f = state().carts.find(cartUid);
    if (f == state().carts.end()) return std::nullopt;
    return f->second.id;
}

// ---- pickup and placement ------------------------------------------------------------------------

bool pickupContraption(Player& player, Actor& cart) {
    if (!config::get<Config>().survivalContraptionPickup && !player.isCreative()) return false;
    auto f = state().carts.find(uidOf(cart));
    if (f == state().carts.end() || f->second.pending) return false;
    bool chest = cart.getEntityTypeId() == ActorType::MinecartChest;
    if (cart.getEntityTypeId() != ActorType::MinecartRideable && !chest) return false; // (no furnace minecart on Bedrock)
    auto& rec = f->second;
    auto* c   = ct::find(rec.id);
    auto* region = regionOf(rec.dim);
    if (!c || !region) return false;
    for (auto const& b : c->blocks()) {
        // Create's ContraptionMovementSetting.isNoPickup (spawners, budding amethyst by default)
        if (ct::movementSetting(b.id) == ct::MovementSetting::NoPickup) {
            actionBar(player, "§c" + trFor(player, "create.contraption.minecart_contraption_illegal_pickup", "A mystical force is binding this Cart Contraption to the world"));
            return true;
        }
    }
    auto snap = ct::ext::snapshot(rec.id);
    // Create's chest minecart contraption: the cart's own inventory travels in the item too
    Json                         cartItems = Json::array();
    std::optional<ContainerComponent*> cartInventory;
    if (chest) {
        if (auto inv = cart.getEntityContext().tryGetComponent<ContainerComponent>()) {
            auto& container = *inv->mContainer;
            cartInventory   = &*inv;
            for (int i = 0; i < container.getContainerSize(); ++i) {
                auto const& s = container.getItem(i);
                if (!s.isNull() && s.mCount > 0) cartItems.push_back({{"slot", i}, {"item", it::toJsonWithCount(s, s.mCount)}});
            }
        }
    }
    if (!snap || snap->dump().size() + cartItems.dump().size() > kMaxItemData) {
        actionBar(player, "§c" + trFor(player, "create.contraption.minecart_contraption_too_big", "This Cart Contraption seems too big to pick up"));
        return true;
    }
    auto item = it::make(chest ? kChestMinecartContraption : kMinecartContraption);
    if (!item) return false;
    if (cartInventory) (*cartInventory)->mContainer->removeAllItems(); // (nothing drops when the cart goes)
    if (rec.coupled) {
        std::erase_if(state().couplings, [&](Coupling const& cp) { return cp.a == rec.cart || cp.b == rec.cart; });
    }
    // Create: the contraption entity is discarded, the item carries the structure
    bool large = c->blocks().size() > 200; // Create's CART_PICKUP
    ct::discard(rec.id);

    Json itemData{{"record", *snap}, {"initial", rec.initial}, {"heading", rec.heading}, {"mode", rec.mode}};
    if (chest) itemData["cart_items"] = std::move(cartItems);
    it::setData(*item, kItemData, itemData);
    give(player, *item);
    log().info("cart contraption #{} picked up by {}", rec.id, player.getRealName());
    if (large) advancements::award(player, "cart_pickup");
    state().carts.erase(f);
    state().dirty = true;
    cart.remove();
    return true;
}

namespace {
/// Places the cart contraption of `held` on `rail`, facing (fx, fz) along the rail.
bool placeFromItem(ItemStack const& held, GP const& rail, int fx, int fz, std::string const& by);
} // namespace

bool placeMinecartContraption(Player& player, GP const& rail) {
    // the structure faces where the player looks, along the rail
    auto [fx, fz] = game::facingOffset(game::playerFacing(player));
    if (!placeFromItem(player.getSelectedItem(), rail, fx, fz, player.getRealName())) return false;
    consumeSelected(player);
    return true;
}

bool dispenseMinecartContraption(BlockSource& region, GP const& dispenser, ItemStack const& stack, cc::Dir facing) {
    // Create's MinecartContraptionItem dispense behaviour: onto a rail in front (or in front, one down)
    if (cc::axisOf(facing) == cc::Axis::Y) return false;
    auto o     = cc::offset(facing);
    GP   front = {dispenser.dim, dispenser.x + o.x, dispenser.y, dispenser.z + o.z};
    auto railAt = [&](GP const& p) {
        auto id = idAt(region, p);
        return isVanillaRail(id) || id == kCartAssembler || id == kControllerRail;
    };
    if (!railAt(front)) {
        GP below{front.dim, front.x, front.y - 1, front.z};
        if (!game::replaceable(region, bp(front)) || !railAt(below)) return false;
        front = below;
    }
    return placeFromItem(stack, front, o.x, o.z, "a dispenser");
}

namespace {
bool placeFromItem(ItemStack const& held, GP const& rail, int fx, int fz, std::string const& by) {
    auto* region = regionOf(rail.dim);
    if (!region) return false;
    std::string heldId = it::idOf(held);
    bool        chest  = heldId == kChestMinecartContraption;
    if (heldId != kMinecartContraption && !chest) return false;
    std::string id = idAt(*region, rail);
    if (!isVanillaRail(id) && id != kCartAssembler && id != kControllerRail) return false;
    auto data = it::getData(held, kItemData);
    if (!data.is_object() || !data.contains("record")) return false;
    if (auto axisX = railAxisX(*region, bp(rail))) {
        if (*axisX && fx == 0) fx = fz, fz = 0;
        if (!*axisX && fz == 0) fz = -fx, fx = 0;
    }
    double heading = cb::headingOf(fx, fz);
    double initial = data.value("initial", 0.0);
    cc::Vec3d at{rail.x + .5, rail.y + state().railY, rail.z + .5};
    auto* cart = spawnMinecart(*region, at, chest ? "minecraft:chest_minecart" : "minecraft:minecart", 0);
    if (!cart) return false;
    if (chest) {
        if (auto inv = cart->getEntityContext().tryGetComponent<ContainerComponent>()) {
            auto& container = *inv->mContainer;
            for (auto const& e : data.value("cart_items", Json::array())) {
                int slot = e.value("slot", -1);
                if (slot >= 0 && slot < container.getContainerSize()) container.setItem(slot, it::fromJson(e.value("item", Json())));
            }
        }
    }
    cc::Transform t;
    t.origin = cc::Vec3d::of(ccp(rail));
    t.axis   = cc::Axis::Y;
    t.pivot  = {0.5, 0, 0.5};
    t.angle  = heading - initial;
    auto cid = ct::ext::restore(data["record"], rail, "cart_assembler", t);
    if (!cid) {
        cart->remove();
        return false;
    }
    CartRecord rec;
    rec.cart    = uidOf(*cart);
    rec.id      = *cid;
    rec.dim     = rail.dim;
    rec.initial = initial;
    rec.heading = heading;
    rec.mode    = data.value("mode", 0);
    rec.baseY   = -state().railY;
    rec.last    = at;
    state().carts[rec.cart] = rec;
    state().dirty           = true;
    log().info("cart contraption #{} placed at {} by {}", *cid, rail.key(), by);
    return true;
}
} // namespace

bool placeMinecartOn(Player& player, GP const& pos, std::string const& itemId) {
    static std::map<std::string, std::string> const carts{
        {"minecraft:minecart",         "minecraft:minecart"        },
        {"minecraft:chest_minecart",   "minecraft:chest_minecart"  },
        {"minecraft:hopper_minecart",  "minecraft:hopper_minecart" },
        {"minecraft:tnt_minecart",     "minecraft:tnt_minecart"    },
        {"minecraft:command_block_minecart", "minecraft:command_block_minecart"},
    };
    auto f = carts.find(itemId);
    if (f == carts.end()) return false;
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    std::string id = idAt(*region, pos);
    if (id != kCartAssembler && id != kControllerRail) return false;
    if (!spawnMinecart(*region, {pos.x + .5, pos.y + state().railY, pos.z + .5}, f->second, 0)) return false;
    consumeSelected(player);
    log().info("minecart placed on {} at {}", id, pos.key());
    return true;
}

bool placeAssembler(Player& player, GP const& clicked, bool consume) {
    auto* region = regionOf(clicked.dim);
    if (!region) return false;
    std::string id = idAt(*region, clicked);
    std::optional<cb::RailType> type;
    if (id == "minecraft:rail") type = cb::RailType::Regular;
    else if (id == "minecraft:golden_rail") type = cb::RailType::Powered;
    else if (id == "minecraft:detector_rail") type = cb::RailType::Detector;
    else if (id == "minecraft:activator_rail") type = cb::RailType::Activator;
    else if (id == kControllerRail) type = cb::RailType::Controller;
    if (!type) {
        actionBar(player, trFor(player, "create.block.cart_assembler.invalid", "Place your Cart Assembler on a rail block"));
        return true;
    }
    auto axisX = railAxisX(*region, bp(clicked));
    int  dir   = id == kControllerRail ? 0 : stateAt(*region, clicked, "rail_direction", 0);
    if (!axisX || dir > 1) return true; // only straight, flat rails (Create: NORTH_SOUTH / EAST_WEST)
    int backwards;
    if (id == kControllerRail) {
        backwards = stateAt(*region, clicked, "levitate:backwards", 0);
    } else {
        auto [fx, fz] = game::facingOffset(game::playerFacing(player));
        backwards     = (fx > 0 || fz > 0) ? 1 : 0; // Create: the player's motion direction is positive
    }
    int powered = redstoneSignal(*region, ccp(clicked)) > 0 ? 1 : 0;
    placeBlock(
        *region,
        clicked,
        kCartAssembler,
        {{"levitate:rail_axis", *axisX ? 1 : 0}, {"levitate:rail_type", static_cast<int>(*type)}, {"levitate:powered", powered}, {"levitate:backwards", backwards}}
    );
    track(clicked);
    if (consume) consumeSelected(player);
    playSound(clicked.dim, {clicked.x + .5, clicked.y + .5, clicked.z + .5}, "dig.stone");
    log().info("cart assembler placed at {} on {} by {}", clicked.key(), id, player.getRealName());
    return true;
}

void orientControllerRail(BlockSource& region, GP const& pos, Player& player) {
    // Create: the rail runs along the player's view; BACKWARDS when looking south or east
    int  facing = game::playerFacing(player); // 0 S, 1 W, 2 N, 3 E
    bool axisX  = facing == 1 || facing == 3;
    bool back   = facing == 0 || facing == 3;
    setStates(region, pos, {{"levitate:rail_axis", axisX ? 1 : 0}, {"levitate:backwards", back ? 1 : 0}});
}

bool cartInteract(Player& player, Actor& target) {
    if (!isMinecart(target)) return false;
    std::string held = it::idOf(player.getSelectedItem());
    auto        uid  = uidOf(target);
    if (held == kWrench) {
        if (state().carts.contains(uid)) return player.canUseAbility(AbilitiesIndex::Build) && pickupContraption(player, target);
        if (couplingCount(uid) == 0) return false;
        int dropped = 0;
        std::erase_if(state().couplings, [&](Coupling const& c) {
            bool hit = (c.a == uid || c.b == uid) && !c.contraption;
            if (hit) ++dropped;
            return hit;
        });
        if (!player.isCreative()) {
            for (int i = 0; i < dropped; ++i)
                if (auto* region = regionOf(static_cast<int>(target.getDimensionId()))) dropCoupling(*region, ccv(target.getPosition()));
        }
        state().dirty = true;
        actionBar(player, trFor(player, "create.minecart_coupling.removed", "Removed all couplings from minecart"));
        log().info("couplings of minecart {} removed by {}", uid, player.getRealName());
        return true;
    }
    if (held != kMinecartCoupling) return false;
    auto  puid = uidOf(player);
    auto& sel  = state().selection;
    auto  prev = sel.find(puid);
    if (prev == sel.end() || !actorById(prev->second)) {
        sel[puid] = uid;
        playSound(static_cast<int>(target.getDimensionId()), ccv(target.getPosition()), "random.click", .5f, 1.f);
        return true;
    }
    if (prev->second == uid) {
        sel.erase(prev);
        return true;
    }
    auto first = prev->second;
    sel.erase(prev);
    auto err = coupleCarts(&player, first, uid);
    if (!err.empty()) {
        static std::map<std::string, std::string> const english{
            {"create.minecart_coupling.too_far",           "Minecarts are too far apart"                       },
            {"create.minecart_coupling.two_couplings_max", "Minecarts cannot have more than two couplings each"},
            {"create.minecart_coupling.unloaded",          "Parts of your train seem to be in unloaded chunks" },
            {"create.minecart_coupling.no_loops",          "Couplings cannot form a loop"                      },
        };
        auto e = english.find(err);
        actionBar(player, "§c" + trFor(player, err, e == english.end() ? err : e->second));
        return true;
    }
    consumeSelected(player);
    playSound(static_cast<int>(target.getDimensionId()), ccv(target.getPosition()), "random.anvil_use", .25f, 1.5f);
    return true;
}

// ---- value box ------------------------------------------------------------------------------------

void registerCartBoxes() {
    vb::BoxSpec box;
    box.id            = "mode";
    box.label         = "create.contraptions.cart_movement_mode";
    box.labelFallback = "Cart Movement Mode";
    box.board         = [](Block const&) {
        return core::common::optionBoard(3, tr("create.contraptions.cart_movement_mode", "Cart Movement Mode"));
    };
    box.centre = [](Block const& block, int face) -> std::optional<core::common::Vec> {
        bool    axisX = game::intState(block, "levitate:rail_axis").value_or(0) == 1;
        auto    f     = static_cast<cc::Dir>(std::clamp(face, 0, 5));
        if (cc::axisOf(f) == cc::Axis::Y || (cc::axisOf(f) == cc::Axis::X) == axisX) return std::nullopt;
        auto n = cc::Vec3d::of(cc::offset(f));
        auto c = cc::Vec3d{0.5, 0.5, 0.5} + n * 0.5;
        return core::common::Vec{c.x, c.y, c.z};
    };
    box.get = [](GP const& p) { return core::common::ValueSettings{0, cartMode(p)}; };
    box.set = [](GP const& p, core::common::ValueSettings v, Player&) { bd::edit(p)["cart"]["mode"] = std::clamp(v.value, 0, 2); };
    box.format = [](Block const&, core::common::ValueSettings v) {
        switch (v.value) {
        case 1:
            return tr("create.contraptions.cart_movement_mode.rotate_paused", "Pause actors while rotating");
        case 2:
            return tr("create.contraptions.cart_movement_mode.rotation_locked", "Lock rotation");
        default:
            return tr("create.contraptions.cart_movement_mode.rotate", "Always face toward motion");
        }
    };
    vb::registerBox(std::string(kCartAssembler), std::move(box));
}

// ---- persistence ----------------------------------------------------------------------------------

void loadCarts() {
    forgetCarts();
    auto j = game::db::load(kDbKey);
    if (!j) return;
    for (auto const& r : j->value("carts", Json::array())) {
        auto rec = recordOf(r);
        if (rec.cart && rec.id) state().carts[rec.cart] = rec;
    }
    for (auto const& c : j->value("couplings", Json::array())) {
        state().couplings.push_back({c.value("a", std::int64_t{0}), c.value("b", std::int64_t{0}), c.value("length", 2.0), c.value("contraption", false)});
    }
    state().railY = j->value("railY", state().railY);
}

void saveCarts() {
    if (!game::db::available()) return;
    Json j{{"carts", Json::array()}, {"couplings", Json::array()}, {"railY", state().railY}};
    for (auto const& [uid, rec] : state().carts) j["carts"].push_back(recordJson(rec));
    for (auto const& c : state().couplings) j["couplings"].push_back({{"a", c.a}, {"b", c.b}, {"length", c.length}, {"contraption", c.contraption}});
    game::db::save(kDbKey, j);
    state().dirty = false;
}

void forgetCarts() {
    lastCouplings() = Json();
    auto railY = state().railY;
    state()    = State{};
    state().railY = railY;
}

} // namespace lv::feature::bearings

// Minecart contraption items (Create's MinecartContraptionItem): dispensers place them onto the
// rail in front, and `minecartContraptionInContainers` (false) keeps them out of shulker boxes
// (Create: canFitInsideContainerItems).
namespace lv::feature::bearings {
namespace cart_items {


namespace {

bool isCartItem(std::string const& id) { return id == kMinecartContraption || id == kChestMinecartContraption; }

bool keptOut(ItemStackBase const& item) {
    return !config::get<Config>().minecartContraptionInContainers && isCartItem(item.getTypeName());
}

/// The dispenser's own random pick (vanilla picks a random filled slot too): a minecart contraption
/// in front of a rail is placed instead of being shot out.
bool dispenseCart(BlockSource& region, BlockPos const& pos) {
    auto* container = region.tryGetContainer(pos).as_ptr();
    if (!container) return false;
    std::vector<int> filled;
    bool             anyCart = false;
    for (int i = 0; i < container->getContainerSize(); ++i) {
        auto const& s = container->getItem(i);
        if (s.isNull() || s.mCount == 0) continue;
        filled.push_back(i);
        anyCart = anyCart || isCartItem(s.getTypeName());
    }
    if (!anyCart) return false;
    static std::mt19937 rng{std::random_device{}()};
    int  slot  = filled[std::uniform_int_distribution<std::size_t>(0, filled.size() - 1)(rng)];
    auto stack = container->getItem(slot);
    if (!isCartItem(stack.getTypeName())) return false;
    auto const& block  = region.getBlock(pos);
    int         facing = game::intState(block, "facing_direction").value_or(0);
    GP          at{static_cast<int>(region.getDimensionId()), pos.x, pos.y, pos.z};
    if (!dispenseMinecartContraption(region, at, stack, static_cast<cc::Dir>(std::clamp(facing, 0, 5)))) return false;
    container->removeItem(slot, 1);
    return true;
}

LL_TYPE_INSTANCE_HOOK(
    CartDispenseHook,
    ll::memory::HookPriority::Normal,
    DispenserBlock,
    &DispenserBlock::$dispenseFrom,
    void,
    BlockSource&    region,
    BlockPos const& pos
) {
    if (!region.getLevel().isClientSide() && getTypeName() == "minecraft:dispenser") {
        try {
            if (dispenseCart(region, pos)) return;
        } catch (std::exception const& e) {
            log().error("bearings: cart dispense failed: {}", e.what());
        }
    }
    origin(region, pos);
}

LL_TYPE_INSTANCE_HOOK(
    CartShulkerScreenHook,
    ll::memory::HookPriority::Normal,
    ShulkerBoxContainerValidation,
    &ShulkerBoxContainerValidation::$isItemAllowedInSlot,
    bool,
    ContainerScreenContext const& screen,
    int const                     slot,
    ItemStackBase const&          item,
    int const                     amount,
    bool                          flag
) {
    if (keptOut(item)) return false;
    return origin(screen, slot, item, amount, flag);
}

LL_TYPE_INSTANCE_HOOK(
    CartShulkerPushHook,
    ll::memory::HookPriority::Normal,
    ShulkerBoxBlockActor,
    &ShulkerBoxBlockActor::$canPushInItem,
    bool,
    int              slot,
    int              face,
    ItemStack const& item
) {
    if (keptOut(item)) return false;
    return origin(slot, face, item);
}

class CartItemHooks {
public:
    void install() {
        CartDispenseHook::hook();
        CartShulkerScreenHook::hook();
        CartShulkerPushHook::hook();
    }
    void uninstall() {
        CartShulkerPushHook::unhook();
        CartShulkerScreenHook::unhook();
        CartDispenseHook::unhook();
    }
};

} // namespace

} // namespace cart_items
} // namespace lv::feature::bearings

LV_REGISTER_MODULE(lv::feature::bearings::cart_items::CartItemHooks);
