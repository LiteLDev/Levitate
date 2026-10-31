// The contraption engine: assembly, per-tick movement, display entities, carried entities,
// collision, seats, actors, mounted storage, disassembly and persistence.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/entity/components/ActorOwnerComponent.h"
#include "mc/deps/vanilla_components/ActorDataDirtyFlagsComponent.h"
#include "mc/deps/vanilla_components/ActorDataFlagComponent.h"
#include "mc/entity/systems/FallingBlockNormalTickSystem.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDataIDs.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/SynchedActorDataEntityWrapper.h"
#include "mc/world/actor/item/FallingBlockActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockChangeContext.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/Container.h"

#include "core/contraption/Collision.h"
#include "feature/contraption/Internal.h"
#include "feature/net/Net.h"
#include "feature/contraption/Registries.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"

LV_REGISTER_CONFIG("contraption", lv::feature::contraption::Config);

namespace lv::feature::contraption {

namespace {

constexpr char kDbPrefix[]  = "lv:contraption:c:";
constexpr char kDbNextId[]  = "lv:contraption:next_id";
constexpr int  kNoUpdates   = 2; ///< clients only (no neighbour updates)

using json = nlohmann::json;

json vec(cc::Vec3d const& v) { return json::array({v.x, v.y, v.z}); }
json pos(cc::Pos const& p) { return json::array({p.x, p.y, p.z}); }
cc::Vec3d vecOf(json const& j) {
    if (!j.is_array() || j.size() < 3) return {};
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}
cc::Pos posOf(json const& j) {
    if (!j.is_array() || j.size() < 3) return {};
    return {j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
}

Vec3 mcVec(cc::Vec3d const& v) { return Vec3{float(v.x), float(v.y), float(v.z)}; }
cc::Vec3d ccVec(Vec3 const& v) { return {v.x, v.y, v.z}; }

Actor* fetchActor(std::int64_t id) {
    if (!id) return nullptr;
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    ActorUniqueID uid;
    uid.rawID = id;
    auto* a   = level->fetchEntity(uid, false);
    return (a && a->isAlive()) ? a : nullptr;
}

/// Inventory sizes of the vanilla containers that are mounted (Create's mounted item storage).
int containerSize(std::string const& id) {
    if (id == "minecraft:chest" || id == "minecraft:trapped_chest" || id == "minecraft:barrel") return 27;
    if (id.ends_with("shulker_box")) return 27;
    if (id == "minecraft:hopper") return 5;
    if (id == "minecraft:dispenser" || id == "minecraft:dropper") return 9;
    return 0;
}

std::optional<CompoundTag> parseSnbt(std::string const& s) {
    if (s.empty()) return std::nullopt;
    auto r = CompoundTag::fromSnbt(s);
    if (!r) return std::nullopt;
    return std::move(*r);
}

// ---- mounted storage --------------------------------------------------------------------------

class MergedStorage final : public Storage {
public:
    struct ItemSlot {
        int       block;
        int       index;
        ItemStack stack;
    };

    explicit MergedStorage(Contraption& c) : mOwner(c) {}

    void build(std::vector<CarriedBlock> const& blocks) {
        mSlots.clear();
        mTanks.clear();
        mAdapted.clear();
        mViewDirty = true;
        for (int b = 0; b < static_cast<int>(blocks.size()); ++b) {
            auto const&        carried = blocks[b];
            std::string const& id      = carried.id;
            if (int size = containerSize(id)) {
                std::vector<ItemStack> items(size);
                if (auto be = parseSnbt(carried.blockEntity); be && be->contains("Items") && (*be)["Items"].is_array()) {
                    for (auto const& tag : (*be)["Items"].get<ListTag>()) {
                        if (!tag || !tag.hold(Tag::Type::Compound)) continue;
                        auto const& item = static_cast<CompoundTag const&>(*tag);
                        int         slot = item.contains("Slot") ? int(item["Slot"].get<ByteTag>().data) : -1;
                        if (slot < 0 || slot >= size) continue;
                        items[slot] = ItemStack::fromTag(item);
                    }
                }
                for (int i = 0; i < size; ++i) mSlots.push_back({b, i, items[i]});
            }
            if (detail::fluidAdapter(id)) mTanks.push_back(b);
            if (detail::itemAdapter(id)) mAdapted.push_back(b);
        }
    }

    /// The slots of one carried container (a dropper's own inventory).
    std::vector<ItemStack*> slotsOf(int block) {
        std::vector<ItemStack*> out;
        for (auto& s : mSlots)
            if (s.block == block) out.push_back(&s.stack);
        return out;
    }

    /// The carried data of an adapted inventory (Create's mounted item storages of other units),
    /// written through Contraption::blockData.
    nlohmann::json* carriedAt(int b) { return mOwner.blockData(mOwner.blocks()[b].local); }

    /// Writes the slots back into the blocks' NBT (before saving / placing).
    void writeBack(std::vector<CarriedBlock>& blocks) const {
        std::map<int, std::vector<ItemSlot const*>> byBlock;
        for (auto const& s : mSlots) byBlock[s.block].push_back(&s);
        auto ctx = SaveContextFactory::createCloneSaveContext();
        for (auto const& [b, slots] : byBlock) {
            auto be = parseSnbt(blocks[b].blockEntity);
            if (!be) continue;
            ListTag list;
            for (auto const* s : slots) {
                if (s->stack.isNull()) continue;
                auto tag = s->stack.save(*ctx);
                if (!tag) continue;
                (*tag)["Slot"] = static_cast<uchar>(s->index);
                list.add(std::move(tag));
            }
            (*be)["Items"]         = std::move(list);
            blocks[b].blockEntity = be->toSnbt(SnbtFormat::Minimize, 0);
        }
    }

    ItemStack insertItem(ItemStack const& stack, bool simulate) override {
        if (stack.isNull()) return stack;
        ItemStack rest = stack;
        // merge into matching stacks first, then empty slots
        for (int pass = 0; pass < 2 && rest.mCount > 0; ++pass) {
            for (auto& s : mSlots) {
                if (rest.mCount == 0) break;
                bool empty = s.stack.isNull();
                if (pass == 0 ? (empty || !s.stack.isStackable(rest)) : !empty) continue;
                int max   = std::max<int>(1, rest.getMaxStackSize());
                int have  = empty ? 0 : s.stack.mCount;
                int moved = std::min<int>(max - have, rest.mCount);
                if (moved <= 0) continue;
                if (!simulate) {
                    if (empty) {
                        s.stack = rest;
                        s.stack.set(moved);
                    } else {
                        s.stack.set(have + moved);
                    }
                }
                rest.set(rest.mCount - moved);
            }
        }
        // then the inventories of other units (vaults, toolboxes, creative crates, depots)
        for (int b : mAdapted) {
            if (rest.isNull() || rest.mCount == 0) break;
            auto* adapter = detail::itemAdapter(mOwner.blocks()[b].id);
            auto* data    = carriedAt(b);
            if (!adapter || !adapter->insert || !data) continue;
            rest = adapter->insert(*data, rest, simulate);
            if (!simulate) mViewDirty = true;
        }
        if (rest.isNull() || rest.mCount == 0) return ItemStack{};
        return rest;
    }

    ItemStack extractItem(std::function<bool(ItemStack const&)> const& filter, int count, bool simulate) override {
        ItemStack out;
        int       taken = 0;
        for (auto& s : mSlots) {
            if (taken >= count) break;
            if (s.stack.isNull()) continue;
            if (filter && !filter(s.stack)) continue;
            if (!out.isNull() && !out.isStackable(s.stack)) continue;
            int n = std::min<int>(count - taken, s.stack.mCount);
            if (out.isNull()) out = s.stack;
            taken += n;
            if (!simulate) {
                int left = s.stack.mCount - n;
                if (left <= 0) s.stack = ItemStack{};
                else s.stack.set(left);
            }
        }
        for (int b : mAdapted) {
            if (taken >= count) break;
            auto* adapter = detail::itemAdapter(mOwner.blocks()[b].id);
            auto* data    = carriedAt(b);
            if (!adapter || !adapter->extract || !data) continue;
            auto same = [&](ItemStack const& st) { return (!filter || filter(st)) && (out.isNull() || out.isStackable(st)); };
            auto got  = adapter->extract(*data, same, count - taken, simulate);
            if (got.isNull() || got.mCount == 0) continue;
            if (out.isNull()) out = got;
            taken += got.mCount;
            if (!simulate) mViewDirty = true;
        }
        if (out.isNull()) return out;
        out.set(taken);
        return out;
    }

    int slotCount() const override {
        mViewDirty = true; // adapted inventories may change outside (a mounted storage form)
        refreshView();
        return static_cast<int>(mSlots.size() + mView.size());
    }
    ItemStack const& slot(int i) const override {
        if (i < static_cast<int>(mSlots.size())) return mSlots.at(i).stack;
        refreshView();
        return mView.at(static_cast<std::size_t>(i) - mSlots.size());
    }

    int fill(std::string const& fluid, int amount, bool simulate) override {
        int accepted = 0;
        for (int b : mTanks) {
            auto& carried = const_cast<CarriedBlock&>(mOwner.blocks()[b]);
            auto* adapter = detail::fluidAdapter(carried.id);
            if (!adapter) continue;
            auto tank = adapter->read(carried.data);
            if (!tank || (!tank->fluid.empty() && tank->fluid != fluid && tank->amount > 0)) continue;
            int n = std::min(amount - accepted, tank->capacity - tank->amount);
            if (n <= 0) continue;
            accepted += n;
            if (!simulate) {
                tank->fluid = fluid;
                tank->amount += n;
                adapter->write(carried.data, *tank);
            }
            if (accepted >= amount) break;
        }
        return accepted;
    }

    std::pair<std::string, int> drain(std::string const& fluid, int amount, bool simulate) override {
        std::string which = fluid;
        int         got   = 0;
        for (int b : mTanks) {
            auto& carried = const_cast<CarriedBlock&>(mOwner.blocks()[b]);
            auto* adapter = detail::fluidAdapter(carried.id);
            if (!adapter) continue;
            auto tank = adapter->read(carried.data);
            if (!tank || tank->amount <= 0) continue;
            if (!which.empty() && tank->fluid != which) continue;
            which = tank->fluid;
            int n = std::min(amount - got, tank->amount);
            got += n;
            if (!simulate) {
                tank->amount -= n;
                if (tank->amount == 0) tank->fluid.clear();
                adapter->write(carried.data, *tank);
            }
            if (got >= amount) break;
        }
        return {which, got};
    }

private:
    /// The adapted inventories' stacks, listed after the vanilla slots.
    void refreshView() const {
        if (!mViewDirty) return;
        mView.clear();
        for (int b : mAdapted) {
            auto const& carried = mOwner.blocks()[b];
            auto*       adapter = detail::itemAdapter(carried.id);
            if (!adapter || !adapter->slots) continue;
            for (auto& st : adapter->slots(carried.data)) mView.push_back(std::move(st));
        }
        mViewDirty = false;
    }

    Contraption&                   mOwner;
    std::vector<ItemSlot>          mSlots;
    std::vector<int>               mTanks;
    std::vector<int>               mAdapted;
    mutable std::vector<ItemStack> mView;
    mutable bool                   mViewDirty = true;
};

} // namespace

// ---- engine -----------------------------------------------------------------------------------

class Engine {
public:
    static Engine& get() {
        static Engine e;
        return e;
    }

