// Selftests of the filter slot and value box rules aligned with Create in U55 (dx 5500..5509):
// FilteringBehaviour.canShortInteract / withPredicate / destroy, ValueSettingsInputHandler (any
// item, not the clipboard) and BulkScrollValueBehaviour's Ctrl flag.

#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/BlockData.h"
#include "feature/common/Filter.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {
using lv::selftest::Context;
using JsonT      = nlohmann::json;
namespace common = lv::feature::common;
namespace ia     = lv::feature::common::interactions;
namespace net    = lv::feature::net;
using lv::core::GlobalPos;

constexpr char const* kTestBlock = "minecraft:smooth_stone";

GlobalPos up(GlobalPos p, int dy) { return {p.dim, p.x, p.y + dy, p.z}; }

ItemStack stackOf(Context& ctx, char const* id) {
    auto s = common::items::make(id, 1);
    ctx.require(s.has_value(), std::string("item ") + id + " exists");
    return *s;
}

/// A right-click on face north of `pos` at the face centre through the shared dispatch.
ia::Result click(Player& p, GlobalPos const& pos, ItemStack const& held) {
    auto&       region  = p.getDimensionBlockSource();
    auto const& block   = region.getBlock(lv::game::blockPos(pos));
    std::string blockId = block.getTypeName();
    std::string itemId  = common::items::isEmpty(held) ? "" : common::items::idOf(held);
    ia::UseOnContext c{p, held, itemId, block, blockId, pos, 2, {0.5, 0.5, 0.0}, false, true};
    return ia::dispatchUseOn(c);
}

std::optional<lv::core::common::Vec> northCentre(Block const&, int face) {
    if (face != 2) return std::nullopt;
    return lv::core::common::faceLocalPoint(face, 8, 8);
}
} // namespace

LV_REGISTER_SELFTEST("common.filter_slot_rules", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace fs = common::filterslot;
    auto pos = up(ctx.testPos(5500), 1);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, kTestBlock), "place the test block");
    fs::SlotSpec slot;
    slot.id      = "lv_u55";
    slot.centre  = northCentre;
    slot.accepts = [](GlobalPos const&, ItemStack const& s) { return common::items::idOf(s) != "minecraft:dirt"; };
    fs::registerSlot(kTestBlock, slot);
    auto& p = ctx.player();
    p.teleport(Vec3{pos.x + 0.5f, pos.y + 0.0f, pos.z - 1.5f}, p.getDimensionId());
    co_await ctx.ticks(2);
    // the slot acts next tick; clicks on one block are debounced for 5 ticks
    auto use = [&](char const* id) {
        auto held = stackOf(ctx, id);
        p.setSelectedItem(held);
        return click(p, pos, held);
    };

    // Create's canShortInteract: the wrench and the arm never become the filter
    auto r = use("levitate:wrench");
    co_await ctx.ticks(7);
    ctx.check(r == ia::Result::Consume && common::items::isEmpty(fs::get(pos, "lv_u55")), "the wrench is not put in the slot");
    use("levitate:mechanical_arm");
    co_await ctx.ticks(7);
    ctx.check(common::items::isEmpty(fs::get(pos, "lv_u55")), "the mechanical arm is not put in the slot");
    // ValueSettingsInputHandler skips the clipboard
    use("levitate:clipboard");
    co_await ctx.ticks(7);
    ctx.check(common::items::isEmpty(fs::get(pos, "lv_u55")), "the clipboard does not become the filter");
    // withPredicate: a refused item keeps the old filter
    use("minecraft:stone");
    co_await ctx.ticks(7);
    ctx.check(common::items::idOf(fs::get(pos, "lv_u55")) == "minecraft:stone", "an item sets the filter");
    use("minecraft:dirt");
    co_await ctx.ticks(7);
    ctx.check(common::items::idOf(fs::get(pos, "lv_u55")) == "minecraft:stone", "a refused item keeps the old filter");

    // FilteringBehaviour.destroy: a filter item drops when the block goes (one central listener)
    fs::set(pos, stackOf(ctx, common::filter::kListFilterId), "lv_u55");
    ctx.setBlock(pos, "minecraft:air");
    co_await ctx.ticks(3);
    auto& region = p.getDimensionBlockSource();
    AABB  box{Vec3{pos.x - 1.f, pos.y - 1.f, pos.z - 1.f}, Vec3{pos.x + 2.f, pos.y + 2.f, pos.z + 2.f}};
    bool  dropped = false;
    for (auto* a : region.getEntities(box, 0.f, ActorType::ItemEntity)) {
        if (!a || a->getEntityTypeId() != ActorType::ItemEntity) continue;
        auto& item = static_cast<ItemActor&>(*a);
        if (common::items::idOf(item.mItem) == common::filter::kListFilterId) {
            dropped = true;
            a->remove();
        }
    }
    ctx.check(dropped, "a filter item in a slot drops when the block is broken");
    fs::unregisterSlot(kTestBlock, "lv_u55");
    p.setSelectedItem(common::items::empty());
});

