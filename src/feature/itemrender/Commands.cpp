// `/create itemrender demo x y z` / `/create itemrender clear` (op): a test pattern of display items
// for e2e scenarios and for checking the client mod's item renderer by eye:
// - row z:     items facing south (+Z), the way filter slots / placards show them;
// - row z + 2: the same items lying on the floor (belts / depots);
// - row z + 4: spinning (basins);
// - row z + 6: one item sliding back and forth along x every 40 ticks (interpolation).

#include "ll/api/command/CommandHandle.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/Actor.h"

#include "feature/common/Items.h"
#include "feature/itemrender/DisplayItems.h"
#include "game/WorldDb.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::itemrender {

// Command parameter types need external linkage.
struct DemoParams {
    int x = 0, y = 0, z = 0;
};

namespace {

constexpr char const* kDemoItems[] = {
    "minecraft:stone",
    "minecraft:diamond",
    "minecraft:iron_sword",
    "minecraft:torch",
    "minecraft:oak_stairs",
    "levitate:andesite_casing",
    "minecraft:apple",
};

struct Demo {
    bool          active = false;
    GlobalPos     origin;
    std::uint64_t tick = 0;
};
Demo& demo() {
    static Demo d;
    return d;
}

DisplayItem itemAt(char const* id, double x, double y, double z, float scale, Style style, int count = 1) {
    DisplayItem d;
    if (auto s = common::items::make(id, count)) d.stack = *s;
    d.at       = {x, y, z};
    d.at.scale = scale;
    d.style    = style;
    return d;
}

void place(GlobalPos const& o) {
    clear(o);
    int n = 0;
    for (auto const* id : kDemoItems) {
        double x = o.x + .5 + n * 1.5;
        set(o, n, itemAt(id, x, o.y + .75, o.z + .5, .5f, Style::Facing));
        auto lying = itemAt(id, x, o.y, o.z + 2.5, .5f, Style::Lying, 16);
        lying.pile = true;
        set(o, 100 + n, lying);
        auto spin = itemAt(id, x, o.y + .25, o.z + 4.5, .5f, Style::Lying);
        spin.spin = 4.f;
        set(o, 200 + n, spin);
        ++n;
    }
}

void tickDemo() {
    auto& d = demo();
    if (!d.active) return;
    ++d.tick;
    if (d.tick % 40 != 1) return;
    bool far    = (d.tick / 40) % 2 == 1;
    auto slider = itemAt("minecraft:diamond", d.origin.x + .5 + (far ? 9.0 : 0.0), d.origin.y, d.origin.z + 6.5, .5f, Style::Lying);
    slider.lerpTicks = 40;
    set(d.origin, 300, slider);
}

} // namespace

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    cmd.overload<DemoParams>().text("itemrender").text("demo").required("x").required("y").required("z").execute(
        [](CommandOrigin const& origin, CommandOutput& output, DemoParams const& p) {
            if (!commands::isAdmin(origin)) return output.error(tr("lv.itemrender.no_permission", "You need operator permission."));
            int  dim = origin.getEntity() ? static_cast<int>(origin.getEntity()->getDimensionId()) : 0;
            auto& d  = demo();
            if (d.active) clear(d.origin);
            d = Demo{true, GlobalPos{dim, p.x, p.y, p.z}, 0};
            place(d.origin);
            log().info("itemrender: demo at {} {} {}", p.x, p.y, p.z);
            output.success(tr("lv.itemrender.demo", "Display item test pattern at {} {} {}", p.x, p.y, p.z));
        }
    );
    cmd.overload().text("itemrender").text("clear").execute([](CommandOrigin const& origin, CommandOutput& output) {
        if (!commands::isAdmin(origin)) return output.error(tr("lv.itemrender.no_permission", "You need operator permission."));
        auto& d = demo();
        if (d.active) clear(d.origin);
        d = Demo{};
        log().info("itemrender: demo cleared");
        output.success(tr("lv.itemrender.cleared", "Display item test pattern removed"));
    });
});

class DemoModule {
public:
    void install() {
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) { tickDemo(); });
        game::db::onWorldClosed([] { demo() = Demo{}; });
    }
    void uninstall() { ll::event::EventBus::getInstance().removeListener(mTick); }

private:
    ll::event::ListenerPtr mTick;
};

} // namespace lv::feature::itemrender

LV_REGISTER_MODULE(lv::feature::itemrender::DemoModule);
