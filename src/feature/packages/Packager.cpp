// Packager and re-packager (SPEC §2.21, §3.6.25): Create 6.0.10's PackagerBlock(Entity) and
// RepackagerBlockEntity. The packager boxes the contents of the inventory behind it on a
// redstone pulse (or a stock network request), unpacks packages put into it into that inventory
// and holds one finished package for a frogport, funnel or player to take. The re-packager
// re-joins the fragments of an order into its final packages.

#include <algorithm>
#include <map>
#include <random>

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"

#include "core/packages/Address.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/packages/PackagesInternal.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::packages {

namespace items     = common::items;
namespace blockdata = common::blockdata;
using common::Access;

namespace {

std::map<GlobalPos, PackagerState>&                     states() {
    static std::map<GlobalPos, PackagerState> m;
    return m;
}
std::map<std::string, UnpackingHandler>&                 unpackers() {
    static std::map<std::string, UnpackingHandler> m;
    return m;
}
std::function<bool(BlockSource&, BlockPos const&)>&      linkProvider() {
    static std::function<bool(BlockSource&, BlockPos const&)> f;
    return f;
}

int facingOf(GlobalPos const& pos) { return std::clamp(stateAt(pos, kFacing).value_or(1), 0, 5); }

bool isRepackagerAt(GlobalPos const& pos) { return idAt(pos) == kRepackagerId; }

/// The inventory the packager works on (Create's InvManipulationBehaviour, opposite of facing).
struct Target {
    GlobalPos                         pos;
    int                               face = -1;
    std::optional<common::items::Handler> handler;
};
std::optional<Target> targetOf(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return std::nullopt;
    int    facing = facingOf(pos);
    Target t;
    t.pos  = offsetFace(pos, oppositeFace(facing));
    t.face = facing;
    if (!isLoaded(t.pos)) return std::nullopt;
    auto id = idAt(t.pos);
    // Create: never a packager's own handler, never a portable storage interface
    if (kindOf(id) == Kind::Packager || kindOf(id) == Kind::Repackager || id == "levitate:portable_storage_interface") return std::nullopt;
    t.handler = common::items::find(Access{*region, game::blockPos(t.pos), t.face});
    if (!t.handler) return std::nullopt;
    return t;
}

Access accessOf(Target const& t) { return Access{*regionOf(t.pos.dim), game::blockPos(t.pos), t.face}; }

/// Key identifying item + data (for summaries).
std::string keyOf(ItemStack const& s) { return items::toJson(items::withCount(s, 1)).dump(); }

void wakeTheFrogs(GlobalPos const& pos) {
    auto up = offsetFace(pos, 1);
    if (kindOf(idAt(up)) != Kind::Frogport) return;
    defer([up] {
        if (kindOf(idAt(up)) == Kind::Frogport) {
            // Create: FrogportBlockEntity.tryPullingFromOwnAndAdjacentInventories
            tickPort(up, Kind::Frogport, true);
        }
    });
}

bool redstoneModeActive(GlobalPos const& pos, bool repackager) { return repackager || !stateAt(pos, kLinked).value_or(0); }

void recheckIfLinksPresent(GlobalPos const& pos) {
    auto linked = stateAt(pos, kLinked);
    if (!linked) return; // the re-packager has no link state
    bool should = false;
    if (auto& f = linkProvider(); f) {
        if (auto* region = regionOf(pos.dim)) should = f(*region, game::blockPos(pos));
    }
    if (should != bool(*linked)) setStates(pos, {{kLinked, should ? 1 : 0}});
}

void updateSignAddress(GlobalPos const& pos, PackagerState& s) {
    s.signAddress.clear();
    for (int face = 0; face < 6; ++face) {
        auto text = signText(offsetFace(pos, face));
        if (!text || cp::isBlank(*text)) continue;
        s.signAddress = *text;
    }
}

bool defaultUnpack(BlockSource& region, BlockPos const& target, int facing, std::vector<ItemStack>& stacks, Json const&, bool simulate) {
    auto handler = common::items::find(Access{region, target, facing});
    if (!handler) return false;
    Access at{region, target, facing};
    if (!simulate) {
        for (auto const& s : stacks) {
            auto rest = handler->insert(at, s, false);
            if (!items::isEmpty(rest)) {
                auto p = target;
                dropItem(int(region.getDimensionId()), {p.x + 0.5, p.y + 1.0, p.z + 0.5}, rest);
            }
        }
        return true;
    }
    // everything must fit: stack the items into a copy of the inventory, and every item must be
    // accepted by the inventory itself (slot rules)
    std::vector<ItemStack> copy;
    int                    n = handler->slots(at);
    for (int i = 0; i < n; ++i) copy.push_back(handler->getSlot(at, i));
    for (auto const& s : stacks) {
        if (!items::isEmpty(insertStackedInto(copy, s))) return false;
        if (!items::isEmpty(handler->insert(at, s, true))) return false;
    }
    return true;
}

void sendFromInventory(GlobalPos const& pos, std::vector<PackagingRequest>* requests);

std::mt19937& orderRng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

/// Create's PackageRepackageHelper.repack: the final packages of a complete order.
std::vector<QueuedBox> repack(int orderId, std::vector<ItemStack> const& fragments) {
    std::vector<QueuedBox> exporting;
    std::string            address;
    Json                   orderContext;
    std::vector<cp::Stack> summary;
    std::map<std::string, ItemStack> prototypes;
    auto addToSummary = [&](ItemStack const& s, int count) {
        auto key = keyOf(s);
        prototypes.emplace(key, items::withCount(s, 1));
        for (auto& e : summary)
            if (e.key == key) {
                e.count += count;
                return;
            }
        summary.push_back({key, count, std::max(1, int(s.getMaxStackSize()))});
    };
    for (auto const& box : fragments) {
        address = addressOf(box);
        if (auto o = orderOf(box); o && !o->context.is_null() && !o->context.empty()) orderContext = o->context;
        for (auto const& s : contentsOf(box))
            if (!items::isEmpty(s)) addToSummary(s, items::countOf(s));
    }
    std::vector<cp::Stack> ordered;
    if (!orderContext.is_null()) {
        // packages split by recipe (Create's repackBasedOnRecipes)
        bool anyRecipe = false;
        for (auto const& craft : orderContext.value("crafts", Json::array())) {
            auto pattern = craft.value("pattern", Json::array());
            int  crafts  = craft.value("count", 0);
            int  toCreate = 0;
            for (int i = 0; i < crafts; ++i) {
                bool ok = true;
                for (auto const& req : pattern) {
                    auto stack = items::fromJson(req.value("item", Json()));
                    if (items::isEmpty(stack)) continue;
                    auto key = keyOf(stack);
                    auto it  = std::find_if(summary.begin(), summary.end(), [&](cp::Stack const& e) { return e.key == key; });
                    if (it == summary.end() || it->count <= 0) {
                        ok = false;
                        break;
                    }
                    it->count -= 1;
                }
                if (!ok) break;
                ++toCreate;
            }
            std::vector<ItemStack> slots;
            for (auto const& req : pattern) {
                if (int(slots.size()) >= kPackageSlots) break;
                slots.push_back(items::withCount(items::fromJson(req.value("item", Json())), 1));
            }
            ItemStack box = containing(slots);
            OrderData d;
            d.orderId       = std::uniform_int_distribution<int>(INT32_MIN, INT32_MAX)(orderRng());
            d.linkIndex     = 0;
            d.isFinalLink   = true;
            d.fragmentIndex = 0;
            d.isFinal       = true;
            d.context       = Json{{"stacks", pattern}, {"crafts", Json::array({Json{{"pattern", pattern}, {"count", 1}}})}};
            setOrder(box, d);
            anyRecipe = true;
            if (toCreate > 0) exporting.push_back({box, toCreate});
        }
        if (!anyRecipe) {
            for (auto const& st : orderContext.value("stacks", Json::array())) {
                auto stack = items::fromJson(st.value("item", Json()));
                if (items::isEmpty(stack)) continue;
                ordered.push_back({keyOf(stack), st.value("count", 0), std::max(1, int(stack.getMaxStackSize()))});
            }
        }
    }
    for (auto const& slots : cp::repackSlots(summary, ordered)) {
        std::vector<ItemStack> stacks;
        for (auto const& s : slots) stacks.push_back(items::withCount(prototypes[s.key], s.count));
        exporting.push_back({containing(stacks), 1});
    }
    for (auto& b : exporting) setAddress(b.box, address);
    for (std::size_t i = 0; i < exporting.size(); ++i) {
        auto& b = exporting[i];
        if (orderOf(b.box)) continue;
        bool      last = i + 1 == exporting.size();
        OrderData d;
        d.orderId     = orderId;
        d.isFinalLink = true;
        d.isFinal     = true;
        d.context     = last ? orderContext : Json();
        setOrder(b.box, d);
    }
    return exporting;
}

void attemptToRepackage(GlobalPos const& pos, PackagerState& s, Target const& t) {
    auto                                 at = accessOf(t);
    int                                  n  = t.handler->slots(at);
    std::map<int, std::vector<ItemStack>> collected;
    int                                  completed = -1;
    for (int slot = 0; slot < n; ++slot) {
        auto stack = t.handler->getSlot(at, slot);
        if (items::isEmpty(stack) || !isPackage(stack)) continue;
        auto order = orderOf(stack);
        if (!order) {
            auto taken = t.handler->extract(at, [stack](ItemStack const& x) { return items::sameItemSameData(x, stack); }, 1, false);
            if (items::isEmpty(taken)) continue;
            s.held            = taken;
            s.animationInward = false;
            s.animationTicks  = kPackagerCycle;
            savePackager(pos);
            syncPackager(pos, s);
            return;
        }
        if (order->orderId == -1) continue;
        auto& list = collected[order->orderId];
        list.push_back(items::withCount(stack, 1));
        std::vector<cp::Fragment> fragments;
        for (auto const& b : list) {
            auto o = orderOf(b);
            fragments.push_back({o->orderId, o->linkIndex, o->isFinalLink, o->fragmentIndex, o->isFinal});
        }
        if (cp::isOrderComplete(fragments)) {
            completed = order->orderId;
            break;
        }
    }
    if (completed == -1) return;
    auto boxes = repack(completed, collected[completed]);
    // take the fragments out of the inventory
    for (int slot = 0; slot < n; ++slot) {
        auto stack = t.handler->getSlot(at, slot);
        if (items::isEmpty(stack) || !isPackage(stack)) continue;
        auto o = orderOf(stack);
        if (!o || o->orderId != completed) continue;
        (void)t.handler->extract(at, [stack](ItemStack const& x) { return items::sameItemSameData(x, stack); }, 1, false);
    }
    if (boxes.empty()) return;
    for (auto& b : boxes) s.queue.push_back(b);
    savePackager(pos);
    log().info("packages: re-packager at {} completed order {} into {} package(s)", pos.key(), completed, boxes.size());
}

} // namespace