    std::map<Id, std::unique_ptr<Contraption>> live;
    std::unordered_set<std::int64_t>           displays;
    Id                                         nextId = 1;
    /// Client mod sync: contraptions whose display entities changed this tick, actor visuals.
    std::set<Id>                                                  displaysChanged;
    std::map<std::tuple<Id, int, int, int>, json>                 visuals;
    /// Client mod sync: the transform last sent to the companions, per contraption.
    std::map<Id, cc::Transform>                                   sentTransforms;
    /// Vanilla viewers: where a display entity was spawned and when. Clients never apply the
    /// movement the server sends for a falling block that isn't falling, so a display stays
    /// where it appeared on a client without the mod.
    struct Shown {
        cc::Vec3d     feet;
        std::uint64_t tick = 0;
    };
    std::unordered_map<std::int64_t, Shown> shown;
    std::uint64_t                           tickCount = 0;
    /// Displays replaced by a new one (vanilla steps, quarter turns), removed a few ticks later:
    /// a client draws a new falling block only some frames after it appears, so the old one
    /// stays until then (no gap in the structure). Still owned (in `displays`) until removed.
    struct Retiring {
        std::int64_t  display = 0;
        std::uint64_t removeAt = 0;
        Id            contraption = 0;
        cc::Pos       local;
        std::string   id;
        int           quarter = 0; ///< the quarter turns its block shows
    };
    /// Just over the client's draw delay (~125 ms): each tick longer keeps the old display drawn
    /// next to its successor, and with vanilla steps every 4 ticks a moving structure showed
    /// twice (two crosses of sails) for a third of the time at 4 ticks.
    static constexpr int  kRetireTicks = 3;
    std::vector<Retiring> retiring;
    std::set<int>                           vanillaDims; ///< dimensions with players without the client mod
    bool                                    forceVanilla = false; ///< selftests

    // -- client mod sync (channel "contraption.*", see client/contraption/ContraptionClient.h) --
    /// [axis, angle, tx, ty, tz, px, py, pz, tiltAxis, tilt]: world = t + p + R(local - p)
    /// (t = origin + offset, R = the turn about axis after the tilt, see cc::Transform).
    static json transformJson(cc::Transform const& t) {
        auto o = t.origin + t.offset;
        return json::array({int(t.axis), t.angle, o.x, o.y, o.z, t.pivot.x, t.pivot.y, t.pivot.z, int(t.tiltAxis), t.tilt});
    }
    static bool sameTransform(cc::Transform const& a, cc::Transform const& b) {
        auto d = (a.origin + a.offset) - (b.origin + b.offset);
        return a.axis == b.axis && a.angle == b.angle && d.lengthSqr() == 0 && (a.pivot - b.pivot).lengthSqr() == 0
            && a.tiltAxis == b.tiltAxis && a.tilt == b.tilt;
    }
    json blocksMessage(Contraption const& c) const {
        json blocks = json::array();
        for (auto const& b : c.mBlocks) {
            if (!b.display) continue;
            // [display, x, y, z, id, collision box (local, relative to the block) or null]
            json box = b.collision ? json::array({b.collision->min.x, b.collision->min.y, b.collision->min.z, b.collision->max.x, b.collision->max.y, b.collision->max.z}) : json();
            blocks.push_back(json::array({b.display, b.local.x, b.local.y, b.local.z, b.id, std::move(box)}));
        }
        json old = json::array();
        for (auto const& r : retiring)
            if (r.contraption == c.mId) old.push_back(json::array({r.display, r.local.x, r.local.y, r.local.z, r.id, r.quarter}));
        json vis = json::array();
        for (auto const& [key, v] : visuals) {
            auto const& [id, x, y, z] = key;
            if (id == c.mId) vis.push_back(json::array({x, y, z, v}));
        }
        return {
            {"id",      c.mId                                 },
            {"dim",     c.mDim                                },
            {"axis",    int(c.mTransform.axis)                },
            {"quarter", c.mDisplayQuarter                         },
            {"transform", transformJson(c.mTransform)         },
            {"blocks",  std::move(blocks)                     },
            {"retiring", std::move(old)                       },
            {"visuals", std::move(vis)                        },
        };
    }
    static void sendToDim(int dim, std::string_view channel, json const& msg) {
        auto level = ll::service::getLevel();
        if (!level) return;
        level->forEachPlayer([&](Player& p) {
            if (static_cast<int>(p.getDimensionId()) == dim) net::send(p, channel, msg);
            return true;
        });
    }
    /// Companions get each changed transform once per tick ("contraption.motion") and move the
    /// display entities from it (Create's approach: the client interpolates the motion).
    void syncMotion() {
        std::map<int, json> byDim;
        for (auto const& [id, c] : live) {
            auto it = sentTransforms.find(id);
            if (it != sentTransforms.end() && sameTransform(it->second, c->mTransform)) continue;
            sentTransforms[id] = c->mTransform;
            auto entry = transformJson(c->mTransform);
            entry.insert(entry.begin(), id);
            auto& list = byDim[c->mDim];
            if (list.is_null()) list = json::array();
            list.push_back(std::move(entry));
        }
        for (auto& [dim, list] : byDim) sendToDim(dim, "contraption.motion", {{"m", std::move(list)}});
    }
    void syncClients() {
        syncMotion();
        if (displaysChanged.empty()) return;
        auto changed = std::move(displaysChanged);
        displaysChanged.clear();
        for (Id id : changed)
            if (auto it = live.find(id); it != live.end()) sendToDim(it->second->mDim, "contraption.blocks", blocksMessage(*it->second));
    }
    void syncAllTo(Player& player) {
        if (!net::isCompanion(player)) return;
        net::send(player, "contraption.reset", json::object());
        for (auto const& [id, c] : live)
            if (c->mDim == static_cast<int>(player.getDimensionId())) net::send(player, "contraption.blocks", blocksMessage(*c));
        // (the blocks messages carry the current transforms; motion follows from the next change)
    }

    void install() {
        static bool joinRegistered = false; // the protocol has no removal of join listeners
        if (!joinRegistered) {
            joinRegistered = true;
            net::onCompanionJoined([this](Player& p) { syncAllTo(p); });
        }
        game::db::onWorldReady([this] { loadAll(); });
        game::db::onWorldSave([this] { saveAll(); });
        game::db::onWorldClosed([this] {
            removeAllDisplays(); // never leave falling blocks in the saved world without the hooks
            clear();
        });
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([this](auto&) {
            tick();
        });
        if (game::db::available()) loadAll();
    }

    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mTick);
        if (game::db::available()) {
            saveAll();
            removeAllDisplays();
        }
        clear();
    }

    void removeAllDisplays() {
        for (auto& [id, c] : live)
            for (auto& b : c->mBlocks)
                if (auto* a = fetchActor(b.display)) a->remove();
        removeRetiring([](Retiring const&) { return true; });
    }

    /// Removes the retired displays `pred` selects; returns the contraptions concerned.
    template <class Pred>
    std::set<Id> removeRetiring(Pred pred) {
        std::set<Id> touched;
        std::erase_if(retiring, [&](Retiring const& r) {
            if (!pred(r)) return false;
            if (auto* a = fetchActor(r.display)) a->remove();
            displays.erase(r.display);
            touched.insert(r.contraption);
            return true;
        });
        return touched;
    }

    /// Replaces the display of `b` (spawned again by the caller); the old one goes a little later.
    void retireDisplay(Contraption const& c, CarriedBlock& b, int quarter) {
        retiring.push_back({b.display, tickCount + kRetireTicks, c.mId, b.local, b.id, quarter});
        shown.erase(b.display);
        b.display = 0;
    }

