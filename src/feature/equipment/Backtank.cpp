// Backtanks and diving gear (Create's BacktankItem / BacktankBlock(Entity) / BacktankUtil,
// DivingHelmetItem, DivingBootsItem, NetheriteDivingHandler): air stored in the item, filled
// by a placed backtank turning on a shaft, used for breathing under water, by air tools, and
// the netherite set's fire immunity.

#include <algorithm>
#include <cmath>
#include <map>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PlayerFogPacket.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeInstanceRef.h"
#include "mc/world/attribute/AttributeModificationContext.h"
#include "mc/world/attribute/AttributeModifier.h"
#include "mc/world/attribute/AttributeModifierOperation.h"
#include "mc/world/attribute/AttributeOperands.h"
#include "mc/world/attribute/MutableAttributeWithContext.h"
#include "mc/world/attribute/SharedAttributes.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/deps/shared_types/v1_26_20/block/MaterialType.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/Level.h"

#include "core/equipment/Gear.h"
#include "feature/advancements/Advancements.h"
#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/equipment/Equipment.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::equipment {

namespace ce    = core::equipment;
namespace items = common::items;
namespace bd    = common::blockdata;
namespace k     = core::kinetics;
using SharedTypes::Legacy::ArmorSlot;

namespace {

/// Durability of the backtank items in the behavior pack (the air bar).
constexpr int kBarDurability = 900;

// ---- kinetic block: a shaft end on the bottom (Create's BacktankBlock.hasShaftTowards(DOWN)) -----
k::KineticSpec backtankSpec(std::string key) {
    k::KineticSpec s;
    s.states          = {};
    s.rotationAxis    = [](k::BlockState const&) -> std::optional<k::Axis> { return k::Axis::Y; };
    s.hasShaftTowards = [](k::BlockState const&, k::Face f) { return f == k::Face::Down; };
    s.impact          = 4;
    s.stressKey       = std::move(key);
    return s;
}
LV_REGISTER_KINETIC_BLOCK(std::string(kCopperBacktank), backtankSpec("copper_backtank"));
LV_REGISTER_KINETIC_BLOCK(std::string(kNetheriteBacktank), backtankSpec("netherite_backtank"));

struct Filler {
    float speed    = 0;
    int   cooldown = 0;
    bool  full     = false;
};
std::map<GlobalPos, Filler>& fillers() {
    static std::map<GlobalPos, Filler> m;
    return m;
}
std::vector<kinetics::ListenerId> gListeners;

/// Creative breaks (no drop) and pickups (the item was given already).
std::map<GlobalPos, bool>& handledBreaks() {
    static std::map<GlobalPos, bool> m;
    return m;
}

Json* blockDoc(GlobalPos const& pos, bool create) {
    if (!create) {
        auto const* d = bd::get(pos);
        if (!d || !d->contains("equipment")) return nullptr;
    }
    auto& doc = bd::edit(pos);
    auto& e   = doc["equipment"];
    if (!e.is_object()) e = Json::object();
    return &e;
}

void effect(Player& p, unsigned id, int ticks, int amplifier = 0) {
    EffectDuration d;
    d.mValue = ticks;
    MobEffectInstance inst(id, d);
    inst.mAmplifier     = amplifier;
    inst.mAmbient       = true;
    inst.mEffectVisible = false;
    p.addEffect(inst);
}

ItemStack backtankItem(std::string const& id, int air, std::string const& name) {
    auto item = items::make(id, 1);
    if (!item) return items::empty();
    setAir(*item, air);
    (void)name;
    return *item;
}

/// Per-player state: the last air value warned about, the attribute modifiers and fog applied.
struct DiverState {
    int  lastAir    = -1;
    bool usingAir   = false;
    int  netherite  = -1; ///< netherite pieces counted in the knockback resistance modifier
    int  bootsSpeed = -1; ///< 1 while the diving boots' speed modifier is applied
    int  fog        = -1; ///< 0 none, 1 diving helmet, 2 netherite diving helmet
};

// ---- netherite diving gear: armour toughness and knockback resistance (Create: ArmorMaterials.NETHERITE) ---------

constexpr int   kNetheriteToughness  = 3;    ///< per piece
constexpr float kNetheriteKnockback  = 0.1f; ///< per piece
constexpr char  kKnockbackModifier[] = "6c0f6b1e-6d2a-4c55-9a43-1f3c0e5b7a01";
constexpr char  kBootsModifier[]     = "6c0f6b1e-6d2a-4c55-9a43-1f3c0e5b7a02";
/// Create's DivingBootsItem multiplies the walking motion under water by 1.3 every tick while it is
/// below 0.145 blocks per tick: the walking speed settles at ~0.15 instead of ~0.08 (water drag
/// 0.8, acceleration 0.02). Vanilla clients move themselves, so their underwater movement speed
/// attribute gets the same top speed; the client mod applies Create's motion itself.
constexpr float kBootsSpeedBonus = 0.875f;

int netheritePieces(Actor const& a) {
    int n = 0;
    if (items::idOf(a.getArmor(ArmorSlot::Head)) == kNetheriteHelmet) ++n;
    if (items::idOf(a.getArmor(ArmorSlot::Torso)) == kNetheriteBacktank) ++n;
    if (items::idOf(a.getArmor(ArmorSlot::Feet)) == kNetheriteBoots) ++n;
    return n;
}

/// Replaces our modifier of `attr` (amount 0: removes it).
void setModifier(Player& p, Attribute const& attr, char const* uuid, char const* name, float amount, AttributeModifierOperation op) {
    auto               m    = p.getMutableAttribute(attr);
    AttributeInstance* inst = m.mInstance->mPtr;
    if (!inst) return;
    auto id = mce::UUID::fromString(uuid);
    inst->removeModifier(id, m.mContext);
    if (amount != 0) inst->addModifier(AttributeModifier(id, name, amount, op, AttributeOperands::OperandCurrent, false), m.mContext);
}

/// Create's ClientEvents#getFogDensity as a fog layer (`assets/pack/rp/fogs/`): the diving helmet
/// sees 6.25 times as far under water, the netherite one also sees 20 blocks in lava. Fog layers
/// reach vanilla clients too (PlayerFogPacket).
void sendFog(Player& p, int fog) {
    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlayerFogSetting);
    if (!packet) return;
    auto&                    pkt = static_cast<PlayerFogPacket&>(*packet);
    std::vector<std::string> stack;
    if (fog == 1) stack.emplace_back("levitate:diving_helmet");
    if (fog == 2) stack.emplace_back("levitate:netherite_diving_helmet");
    pkt.mFogStack = stack;
    pkt.sendTo(p);
}
std::map<std::int64_t, DiverState>& divers() {
    static std::map<std::int64_t, DiverState> m;
    return m;
}