// ---- state ---------------------------------------------------------------------------------------

PackagerState& packager(GlobalPos const& pos) {
    auto& m  = states();
    auto  it = m.find(pos);
    if (it != m.end()) return it->second;
    PackagerState s;
    if (auto const* doc = blockdata::get(pos); doc && doc->contains("packages")) {
        auto const& j       = (*doc)["packages"];
        s.held              = items::fromJson(j.value("held", Json()));
        s.previouslyUnwrapped = items::fromJson(j.value("prev", Json()));
        for (auto const& q : j.value("queue", Json::array())) s.queue.push_back({items::fromJson(q.value("box", Json())), q.value("count", 1)});
        s.animationTicks  = j.value("anim", 0);
        s.animationInward = j.value("inward", true);
        s.redstonePowered = j.value("active", false);
        s.buttonCooldown  = j.value("cooldown", 0);
        s.signAddress     = j.value("sign", std::string{});
    }
    s.lastPowerSeen = stateAt(pos, kPowered).value_or(0) != 0;
    return m.emplace(pos, std::move(s)).first->second;
}

void savePackager(GlobalPos const& pos) {
    auto it = states().find(pos);
    if (it == states().end()) return;
    auto const& s = it->second;
    Json        q = Json::array();
    for (auto const& e : s.queue) q.push_back({{"box", items::toJson(e.box)}, {"count", e.count}});
    blockdata::edit(pos)["packages"] = {
        {"held",     items::toJson(s.held)               },
        {"prev",     items::toJson(s.previouslyUnwrapped)},
        {"queue",    q                                   },
        {"anim",     s.animationTicks                    },
        {"inward",   s.animationInward                   },
        {"active",   s.redstonePowered                   },
        {"cooldown", s.buttonCooldown                    },
        {"sign",     s.signAddress                       },
    };
}