    // -- persistence --
    json toJson(Contraption& c) {
        if (c.mStorage) static_cast<MergedStorage&>(*c.mStorage).writeBack(c.mBlocks);
        json j;
        j["id"]     = c.mId;
        j["dim"]    = c.mDim;
        j["kind"]   = c.mKind;
        j["anchor"] = json::array({c.mAnchor.x, c.mAnchor.y, c.mAnchor.z});
        auto& t     = c.mTransform;
        j["transform"] = {
            {"origin", vec(t.origin)       },
            {"offset", vec(t.offset)       },
            {"axis",   int(t.axis)         },
            {"angle",  t.angle             },
            {"pivot",  vec(t.pivot)        },
            {"tiltAxis", int(t.tiltAxis)   },
            {"tilt",   t.tilt              }
        };
        j["motion"] = {
            {"velocity", vec(c.mMotion.velocity)    },
            {"angular",  c.mMotion.angularVelocity  },
            {"follow",   c.mMotion.follow.value_or(0)},
            {"followOffset", vec(c.mMotion.followOffset)},
            {"ticks",    c.mMotion.ticks            }
        };
        j["glue"] = json::array();
        for (auto const& g : c.mGlue) j["glue"].push_back({pos(g.min), pos(g.max)});
        j["seated"] = json::array();
        for (auto const& [p, uid] : c.mSeated) j["seated"].push_back({pos(p), uid});
        j["anchorData"] = c.mAnchorData;
        j["blocks"]     = json::array();
        for (auto const& b : c.mBlocks) {
            json e{
                {"p",  pos(b.local)},
                {"b",  b.block     },
                {"be", b.blockEntity},
                {"d",  b.data      },
                {"a",  b.actorData },
                {"e",  b.display   },
                {"br", b.brittle   }
            };
            if (b.collision) e["c"] = {vec(b.collision->min), vec(b.collision->max)};
            j["blocks"].push_back(std::move(e));
        }
        return j;
    }

    std::unique_ptr<Contraption> fromJson(json const& j) {
        auto c   = std::make_unique<Contraption>();
        c->mId   = j.value("id", Id{0});
        c->mDim  = j.value("dim", 0);
        c->mKind = j.value("kind", std::string("test"));
        auto a   = posOf(j.value("anchor", json::array()));
        c->mAnchor = {c->mDim, a.x, a.y, a.z};
        auto const& t      = j.value("transform", json::object());
        c->mTransform.origin = vecOf(t.value("origin", json::array()));
        c->mTransform.offset = vecOf(t.value("offset", json::array()));
        c->mTransform.axis   = static_cast<cc::Axis>(std::clamp(t.value("axis", 1), 0, 2));
        c->mTransform.angle  = t.value("angle", 0.0);
        c->mTransform.pivot  = vecOf(t.value("pivot", json::array({0.5, 0.5, 0.5})));
        c->mTransform.tiltAxis = static_cast<cc::Axis>(std::clamp(t.value("tiltAxis", 0), 0, 2));
        c->mTransform.tilt     = t.value("tilt", 0.0);
        auto const& m        = j.value("motion", json::object());
        c->mMotion.velocity        = vecOf(m.value("velocity", json::array({0, 0, 0})));
        c->mMotion.angularVelocity = m.value("angular", 0.0);
        if (auto f = m.value("follow", std::int64_t{0})) c->mMotion.follow = f;
        c->mMotion.followOffset = vecOf(m.value("followOffset", json::array({0, 0, 0})));
        c->mMotion.ticks        = m.value("ticks", -1);
        for (auto const& g : j.value("glue", json::array())) c->mGlue.push_back({posOf(g[0]), posOf(g[1])});
        for (auto const& s : j.value("seated", json::array())) c->mSeated.emplace_back(posOf(s[0]), s[1].get<std::int64_t>());
        c->mAnchorData = j.value("anchorData", json::object());
        for (auto const& e : j.value("blocks", json::array())) {
            CarriedBlock b;
            b.local       = posOf(e["p"]);
            b.block       = e.value("b", std::string());
            b.blockEntity = e.value("be", std::string());
            b.data        = e.value("d", json::object());
            b.actorData   = e.value("a", json::object());
            b.display     = e.value("e", std::int64_t{0});
            b.brittle     = e.value("br", false);
            if (e.contains("c")) b.collision = cc::Box{vecOf(e["c"][0]), vecOf(e["c"][1])};
            if (auto const* block = detail::blockFromSnbt(b.block)) {
                b.states = detail::statesOf(*block);
                b.id     = block->getTypeName();
            }
            c->mBlocks.push_back(std::move(b));
        }
        finish(*c);
        return c;
    }

    /// Caches and storage after the blocks are known.
    void finish(Contraption& c) {
        c.mBoxes.clear();
        bool first = true;
        for (auto const& b : c.mBlocks) {
            auto unit = cc::Box::unit(b.local);
            if (first) c.mBounds = unit;
            c.mBounds.min = {std::min(c.mBounds.min.x, unit.min.x), std::min(c.mBounds.min.y, unit.min.y), std::min(c.mBounds.min.z, unit.min.z)};
            c.mBounds.max = {std::max(c.mBounds.max.x, unit.max.x), std::max(c.mBounds.max.y, unit.max.y), std::max(c.mBounds.max.z, unit.max.z)};
            first         = false;
            if (b.collision) c.mBoxes.push_back(b.collision->moved(cc::Vec3d::of(b.local)));
            if (b.display) displays.insert(b.display);
        }
        refreshBoxBlocks(c);
        auto storage = std::make_unique<MergedStorage>(c);
        storage->build(c.mBlocks);
        c.mStorage = std::move(storage);
        c.mDisplayQuarter = cc::quarterTurnsOf(c.mTransform.angle);
    }

    void save(Contraption& c) { game::db::save(kDbPrefix + std::to_string(c.mId), toJson(c)); }

    void saveAll() {
        if (!game::db::available()) return;
        for (auto& [id, c] : live) save(*c);
        game::db::save(kDbNextId, json{{"next", nextId}});
    }

    void loadAll() {
        clear();
        if (auto n = game::db::load(kDbNextId)) nextId = std::max<Id>(1, n->value("next", Id{1}));
        std::vector<std::string> keys;
        game::db::forEachKey(kDbPrefix, [&](std::string_view suffix) { keys.emplace_back(suffix); });
        for (auto const& k : keys) {
            auto j = game::db::load(kDbPrefix + k);
            if (!j) continue;
            try {
                auto c = fromJson(*j);
                if (!c->mId) continue;
                nextId = std::max(nextId, c->mId + 1);
                live[c->mId] = std::move(c);
            } catch (std::exception const& e) {
                log().error("contraption {} could not be loaded: {}", k, e.what());
            }
        }
        if (!live.empty()) log().info("contraptions loaded: {}", live.size());
    }

    void clear() {
        collisionCache.clear();
        live.clear();

        displays.clear();
        displaysChanged.clear();
        visuals.clear();
        sentTransforms.clear();
        shown.clear();
        vanillaDims.clear();
        retiring.clear();
    }

    // -- display entities --
    static void setDisplayRotation(::Actor& actor, DisplayRotation const& rot) {
        actor.mBuiltInComponents->mActorRotationComponent->mRot = Vec2{rot.pitch, rot.yaw};
    }

    std::int64_t spawnDisplay(BlockSource& region, Block const& block, cc::Vec3d const& at, cc::Axis axis, double residual) {
        auto level = ll::service::getLevel();
        if (!level) return 0;
        auto rot   = encodeDisplay(int(axis), residual);
        auto owner = level->getActorFactory().createSpawnedActor(
            ActorDefinitionIdentifier("minecraft:falling_block"),
            nullptr,
            mcVec(at),
            Vec2{rot.pitch, rot.yaw}
        );
        if (!owner) return 0;
        auto* actor = Actor::tryGetFromEntity(*owner, false);
        if (!actor) return 0;
        static_cast<FallingBlockActor*>(actor)->setFallingBlock(block, false);
        // no gravity: a vanilla client simulates a falling block itself and would drop a display
        // without a real block under it (a train carriage over its bogey display, a structure in
        // the air) down to the ground; the server and the client mod don't tick displays at all
        auto noGravity = [](::Actor& a) {
            auto& data  = a.mEntityData.get();
            auto* flags = data.mFlagData.get();
            flags->mValue.set(static_cast<std::size_t>(::ActorFlags::HasGravity), false);
            flags->mValue.set(static_cast<std::size_t>(::ActorFlags::Noai), true);
            data.mDirtyFlags.get()->mDirtyFlags.get().set(0); // the flags data item
        };
        noGravity(*actor);
        auto* added = level->addEntity(region, std::move(owner));
        if (!added) return 0;
        noGravity(*added); // adding the entity resets its flags from the entity definition
        added->addTag(kDisplayTag); // tags need the entity in the level
        setDisplayRotation(*added, rot);
        // a spawned falling block stands half a block higher than one teleported to the same
        // place: put it where the teleports keep it, before clients first see it
        added->teleportTo(mcVec(at), true, 0, 0, false);
        auto id = added->getOrCreateUniqueID().rawID;
        displays.insert(id);
        shown[id] = {at, tickCount};
        return id;
    }

    void removeDisplay(CarriedBlock& b) {
        if (auto* a = fetchActor(b.display)) a->remove();
        displays.erase(b.display);
        shown.erase(b.display);
        b.display = 0;
    }

    /// Displayed block: the carried block rotated by the nearest quarter turn.
    Block const* displayBlock(Contraption const& c, CarriedBlock const& b, int quarter) {
        auto const* block = detail::blockFromSnbt(b.block);
        if (!block || quarter % 4 == 0) return block;
        return rotatedBlock(*block, b.states, c.mTransform.axis, quarter);
    }

