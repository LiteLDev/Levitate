// Create's PotatoCannonItem#appendHoverText in the client mod (U59): the potato cannon's tooltip
// names the ammo the local player would fire next and its attack damage (x1 + 0.2 per Power
// level), reload ticks and knockback (+0.5 per Punch level).

#ifdef LV_CLIENT

#include <cmath>
#include <format>
#include <string>

#include "ll/api/memory/Hook.h"

#include "mc/client/player/LocalPlayer.h"
#include "mc/safety/RedactableString.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemStackBase.h"
#include "mc/world/item/enchanting/Enchant.h"
#include "mc/world/item/enchanting/EnchantUtils.h"
#include "mc/world/level/Level.h"

#include "client/Client.h"
#include "core/equipment/Gear.h"
#include "feature/common/Items.h"
#include "mod/Module.h"

namespace lv::client::equipment {

namespace {

namespace ce    = lv::core::equipment;
namespace items = lv::feature::common::items;

constexpr char kGray[]      = "\xc2\xa7" "7";
constexpr char kGreen[]     = "\xc2\xa7" "a";
constexpr char kDarkGreen[] = "\xc2\xa7" "2";

/// The ammo the player fires next (vanilla getProjectile order: off hand, main hand, inventory).
ItemStack const* nextAmmo(LocalPlayer& p) {
    auto check = [](ItemStack const& s) { return !items::isEmpty(s) && ce::potatoTypeFor(items::idOf(s)) != nullptr; };
    if (check(p.getOffhandSlot())) return &p.getOffhandSlot();
    if (check(p.getSelectedItem())) return &p.getSelectedItem();
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i)
        if (check(inv.getItem(i))) return &inv.getItem(i);
    return nullptr;
}

std::string number(float v) {
    if (v == std::floor(v)) return std::to_string(static_cast<int>(v));
    return std::format("{}", v);
}

void appendAmmo(std::string& text, ItemStackBase const& cannon) {
    auto* lp = currentPlayer();
    if (!lp) return;
    auto const* ammo = nextAmmo(*lp);
    if (!ammo) return;
    auto const* type = ce::potatoTypeFor(items::idOf(*ammo));
    if (!type) return;
    int   power     = EnchantUtils::getEnchantLevel(Enchant::Type::Power, cannon);
    int   punch     = EnchantUtils::getEnchantLevel(Enchant::Type::Punch, cannon);
    float damageMul = 1 + power * .2f;
    float extraKb   = punch * .5f;
    std::string damage    = (damageMul > 1 ? kGreen : kDarkGreen) + number(type->damage * damageMul) + kDarkGreen;
    std::string reload    = std::string(kDarkGreen) + std::to_string(type->reload);
    std::string knockback = (extraKb > 0 ? kGreen : kDarkGreen) + number(type->knockback + extraKb) + kDarkGreen;
    text += "\n";
    text += "\n" + std::string(kGray) + items::displayName(*ammo) + ":";
    text += "\n " + std::string(kDarkGreen) + ctr("create.potato_cannon.ammo.attack_damage", "{0} Attack Damage", damage);
    text += "\n " + std::string(kDarkGreen) + ctr("create.potato_cannon.ammo.reload_ticks", "{0} Reload Ticks", reload);
    text += "\n " + std::string(kDarkGreen) + ctr("create.potato_cannon.ammo.knockback", "{0} Knockback", knockback);
}

} // namespace

} // namespace lv::client::equipment

LL_TYPE_INSTANCE_HOOK(
    PotatoCannonHovertextHook,
    ll::memory::HookPriority::Normal,
    ItemStackBase,
    &ItemStackBase::getFormattedHovertext,
    Bedrock::Safety::RedactableString,
    Level& level,
    bool   showCategory
) {
    auto r = origin(level, showCategory);
    if (isNull() || getTypeName() != "levitate:potato_cannon") return r;
    try {
        lv::client::equipment::appendAmmo(r.mUnredactedString, *this);
        if (r.mRedactedString) lv::client::equipment::appendAmmo(*r.mRedactedString, *this);
    } catch (...) {
    }
    return r;
}

namespace lv::client::equipment {
namespace {
struct PotatoTooltipModule {
    void install() { PotatoCannonHovertextHook::hook(); }
    void uninstall() { PotatoCannonHovertextHook::unhook(); }
};
} // namespace
} // namespace lv::client::equipment

LV_REGISTER_MODULE(lv::client::equipment::PotatoTooltipModule);

#endif // LV_CLIENT