void resetPackagers() { states().clear(); }
void forgetPackager(GlobalPos const& pos) { states().erase(pos); }

// ---- ticking ---------------------------------------------------------------------------------------

namespace {
void activate(GlobalPos const& pos, bool repackager) {
    auto& s           = packager(pos);
    s.redstonePowered = true;
    recheckIfLinksPresent(pos);
    if (!redstoneModeActive(pos, repackager)) return;
    updateSignAddress(pos, s);
    sendFromInventory(pos, nullptr);
    // don't send several packages for one button signal
    if (s.buttonCooldown <= 0) s.buttonCooldown = 40;
    savePackager(pos);
}
} // namespace

void tickPackager(GlobalPos const& pos, bool repackager, bool lazy) {
    auto& s = packager(pos);
    // neighbour updates (Create's neighborChanged): rising edge activates
    bool power = redstonePowered(pos);
    if (power != s.lastPowerSeen) {
        s.lastPowerSeen = power;
        setStates(pos, {{kPowered, power ? 1 : 0}});
        if (power) {
            log().info("packages: {} at {} activated by redstone", repackager ? "re-packager" : "packager", pos.key());
            activate(pos, repackager);
        }
    }
    if (lazy) {
        recheckIfLinksPresent(pos);
        if (s.redstonePowered) {
            s.redstonePowered = stateAt(pos, kPowered).value_or(0) != 0;
            if (redstoneModeActive(pos, repackager)) {
                updateSignAddress(pos, s);
                sendFromInventory(pos, nullptr);
            }
        }
    }

    if (s.buttonCooldown > 0) s.buttonCooldown--;
    if (s.animationTicks == 0) {
        s.previouslyUnwrapped = items::empty();
        if (!s.queue.empty() && items::isEmpty(s.held)) {
            auto& entry = s.queue.front();
            s.held      = entry.box;
            if (--entry.count <= 0) s.queue.erase(s.queue.begin());
            s.animationInward = false;
            s.animationTicks  = kPackagerCycle;
            savePackager(pos);
            syncPackager(pos, s);
        }
        return;
    }
    // sounds (Create plays them on the client at these ticks)
    if (s.animationTicks == kPackagerCycle - (s.animationInward ? 5 : 1)) playSound(pos.dim, centre(pos), "levitate.packages.packager");
    if (s.animationTicks == (s.animationInward ? 1 : 5)) playSound(pos.dim, centre(pos), "random.door_close", 0.25f, 0.75f);
    s.animationTicks--;
    if (s.animationTicks == 0) {
        wakeTheFrogs(pos);
        savePackager(pos);
    }
}