    static Block const* rotatedBlock(Block const& block, cc::States states, cc::Axis axis, int quarter) {
        if (cc::normQuarter(quarter) == 0) return &block;
        cc::rotateStates(states, axis, quarter, block.getTypeName());
        if (auto const* h = detail::rotationHandler(block.getTypeName())) (*h)(states, axis, quarter);
        return detail::withStates(block, states);
    }

    /// Vanilla clients see a display only where it was spawned. While one is in the dimension, a
    /// display that moved away from there is spawned again at its place: half a block at a
    /// time, and once more where it comes to rest. Clients with the mod move the displays
    /// themselves (client/contraption/ContraptionClient.cpp).
    static constexpr double kStepDistance = 0.5;
    static constexpr int    kStepTicks    = 4;  ///< at most every 4 ticks per block
    static constexpr int    kSettleTicks  = 10; ///< smaller offsets are caught up after this
    bool needsRespawn(Contraption const& c, CarriedBlock const& b, cc::Vec3d const& feet) const {
        if (!vanillaDims.contains(c.mDim)) return false;
        auto it = shown.find(b.display);
        if (it == shown.end()) return false;
        double        drift = (feet - it->second.feet).length();
        std::uint64_t age   = tickCount - it->second.tick;
        return (drift >= kStepDistance && age >= kStepTicks) || (drift > 1e-3 && age >= kSettleTicks);
    }

    void syncDisplays(Contraption& c, BlockSource& region) {
        // The displays show the block turned by mDisplayQuarter; the rest of the angle is their
        // own turn. Vanilla clients draw blocks axis-aligned, so while one watches, every display
        // is spawned again at each new nearest quarter turn. Clients with the mod turn the blocks
        // by any angle: without vanilla viewers the displays stay (no churn every 90 degrees).
        int  quarter = cc::quarterTurnsOf(c.mTransform.angle);
        bool requart = quarter != c.mDisplayQuarter && vanillaDims.contains(c.mDim);
        int  shownQuarter = c.mDisplayQuarter;
        if (requart) c.mDisplayQuarter = quarter;
        quarter         = c.mDisplayQuarter;
        double residual = std::remainder(c.mTransform.angle - quarter * 90.0, 360.0);
        for (auto& b : c.mBlocks) {
            cc::Vec3d center = c.mTransform.toWorld(cc::Vec3d::center(b.local));
            cc::Vec3d feet   = center - cc::Vec3d{0, 0.5, 0};
            auto*     actor  = fetchActor(b.display);
            if (actor && (requart || needsRespawn(c, b, feet))) {
                retireDisplay(c, b, shownQuarter);
                actor = nullptr;
            }
            if (!actor) {
                if (b.display) {
                    displays.erase(b.display);
                    shown.erase(b.display);
                }
                b.display = 0;
                if (!region.hasChunksAt(game::blockPosOf(mcVec(center)), 0, false)) continue;
                if (auto const* block = displayBlock(c, b, quarter)) {
                    b.display = spawnDisplay(region, *block, feet, c.mTransform.axis, residual);
                    if (b.display) displaysChanged.insert(c.mId);
                }
                continue;
            }
            auto rot     = encodeDisplay(int(c.mTransform.axis), residual);
            auto current = actor->getRotation();
            bool turn    = std::abs(current.x - rot.pitch) > 1e-3f || std::abs(current.y - rot.yaw) > 1e-3f;
            if (turn) setDisplayRotation(*actor, rot);
            // the teleport carries the rotation to the clients
            if (turn || (ccVec(actor->getFeetPos()) - feet).lengthSqr() > 1e-8) actor->teleportTo(mcVec(feet), true, 0, 0, false);
        }
    }

    // -- world bounds --
    AABB worldBounds(Contraption const& c, double grow) const {
        auto const& b = c.mBounds;
        cc::Vec3d   lo{1e18, 1e18, 1e18}, hi{-1e18, -1e18, -1e18};
        for (int i = 0; i < 8; ++i) {
            cc::Vec3d corner{i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z};
            auto      w = c.mTransform.toWorld(corner);
            lo          = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
            hi          = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
        }
        return AABB{mcVec(lo - cc::Vec3d{grow, grow, grow}), mcVec(hi + cc::Vec3d{grow, grow, grow})};
    }

    std::vector<Actor*> nearbyEntities(Contraption const& c, BlockSource& region) {
        std::vector<Actor*> out;
        auto                bounds     = worldBounds(c, 2.0);
        auto                candidates = region.getEntities(bounds, 0.0f);
        // players aren't in the entity query: add the ones in range
        if (auto level = ll::service::getLevel()) {
            level->forEachPlayer([&](Player& p) {
                if (static_cast<int>(p.getDimensionId()) == c.mDim && bounds.contains(p.getFeetPos())) candidates.push_back(&p);
                return true;
            });
        }
        std::unordered_set<::Actor*> seen;
        // seated entities and the seat entities they ride move with their seat, not as passengers
        for (auto const& s : c.mSeated) {
            if (auto* rider = fetchActor(s.second)) {
                seen.insert(rider);
                if (auto* v = rider->getVehicle()) seen.insert(v);
            }
        }
        for (auto* a : candidates) {
            if (!seen.insert(a).second) continue;
            if (!a || !a->isAlive()) continue;
            if (a->getEntityTypeId() == ActorType::FallingBlock && detail::isDisplayActor(*a)) continue;
            out.push_back(a);
        }
        return out;
    }

    static void moveActorBy(Actor& a, cc::Vec3d const& delta) {
        if (delta.lengthSqr() < 1e-10) return;
        if (a.isPlayer()) {
            // players are teleported by their feet (their position is at eye height)
            static_cast<Player&>(a).teleport(a.getFeetPos() + mcVec(delta), a.getDimensionId());
        } else {
            a.teleportTo(a.getPosition() + mcVec(delta), false, 0, 0, true);
        }
    }

    // -- actors --
    template <class F>
    void forEachActor(Contraption& c, cc::Transform const& t, cc::Transform const& before, F&& fn) {
        for (auto& b : c.mBlocks) {
            auto const* actor = detail::actorFor(b.id);
            if (!actor) continue;
            cc::Vec3d center = t.toWorld(cc::Vec3d::center(b.local));
            cc::Vec3d prev   = before.toWorld(cc::Vec3d::center(b.local));
            auto      wp     = game::blockPosOf(mcVec(center));
            MovementContext ctx{c, b.local, {c.mDim, wp.x, wp.y, wp.z}, center, center - prev, b.states, b.actorData};
            if (actor->isActive && !actor->isActive(ctx)) continue;
            fn(*actor, ctx, b);
        }
    }

    // -- tick --
    void tick() {
        ++tickCount;
        if (!retiring.empty())
            for (Id id : removeRetiring([&](Retiring const& r) { return r.removeAt <= tickCount; }))
                if (live.contains(id)) displaysChanged.insert(id);
        if (live.empty()) {
            collisionCache.clear();
            return;
        }
        vanillaDims.clear();
        if (auto level = ll::service::getLevel()) {
            level->forEachPlayer([&](Player& p) {
                if (!p.isSimulatedPlayer() && !net::isCompanion(p)) vanillaDims.insert(static_cast<int>(p.getDimensionId()));
                return true;
            });
        }
        if (forceVanilla)
            for (auto const& [id, c] : live) vanillaDims.insert(c->mDim);
        collisionCache.clear();
        std::vector<Id> ids;
        for (auto const& [id, c] : live) ids.push_back(id);
        for (Id id : ids) {
            auto it = live.find(id);
            if (it == live.end()) continue;
            try {
                tickOne(*it->second);
            } catch (std::exception const& e) {
                log().error("contraption #{} tick failed: {}", id, e.what());
            }
        }
        rebuildCollisionCache();
        syncClients();
    }

    // -- block collision for entity movement (Create: contraption colliders) --
    struct CollisionEntry {
        int                  dim = 0;
        AABB                 bounds;
        std::vector<AABB>    boxes; ///< world boxes (the bounding box of each turned block box)
        std::vector<Block const*> blocks; ///< the carried block of each box
    };
    std::vector<CollisionEntry> collisionCache;

    void rebuildCollisionCache() {
        collisionCache.clear();
        for (auto const& [id, c] : live) {
            if (c->mBoxes.empty()) continue;
            CollisionEntry e;
            e.dim    = c->mDim;
            e.bounds = worldBounds(*c, 0.0);
            e.boxes.reserve(c->mBoxes.size());
            for (std::size_t i = 0; i < c->mBoxes.size(); ++i) {
                // Create collides with the oriented boxes: turned blocks give several thinner boxes
                for (auto const& w : cc::worldBoxes(c->mTransform, c->mBoxes[i])) {
                    e.boxes.push_back(AABB{mcVec(w.min), mcVec(w.max)});
                    e.blocks.push_back(i < c->mBoxBlocks.size() ? c->mBoxBlocks[i] : nullptr);
                }
            }
            collisionCache.push_back(std::move(e));
        }
    }

    void forEachCollision(int dim, AABB const& query, std::function<void(AABB const&, Block const*)> const& fn) const {
        for (auto const& e : collisionCache) {
            if (e.dim != dim || !e.bounds.intersects(query)) continue;
            for (std::size_t i = 0; i < e.boxes.size(); ++i)
                if (e.boxes[i].intersects(query)) fn(e.boxes[i], i < e.blocks.size() ? e.blocks[i] : nullptr);
        }
    }

