// Cardboard armour and the cardboard sword (U49, SPEC §3.6.31): Create's CardboardArmorItem /
// CardboardArmorHandler(Client) / AllArmorMaterials.CARDBOARD, CardboardSwordItem / AllToolMaterials /
// KnockbackPacket, and the trim advancement of SmithingMenuMixin.
//
// Hiding: a player in a full cardboard set who sneaks (not flying) is made invisible by the server;
// every client then draws the armour's attachables only, and the chestplate's attachable draws a
// cardboard package instead of the armour (the other pieces draw nothing), which is Create's
// CardboardArmorHandlerClient for vanilla clients too. Mobs can't pick the hiding player as a target
// (Create: visibility 0) and forget it every 16 ticks.

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeInstanceConstRef.h"
#include "mc/world/attribute/SharedAttributes.h"
#include "mc/world/effect/EffectDuration.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/inventory/network/ItemStackNetResult.h"
#include "mc/world/inventory/network/crafting/ItemStackRequestActionCraftHandler.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"
#include "mc/world/item/enchanting/Enchant.h"
#include "mc/world/item/enchanting/EnchantResult.h"
#include "mc/world/item/enchanting/EnchantUtils.h"
#include "mc/world/item/enchanting/EnchantmentInstance.h"
#include "mc/world/item/enchanting/ItemEnchants.h"
#include "mc/world/level/Level.h"

#include "feature/advancements/Advancements.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/equipment/Equipment.h"
#include "game/WorldDb.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::equipment {

namespace items = common::items;
namespace ia    = common::interactions;
using SharedTypes::Legacy::ArmorSlot;

namespace {

constexpr char kBonk[] = "levitate.cardboard_sword.bonk";

constexpr char kDisguise[]    = "levitate:cardboard_disguise";
constexpr char kDisguiseTag[] = "lv_cardboard_disguise";

struct State {
    std::set<std::int64_t>                 hiding;    ///< players hiding as a box (uids)
    std::map<std::int64_t, std::int64_t>   disguises; ///< player uid -> box entity uid
    std::set<std::int64_t>                 seen;      ///< players whose invisibility was checked once
    std::uint64_t                          tick = 0;
};
State& st() {
    static State s;
    return s;
}


} // namespace

bool isCardboardArmor(std::string_view id) {
    return id == kCardboardHelmet || id == kCardboardChestplate || id == kCardboardLeggings || id == kCardboardBoots;
}

bool hidingAsBox(Actor const& actor) {
    if (!actor.hasType(ActorType::Mob) || !actor.isSneaking()) return false;
    if (actor.isPlayer() && static_cast<Player const&>(actor).isFlying()) return false;
    return items::idOf(actor.getArmor(ArmorSlot::Head)) == kCardboardHelmet && items::idOf(actor.getArmor(ArmorSlot::Torso)) == kCardboardChestplate
        && items::idOf(actor.getArmor(ArmorSlot::Legs)) == kCardboardLeggings && items::idOf(actor.getArmor(ArmorSlot::Feet)) == kCardboardBoots;
}