// ---- packing -------------------------------------------------------------------------------------

namespace {
void sendFromInventory(GlobalPos const& pos, std::vector<PackagingRequest>* queuedRequests) {
    auto& s          = packager(pos);
    bool  repackager = isRepackagerAt(pos);
    if (repackager) {
        if (!items::isEmpty(s.held) || s.animationTicks != 0 || s.buttonCooldown > 0) return;
        if (!s.queue.empty()) return;
        auto t = targetOf(pos);
        if (!t) return;
        attemptToRepackage(pos, s, *t);
        if (items::isEmpty(s.held)) return;
        updateSignAddress(pos, s);
        if (!cp::isBlank(s.signAddress)) setAddress(s.held, s.signAddress);
        savePackager(pos);
        return;
    }
    if (!queuedRequests && (!items::isEmpty(s.held) || s.animationTicks != 0 || s.buttonCooldown > 0)) return;
    auto t = targetOf(pos);
    if (!t) return;
    auto at = accessOf(*t);

    bool                   anyItemPresent = false;
    std::vector<ItemStack> extracted(kPackageSlots);
    ItemStack              extractedPackage;
    PackagingRequest*      next = nullptr;
    std::optional<std::string> fixedAddress;
    int                    fixedOrderId = 0;
    int                    linkIndex = 0, packageIndex = 0;
    bool                   finalLink = false, finalPackage = false;
    Json                   orderContext;
    bool                   requestQueue = queuedRequests != nullptr;

    if (requestQueue && !queuedRequests->empty()) {
        next         = &queuedRequests->front();
        fixedAddress = next->address;
        fixedOrderId = next->orderId;
        linkIndex    = next->linkIndex;
        finalLink    = next->finalLink;
        packageIndex = next->packageCounter++;
        orderContext = next->context;
    }

    bool stop = false;
    for (int i = 0; i < kPackageSlots && !stop; ++i) {
        bool continuePacking = true;
        while (continuePacking && !stop) {
            continuePacking = false;
            int slots       = t->handler->slots(at);
            for (int slot = 0; slot < slots; ++slot) {
                if (requestQueue && !next) break;
                auto inSlot = t->handler->getSlot(at, slot);
                if (items::isEmpty(inSlot)) continue;
                if (requestQueue && !items::sameItemSameData(inSlot, next->item)) continue;
                int initial = requestQueue ? std::min(64, next->count) : 64;
                int want    = std::min(initial, items::countOf(inSlot));
                bool bulky  = !fitsInPackage(inSlot);
                if (bulky && anyItemPresent) continue;
                // how much fits into the package
                auto probe = extracted;
                int  fits  = want - items::countOf(insertStackedInto(probe, items::withCount(inSlot, want)));
                if (fits <= 0) continue;
                auto got = t->handler->extract(at, [inSlot](ItemStack const& x) { return items::sameItemSameData(x, inSlot); }, fits, false);
                if (items::isEmpty(got)) continue;
                anyItemPresent = true;
                auto rest      = insertStackedInto(extracted, got);
                if (!items::isEmpty(rest)) (void)t->handler->insert(at, rest, false);
                int transferred = items::countOf(got) - items::countOf(rest);
                if (isPackage(got)) extractedPackage = got;
                if (!requestQueue) {
                    if (bulky) stop = true;
                    if (stop) break;
                    continue;
                }
                next->count -= transferred;
                if (next->count > 0) {
                    if (bulky) stop = true;
                    if (stop) break;
                    continue;
                }
                finalPackage = true;
                queuedRequests->erase(queuedRequests->begin());
                next = nullptr;
                if (queuedRequests->empty()) {
                    stop = true;
                    break;
                }
                auto& candidate = queuedRequests->front();
                if (candidate.address != *fixedAddress || candidate.orderId != fixedOrderId) {
                    stop = true;
                    break;
                }
                candidate.packageCounter = packageIndex + 1;
                next                     = &candidate;
                finalPackage             = false;
                continuePacking          = true;
                if (!next->context.is_null()) orderContext = next->context;
                if (bulky) stop = true;
                break;
            }
        }
    }

    if (!anyItemPresent) {
        if (next) queuedRequests->erase(queuedRequests->begin());
        return;
    }

    ItemStack box = items::isEmpty(extractedPackage) ? containing(extracted) : extractedPackage;
    clearAddress(box);
    if (fixedAddress) setAddress(box, *fixedAddress);
    if (requestQueue) {
        OrderData d;
        d.orderId       = fixedOrderId;
        d.linkIndex     = linkIndex;
        d.isFinalLink   = finalLink;
        d.fragmentIndex = packageIndex;
        d.isFinal       = finalPackage;
        d.context       = orderContext;
        setOrder(box, d);
    }
    if (!requestQueue && !cp::isBlank(s.signAddress)) setAddress(box, s.signAddress);
    int stacks = 0;
    for (auto const& e : contentsOf(box)) stacks += items::isEmpty(e) ? 0 : 1;
    log().info("packages: packager at {} packed a package for '{}' ({} stacks)", pos.key(), addressOf(box), stacks);

    if (!items::isEmpty(s.held) || s.animationTicks != 0) {
        s.queue.push_back({box, 1});
        savePackager(pos);
        return;
    }
    s.held            = box;
    s.animationInward = false;
    s.animationTicks  = kPackagerCycle;
    savePackager(pos);
    syncPackager(pos, s);
}
} // namespace