    void tickOne(Contraption& c) {
        auto* region = detail::regionOf(c.mDim);
        if (!region) return;
        if (c.mKind == kStabilizedKind) {
            auto const& st     = c.mAnchorData.value("stabilized", json::object());
            auto        parent = live.find(st.value("parent", Id{0}));
            if (parent == live.end()) {
                // the parent is gone (placed or discarded): the sub-contraption is placed too
                Id id = c.mId;
                log().info("contraption #{}: its parent is gone, placed", id);
                disassemble(id);
                return;
            }
            c.mTarget = stabilizedTransform(c, *parent->second, posOf(st.value("local", json::array())), false);
        }
        // where the structure is now: frozen while that chunk is unloaded (Create's entities stop too)
        auto here = game::blockPosOf(mcVec(c.mTransform.toWorld(c.mBounds.center())));
        if (!region->hasChunksAt(here, 0, false)) return;

        cc::Transform before      = c.mTransform;
        cc::Transform next        = before;
        Motion const  motionAtTick = c.mMotion;
        if (c.mTarget) {
            next = *c.mTarget;
            c.mTarget.reset();
        } else {
            if (c.mMotion.follow) {
                if (auto* a = fetchActor(*c.mMotion.follow)) {
                    next.offset = ccVec(a->getPosition()) + c.mMotion.followOffset - next.origin;
                }
            }
            next.offset += c.mMotion.velocity;
            next.angle += c.mMotion.angularVelocity;
            if (c.mMotion.ticks > 0 && --c.mMotion.ticks == 0) {
                c.mMotion = {};
            }
        }
        bool moving = next.offset.x != before.offset.x || next.offset.y != before.offset.y || next.offset.z != before.offset.z
                   || next.angle != before.angle || next.axis != before.axis || next.tilt != before.tilt;


        // actors
        if (moving && !c.mMoving) {
            forEachActor(c, before, before, [](MovementBehaviour const& a, MovementContext& ctx, CarriedBlock&) {
                if (a.startMoving) a.startMoving(ctx);
            });
        }
        bool stalled = false;
        if (moving) {
            if (!c.mActorsPaused) forEachActor(c, next, before, [&](MovementBehaviour const& a, MovementContext& ctx, CarriedBlock& b) {
                if (a.tick) a.tick(ctx);
                cc::Pos wp{ctx.worldPos.x, ctx.worldPos.y, ctx.worldPos.z};
                if (!b.visited || *b.visited != wp) {
                    b.visited = wp;
                    if (a.visitNewPosition) a.visitNewPosition(ctx, ctx.worldPos);
                }
                stalled = stalled || ctx.stall;
            });
        } else if (c.mMoving) {
            forEachActor(c, before, before, [](MovementBehaviour const& a, MovementContext& ctx, CarriedBlock&) {
                if (a.stopMoving) a.stopMoving(ctx);
            });
        }
        if (!moving && c.mTicksWhileStill) {
            // Create ticks every actor each tick: the ones that act while still (doors)
            forEachActor(c, before, before, [](MovementBehaviour const& a, MovementContext& ctx, CarriedBlock&) {
                if (a.tickWhileStill && a.tick) a.tick(ctx);
            });
        }
        c.mMoving = moving; // a stall keeps the actors "moving" (no new startMoving)
        if (stalled) {
            // Create: a stalled contraption holds still; the anchor's motion waits too
            next = before;
            if (!c.mTarget) c.mMotion = motionAtTick;
        }

        auto nearby = nearbyEntities(c, *region);
        if (moving && !stalled) {
            // carry what stands on it, then move
            for (auto* a : nearby) {
                cc::Vec3d feet = ccVec(a->getFeetPos());
                if (!cc::standsOn(before, c.mBoxes, feet)) continue;
                moveActorBy(*a, cc::carry(before, next, feet) - feet);
            }
            c.mTransform = next;
        }
        // collision (also when still: the blocks are gone from the world, the structure is the floor)
        if (!c.mBoxes.empty()) {
            for (auto* a : nearby) {
                if (a->getEntityTypeId() == ActorType::ItemEntity) continue;
                auto const& bb = a->getAABB();
                cc::Box     box{ccVec(bb.min), ccVec(bb.max)};
                auto        r = cc::collide(c.mTransform, c.mBoxes, box);
                if (r.hit && r.push.lengthSqr() > 1e-8) moveActorBy(*a, r.push);
            }
        }
        // seated entities follow their seat
        for (auto it = c.mSeated.begin(); it != c.mSeated.end();) {
            auto* a = fetchActor(it->second);
            if (!a || (a->isPlayer() && a->isSneaking())) {
                if (a) log().info("contraption #{}: {} left a seat", c.mId, a->getTypeName());
                it = c.mSeated.erase(it);
                continue;
            }
            double h = 0.5;
            for (auto const& b : c.mBlocks)
                if (b.local == it->first) h = detail::seatHeight(b.id).value_or(0.5);
            cc::Vec3d seatFeet = c.mTransform.toWorld(cc::Vec3d::of(it->first) + cc::Vec3d{0.5, h, 0.5});
            // a rider sitting on a seat entity stays mounted: the vehicle moves and carries it
            // (teleporting the rider would make it dismount)
            if (auto* vehicle = a->getVehicle()) moveActorBy(*vehicle, seatFeet - ccVec(a->getFeetPos()));
            else moveActorBy(*a, seatFeet - ccVec(a->getFeetPos()));
            ++it;
        }
        syncDisplays(c, *region);
    }

    // -- stabilized sub-contraptions (Create's StabilizedContraption) --
    static constexpr char kStabilizedKind[]   = "stabilized";
    static constexpr char kMechanicalBearing[] = "levitate:mechanical_bearing";

    /// Create's canBeStabilized: bearing contraptions stabilize bearings on their own axis (not one
    /// facing back into the anchor), clockwork hands not next to the anchor, cart contraptions
    /// every bearing, the others none.
    static bool canBeStabilized(AssembleOptions const& o, cc::Pos const& anchor, cc::Pos const& p, cc::Dir facing) {
        if (o.kind == "cart_assembler") return true;
        bool bearing = o.kind == "mechanical_bearing" || o.kind == "windmill_bearing";
        if (!bearing && o.kind != "clockwork_bearing") return false;
        if (o.start.empty() || cc::axisOf(facing) != o.axis) return false;
        cc::Pos first = o.start.front();
        if (o.kind == "clockwork_bearing") return p != first && p + cc::offset(facing) != first;
        std::optional<cc::Dir> parentFacing;
        for (auto d : cc::kAllDirs)
            if (anchor + cc::offset(d) == first) parentFacing = d;
        return !(p == first && parentFacing && facing == cc::opposite(*parentFacing));
    }

    /// The stabilized sub-contraptions of `parent`, with the bearing's local position in it.
    std::vector<std::pair<Id, cc::Pos>> childrenOf(Id parent) const {
        std::vector<std::pair<Id, cc::Pos>> out;
        for (auto const& [id, c] : live) {
            if (c->mKind != kStabilizedKind) continue;
            auto const& st = c->mAnchorData.value("stabilized", json::object());
            if (st.value("parent", Id{0}) == parent) out.emplace_back(id, posOf(st.value("local", json::array())));
        }
        return out;
    }

    /// The transform of a stabilized sub-contraption: its bearing block follows the parent, the
    /// structure keeps its assembly orientation (StabilizedContraption: no rotation of its own).
    static cc::Transform stabilizedTransform(Contraption const& child, Contraption const& parent, cc::Pos const& bearingLocal, bool grid) {
        cc::Transform t = child.mTransform;
        t.angle         = 0;
        t.tilt          = 0;
        cc::Vec3d at    = grid ? cc::Vec3d::of(parent.mTransform.placeBlock(bearingLocal))
                               : parent.mTransform.toWorld(cc::Vec3d::center(bearingLocal)) - cc::Vec3d{0.5, 0.5, 0.5};
        t.offset        = at - t.origin;
        return t;
    }

