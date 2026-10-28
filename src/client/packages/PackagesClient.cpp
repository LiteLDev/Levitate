// Client mod of the packages unit (U25): draws the chains of chain conveyors and the packages
// hanging on them, the frogport's tongue catching / sending packages and the packager's tray,
// and the native package port screen (frogport / postbox). Data comes from the server through
// the companion protocol ("packages.*" channels, src/feature/packages/Sync.cpp).

#ifdef LV_CLIENT

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/common/client/renderer/helpers/MeshHelpers.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/world/item/ItemStack.h"

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "core/packages/Chain.h"
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::packages {

namespace cp    = core::packages;
namespace items = feature::common::items;
using Json      = nlohmann::json;
using Clock     = std::chrono::steady_clock;

namespace {

struct Box {
    cp::V3 prev, now;
    double yaw = 0;
};
struct Chain {
    cp::V3 start, end, start2;
};
struct ConveyorView {
    std::vector<Chain>   chains;
    std::map<int, Box>   boxes;
    Clock::time_point    updated;
};
struct FrogView {
    cp::V3             pos;
    cp::V3             target;
    bool               hasTarget = false;
    bool               active    = false;
    bool               deposit   = false;
    Clock::time_point  started;
};
struct TrayView {
    cp::V3            pos;
    int               facing = 1;
    bool              inward = false;
    bool              hasBox = false;
    Clock::time_point started;
};

std::mutex                                         gMutex;
using Key = std::tuple<int, int, int, int>; // dim, x, y, z
std::map<Key, ConveyorView>                        gConveyors;
std::map<Key, FrogView>                            gFrogs;
std::map<Key, TrayView>                            gTrays;
bool                                               gLoggedFrame = false;

cp::V3 v3(Json const& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }
Key key(int dim, Json const& j) { return {dim, j.at(0).get<int>(), j.at(1).get<int>(), j.at(2).get<int>()}; }

float lin(int c) { return float(std::pow(c / 255.0, 2.2)); }

struct Quad {
    cp::V3 v[4];
    float  r, g, b;
};

/// An axis-aligned box (centre, half extents) as 6 quads.
void cube(std::vector<Quad>& out, cp::V3 c, cp::V3 h, float r, float g, float b) {
    cp::V3 p[8];
    for (int i = 0; i < 8; ++i) p[i] = {c.x + ((i & 1) ? h.x : -h.x), c.y + ((i & 2) ? h.y : -h.y), c.z + ((i & 4) ? h.z : -h.z)};
    int const faces[6][4] = {{2, 3, 7, 6}, {0, 4, 5, 1}, {0, 1, 3, 2}, {4, 6, 7, 5}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    float const shade[6] = {1.f, 0.55f, 0.8f, 0.8f, 0.68f, 0.68f};
    for (int f = 0; f < 6; ++f) out.push_back({{p[faces[f][0]], p[faces[f][1]], p[faces[f][2]], p[faces[f][3]]}, r * shade[f], g * shade[f], b * shade[f]});
}

/// A thin beam from `a` to `b` (chain links / tongue): a box stretched along the segment.
void beam(std::vector<Quad>& out, cp::V3 a, cp::V3 b, double w, float r, float g, float bl) {
    cp::V3 d = b - a;
    double len = d.length();
    if (len < 1e-6) return;
    cp::V3 f    = d * (1 / len);
    cp::V3 up   = std::abs(f.y) > 0.9 ? cp::V3{1, 0, 0} : cp::V3{0, 1, 0};
    cp::V3 s    = f.cross(up).normalized() * w;
    cp::V3 u    = s.cross(f).normalized() * w;
    cp::V3 c[8] = {a - s - u, a + s - u, a + s + u, a - s + u, b - s - u, b + s - u, b + s + u, b - s + u};
    int const faces[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
    for (int i = 0; i < 6; ++i) {
        float k = i < 2 ? 0.7f : (i % 2 ? 0.85f : 1.f);
        out.push_back({{c[faces[i][0]], c[faces[i][1]], c[faces[i][2]], c[faces[i][3]]}, r * k, g * k, bl * k});
    }
}

/// Chain links along a strand: alternating light / dark links.
void strand(std::vector<Quad>& out, cp::V3 a, cp::V3 b) {
    double len   = (b - a).length();
    int    links = std::max(1, int(len / 0.25));
    for (int i = 0; i < links; ++i) {
        cp::V3 p0    = a + (b - a) * (double(i) / links);
        cp::V3 p1    = a + (b - a) * (double(i + 1) / links);
        bool   light = i % 2 == 0;
        beam(out, p0, p1, light ? 0.045 : 0.03, light ? lin(0x8a) : lin(0x55), light ? lin(0x8c) : lin(0x57), light ? lin(0x93) : lin(0x5e));
    }
}

void packageCube(std::vector<Quad>& out, cp::V3 at) { cube(out, at, {0.3, 0.25, 0.3}, lin(0xb8), lin(0x8a), lin(0x52)); }

mce::MaterialPtr& material() {
    static mce::MaterialPtr m(mce::RenderMaterialGroup::switchable(), HashedString("wireframe"));
    return m;
}

void renderPackages(BaseActorRenderContext& ctx) {
    auto* player = currentPlayer();
    if (!player) return;
    auto const& impl = ctx.mImpl;
    if (!impl) return;
    Vec3   origin = impl->mCameraTargetPosition;
    cp::V3 o{origin.x, origin.y, origin.z};
    int    dim = static_cast<int>(player->getDimensionId());
    auto   now = Clock::now();

    std::vector<Quad> quads;
    {
        std::lock_guard lock(gMutex);
        for (auto it = gConveyors.begin(); it != gConveyors.end();) {
            if (now - it->second.updated > std::chrono::seconds(3)) {
                it = gConveyors.erase(it);
                continue;
            }
            if (std::get<0>(it->first) != dim) {
                ++it;
                continue;
            }
            auto const& cv = it->second;
            for (auto const& c : cv.chains) {
                strand(quads, c.start, c.end);
                strand(quads, c.start2, c.start2 + (c.end - c.start));
            }
            double t = std::min(1.0, std::chrono::duration<double>(now - cv.updated).count() / 0.2);
            for (auto const& [id, b] : cv.boxes) {
                cp::V3 p = b.prev + (b.now - b.prev) * t;
                beam(quads, p, p - cp::V3{0, 0.35, 0}, 0.02, lin(0x6b), lin(0x4e), lin(0x2b)); // the rope
                packageCube(quads, p - cp::V3{0, 9 / 16.0, 0});
            }
            ++it;
        }
        for (auto const& [k, f] : gFrogs) {
            if (std::get<0>(k) != dim || !f.active || !f.hasTarget) continue;
            double s = std::chrono::duration<double>(now - f.started).count() / 0.5; // 10 ticks
            if (s >= 1) continue;
            double reach  = s < 0.5 ? s * 2 : (1 - s) * 2;
            cp::V3 mouth  = f.pos + cp::V3{0.5, 0.75, 0.5};
            cp::V3 tip    = mouth + (f.target - mouth) * reach;
            beam(quads, mouth, tip, 0.04, lin(0xd0), lin(0x6a), lin(0x7a));
            // the box rides the tongue: from the frog out (sending) or in from the chain (catching)
            if ((f.deposit && s < 0.5) || (!f.deposit && s >= 0.5)) packageCube(quads, tip - cp::V3{0, 0.3, 0});
        }
        for (auto const& [k, tr] : gTrays) {
            if (std::get<0>(k) != dim || !tr.hasBox) continue;
            double s = std::chrono::duration<double>(now - tr.started).count() / 1.0; // 20 ticks
            if (s >= 1) continue;
            static int const dx[] = {0, 0, 0, 0, -1, 1}, dy[] = {-1, 1, 0, 0, 0, 0}, dz[] = {0, 0, -1, 1, 0, 0};
            double out = tr.inward ? (1 - s) : s;
            cp::V3 c   = tr.pos + cp::V3{0.5, 0.5, 0.5} + cp::V3{double(dx[tr.facing]), double(dy[tr.facing]), double(dz[tr.facing])} * (0.25 + 0.35 * out);
            packageCube(quads, c);
        }
    }
    if (quads.empty()) return;
    ScreenContext& sc   = ctx.mScreenContext;
    Tessellator&   tess = sc.tessellator;
    tess.begin({}, mce::PrimitiveMode::QuadList, int(quads.size() * 4), false);
    for (auto const& q : quads) {
        tess.color(q.r, q.g, q.b, 1.0f);
        for (auto const& v : q.v) tess.vertex(float(v.x - o.x), float(v.y - o.y), float(v.z - o.z));
    }
    MeshHelpers::renderMeshImmediately(sc, tess, material(), OffscreenCaptureDescription{});
    if (!gLoggedFrame) {
        gLoggedFrame = true;
        log().info("packages: rendering chain conveyors ({} quads)", quads.size());
    }
}

// ---- messages ------------------------------------------------------------------------------------

void onConveyors(Json const& msg) {
    std::lock_guard lock(gMutex);
    int dim = msg.value("dim", 0);
    auto now = Clock::now();
    for (auto const& c : msg.value("list", Json::array())) {
        try {
            auto& view = gConveyors[key(dim, c.at("pos"))];
            view.chains.clear();
            for (auto const& ch : c.value("chains", Json::array())) view.chains.push_back({v3(ch.at("start")), v3(ch.at("end")), v3(ch.at("start2"))});
            std::map<int, Box> boxes;
            for (auto const& b : c.value("boxes", Json::array())) {
                int    id  = b.value("id", 0);
                cp::V3 at  = v3(b.at("at"));
                auto   old = view.boxes.find(id);
                boxes[id]  = Box{old != view.boxes.end() ? old->second.now : at, at, b.value("yaw", 0.0)};
            }
            view.boxes   = std::move(boxes);
            view.updated = now;
        } catch (...) {
        }
    }
}

void onFrog(Json const& msg) {
    std::lock_guard lock(gMutex);
    try {
        auto& f     = gFrogs[key(msg.value("dim", 0), msg.at("pos"))];
        f.pos       = v3(msg.at("pos"));
        f.active    = msg.value("active", false);
        f.deposit   = msg.value("deposit", false);
        f.hasTarget = msg.contains("target");
        if (f.hasTarget) f.target = v3(msg.at("target"));
        if (f.active) f.started = Clock::now();
    } catch (...) {
    }
}

void onTray(Json const& msg) {
    std::lock_guard lock(gMutex);
    try {
        auto& t   = gTrays[key(msg.value("dim", 0), msg.at("pos"))];
        t.pos     = v3(msg.at("pos"));
        t.facing  = std::clamp(msg.value("facing", 1), 0, 5);
        t.inward  = msg.value("inward", false);
        t.hasBox  = !msg.value("item", std::string{}).empty();
        t.started = Clock::now();
    } catch (...) {
    }
}

// ---- port screen -----------------------------------------------------------------------------------

using screen::Button;
using screen::Rect;
namespace color = screen::color;

std::shared_ptr<ItemStack> stackOf(Json const& j) {
    auto s = items::fromJson(j);
    if (items::isEmpty(s)) return nullptr;
    return std::make_shared<ItemStack>(s);
}

class PortScreen : public screen::Screen {
public:
    PortScreen(Json const& data, screen::SendFn send)
    : Screen(data.value("postbox", false) ? ctr("lv.packages.postbox", "Postbox") : ctr("lv.packages.frogport", "Package Frogport"), 176, 104),
      mSend(std::move(send)),
      mData(data) {
        showInventory = true;
    }

    void init() override {
        add<screen::Label>(Rect{7, 18, 162, 9}, ctr("create.gui.package_port.catch_packages", "Catch packages addressed to..."), color::kTextDark);
        mAddress = &add<screen::Label>(Rect{7, 30, 120, 9}, "", color::kTextGold, screen::Align::Left, true);
        auto& edit = add<screen::IconButton>(Rect{151, 26, 18, 18}, "A", [this] { mSend({{"op", "address"}}); });
        edit.tooltip = {ctr("lv.packages.port.address", "Edit address"), ctr("create.gui.package_port.catch_packages_wildcard", "Use * as a text wildcard"),
                        ctr("create.gui.package_port.catch_packages_empty", "Leave empty to match non-addressed")};
        for (int i = 0; i < 18; ++i) {
            auto r = core::common::gridCell(7, 46, 9, 18, i);
            mSlots.push_back(&add<screen::Slot>(r, [this, i](Button, bool) { mSend({{"op", "take"}, {"index", i}}); }));
        }
        mSendRecv = &add<screen::IconButton>(Rect{7, 84, 18, 18}, "<>", [this] { mSend({{"op", "accepts"}, {"value", true}}); });
        mSendRecv->tooltip = {ctr("create.gui.package_port.send_and_receive", "Send and receive packages")};
        mSendOnly = &add<screen::IconButton>(Rect{27, 84, 18, 18}, ">", [this] { mSend({{"op", "accepts"}, {"value", false}}); });
        mSendOnly->tooltip = {ctr("create.gui.package_port.send_only", "Only send packages")};
        mMode = &add<screen::Label>(Rect{50, 89, 120, 9}, "", color::kTextDark);
        onData(mData);
    }

    void onData(Json const& data) override {
        mData = data;
        auto address = data.value("address", std::string{});
        mAddress->text = address.empty() ? "-" : address;
        auto inv       = data.value("inventory", Json::array());
        for (std::size_t i = 0; i < mSlots.size(); ++i) {
            mSlots[i]->stack   = i < inv.size() ? stackOf(inv[i]) : nullptr;
            mSlots[i]->tooltip = mSlots[i]->stack ? std::vector<std::string>{mSlots[i]->stack->getName()} : std::vector<std::string>{};
        }
        bool accepts         = data.value("accepts", true);
        mSendRecv->toggled   = accepts;
        mSendOnly->toggled   = !accepts;
        mMode->text = accepts ? ctr("create.gui.package_port.send_and_receive", "Send and receive packages") : ctr("create.gui.package_port.send_only", "Only send packages");
        if (!data.value("target", true)) mMode->text = ctr("create.gui.package_port.not_targeting_anything", "No target selected");
    }

    void clickInventory(int slot, ItemStack const& stack, Button, bool) override {
        if (items::isEmpty(stack)) return;
        mSend({{"op", "insert"}, {"slot", slot}});
    }

    void onClose() override { mSend({{"op", "close"}}); }

private:
    screen::SendFn             mSend;
    Json                       mData;
    screen::Label*             mAddress{};
    screen::Label*             mMode{};
    screen::IconButton *       mSendRecv{}, *mSendOnly{};
    std::vector<screen::Slot*> mSlots;
};

} // namespace

LL_TYPE_INSTANCE_HOOK(
    PackagesRenderHook,
    ll::memory::HookPriority::Normal,
    LevelRendererCamera,
    &LevelRendererCamera::$renderBlockEntities,
    void,
    BaseActorRenderContext& renderContext,
    bool                    renderAlphaLayer
) {
    origin(renderContext, renderAlphaLayer);
    if (renderAlphaLayer) return;
    try {
        renderPackages(renderContext);
    } catch (...) {
    }
}

class PackagesClientModule {
public:
    void install() {
        PackagesRenderHook::hook();
        feature::net::client::on("packages.conveyors", onConveyors);
        feature::net::client::on("packages.conveyor", [](Json const& msg) {
            std::lock_guard lock(gMutex);
            try {
                auto const& p = msg.at("pos");
                gConveyors.erase(Key{p.at(0).get<int>(), p.at(1).get<int>(), p.at(2).get<int>(), p.at(3).get<int>()});
            } catch (...) {
            }
        });
        feature::net::client::on("packages.frog", onFrog);
        feature::net::client::on("packages.frog_anticipate", [](Json const&) {});
        feature::net::client::on("packages.packager", onTray);
        screen::registerServerScreen("packages.port", [](Json const& data, screen::SendFn send) {
            return std::make_unique<PortScreen>(data, std::move(send));
        });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(gMutex);
            gConveyors.clear();
            gFrogs.clear();
            gTrays.clear();
            gLoggedFrame = false;
        });
    }
    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mExit);
        PackagesRenderHook::unhook();
    }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::packages

LV_REGISTER_MODULE(lv::client::packages::PackagesClientModule);

#endif // LV_CLIENT