bool unwrapBox(GlobalPos const& pos, ItemStack const& box, bool simulate) {
    auto& s = packager(pos);
    if (s.animationTicks > 0) return false;
    auto* region = regionOf(pos.dim);
    if (!region) return false;
    if (isRepackagerAt(pos)) {
        auto t = targetOf(pos);
        if (!t) return false;
        auto rest = t->handler->insert(accessOf(*t), items::withCount(box, 1), simulate);
        if (!items::isEmpty(rest)) return false;
        if (simulate) return true;
    } else {
        std::vector<ItemStack> stacks;
        for (auto const& c : contentsOf(box))
            if (!items::isEmpty(c)) stacks.push_back(c);
        if (stacks.empty()) return true;
        int  facing = facingOf(pos);
        auto target = offsetFace(pos, oppositeFace(facing));
        if (!isLoaded(target)) return false;
        auto it      = unpackers().find(idAt(target));
        auto handler = it != unpackers().end() ? it->second : UnpackingHandler(defaultUnpack);
        if (!handler(*region, game::blockPos(target), facing, stacks, orderContextOf(box), simulate)) return false;
        if (simulate) return true;
    }
    s.previouslyUnwrapped = items::withCount(box, 1);
    s.animationInward     = true;
    s.animationTicks      = kPackagerCycle;
    savePackager(pos);
    syncPackager(pos, s);
    log().info("packages: {} at {} unpacked a package for '{}'", isRepackagerAt(pos) ? "re-packager" : "packager", pos.key(), addressOf(box));
    return true;
}