void warn(Player& player, int before, int after) {
    int w = ce::backtankWarning(before, after, maxAir());
    if (w == 0) return;
    auto pos = player.getPosition();
    int  dim = static_cast<int>(player.getDimensionId());
    if (w == 2) subtitle(player, "§c⚠ " + trFor(player, "create.backtank.depleted", "Backtank pressure depleted"));
    else subtitle(player, "§6⚠ " + trFor(player, "create.backtank.low", "Backtank pressure low"));
    sound(dim, pos.x, pos.y, pos.z, "note.bass", 1.f, 1.25f);
    sound(dim, pos.x, pos.y, pos.z, "random.fizz", .5f, .5f);
    log().info("equipment: backtank of {} is {}", player.getRealName(), w == 2 ? "depleted" : "low");
}

/// The worn backtank (Bedrock has one chest slot: Create's list of backtanks has one entry).
ItemStack const* wornBacktank(Player& p) {
    auto const& chest = p.getArmor(ArmorSlot::Torso);
    if (items::isEmpty(chest) || !isBacktank(items::idOf(chest))) return nullptr;
    return &chest;
}

/// Changes the air of the worn backtank (writes the item back).
void consumeAir(Player& p, int amount) {
    auto const* worn = wornBacktank(p);
    if (!worn || amount <= 0) return;
    ItemStack copy   = *worn;
    int       before = airOf(copy);
    int       after  = std::max(0, before - amount);
    setAir(copy, after);
    p.setArmor(ArmorSlot::Torso, copy);
    p.sendInventory(false);
    warn(p, before, after);
}