LV_REGISTER_SELFTEST("common.value_box_rules", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace vb = common::valuebox;
    auto pos = up(ctx.testPos(5501), 1);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, kTestBlock), "place the test block");
    struct Seen {
        int  sets = 0, value = 3;
        bool ctrl = false;
    };
    auto seen = std::make_shared<Seen>();
    vb::registerBox(kTestBlock, {
        .id            = "lv_u55",
        .labelFallback = "U55",
        .board         = [](Block const&) { return lv::core::common::plainBoard(16, 4, ""); },
        .centre        = northCentre,
        .get           = [seen](GlobalPos const&) { return lv::core::common::ValueSettings{0, seen->value}; },
        .set =
            [seen](GlobalPos const&, lv::core::common::ValueSettings v, Player&) {
                ++seen->sets;
                seen->value = v.value;
                seen->ctrl  = vb::ctrlDown();
            },
        .bulk = true,
    });
    auto& p = ctx.player();
    p.teleport(Vec3{pos.x + 0.5f, pos.y + 0.0f, pos.z - 1.5f}, p.getDimensionId());
    co_await ctx.ticks(2);
    // ValueSettingsInputHandler: any held item works a value box
    auto stone = stackOf(ctx, "minecraft:stone");
    p.setSelectedItem(stone);
    ctx.check(click(p, pos, stone) == ia::Result::Consume, "a click with an item opens the value box");
    co_await ctx.ticks(7);
    auto clip = stackOf(ctx, "levitate:clipboard");
    p.setSelectedItem(clip);
    click(p, pos, clip);
    co_await ctx.ticks(1);
    ctx.check(seen->sets == 0, "the clipboard passes the value box");
    co_await ctx.ticks(7);

    // BulkScrollValueBehaviour: the Ctrl flag reaches `set`; an unchanged value does nothing
    auto posJ = JsonT::array({pos.dim, pos.x, pos.y, pos.z});
    net::injectFromClient(p, "valuebox.set", {{"pos", posJ}, {"box", "lv_u55"}, {"row", 0}, {"value", 9}, {"ctrl", true}});
    ctx.check(seen->sets == 1 && seen->value == 9 && seen->ctrl, "a Ctrl confirm sets with ctrlDown()");
    ctx.check(!vb::ctrlDown(), "ctrlDown() is only true during set");
    net::injectFromClient(p, "valuebox.set", {{"pos", posJ}, {"box", "lv_u55"}, {"row", 0}, {"value", 9}});
    ctx.check(seen->sets == 1, "an unchanged value is not set again");
    net::injectFromClient(p, "valuebox.set", {{"pos", posJ}, {"box", "lv_u55"}, {"row", 0}, {"value", 5}});
    ctx.check(seen->sets == 2 && !seen->ctrl, "a plain confirm sets without ctrlDown()");
    vb::unregisterBox(kTestBlock, "lv_u55");
    p.setSelectedItem(common::items::empty());
    common::blockdata::erase(pos);
});