    // -- assembly --
    AssembleResult assemble(AssembleOptions const& o) {
        AssembleResult result;
        auto const&    cfg    = config::get<Config>();
        auto*          region = detail::regionOf(o.anchor.dim);
        auto           fail   = [&](std::string key, std::string fallback, std::vector<std::string> args) {
            result.errorKey  = std::move(key);
            result.errorArgs = args;
            std::string text(translate(result.errorKey, fallback));
            result.errorText = core::formatMessage(text, args);
            log().info("contraption assembly failed at {} {} {}: {}", o.anchor.x, o.anchor.y, o.anchor.z, result.errorText);
            return result;
        };
        if (!region) return fail("create.gui.assembly.exception", "This Contraption was unable to assemble:", {});

        cc::Pos anchor{o.anchor.x, o.anchor.y, o.anchor.z};
        int     dim = o.anchor.dim;
        std::vector<cc::Pos> start = o.start;
        if (start.empty()) start.push_back(anchor);
        bool includeAnchor = o.includeAnchor || o.start.empty();

        // Create's moveBearing: a mechanical bearing carried by a bearing (same axis) or a cart
        // contraption becomes a stabilized sub-contraption instead of taking its front along
        std::map<cc::Pos, cc::Dir> subBearings;
        auto query = [&](cc::Pos const& p) {
            auto info = detail::describe(*region, dim, p);
            if (p == anchor && !includeAnchor) info.anchor = true;
            if (info.id == kMechanicalBearing && !info.anchor) {
                auto states = detail::statesOf(region->getBlock(detail::toBlockPos(p)));
                auto it     = states.find("levitate:facing");
                auto const* v = it == states.end() ? nullptr : std::get_if<std::int64_t>(&it->second);
                auto facing = static_cast<cc::Dir>(std::clamp<std::int64_t>(v ? *v : 1, 0, 5));
                if (canBeStabilized(o, anchor, p, facing)) {
                    info.pulls &= static_cast<cc::DirMask>(~cc::bit(facing));
                    subBearings[p] = facing;
                }
            }
            return info;
        };
        auto glue = [&](cc::Pos const& p, cc::Dir d) { return detail::glued(dim, p, d); };
        cc::AssemblyLimits limits;
        limits.maxBlocks = cfg.maxBlocksMoved;
        limits.forced    = o.forced;
        limits.minY      = region->getMinHeight();
        limits.maxY      = region->getMaxHeight();
        auto r           = cc::assemble(start, query, glue, limits);
        // Create's moveBlock hooks that change the world first (a running windmill places its
        // sails): collect again once they did
        for (int round = 0; round < 4 && r.ok() && detail::hasBeforeCapture(); ++round) {
            bool changed = false;
            for (auto const& p : r.blocks) {
                auto const* hook = detail::beforeCapture(region->getBlock(detail::toBlockPos(p)).getTypeName());
                if (hook && (*hook)(*region, {dim, p.x, p.y, p.z})) changed = true;
            }
            if (!changed) break;
            r = cc::assemble(start, query, glue, limits);
        }
        switch (r.error) {
        case cc::AssemblyError::Unmovable:
            return fail(
                "create.gui.assembly.exception.unmovableBlock",
                "Unmovable Block ({3}) at [{0},{1},{2}]",
                {std::to_string(r.errorPos.x), std::to_string(r.errorPos.y), std::to_string(r.errorPos.z), r.errorBlock}
            );
        case cc::AssemblyError::UnloadedChunk:
            return fail(
                "create.gui.assembly.exception.chunkNotLoaded",
                "The Block at [{0},{1},{2}] was not in a loaded chunk",
                {std::to_string(r.errorPos.x), std::to_string(r.errorPos.y), std::to_string(r.errorPos.z)}
            );
        case cc::AssemblyError::TooLarge:
            return fail(
                "create.gui.assembly.exception.structureTooLarge",
                "There are too many Blocks included in the contraption.\nThe configured maximum is: {0}",
                {std::to_string(cfg.maxBlocksMoved)}
            );
        default:
            break;
        }
        if (r.blocks.empty()) {
            return fail("create.gui.assembly.exception", "This Contraption was unable to assemble:", {});
        }

        auto c       = std::make_unique<Contraption>();
        c->mId       = nextId++;
        c->mDim      = dim;
        c->mKind     = o.kind;
        c->mAnchor   = o.anchor;
        c->mTransform.origin = cc::Vec3d::of(anchor);
        c->mTransform.axis   = o.axis;
        c->mTransform.pivot  = o.pivot;

        auto saveCtx = SaveContextFactory::createCloneSaveContext();
        std::unordered_set<cc::Pos, cc::PosHash> positions(r.blocks.begin(), r.blocks.end());
        for (auto const& p : r.blocks) {
            auto        bp    = detail::toBlockPos(p);
            auto const& block = region->getBlock(bp);
            CarriedBlock b;
            b.local     = p - anchor;
            b.id        = block.getTypeName();
            b.brittle   = detail::describe(*region, dim, p).brittle;
            b.block     = detail::snbtOf(block);
            b.states    = detail::statesOf(block);
            b.collision = detail::collisionOf(*region, p);
            if (auto* be = region->getBlockEntity(bp)) {
                CompoundTag tag;
                if (be->save(tag, *saveCtx)) {
                    if (tag.contains("pairx") && tag.contains("pairz")) {
                        cc::Pos pair{tag["pairx"].get<IntTag>().data, p.y, tag["pairz"].get<IntTag>().data};
                        b.data["lv:pair"] = pos(pair - anchor);
                    }
                    b.blockEntity = tag.toSnbt(SnbtFormat::Minimize, 0);
                }
            }
            core::GlobalPos gp{dim, p.x, p.y, p.z};
            for (auto const& carrier : detail::carriers()) {
                if (!carrier.capture) continue;
                if (auto d = carrier.capture(*region, gp)) b.data[carrier.name] = std::move(*d);
            }
            c->mBlocks.push_back(std::move(b));
        }
        for (auto const& g : detail::takeGlue(dim, positions)) c->mGlue.push_back(g.moved(cc::Pos{} - anchor));

        // seat entities sitting in seat blocks
        for (auto const& b : c->mBlocks) {
            auto id = b.id;
            if (!detail::seatHeight(id)) continue;
            cc::Pos wp = b.local + anchor;
            for (auto* a : region->getEntities(AABB{mcVec(cc::Vec3d::of(wp)), mcVec(cc::Vec3d::of(wp) + cc::Vec3d{1, 1, 1})}, 0.0f)) {
                if (!a || a->getEntityTypeId() == ActorType::FallingBlock) continue;
                if (a->hasPassenger()) continue; // the seat entity: its rider is the seated one

                if (game::blockPosOf(a->getFeetPos()) != detail::toBlockPos(wp)) continue;
                c->mSeated.emplace_back(b.local, a->getOrCreateUniqueID().rawID);
                break;
            }
        }

        // lift the blocks: containers emptied first (their items travel in the NBT), brittle
        // blocks before the blocks they hang on, no neighbour updates until everything is gone
        auto const* air = Block::tryGetFromRegistry(HashedString("minecraft:air")).as_ptr();
        std::vector<std::size_t> order(c->mBlocks.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return c->mBlocks[a].brittle && !c->mBlocks[b].brittle;
        });
        for (auto i : order) {
            auto bp = detail::toBlockPos(r.blocks[i]);
            if (auto container = region->tryGetContainer(bp)) container->removeAllItems();
            BlockChangeContext ctx{};
            region->setBlock(bp, *air, kNoUpdates, nullptr, ctx);
        }
        for (auto const& p : r.blocks) region->updateNeighborsAt(detail::toBlockPos(p));