// ---- interaction ---------------------------------------------------------------------------------

bool usePackager(Player& player, GlobalPos const& pos) {
    auto& s    = packager(pos);
    auto  held = player.getSelectedItem();
    if (items::isEmpty(s.held)) {
        if (s.animationTicks > 0) return true;
        if (isPackage(held)) {
            if (!unwrapBox(pos, held, true)) return true;
            unwrapBox(pos, held, false);
            player.setSelectedItem(items::withCount(held, items::countOf(held) - 1)); // Create shrinks in creative too
            player.sendInventory(false);
            playSound(pos.dim, centre(pos), "levitate.packages.depot_plop");
        }
        return true;
    }
    if (s.animationTicks > 0) return true;
    giveOrDrop(player, s.held);
    auto p = player.getFeetPos();
    playSound(player.getDimensionId(), {p.x, p.y, p.z}, "random.pop", 0.2f, 1.f);
    s.held = items::empty();
    savePackager(pos);
    log().info("packages: {} took the package from the packager at {}", player.getRealName(), pos.key());
    return true;
}

std::optional<int> packagerPlacementFacing(Player& player, GlobalPos const& pos, bool) {
    auto* region = regionOf(pos.dim);
    if (!region) return std::nullopt;
    auto               faces = nearestLookingFaces(lookVec(player));
    std::optional<int> preferred;
    for (int face : faces) {
        auto n  = offsetFace(pos, face);
        auto id = idAt(n);
        if (id.empty() || kindOf(id) == Kind::Packager || kindOf(id) == Kind::Repackager) continue;
        if (common::items::has(*region, game::blockPos(n), -1)) {
            preferred = oppositeFace(face);
            break;
        }
    }
    if (!preferred) preferred = player.isSneaking() ? faces[0] : oppositeFace(faces[0]);
    if (idAt(offsetFace(pos, oppositeFace(*preferred))) == "levitate:portable_storage_interface") {
        actionBar(player, trFor(player, "create.packager.no_portable_storage", "Packagers cannot interface with Portable Storage directly"));
        return std::nullopt;
    }
    return preferred;
}