namespace {

bool hiddenTarget(ActorUniqueID id) {
    if (st().hiding.empty() || id.rawID == -1) return false;
    auto* p = playerById(id.rawID);
    return p && hidingAsBox(*p);
}

void addEffect(Actor& a, unsigned id, int ticks, int amplifier) {
    EffectDuration d;
    d.mValue = ticks;
    MobEffectInstance inst(id, d);
    inst.mAmplifier     = amplifier;
    inst.mAmbient       = true;
    inst.mEffectVisible = false;
    a.addEffect(inst);
}

Actor* actorById(std::int64_t uid) {
    auto level = ll::service::getLevel();
    if (!level || !uid) return nullptr;
    ActorUniqueID id;
    id.rawID = uid;
    return level->fetchEntity(id, false);
}

/// Keeps the box entity of a hiding player at its feet, turned with it (CardboardArmorHandlerClient
/// draws the box at the player); spawns it with a random plain package style.
void placeDisguise(Player& p) {
    auto& s   = st();
    auto  uid = uidOf(p);
    auto  feet = p.getFeetPos();
    Vec2  rot{0, p.getRotation().y};
    auto* box = actorById(s.disguises.contains(uid) ? s.disguises[uid] : 0);
    if (box && box->getDimensionId() != p.getDimensionId()) {
        box->remove();
        box = nullptr;
    }
    if (!box) {
        auto level = ll::service::getLevel();
        int  style = std::min(3, static_cast<int>(random01() * 4));
        auto owner = level->getActorFactory().createSpawnedActor(
            ActorDefinitionIdentifier("levitate", "cardboard_disguise", "levitate:style_" + std::to_string(style)), nullptr, feet, rot
        );
        if (!owner) return;
        box = level->addEntity(p.getDimensionBlockSource(), std::move(owner));
        if (!box) return;
        box->addTag(kDisguiseTag);
        s.disguises[uid] = uidOf(*box);
    }
    box->moveTo(feet, rot);
}

void removeDisguise(std::int64_t uid) {
    auto& s = st();
    if (auto it = s.disguises.find(uid); it != s.disguises.end()) {
        if (auto* box = actorById(it->second)) box->remove();
        s.disguises.erase(it);
    }
}

/// Box entities nobody wears any more (a reload, a crash).
void sweepDisguises() {
    auto level = ll::service::getLevel();
    if (!level) return;
    std::set<std::int64_t> live;
    for (auto const& [p, e] : st().disguises) live.insert(e);
    std::vector<Actor*> orphans;
    for (auto* a : level->getRuntimeActorList())
        if (a && a->getTypeName() == kDisguise && !live.contains(uidOf(*a))) orphans.push_back(a);
    for (auto* a : orphans) a->remove();
}

/// CardboardArmorHandler: who hides, invisibility, the advancement, mobs losing their target.
void tickHiding() {
    auto level = ll::service::getLevel();
    if (!level) return;
    auto& s = st();
    ++s.tick;
    std::set<std::int64_t> now;
    level->forEachPlayer([&](Player& p) {
        if (p.isClientSide()) return true;
        auto uid  = uidOf(p);
        bool hide = p.isAlive() && hidingAsBox(p);
        bool first = s.seen.insert(uid).second;
        if (hide) {
            now.insert(uid);
            if (!p.isInvisible()) p.setInvisible(true);
            placeDisguise(p);
            if (!s.hiding.contains(uid)) {
                // Create awards it when the player's hitbox shrinks to the box (EntityEvent.Size)
                advancements::award(p, "cardboard_armor");
                log().info("equipment: {} hides as a cardboard box", p.getRealName());
            }
        } else if (first) {
            // a player who left (or the world closed) while hiding keeps the saved invisible flag:
            // back to what the effects say the first time we see them not hiding
            if (p.isInvisible()) p.updateInvisibilityStatus();
        } else if (s.hiding.contains(uid)) {
            p.updateInvisibilityStatus(); // back to what the effects say
            removeDisguise(uid);
            log().info("equipment: {} stops hiding", p.getRealName());
        }
        return true;
    });
    // players who left while hiding
    for (auto it = s.disguises.begin(); it != s.disguises.end();) {
        if (now.contains(it->first)) {
            ++it;
            continue;
        }
        if (auto* box = actorById(it->second)) box->remove();
        s.seen.erase(it->first); // checked again when they come back
        it = s.disguises.erase(it);
    }
    if (s.tick % 100 == 0) sweepDisguises();
    s.hiding = std::move(now);
    if (s.hiding.empty()) return;
    // mobsMayLoseTargetWhenItIsWearingCardboard: every 16 ticks of each mob
    for (auto* a : level->getRuntimeActorList()) {
        if (!a || !a->isAlive() || a->isPlayer() || !a->hasType(ActorType::Mob)) continue;
        if ((a->mTickCount + 0) % 16 != 0) continue;
        if (hiddenTarget(a->mTargetId)) static_cast<Mob*>(a)->setTarget(nullptr);
        if (hiddenTarget(a->mLastHurtByMobId)) {
            a->setLastHurtByMob(nullptr);
            a->setLastHurtByPlayer(nullptr);
        }
    }
}

std::vector<ia::HandlerId>& handlerIds() {
    static std::vector<ia::HandlerId> ids;
    return ids;
}

} // namespace