        finish(*c);
        auto& ref = *c;
        live[ref.mId] = std::move(c);
        syncDisplays(ref, *region);
        Id parentId = ref.mId;
        for (auto const& [sub, facing] : subBearings) {
            if (!positions.contains(sub)) continue;
            AssembleOptions so;
            so.anchor = {dim, sub.x, sub.y, sub.z};
            so.start  = {sub + cc::offset(facing)};
            so.axis   = cc::axisOf(facing);
            so.kind   = kStabilizedKind;
            if (!region->hasChunksAt(detail::toBlockPos(so.start[0]), 0, false)) continue;
            if (!detail::describe(*region, dim, so.start[0]).necessary) continue;
            auto child = assemble(so);
            if (!child.ok()) continue;
            auto* sc = find(*child.id);
            auto* parent = find(parentId);
            if (!sc || !parent) continue;
            sc->mAnchorData["stabilized"] = {{"parent", parentId}, {"local", pos(sub - anchor)}};
            save(*sc);
            log().info("contraption #{}: stabilized sub-contraption #{} on the bearing at {} {} {}", parentId, *child.id, sub.x, sub.y, sub.z);
        }
        auto* parentNow = find(parentId);
        if (!parentNow) return result;
        auto& ref2 = *parentNow;
        save(ref2);
        game::db::save(kDbNextId, json{{"next", nextId}});
        log().info("contraption #{} assembled: {} blocks ({}) at {} {} {}", ref2.mId, ref2.mBlocks.size(), ref2.mKind, anchor.x, anchor.y, anchor.z);
        result.id = ref2.mId;
        return result;
    }

    // -- disassembly --
    bool disassemble(Id id) {
        auto it = live.find(id);
        if (it == live.end()) return false;
        auto& c      = *it->second;
        auto* region = detail::regionOf(c.mDim);
        if (!region) return false;
        if (c.mMoving) {
            forEachActor(c, c.mTransform, c.mTransform, [](MovementBehaviour const& a, MovementContext& ctx, CarriedBlock&) {
                if (a.stopMoving) a.stopMoving(ctx);
            });
        }
        if (c.mStorage) static_cast<MergedStorage&>(*c.mStorage).writeBack(c.mBlocks);
        int  quarter = c.mTransform.placeQuarterTurns();
        auto axis    = c.mTransform.axis;

        struct Placed {
            CarriedBlock* b;
            cc::Pos       target;
            Block const*  block;
            bool          brittle;
        };
        std::vector<Placed> plan;
        for (auto& b : c.mBlocks) {
            auto const* original = detail::blockFromSnbt(b.block);
            if (!original) continue;
            auto const* block = rotatedBlock(*original, b.states, axis, quarter);
            plan.push_back({&b, c.mTransform.placeBlock(b.local), block, b.brittle});
        }
        std::stable_sort(plan.begin(), plan.end(), [](Placed const& a, Placed const& b) {
            if (a.brittle != b.brittle) return !a.brittle;
            return a.target.y < b.target.y;
        });

        // remove the display entities first so they never show next to the placed blocks
        for (auto& b : c.mBlocks) removeDisplay(b);
        removeRetiring([id = c.mId](Retiring const& r) { return r.contraption == id; });

        int placed = 0, dropped = 0;
        std::vector<cc::Pos> placedAt;
        bool const shouldDrop = !config::get<Config>().noDropWhenContraptionReplaceBlocks;
        auto       level      = ll::service::getLevel();
        for (auto& p : plan) {
            auto bp = detail::toBlockPos(p.target);
            bool ok = p.block && region->hasChunksAt(bp, 0, false) && p.target.y >= region->getMinHeight()
                   && p.target.y < region->getMaxHeight();
            if (ok) {
                // Create's addBlocksToWorld: the carried block gives way only to an unbreakable
                // block, or (having no collision shape itself) to a block with one; otherwise the
                // world block is destroyed (dropping, unless noDropWhenContraptionReplaceBlocks)
                auto const& existing = region->getBlock(bp);
                if (!existing.isAir()) {
                    bool unbreakable = existing.getBlockType().getDestroySpeed() < 0;
                    bool shapeless   = !p.b->collision && detail::collisionOf(*region, p.target).has_value();
                    ok               = !unbreakable && !shapeless;
                    std::string const existingId = existing.getTypeName();
                    bool liquid = existingId == "minecraft:water" || existingId == "minecraft:flowing_water" || existingId == "minecraft:lava"
                               || existingId == "minecraft:flowing_lava";
                    if (ok && !liquid && level) {
                        log().info("contraption #{}: {} at {} {} {} replaced by {}", c.mId, existingId, p.target.x, p.target.y, p.target.z, p.b->id);
                        if (auto container = region->tryGetContainer(bp); container && shouldDrop) {
                            // (vanilla's destroyBlock drops the block, the container keeps its items)
                            for (int i = 0; i < container->getContainerSize(); ++i) {
                                auto const& item = container->getItem(i);
                                if (!item.isNull()) level->getSpawner().spawnItem(*region, item, nullptr, mcVec(cc::Vec3d::center(p.target)), 10);
                            }
                            container->removeAllItems();
                        }
                        BlockChangeContext destroyCtx{};
                        level->destroyBlock(*region, bp, shouldDrop, destroyCtx);
                    }
                }
            }
            if (!ok) {
                log().info(
                    "contraption #{}: {} can't be placed at {} {} {} ({}), dropped",
                    c.mId,
                    p.b->id,
                    p.target.x,
                    p.target.y,
                    p.target.z,
                    region->hasChunksAt(bp, 0, false) ? region->getBlock(bp).getTypeName() : std::string("unloaded")
                );
                // Create: the contraption's block drops as an item, the world block stays
                if (p.block && shouldDrop) {
                    detail::dropItemAt(*region, cc::Vec3d::center(p.target), *p.block, p.b->blockEntity);
                    dropContents(*region, p.target, p.b->blockEntity);
                }
                ++dropped;
                continue;
            }
            // Create's addBlocksToWorld: ropes, magnets and doors turned about a horizontal axis
            // break where they land (a door drops once, from its lower half)
            bool door = (p.b->id.ends_with("_door") && p.b->id.find("trapdoor") == std::string::npos) || p.b->id == "minecraft:wooden_door";
            if (cc::normQuarter(quarter) != 0 && axis != cc::Axis::Y
                && (door || p.b->id == "levitate:rope" || p.b->id == "levitate:pulley_magnet")) {
                auto upper = p.b->states.find("upper_block_bit");
                bool lower = upper == p.b->states.end() || std::get_if<std::int64_t>(&upper->second) == nullptr
                          || *std::get_if<std::int64_t>(&upper->second) == 0;
                if (shouldDrop && (!door || lower)) detail::dropItemAt(*region, cc::Vec3d::center(p.target), *p.block, p.b->blockEntity);
                ++dropped;
                continue;
            }
            std::shared_ptr<BlockActor> be;
            if (auto tag = parseSnbt(p.b->blockEntity)) {
                (*tag)["x"] = p.target.x;
                (*tag)["y"] = p.target.y;
                (*tag)["z"] = p.target.z;
                if (p.b->data.contains("lv:pair")) {
                    auto pair  = c.mTransform.placeBlock(posOf(p.b->data["lv:pair"]));
                    (*tag)["pairx"] = pair.x;
                    (*tag)["pairz"] = pair.z;
                }
                be = BlockActor::create(*tag, bp);
            }
            BlockChangeContext ctx{};
            if (be) region->setBlock(bp, *p.block, kNoUpdates, be, nullptr, ctx);
            else region->setBlock(bp, *p.block, kNoUpdates, nullptr, ctx);
            if (!p.b->blockEntity.empty()) {
                restoreContainer(*region, bp, p.b->blockEntity);
                if (auto* placedBe = region->getBlockEntity(bp)) placedBe->refresh(*region);
            }
            core::GlobalPos gp{c.mDim, p.target.x, p.target.y, p.target.z};
            for (auto const& carrier : detail::carriers()) {
                if (!carrier.restore || !p.b->data.contains(carrier.name)) continue;
                json data = p.b->data[carrier.name];
                if (carrier.rotate && quarter) carrier.rotate(data, axis, quarter);
                carrier.restore(*region, gp, data);
            }
            if (p.b->id == kSticker) detail::trackSticker(c.mDim, p.target); // keeps reacting to redstone
            placedAt.push_back(p.target);
            ++placed;
        }
        for (auto const& p : placedAt) region->updateNeighborsAt(detail::toBlockPos(p));
        for (auto const& g : c.mGlue) {
            addGlue(c.mDim, cc::transformBox(g, [&](cc::Pos const& p) { return c.mTransform.placeBlock(p); }));
        }
        log().info("contraption #{} disassembled: {} placed, {} dropped", c.mId, placed, dropped);
        Id parentId = c.mId;
        auto children = childrenOf(parentId);
        for (auto const& [childId, local] : children) {
            auto ci = live.find(childId);
            if (ci == live.end()) continue;
            ci->second->mTransform = stabilizedTransform(*ci->second, c, local, true);
        }
        forget(it);
        for (auto const& [childId, local] : children) disassemble(childId);
        return true;
    }

    /// Create's contraption entity setBlock: new states for a carried block (shape and display follow).
    bool setBlockStates(Id id, cc::Pos const& local, cc::States const& states) {
        auto it = live.find(id);
        if (it == live.end()) return false;
        auto& c = *it->second;
        for (auto& b : c.mBlocks) {
            if (b.local != local) continue;
            auto const* original = detail::blockFromSnbt(b.block);
            if (!original) return false;
            auto const* next = detail::withStates(*original, states);
            if (!next || next == original) return false;
            b.block  = detail::snbtOf(*next);
            b.states = detail::statesOf(*next);
            b.id     = next->getTypeName();
            if (auto* region = detail::regionOf(c.mDim)) b.collision = detail::collisionOfBlock(*region, *next, toCc(c.mAnchor));
            c.mBoxes.clear();
            for (auto const& o : c.mBlocks)
                if (o.collision) c.mBoxes.push_back(o.collision->moved(cc::Vec3d::of(o.local)));
            refreshBoxBlocks(c);
            if (b.display) {
                retireDisplay(c, b, c.mDisplayQuarter); // spawned again with the new block this tick
                displaysChanged.insert(c.mId);
            }
            return true;
        }
        return false;
    }
    static cc::Pos toCc(core::GlobalPos const& p) { return {p.x, p.y, p.z}; }

    /// The block of each collision box (entity movement collides with them) and whether an actor
    /// ticks while the contraption stands still: cached when the blocks change.
    static void refreshBoxBlocks(Contraption& c) {
        c.mBoxBlocks.clear();
        c.mTicksWhileStill = false;
        for (auto const& b : c.mBlocks) {
            if (b.collision) c.mBoxBlocks.push_back(detail::blockFromSnbt(b.block));
            if (auto const* a = detail::actorFor(b.id); a && a->tickWhileStill) c.mTicksWhileStill = true;
        }
    }

    /// Create's AbstractContraptionEntity#discard: the structure goes, nothing is placed or dropped.
    bool discard(Id id) {
        auto it = live.find(id);
        if (it == live.end()) return false;
        auto& c = *it->second;
        if (c.mMoving) {
            forEachActor(c, c.mTransform, c.mTransform, [](MovementBehaviour const& a, MovementContext& ctx, CarriedBlock&) {
                if (a.stopMoving) a.stopMoving(ctx);
            });
        }
        for (auto& b : c.mBlocks) removeDisplay(b);
        removeRetiring([id](Retiring const& r) { return r.contraption == id; });
        log().info("contraption #{} discarded: {} blocks", c.mId, c.mBlocks.size());
        auto children = childrenOf(id);
        forget(it);
        // its stabilized sub-contraptions are placed where they are
        for (auto const& [childId, local] : children) disassemble(childId);
        return true;
    }

    /// Drops a contraption whose displays are gone: its record, the companions' copy, the caches.
    static cc::Box boundsOf(Contraption const& c) { return c.mBounds; }

    void forget(std::map<Id, std::unique_ptr<Contraption>>::iterator it) {

        auto& c = *it->second;
        game::db::erase(kDbPrefix + std::to_string(c.mId));
        sendToDim(c.mDim, "contraption.remove", {{"id", c.mId}});
        sentTransforms.erase(c.mId);
        std::erase_if(visuals, [id = c.mId](auto const& e) { return std::get<0>(e.first) == id; });
        displaysChanged.erase(c.mId);
        live.erase(it);
        rebuildCollisionCache(); // its blocks stop colliding at once
    }

    /// Puts the carried items into the placed container when the block entity didn't take them.
    static void restoreContainer(BlockSource& region, BlockPos const& bp, std::string const& snbt) {
        auto be = parseSnbt(snbt);
        if (!be || !be->contains("Items") || !(*be)["Items"].is_array()) return;
        auto container = region.tryGetContainer(bp);
        if (!container) return;
        for (int i = 0; i < container->getContainerSize(); ++i)
            if (!container->getItem(i).isNull()) return; // already loaded from the NBT
        for (auto const& tag : (*be)["Items"].get<ListTag>()) {
            if (!tag || !tag.hold(Tag::Type::Compound)) continue;
            auto const& item = static_cast<CompoundTag const&>(*tag);
            int         slot = item.contains("Slot") ? int(item["Slot"].get<ByteTag>().data) : -1;
            if (slot < 0 || slot >= container->getContainerSize()) continue;
            container->setItem(slot, ItemStack::fromTag(item));
        }
    }

    void dropContents(BlockSource& region, cc::Pos const& at, std::string const& snbt) {
        auto be = parseSnbt(snbt);
        if (!be || !be->contains("Items") || !(*be)["Items"].is_array()) return;
        auto level = ll::service::getLevel();
        if (!level) return;
        for (auto const& tag : (*be)["Items"].get<ListTag>()) {
            if (!tag || !tag.hold(Tag::Type::Compound)) continue;
            auto item = ItemStack::fromTag(static_cast<CompoundTag const&>(*tag));
            if (!item.isNull()) level->getSpawner().spawnItem(region, item, nullptr, mcVec(cc::Vec3d::center(at)), 10);
        }
    }