void onPackagerPlaced(GlobalPos const& pos, int facing) {
    bool power = redstonePowered(pos);
    setStates(pos, {{kFacing, facing}, {kPowered, power ? 1 : 0}});
    auto& s         = packager(pos);
    s.lastPowerSeen = power;
    savePackager(pos);
}

void installPackagerCapabilities() {
    common::items::Handler h;
    h.slots   = [](Access const&) { return 1; };
    h.getSlot = [](Access const& a, int) { return packager(game::globalPos(a.region, a.pos)).held; };
    h.insert  = [](Access const& a, ItemStack const& stack, bool simulate) -> ItemStack {
        auto  pos = game::globalPos(a.region, a.pos);
        auto& s   = packager(pos);
        if (!items::isEmpty(s.held) || !s.queue.empty() || !isPackage(stack)) return stack;
        if (!unwrapBox(pos, stack, true)) return stack;
        if (!simulate) unwrapBox(pos, stack, false);
        return items::withCount(stack, items::countOf(stack) - 1);
    };
    h.extract = [](Access const& a, common::items::ItemPredicate const& filter, int amount, bool simulate) -> ItemStack {
        auto  pos = game::globalPos(a.region, a.pos);
        auto& s   = packager(pos);
        if (s.animationTicks != 0 || items::isEmpty(s.held) || amount <= 0 || (filter && !filter(s.held))) return items::empty();
        ItemStack box = s.held;
        if (!simulate) {
            s.held = items::empty();
            savePackager(pos);
        }
        return box;
    };
    common::items::registerBlock(kPackagerId, h);
    common::items::registerBlock(kRepackagerId, h);
}

// ---- contract ------------------------------------------------------------------------------------

bool isPackager(GlobalPos const& pos) {
    auto k = kindOf(idAt(pos));
    return k == Kind::Packager || k == Kind::Repackager;
}

std::vector<ItemStack> availableItems(GlobalPos const& pos) {
    std::vector<ItemStack> out;
    if (!isPackager(pos)) return out;
    auto t = targetOf(pos);
    if (!t) return out;
    auto at = accessOf(*t);
    int  n  = t->handler->slots(at);
    for (int i = 0; i < n; ++i) {
        auto s = t->handler->getSlot(at, i);
        if (items::isEmpty(s)) continue;
        bool merged = false;
        for (auto& o : out)
            if (items::sameItemSameData(o, s)) {
                o      = items::withCount(o, items::countOf(o) + items::countOf(s));
                merged = true;
                break;
            }
        if (!merged) out.push_back(s);
    }
    return out;
}

bool isTooBusyFor(GlobalPos const& pos, RequestType type) {
    auto queue = packager(pos).queue.size();
    switch (type) {
    case RequestType::Player: return queue >= 50;
    case RequestType::Redstone: return queue >= 20;
    case RequestType::Restock: return queue >= 10;
    }
    return false;
}

void attemptToSend(GlobalPos const& pos, std::vector<PackagingRequest>& queue) {
    if (!isPackager(pos)) return;
    sendFromInventory(pos, &queue);
}

void setLinkProvider(std::function<bool(BlockSource&, BlockPos const&)> provider) { linkProvider() = std::move(provider); }

void registerUnpackingHandler(std::string const& blockId, UnpackingHandler handler) { unpackers()[blockId] = std::move(handler); }

} // namespace lv::feature::packages