std::int64_t disguiseOf(Player const& player) {
    auto it = st().disguises.find(player.getOrCreateUniqueID().rawID);
    return it == st().disguises.end() ? 0 : it->second;
}

bool cardboardBonk(Player& attacker, Actor& target) {
    if (items::idOf(attacker.getSelectedItem()) != kCardboardSword) return false;
    // LivingEntity targets only, and arthropods take the hit (a real sword hit)
    if (!target.hasType(ActorType::Mob) || target.hasCategory(ActorCategory::Arthropod)) return false;
    auto const& a = attacker.getPosition();
    sound(static_cast<int>(attacker.getDimensionId()), a.x, a.y, a.z, kBonk, .75f, 1.85f);
    // Player#attack's knockback without the damage: 2 + Knockback, +1 when sprinting
    float strength = 2.f + static_cast<float>(EnchantUtils::getEnchantLevel(Enchant::Type::Knockback, attacker.getSelectedItem()));
    if (attacker.isSprinting()) strength += 1.f;
    // LivingEntity#knockback(strength / 2, sin(yaw), -cos(yaw)): pushed along the attacker's look
    double kb  = strength * .5;
    auto   ref = target.getAttribute(SharedAttributes::KNOCKBACK_RESISTANCE());
    if (auto const* inst = static_cast<AttributeInstance const*>(ref.mPtr)) kb *= 1.0 - std::clamp<double>(inst->mCurrentValue, 0, 1);
    if (kb > 0) {
        auto const& look = attacker.getViewVector(1.f);
        double      h    = std::sqrt(double(look.x) * look.x + double(look.z) * look.z);
        double      dx = h > 1e-6 ? look.x / h : 0, dz = h > 1e-6 ? look.z / h : 0;
        auto const& v  = target.getPosDelta();
        double      vy = target.isOnGround() ? std::min(0.4, v.y / 2 + kb) : v.y;
        if (target.getVehicle()) target.stopRiding(true, false, false, false);
        setMotion(target, v.x / 2 + dx * kb, vy, v.z / 2 + dz * kb);
    }
    // MobCategory MISC / CREATURE (not players): slowness X for 3 s, ambient, hidden
    if (!target.isPlayer() && !target.hasCategory(ActorCategory::Monster) && !target.hasCategory(ActorCategory::WaterSpawning)
        && !target.hasCategory(ActorCategory::Ambient))
        addEffect(target, 2, 60, 9);
    auto const& av = attacker.getPosDelta();
    setMotion(attacker, av.x * .6, av.y, av.z * .6);
    attacker.setSprinting(false);
    log().info("equipment: {} bonked {} with a cardboard sword", attacker.getRealName(), target.getTypeName());
    return true;
}

namespace {

void installInteractions() {
    auto& ids = handlerIds();
    // cardboardSwordsCannotHurtYou (priority HIGHEST in Create)
    ids.push_back(ia::onAttack({.item = std::string(kCardboardSword)}, [](ia::EntityContext const& c) {
        try {
            if (cardboardBonk(c.player, c.entity)) return ia::Result::Consume;
        } catch (std::exception const& e) {
            log().error("equipment: cardboard sword failed: {}", e.what());
        }
        return ia::Result::Pass;
    }, ia::priority::Highest));
    // cardboardSwordsMakeNoiseOnClick
    ids.push_back(ia::onStartDestroy({.item = std::string(kCardboardSword)}, [](ia::DestroyContext const& c) {
        sound(c.pos.dim, c.pos.x + .5, c.pos.y + .5, c.pos.z + .5, kBonk, .5f, 1.85f);
        return ia::Result::Pass;
    }));
}

} // namespace

} // namespace lv::feature::equipment

namespace eq = lv::feature::equipment;

// Create's CardboardArmorHandler#playersStealthWhenWearingCardboard (visibility 0): a mob can't choose a
// hiding player as its target, unless the player just hurt it (vanilla's HurtByTargetGoal ignores
// visibility; the 16-tick check makes it forget again).
LL_TYPE_INSTANCE_HOOK(EquipmentCardboardTargetHook, ll::memory::HookPriority::Normal, Mob, &Mob::$setTarget, void, Actor* target) {
    if (target && !eq::st().hiding.empty() && target->isPlayer() && eq::hidingAsBox(*target)
        && mLastHurtByMobId->rawID != target->getOrCreateUniqueID().rawID)
        return;
    origin(target);
}

