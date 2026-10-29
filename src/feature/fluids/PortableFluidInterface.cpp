// Portable fluid interface (Create's PortableFluidInterfaceBlockEntity + PortableStorageInterface-
// Movement): one on a contraption, one in the world facing it. When the moving one passes the
// stationary one (within 2 blocks along its facing, facing each other) the contraption stalls at
// the closest point and the stationary interface exposes the contraption's tanks to pipes. Every
// transfer keeps the connection for `psiTimeout` ticks; after that (or when the stationary one is
// powered) the contraption moves on.

#include <cmath>

#include "feature/contraption/Contraption.h"
#include "feature/fluids/FluidsInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"

namespace lv::feature::fluids {

namespace cfl = common::fluids;
namespace ctr = feature::contraption;
namespace cc  = core::contraption;

namespace {

struct Station {
    cf::PsiTimer timer;
    ctr::Id      contraption = 0;
};

Station loadStation(GlobalPos const& p) {
    Station s;
    s.timer.timeout = config().psiTimeout;
    if (auto const* d = dataAt(p)) {
        s.timer.transferTimer = d->value("timer", 0);
        s.timer.keepAlive     = d->value("keepAlive", 0);
        s.timer.connected     = d->value("connected", false);
        s.contraption         = d->value("contraption", ctr::Id{0});
    }
    return s;
}

void saveStation(GlobalPos const& p, Station const& s) {
    auto& d = editData(p);
    if (!s.timer.connected && s.timer.transferTimer <= 0) {
        d.erase("timer");
        d.erase("keepAlive");
        d.erase("connected");
        d.erase("contraption");
        return;
    }
    d["timer"]       = s.timer.transferTimer;
    d["keepAlive"]   = s.timer.keepAlive;
    d["connected"]   = s.timer.connected;
    d["contraption"] = s.contraption;
}

/// The contraption the stationary interface at `p` is connected to (nullptr if none / gone).
ctr::Contraption* connectedTo(GlobalPos const&, Station& s) {
    if (!s.timer.connected || !s.timer.isConnected()) return nullptr;
    return ctr::find(s.contraption);
}

/// Create's startTransferringTo.
void startTransferring(GlobalPos const& p, ctr::Id id) {
    auto s = loadStation(p);
    if (s.timer.connected && s.contraption == id) return;
    s.contraption = id;
    s.timer.start();
    saveStation(p, s);
    log().info("portable fluid interface at {} {} {} connected to contraption #{}", p.x, p.y, p.z, id);
    playSound(p, "random.door_open", 0.3f, 1.4f);
}

/// The moving interface's facing in the world (nullopt while it isn't axis aligned).
std::optional<int> worldFacing(ctr::MovementContext const& ctx) {
    auto it = ctx.states.find(kFacing);
    if (it == ctx.states.end()) return std::nullopt;
    auto const* v = std::get_if<std::int64_t>(&it->second);
    if (!v || *v < 0 || *v > 5) return std::nullopt;
    auto      o = cc::offset(static_cast<cc::Dir>(*v));
    cc::Vec3d d = ctx.contraption.transform().rotateVector({double(o.x), double(o.y), double(o.z)});
    static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
    for (int f = 0; f < 6; ++f)
        if (std::abs(d.x - dx[f]) < 1e-3 && std::abs(d.y - dy[f]) < 1e-3 && std::abs(d.z - dz[f]) < 1e-3) return f;
    return std::nullopt;
}

/// Create's getStationaryInterfaceAt: a portable fluid interface facing back, not powered.
bool stationaryAt(GlobalPos const& q, int facing) {
    if (blockIdAt(q) != kPfi) return false;
    if (intStateAt(q, kFacing, -1) != cf::oppositeFace(facing)) return false;
    return !poweredAt(q);
}

GlobalPos posOf(Json const& j) { return {j.at(0).get<int>(), j.at(1).get<int>(), j.at(2).get<int>(), j.at(3).get<int>()}; }

void reset(ctr::MovementContext& ctx) {
    ctx.data.erase("working");
    ctx.data.erase("target");
    ctx.data.erase("stall");
    ctx.stall = false;
}

/// Create's findInterface: a stationary interface directly in front of the moving one or with
/// one block of space between them. The contraption stops where the moving one is now.
bool findInterface(ctr::MovementContext& ctx, GlobalPos const& at) {
    auto facing = worldFacing(ctx);
    if (!facing) return false;
    for (int i = 1; i <= 2; ++i) {
        GlobalPos q = offsetPos(at, *facing, i);
        if (!stationaryAt(q, *facing)) continue;
        ctx.data["working"] = {q.dim, q.x, q.y, q.z};
        ctx.data["target"]  = {at.dim, at.x, at.y, at.z};
        startTransferring(q, ctx.contraption.id());
        return true;
    }
    return false;
}

void actorTick(ctr::MovementContext& ctx) {
    if (!ctx.data.contains("working")) {
        if (!findInterface(ctx, ctx.worldPos)) {
            reset(ctx);
            return;
        }
    }
    GlobalPos q      = posOf(ctx.data["working"]);
    GlobalPos t      = posOf(ctx.data.value("target", ctx.data["working"]));
    cc::Vec3d target = cc::Vec3d::center({t.x, t.y, t.z});
    bool      stall  = ctx.data.value("stall", false);
    // Create: stall where the next step would move away from the stopping point
    if (!stall && (ctx.position - target).length() < (ctx.position + ctx.motion - target).length()) stall = true;
    auto facing = worldFacing(ctx);
    if (!facing) return;
    if (!stationaryAt(q, *facing)) {
        reset(ctx);
        return;
    }
    auto s = loadStation(q);
    if (!s.timer.connected) {
        startTransferring(q, ctx.contraption.id());
        s = loadStation(q);
    }
    s.timer.keepAlive = 2;
    saveStation(q, s);
    // the stationary interface's timer ran down: move on
    if (stall && s.timer.releasesContraption()) {
        stall = false;
        if (ctx.data.value("stall", false)) log().info("portable fluid interface on contraption #{} moves on", ctx.contraption.id());
    }
    ctx.data["stall"] = stall;
    ctx.stall         = stall;
}

} // namespace

void tickPfi(GlobalPos const& p) {
    auto s = loadStation(p);
    if (!s.timer.connected && s.timer.transferTimer <= 0) return;
    bool ended = s.timer.tick(poweredAt(p));
    if (s.timer.connected && !ctr::find(s.contraption)) {
        s.timer.connected     = false;
        s.timer.transferTimer = 0;
        s.timer.keepAlive     = 0;
        ended                 = true;
    }
    if (ended) log().info("portable fluid interface at {} {} {} disconnected", p.x, p.y, p.z);
    saveStation(p, s);
}

void registerPfi() {
    cfl::Handler h;
    h.tanks = [](common::Access const& at) {
        auto p = game::globalPos(at.region, at.pos);
        auto s = loadStation(p);
        std::vector<cfl::Tank> out;
        if (auto* c = connectedTo(p, s)) {
            auto peek  = c->storage().drain("", 1 << 30, true);
            auto probe = peek.first.empty() ? std::string("minecraft:water") : peek.first;
            int  space = c->storage().fill(probe, 1 << 30, true);
            out.push_back({FluidStack{peek.first, peek.second}, peek.second + space});
        }
        return out;
    };
    h.fill = [](common::Access const& at, FluidStack const& stack, bool simulate) {
        auto p = game::globalPos(at.region, at.pos);
        auto s = loadStation(p);
        auto* c = connectedTo(p, s);
        if (!c || stack.empty()) return 0;
        int n = c->storage().fill(stack.fluid, stack.amount, simulate);
        if (!simulate && n > 0) {
            s.timer.transferred();
            saveStation(p, s);
        }
        return n;
    };
    h.drain = [](common::Access const& at, FluidStack const& request, bool simulate) {
        auto p = game::globalPos(at.region, at.pos);
        auto s = loadStation(p);
        auto* c = connectedTo(p, s);
        if (!c || request.amount <= 0) return FluidStack{};
        auto [fluid, n] = c->storage().drain(request.fluid, request.amount, simulate);
        if (n <= 0) return FluidStack{};
        if (!simulate) {
            s.timer.transferred();
            saveStation(p, s);
        }
        return FluidStack{fluid, n};
    };
    cfl::registerBlock(kPfi, h);

    ctr::MovementBehaviour b;
    b.tick             = actorTick;
    b.visitNewPosition = [](ctr::MovementContext& ctx, GlobalPos const& pos) {
        if (!findInterface(ctx, pos)) ctx.data.erase("working");
    };
    b.stopMoving = [](ctr::MovementContext& ctx) { reset(ctx); };
    ctr::registerActor(kPfi, b);
}

} // namespace lv::feature::fluids