bool fireResistantSet(Player& p) {
    auto id = [&](ArmorSlot s) { return items::idOf(p.getArmor(s)); };
    auto const* tank = wornBacktank(p);
    return id(ArmorSlot::Head) == kNetheriteHelmet && tank && items::idOf(*tank) == kNetheriteBacktank && airOf(*tank) > 0
        && id(ArmorSlot::Legs) == "minecraft:netherite_leggings"
        && (id(ArmorSlot::Feet) == kNetheriteBoots || id(ArmorSlot::Feet) == "minecraft:netherite_boots");
}

void sendAirHud(Player& p, int air, bool show) {
    if (net::isCompanion(p)) {
        net::send(p, "equipment.air", {{"air", air}, {"max", maxAir()}, {"show", show}});
    } else if (show) {
        actionBar(p, (air < 60 ? "§c" : "§f") + ce::backtankTimeLeft(air));
    }
}

} // namespace

bool isBacktank(std::string_view id) { return id == kCopperBacktank || id == kNetheriteBacktank; }

int maxAir() { return std::max(1, config().airInBacktank); }

int airOf(ItemStack const& stack) {
    auto j = items::getData(stack, kAirData);
    return j.is_number() ? std::clamp(j.get<int>(), 0, maxAir()) : 0;
}

void setAir(ItemStack& stack, int air) {
    air = std::clamp(air, 0, maxAir());
    items::setData(stack, kAirData, air);
    stack.setDamageValue(static_cast<short>(ce::backtankBarDamage(air, maxAir(), kBarDurability)));
}

bool useAirOrDurability(Player& player, bool mainHand, int usesPerTank) {
    if (player.isCreative()) return true;
    if (usesPerTank <= 0) return true; // Create: 0 makes the tool unbreakable
    auto const* worn = wornBacktank(player);
    if (worn && airOf(*worn) > 0) {
        consumeAir(player, ce::airToolCost(maxAir(), usesPerTank));
        return true;
    }
    // no air: the tool takes 1 durability
    ItemStack held = mainHand ? player.getSelectedItem() : player.getOffhandSlot();
    if (items::isEmpty(held)) return false;
    held.hurtAndBreak(1, &player);
    if (mainHand) player.setSelectedItem(held);
    else player.setOffhandSlot(held);
    player.sendInventory(false);
    return false;
}

int blockAir(GlobalPos const& pos) {
    auto const* d = bd::get(pos);
    if (!d || !d->contains("equipment")) return 0;
    return (*d)["equipment"].value("air", 0);
}

void setBlockAir(GlobalPos const& pos, int air) {
    auto* e   = blockDoc(pos, true);
    (*e)["air"] = std::clamp(air, 0, maxAir());
}

void backtankPlaced(Player& player, GlobalPos const& pos, int air, std::string const& name) {
    int facing = game::opposite(game::playerFacing(player));
    place(pos, idAt(pos), {{kFacing, facing}});
    auto* e    = blockDoc(pos, true);
    (*e)["air"] = std::clamp(air, 0, maxAir());
    if (!name.empty()) (*e)["name"] = name;
    log().info("equipment: backtank placed at {} {} {} with {} air", pos.x, pos.y, pos.z, air);
}

bool useBacktankBlock(Player& player, ItemStack& held, GlobalPos const& pos) {
    // Create: with an empty chest slot, not sneaking and not holding a block item: wear it
    if (player.isSneaking()) return false;
    if (!items::isEmpty(player.getArmor(ArmorSlot::Torso))) return false;
    if (!items::isEmpty(held) && game::blockWith(items::idOf(held))) return false;
    std::string id   = idAt(pos);
    int         air  = blockAir(pos);
    handledBreaks()[pos] = true;
    if (!clearAt(pos)) {
        handledBreaks().erase(pos);
        return false;
    }
    handledBreaks().erase(pos);
    player.setArmor(ArmorSlot::Torso, backtankItem(id, air, {}));
    player.sendInventory(false);
    sound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "random.pop", .75f, 1.f);
    log().info("equipment: {} put on the backtank from {} {} {} ({} air)", player.getRealName(), pos.x, pos.y, pos.z, air);
    return true;
}