private:
    ll::event::ListenerPtr mTick;
};

// ---- public API -------------------------------------------------------------------------------

Storage& Contraption::storage() { return *mStorage; }

core::GlobalPos Contraption::placement(cc::Pos const& local) const {
    auto p = mTransform.placeBlock(local);
    return {mDim, p.x, p.y, p.z};
}

bool Contraption::seat(cc::Pos const& local, std::int64_t actorUniqueId) {
    bool isSeat = false;
    for (auto const& b : mBlocks)
        if (b.local == local && detail::seatHeight(b.id)) isSeat = true;
    if (!isSeat) return false;
    for (auto const& s : mSeated)
        if (s.first == local) return false;
    unseat(actorUniqueId);
    mSeated.emplace_back(local, actorUniqueId);
    return true;
}

void Contraption::unseat(std::int64_t actorUniqueId) {
    std::erase_if(mSeated, [&](auto const& s) { return s.second == actorUniqueId; });
}

AssembleResult assemble(AssembleOptions const& options) { return Engine::get().assemble(options); }

Contraption* find(Id id) {
    auto& live = Engine::get().live;
    auto  it   = live.find(id);
    return it == live.end() ? nullptr : it->second.get();
}

bool disassemble(Id id) { return Engine::get().disassemble(id); }

bool discard(Id id) { return Engine::get().discard(id); }

bool setBlockStates(Id id, cc::Pos const& local, cc::States const& states) { return Engine::get().setBlockStates(id, local, states); }

std::optional<cc::Box> bounds(Id id) {
    auto& live = Engine::get().live;
    auto  it   = live.find(id);
    if (it == live.end() || it->second->blocks().empty()) return std::nullopt;
    return Engine::boundsOf(*it->second);
}



ItemStorageAdapter slotStorage(
    std::function<std::vector<ItemStack>(nlohmann::json const& carried)>             read,
    std::function<void(nlohmann::json& carried, std::vector<ItemStack> const& slots)> write
) {
    ItemStorageAdapter a;
    a.slots  = read;
    a.insert = [read, write](nlohmann::json& carried, ItemStack const& stack, bool simulate) {
        if (stack.isNull() || stack.mCount == 0) return stack;
        auto      slots = read(carried);
        ItemStack rest  = stack;
        for (int pass = 0; pass < 2 && rest.mCount > 0; ++pass) {
            for (auto& s : slots) {
                if (rest.mCount == 0) break;
                bool empty = s.isNull() || s.mCount == 0;
                if (pass == 0 ? (empty || !s.isStackable(rest)) : !empty) continue;
                int max   = std::max<int>(1, rest.getMaxStackSize());
                int have  = empty ? 0 : s.mCount;
                int moved = std::min<int>(max - have, rest.mCount);
                if (moved <= 0) continue;
                if (empty) {
                    s = rest;
                    s.set(moved);
                } else {
                    s.set(have + moved);
                }
                rest.set(rest.mCount - moved);
            }
        }
        if (!simulate && rest.mCount != stack.mCount) write(carried, slots);
        return rest.mCount == 0 ? ItemStack{} : rest;
    };
    a.extract = [read, write](nlohmann::json& carried, std::function<bool(ItemStack const&)> const& filter, int count, bool simulate) {
        auto      slots = read(carried);
        ItemStack out;
        int       taken = 0;
        for (auto& s : slots) {
            if (taken >= count) break;
            if (s.isNull() || s.mCount == 0) continue;
            if (filter && !filter(s)) continue;
            if (!out.isNull() && !out.isStackable(s)) continue;
            int n = std::min<int>(count - taken, s.mCount);
            if (out.isNull()) out = s;
            taken += n;
            int left = s.mCount - n;
            if (left <= 0) s = ItemStack{};
            else s.set(left);
        }
        if (out.isNull()) return out;
        if (!simulate) write(carried, slots);
        out.set(taken);
        return out;
    };
    return a;
}

void setActorVisual(Id id, cc::Pos const& local, nlohmann::json visual) {
    auto& e  = Engine::get();
    auto  it = e.live.find(id);
    if (it == e.live.end()) return;
    auto key = std::tuple<Id, int, int, int>{id, local.x, local.y, local.z};
    if (visual.is_null()) e.visuals.erase(key);
    else e.visuals[key] = visual;
    Engine::sendToDim(
        it->second->dimension(),
        "contraption.visual",
        {{"id", id}, {"local", json::array({local.x, local.y, local.z})}, {"visual", std::move(visual)}}
    );
}

std::vector<Id> all() {
    std::vector<Id> out;
    for (auto const& [id, c] : Engine::get().live) out.push_back(id);
    return out;
}

namespace detail {
void saveAll() { Engine::get().saveAll(); }
void reloadAll() {
    Engine::get().saveAll();
    Engine::get().loadAll();
}
bool ownsDisplay(std::int64_t uniqueId) { return Engine::get().displays.contains(uniqueId); }
void appendCollision(int dim, AABB const& query, std::vector<AABB>& out) {
    Engine::get().forEachCollision(dim, query, [&](AABB const& box, Block const*) { out.push_back(box); });
}
void forEachCollision(int dim, AABB const& query, std::function<void(AABB const&, Block const*)> const& fn) {
    Engine::get().forEachCollision(dim, query, fn);
}

void forceVanillaViewers(bool on) { Engine::get().forceVanilla = on; }
std::vector<ItemStack*> blockSlots(Contraption& c, cc::Pos const& local) {
    auto const& blocks = c.blocks();
    for (int i = 0; i < static_cast<int>(blocks.size()); ++i)
        if (blocks[i].local == local) return static_cast<MergedStorage&>(c.storage()).slotsOf(i);
    return {};
}
bool isDisplayActor(::Actor const& actor) {
    return ownsDisplay(actor.getOrCreateUniqueID().rawID) || actor.hasTag(kDisplayTag);
}
} // namespace detail

// ---- display entity hook: our falling blocks never fall; orphans (from a saved world) vanish --

LL_TYPE_INSTANCE_HOOK(
    DisplayTickHook,
    ll::memory::HookPriority::Normal,
    FallingBlockActor,
    &FallingBlockActor::doNormalTick,
    void,
    ITickDelegate& delegate
) {
    if (isClientSide() ? displayAxis(getRotation().x) >= 0 : detail::isDisplayActor(*this)) {
        if (!isClientSide() && !detail::ownsDisplay(getOrCreateUniqueID().rawID)) remove();
        return;
    }
    origin(delegate);
}

// (the server-side tick of the actor itself: landing would place the block. Client-side actors
// keep their tick, which interpolates their position.)
LL_TYPE_INSTANCE_HOOK(
    DisplayActorTickHook,
    ll::memory::HookPriority::Normal,
    FallingBlockActor,
    &FallingBlockActor::$normalTick,
    void
) {
    if (!isClientSide() && detail::isDisplayActor(*this)) {
        if (!detail::ownsDisplay(getOrCreateUniqueID().rawID)) remove();
        return;
    }
    origin();
}

// (the falling block system ticks them through here; landing would place the block)
LL_TYPE_STATIC_HOOK(
    DisplaySystemHook,
    ll::memory::HookPriority::Normal,
    FallingBlockNormalTickSystem,
    &FallingBlockNormalTickSystem::_doFallingBlockNormalTickSystem,
    void,
    StrictEntityContext const& entity,
    ActorOwnerComponent&       owner,
    ITickDelegate&             onLand
) {
    std::unique_ptr<::Actor>& ownerPtr = owner.mActor;
    ::Actor*                  actor    = ownerPtr.get();
    if (actor && (actor->isClientSide() ? displayAxis(actor->getRotation().x) >= 0 : detail::isDisplayActor(*actor))) {
        if (!actor->isClientSide() && !detail::ownsDisplay(actor->getOrCreateUniqueID().rawID)) actor->remove();
        return;
    }
    origin(entity, owner, onLand);
}

// Crash safety: display entities are never written to the world. Contraptions are saved in our
// own DB and their displays are spawned again after a load, so a world saved while blocks were
// moving (autosave, or the last save before a crash) holds no falling blocks that could land in a
// world opened without Levitate. (Displays in a chunk that unloads simply vanish with it.)
LL_TYPE_INSTANCE_HOOK(
    DisplaySaveHook,
    ll::memory::HookPriority::Normal,
    ::Actor,
    &::Actor::save,
    bool,
    CompoundTag& entityTag
) {
    if (!isClientSide() && getEntityTypeId() == ActorType::FallingBlock && detail::isDisplayActor(*this)) return false;
    return origin(entityTag);
}

class EngineModule {
public:
    void install() {
        DisplaySaveHook::hook();
        DisplayTickHook::hook();
        DisplaySystemHook::hook();
        DisplayActorTickHook::hook();
        Engine::get().install();
    }
    void uninstall() {
        Engine::get().uninstall();
        DisplayActorTickHook::unhook();
        DisplaySystemHook::unhook();
        DisplayTickHook::unhook();
        DisplaySaveHook::unhook();
    }
};

} // namespace lv::feature::contraption

LV_REGISTER_MODULE(lv::feature::contraption::EngineModule);