// CardboardSwordItem#supportsEnchantment / #isBookEnchantable: Knockback only.
LL_STATIC_HOOK(
    EquipmentCardboardEnchantHook,
    ll::memory::HookPriority::Normal,
    &EnchantUtils::canEnchant,
    EnchantResult,
    ItemStackBase const&       item,
    EnchantmentInstance const& enchant,
    bool                       allowNonVanilla
) {
    if (!item.isNull() && item.getTypeName() == eq::kCardboardSword && enchant.mEnchantType != Enchant::Type::Knockback) {
        EnchantResult fail{};
        fail.result = EnchantResultType::Fail;
        return fail;
    }
    return origin(item, enchant, allowNonVanilla);
}

// The enchanting table only offers Knockback for the cardboard sword.
LL_STATIC_HOOK(
    EquipmentCardboardSelectHook,
    ll::memory::HookPriority::Normal,
    &EnchantUtils::selectEnchantments,
    ItemEnchants,
    IRandom&    random,
    Item const* item,
    int         enchantCost,
    int         valueBuff,
    bool        treasure
) {
    auto out = origin(random, item, enchantCost, valueBuff, treasure);
    if (item && item->mFullName->getString() == eq::kCardboardSword) {
        auto& lists = out.mItemEnchants.get();
        for (auto& l : lists) std::erase_if(l, [](EnchantmentInstance const& e) { return e.mEnchantType != Enchant::Type::Knockback; });
    }
    return out;
}

// SmithingMenuMixin: taking trimmed cardboard armour out of the smithing table awards
// cardboard_armor_trim (the craft results of the table's item stack request carry the trim).
LL_TYPE_INSTANCE_HOOK(
    EquipmentCardboardTrimHook,
    ll::memory::HookPriority::Normal,
    ItemStackRequestActionCraftHandler,
    &ItemStackRequestActionCraftHandler::_initCraftResults,
    ItemStackNetResult,
    std::vector<ItemInstance> const& results,
    uchar                            numCrafts
) {
    try {
        Player& player = mPlayer;
        if (!player.isClientSide()) {
            for (auto const& r : results) {
                if (r.isNull() || !eq::isCardboardArmor(r.getTypeName()) || !r.mUserData || !r.mUserData->contains("Trim")) continue;
                if (lv::feature::advancements::award(player, "cardboard_armor_trim"))
                    lv::log().info("equipment: {} trimmed {}", player.getRealName(), r.getTypeName());
                break;
            }
        }
    } catch (std::exception const& e) {
        lv::log().error("equipment: cardboard trim check failed: {}", e.what());
    }
    return origin(results, numCrafts);
}

namespace lv::feature::equipment {
namespace {

class CardboardModule {
public:
    void install() {
        static bool once = false;
        if (!once) {
            once = true;
            game::db::onWorldClosed([] { st() = State{}; });
        }
        installInteractions();
        EquipmentCardboardTargetHook::hook();
        EquipmentCardboardEnchantHook::hook();
        EquipmentCardboardSelectHook::hook();
        EquipmentCardboardTrimHook::hook();
        mTick = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>([](auto&) {
            try {
                tickHiding();
            } catch (std::exception const& e) {
                log().error("equipment: cardboard tick failed: {}", e.what());
            }
        });
    }

    void uninstall() {
        ll::event::EventBus::getInstance().removeListener(mTick);
        EquipmentCardboardTrimHook::unhook();
        EquipmentCardboardSelectHook::unhook();
        EquipmentCardboardEnchantHook::unhook();
        EquipmentCardboardTargetHook::unhook();
        for (auto id : handlerIds()) ia::remove(id);
        handlerIds().clear();
    }

private:
    ll::event::ListenerPtr mTick;
};

} // namespace
} // namespace lv::feature::equipment

LV_REGISTER_MODULE(lv::feature::equipment::CardboardModule);