/// Before a player breaks a backtank block.
void noteBacktankBreak(GlobalPos const& p, bool creative) {
    blockDoc(p, true);
    if (creative) handledBreaks()[p] = true;
}

void clearBacktankBreak(GlobalPos const& p) { handledBreaks().erase(p); }

void tickBacktanks() {
    for (auto& [pos, f] : fillers()) {
        if (f.speed == 0) continue;
        if (f.cooldown > 0) {
            --f.cooldown;
            continue;
        }
        if (!isBacktank(idAt(pos))) continue;
        int air = blockAir(pos);
        if (air >= maxAir()) continue;
        int next = std::min(maxAir(), air + ce::backtankFillAmount(f.speed));
        setBlockAir(pos, next);
        f.cooldown = ce::backtankFillDelay(f.speed);
        if (currentTick() % 10 == 0) particle(pos.dim, pos.x + .5, pos.y + 1.0, pos.z + .5, "minecraft:bubble_column_up_particle");
        if (next >= maxAir()) {
            // Create: "confirm" sound and a ring of spit particles when full
            sound(pos.dim, pos.x + .5, pos.y + .5, pos.z + .5, "random.orb", .4f, 1.f);
            for (int i = 0; i < 36; i += 3) {
                double a = i * 10.0 * 3.14159265 / 180.0;
                particle(pos.dim, pos.x + .5 + std::cos(a) * .6, pos.y + .8, pos.z + .5 + std::sin(a) * .6, "minecraft:basic_smoke_particle");
            }
            log().info("equipment: backtank at {} {} {} is full", pos.x, pos.y, pos.z);
        }
    }
}

/// Create's Entity#isInLava: lava where the player stands.
bool lavaAtFeet(Player& p) {
    auto pos = p.getFeetBlockPos();
    auto id  = idAt(GlobalPos{static_cast<int>(p.getDimensionId()), pos.x, pos.y, pos.z});
    return id == "minecraft:lava" || id == "minecraft:flowing_lava";
}

void tickDivingGear() {
    auto level = ll::service::getLevel();
    if (!level) return;
    bool second = currentTick() % 20 == 0;
    level->forEachPlayer([&](Player& p) {
        if (p.isClientSide() || !p.isAlive()) return true;
        auto  uid  = uidOf(p);
        auto& st   = divers()[uid];
        auto  head = items::idOf(p.getArmor(ArmorSlot::Head));
        auto  feet = items::idOf(p.getArmor(ArmorSlot::Feet));
        auto const* tank = wornBacktank(p);
        int   air  = tank ? airOf(*tank) : 0;
        bool  helmet = head == kCopperHelmet || head == kNetheriteHelmet;
        bool  underWater = p.isUnderLiquid(SharedTypes::v1_26_20::MaterialType::Water);
        bool  inLava     = p.isUnderLiquid(SharedTypes::v1_26_20::MaterialType::Lava);
        bool  breathing  = false;
        // Create's DivingHelmetItem#breatheUnderwater: lava diving with the netherite helmet
        if (head == kNetheriteHelmet && tank && air > 0 && (inLava || lavaAtFeet(p))) advancements::award(p, "diving_suit_lava");
        // helmet: breathing from the backtank (1 air per second while it is needed)
        if (helmet && tank && air > 0 && !p.isCreative()) {
            if (underWater) {
                breathing = true;
                effect(p, 13, 40); // water breathing while the tank provides air
                if (second) consumeAir(p, 1);
                advancements::award(p, "diving_suit");
            } else if (inLava && head == kNetheriteHelmet && items::idOf(*tank) == kNetheriteBacktank) {
                breathing = true;
                if (second) consumeAir(p, 1);
            }
        }
        // the netherite set makes the wearer immune to fire
        if (fireResistantSet(p)) effect(p, 12, 40);
        // netherite pieces: knockback resistance (the toughness is added by the hook below)
        int pieces = netheritePieces(p);
        if (pieces != st.netherite) {
            setModifier(p, SharedAttributes::KNOCKBACK_RESISTANCE(), kKnockbackModifier, "Netherite diving gear", kNetheriteKnockback * static_cast<float>(pieces),
                        AttributeModifierOperation::OperationAddition);
            st.netherite = pieces;
        }
        // the diving helmet's fog
        int fog = head == kNetheriteHelmet ? 2 : head == kCopperHelmet ? 1 : 0;
        if (fog != st.fog) {
            if (fog != 0 || st.fog > 0) sendFog(p, fog);
            st.fog = fog;
        }
        // boots: sink and walk on the ground under water (Create's DivingBootsItem.accelerateDescentUnderwater);
        // the client mod moves its own player, vanilla clients get motion packets and a faster underwater walk
        bool boots     = feet == kCopperBoots || feet == kNetheriteBoots;
        bool affected  = boots && p.isInWater() && !p.isSwimming() && !p.isFlying();
        bool companion = net::isCompanion(p);
        int  speedMod  = affected && !companion ? 1 : 0;
        if (speedMod != st.bootsSpeed) {
            setModifier(p, SharedAttributes::UNDERWATER_MOVEMENT_SPEED(), kBootsModifier, "Diving boots", speedMod ? kBootsSpeedBonus : 0.f,
                        AttributeModifierOperation::OperationMultiplyTotal);
            st.bootsSpeed = speedMod;
        }
        if (affected && !companion) {
            auto const& v = p.getPosDelta();
            double      vy = v.y;
            double      vx = v.x, vz = v.z;
            bool        jumping = p.isJumping();
            if (jumping && p.isOnGround()) vy += 0.5;
            else if (!p.isOnGround()) vy -= 0.05;
            if (!p.isOnGround() || jumping) setMotion(p, vx, vy, vz);
        }
        // air HUD (Create: shown under water without breath, or in lava)
        bool show = tank && (breathing || ((underWater || inLava) && !p.isCreative()));
        if (show != st.usingAir || (show && second)) sendAirHud(p, air, show);
        st.usingAir = show;
        st.lastAir  = air;
        return true;
    });
}
void resetBacktanks() {
    fillers().clear();
    divers().clear();
    handledBreaks().clear();
}

void installBacktanks() {
    static bool done = false;
    if (!done) {
        done = true;
        bd::onErase([](GlobalPos const& p, Json const& doc, std::string const& prevId) {
            if (prevId.empty() || !isBacktank(prevId)) return;
            fillers().erase(p);
            if (handledBreaks().contains(p)) return;
            int  air  = doc.contains("equipment") ? doc["equipment"].value("air", 0) : 0;
            auto item = backtankItem(prevId, air, {});
            defer([p, item] { dropAt(p.dim, p.x + .5, p.y + .5, p.z + .5, item); });
        });
    }
    for (auto id : {kCopperBacktank, kNetheriteBacktank}) {
        gListeners.push_back(kinetics::onSpeedChanged(std::string(id), [](kinetics::SpeedChange const& c) {
            if (c.removed) {
                fillers().erase(c.pos);
                return;
            }
            auto& f = fillers()[c.pos];
            f.speed = c.speed;
        }));
    }
}

void uninstallBacktanks() {
    for (auto id : gListeners) kinetics::removeListener(id);
    gListeners.clear();
}

} // namespace lv::feature::equipment

// Create: the netherite diving gear is ArmorMaterials.NETHERITE (toughness 3 per piece); Bedrock's
// data-driven wearables have protection only.
LL_TYPE_INSTANCE_HOOK(EquipmentToughnessHook, ll::memory::HookPriority::Normal, Mob, &Mob::getToughnessValue, int) {
    return origin() + lv::feature::equipment::netheriteToughness(*this);
}

namespace lv::feature::equipment {
int  netheriteToughness(Actor const& a) { return kNetheriteToughness * netheritePieces(a); }
void hookToughness(bool on) {
    if (on) EquipmentToughnessHook::hook();
    else EquipmentToughnessHook::unhook();
}
} // namespace lv::feature::equipment
