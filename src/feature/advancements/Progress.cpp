// Advancement progress (U45): per-player progress in the world DB, the block owners of Create's
// AdvancementBehaviour, awarding (toast, chat line, log), the tree screen / form and the
// `/create advancements` command.

#include <algorithm>
#include <map>
#include <set>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/form/SimpleForm.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/network/packet/ToastRequestPacket.h"
#include "mc/platform/UUID.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "core/advancements/Advancements.h"
#include "feature/advancements/Advancements.h"
#include "feature/advancements/Internal.h"
#include "feature/common/Screens.h"
#include "feature/net/Net.h"
#include "game/WorldDb.h"
#include "mod/Commands.h"
#include "mod/I18n.h"
#include "mod/Log.h"

namespace lv::feature::advancements {

namespace ca = core::advancements;

namespace {

constexpr char kPlayerKey[] = "lv:advancements:player:";
constexpr char kOwnersKey[] = "lv:advancements:owners";

struct Progress {
    std::set<std::string> done;
    bool                  dirty = false;
};

std::map<std::string, Progress>& progress() {
    static std::map<std::string, Progress> m;
    return m;
}
std::map<GlobalPos, std::string>& owners() {
    static std::map<GlobalPos, std::string> m;
    return m;
}
bool gOwnersLoaded = false;
bool gOwnersDirty  = false;

std::string uuidOf(Player const& p) { return p.getUuid().asString(); }

Progress& progressOf(std::string const& uuid) {
    auto& m  = progress();
    auto  it = m.find(uuid);
    if (it != m.end()) return it->second;
    Progress pr;
    if (game::db::available()) {
        if (auto j = game::db::load(kPlayerKey + uuid); j && j->is_object()) {
            for (auto const& id : j->value("done", nlohmann::json::array()))
                if (id.is_string() && ca::find(id.get<std::string>())) pr.done.insert(id.get<std::string>());
        }
    }
    return m.emplace(uuid, std::move(pr)).first->second;
}

void saveProgress(std::string const& uuid, Progress& pr) {
    if (!game::db::available()) return;
    nlohmann::json done = nlohmann::json::array();
    for (auto const& d : ca::all())
        if (pr.done.contains(std::string(d.id))) done.push_back(d.id);
    game::db::save(kPlayerKey + uuid, {{"done", done}});
    pr.dirty = false;
}

void loadOwners() {
    if (gOwnersLoaded || !game::db::available()) return;
    gOwnersLoaded = true;
    auto j        = game::db::load(kOwnersKey);
    if (!j || !j->is_array()) return;
    for (auto const& e : *j) {
        if (!e.is_array() || e.size() != 5 || !e[4].is_string()) continue;
        owners()[GlobalPos{e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>()}] = e[4].get<std::string>();
    }
}

void saveOwners() {
    if (!gOwnersDirty || !game::db::available()) return;
    nlohmann::json arr = nlohmann::json::array();
    for (auto const& [p, u] : owners()) arr.push_back({p.dim, p.x, p.y, p.z, u});
    game::db::save(kOwnersKey, arr);
    gOwnersDirty = false;
}

Player* playerByUuid(std::string const& uuid) {
    auto level = ll::service::getLevel();
    if (!level || uuid.empty()) return nullptr;
    return level->getPlayer(mce::UUID::fromString(uuid));
}

/// Create's removeAwarded: forget the owner once they have every advancement of the block.
void dropOwnerIfComplete(GlobalPos const& pos) {
    auto it = owners().find(pos);
    if (it == owners().end()) return;
    auto const& list = blockAdvancements(blockIdAt(pos));
    auto&       pr   = progressOf(it->second);
    bool        all  = !list.empty() && std::ranges::all_of(list, [&](std::string_view a) { return pr.done.contains(std::string(a)); });
    if (list.empty() || all) {
        owners().erase(it);
        gOwnersDirty = true;
    }
}

void toastFor(Player& player, ca::Def const& d) {
    if (!ca::toast(d.type)) return;
    if (net::isCompanion(player)) {
        net::send(player, "advancements.toast", {{"id", d.id}});
        return;
    }
    std::string head = ca::goal(d.type) ? trFor(player, "lv.advancements.toast.goal", "Goal Reached!")
                                        : trFor(player, "lv.advancements.toast.task", "Advancement Made!");
    ToastRequestPacket pkt("§e" + head, trFor(player, "advancement.create." + std::string(d.id), std::string(d.id)));
    pkt.sendTo(player);
}

void announce(Player& player, ca::Def const& d) {
    if (!ca::announce(d.type)) return;
    auto level = ll::service::getLevel();
    if (!level) return;
    std::string name = player.getRealName();
    std::string id(d.id);
    level->forEachPlayer([&](Player& p) {
        if (p.isClientSide()) return true;
        std::string title = trFor(p, "advancement.create." + id, id);
        std::string line  = ca::goal(d.type) ? trFor(p, "lv.advancements.chat.goal", "{0} has reached the goal {1}", name, "§a[" + title + "]§r")
                                             : trFor(p, "lv.advancements.chat.task", "{0} has made the advancement {1}", name, "§a[" + title + "]§r");
        p.sendMessage(line);
        return true;
    });
}

// ---- tree screen and form --------------------------------------------------------------------------------------

nlohmann::json treeData(Player& player) {
    nlohmann::json done = nlohmann::json::array();
    for (auto const& id : doneOf(player)) done.push_back(id);
    return {{"done", done}};
}

void openDetail(Player& player, int index);

void openForm(Player& player) {
    auto const& pr    = progressOf(uuidOf(player));
    auto        defs  = ca::all();
    auto        vis   = ca::visibility([&](std::string_view id) { return pr.done.contains(std::string(id)); });
    int         total = static_cast<int>(defs.size());
    ll::form::SimpleForm form;
    form.setTitle(trFor(player, "advancement.create.root", "Welcome to Create"));
    form.setContent(trFor(player, "lv.advancements.form.content", "Create: {0} of {1} advancements made", static_cast<int>(pr.done.size()), total));
    for (std::size_t i = 0; i < defs.size(); ++i) {
        if (!vis[i]) continue;
        auto const& d     = defs[i];
        bool        isDone = pr.done.contains(std::string(d.id));
        std::string title = trFor(player, "advancement.create." + std::string(d.id), std::string(d.id));
        std::string label = (isDone ? "§2" : "§8") + title;
        if (isDone) label += "\n§2" + trFor(player, "lv.advancements.form.done", "Done");
        int idx = static_cast<int>(i);
        form.appendButton(label, [idx](Player& p) { openDetail(p, idx); });
    }
    form.sendTo(player);
}

void openDetail(Player& player, int index) {
    auto        d     = ca::all()[static_cast<std::size_t>(index)];
    std::string id(d.id);
    bool        isDone = has(player, id);
    ll::form::SimpleForm form;
    form.setTitle(trFor(player, "advancement.create." + id, id));
    std::string content = "§6" + trFor(player, "advancement.create." + id + ".desc", "") + "§r";
    if (!d.parent.empty())
        content += "\n\n" + trFor(player, "lv.advancements.form.parent", "After: {0}", trFor(player, "advancement.create." + std::string(d.parent), std::string(d.parent)));
    if (isDone) content += "\n\n§2" + trFor(player, "lv.advancements.form.done", "Done");
    form.setContent(content);
    form.appendButton(trFor(player, "lv.advancements.form.back", "Back"), [](Player& p) { openForm(p); });
    form.sendTo(player);
}

} // namespace

// ---- internal (Internal.h) -----------------------------------------------------------------------------------------

std::vector<std::string_view> const& blockAdvancements(std::string_view blockId) {
    static std::map<std::string_view, std::vector<std::string_view>> const table = {
        // Create's registerAwardables per block entity, for the blocks Levitate has
        {"levitate:portable_storage_interface", {"portable_storage_interface"}},
        {"levitate:clockwork_bearing", {"clockwork_bearing", "contraption_actors"}},
        {"levitate:mechanical_bearing", {"contraption_actors"}},
        {"levitate:windmill_bearing", {"contraption_actors", "windmill", "windmill_maxed"}},
        {"levitate:elevator_pulley", {"contraption_actors"}},
        {"levitate:gantry_carriage", {"contraption_actors"}},
        {"levitate:cart_assembler", {"contraption_actors"}},
        {"levitate:mechanical_piston", {"contraption_actors"}},
        {"levitate:sticky_mechanical_piston", {"contraption_actors"}},
        {"levitate:rope_pulley", {"contraption_actors", "pulley_maxed"}},
        {"levitate:steam_whistle", {"steam_whistle"}},
        {"levitate:copper_backtank", {"backtank"}},
        {"levitate:netherite_backtank", {"backtank"}},
        {"levitate:item_drain", {"drain", "chained_drain"}},
        {"levitate:hose_pulley", {"water_supply", "cross_streams", "honey_drain", "hose_pulley", "hose_pulley_lava"}},
        {"levitate:fluid_pipe", {"water_supply", "cross_streams", "honey_drain"}},
        {"levitate:glass_fluid_pipe", {"water_supply", "cross_streams", "honey_drain"}},
        {"levitate:encased_fluid_pipe", {"water_supply", "cross_streams", "honey_drain"}},
        {"levitate:smart_fluid_pipe", {"water_supply", "cross_streams", "honey_drain"}},
        {"levitate:fluid_valve", {"water_supply", "cross_streams", "honey_drain"}},
        {"levitate:mechanical_pump", {"water_supply", "cross_streams", "honey_drain", "mechanical_pump_0"}},
        {"levitate:spout", {"spout", "foods"}},
        {"levitate:fluid_tank", {"steam_engine_maxed", "pipe_organ"}},
        {"levitate:mechanical_crafter", {"mechanical_crafter", "crafter_lazy_000"}},
        {"levitate:crushing_wheel", {"crushing_wheel", "crusher_maxed_0000"}},
        {"levitate:deployer",
         {"train_casing_00", "andesite_casing", "brass_casing", "copper_casing", "fist_bump", "deployer", "self_deploying"}},
        {"levitate:encased_fan", {"encased_fan", "fan_processing"}},
        {"levitate:stressometer", {"stressometer", "stressometer_maxed"}},
        {"levitate:mechanical_arm", {"arm_blaze_burner", "arm_many_targets", "mechanical_arm", "musical_arm"}},
        {"levitate:millstone", {"millstone"}},
        {"levitate:mechanical_mixer", {"mechanical_mixer"}},
        {"levitate:mechanical_press", {"mechanical_press", "compacting", "track_crafting_factory"}},
        {"levitate:mechanical_saw", {"saw_processing"}},
        {"levitate:rotation_speed_controller", {"speed_controller"}},
        {"levitate:steam_engine", {"steam_engine"}},
        {"levitate:water_wheel", {"lava_wheel_00000", "water_wheel"}},
        {"levitate:large_water_wheel", {"lava_wheel_00000", "water_wheel"}},
        {"levitate:chute", {"chute"}},
        {"levitate:smart_chute", {"chute"}},
        {"levitate:andesite_funnel", {"funnel"}},
        {"levitate:brass_funnel", {"funnel"}},
        {"levitate:display_link", {"display_link", "display_board_0"}},
        {"levitate:track_station", {"contraption_actors", "train", "long_train", "conductor"}},
    };
    static std::vector<std::string_view> const none;
    auto it = table.find(blockId);
    return it == table.end() ? none : it->second;
}

void resetState() {
    progress().clear();
    owners().clear();
    gOwnersLoaded = false;
    gOwnersDirty  = false;
}

void flush() {
    for (auto& [uuid, pr] : progress())
        if (pr.dirty) saveProgress(uuid, pr);
    saveOwners();
}

std::vector<std::pair<GlobalPos, std::string>> ownedBlocks() {
    loadOwners();
    return {owners().begin(), owners().end()};
}

// ---- contract (Advancements.h) ---------------------------------------------------------------------------------------

bool award(Player& player, std::string_view id) {
    auto const* d = ca::find(id);
    if (!d || player.isClientSide()) return false;
    auto  uuid = uuidOf(player);
    auto& pr   = progressOf(uuid);
    if (!pr.done.insert(std::string(id)).second) return false;
    saveProgress(uuid, pr);
    toastFor(player, *d);
    announce(player, *d);
    log().info("advancements: {} made {}", player.getRealName(), id);
    return true;
}

bool has(Player const& player, std::string_view id) { return progressOf(uuidOf(player)).done.contains(std::string(id)); }

std::vector<std::string> doneOf(Player const& player) {
    auto const&              pr = progressOf(uuidOf(player));
    std::vector<std::string> out;
    for (auto const& d : ca::all())
        if (pr.done.contains(std::string(d.id))) out.emplace_back(d.id);
    return out;
}

bool revoke(Player& player, std::string_view id) {
    auto  uuid = uuidOf(player);
    auto& pr   = progressOf(uuid);
    if (pr.done.erase(std::string(id)) == 0) return false;
    saveProgress(uuid, pr);
    log().info("advancements: {} lost {}", player.getRealName(), id);
    return true;
}

void setOwner(GlobalPos const& pos, Player const& player) {
    loadOwners();
    auto const& list = blockAdvancements(blockIdAt(pos));
    if (list.empty()) return;
    auto const& pr = progressOf(uuidOf(player));
    if (std::ranges::all_of(list, [&](std::string_view a) { return pr.done.contains(std::string(a)); })) return;
    owners()[pos] = uuidOf(player);
    gOwnersDirty  = true;
}

std::string ownerOf(GlobalPos const& pos) {
    loadOwners();
    auto it = owners().find(pos);
    return it == owners().end() ? std::string() : it->second;
}

namespace {
Player* ownerPlayer(GlobalPos const& pos, std::string_view id) {
    loadOwners();
    auto it = owners().find(pos);
    if (it == owners().end()) return nullptr;
    // the block changed since (Create: a new block entity has no owner)
    auto const& list = blockAdvancements(blockIdAt(pos));
    if (std::ranges::find(list, id) == list.end()) {
        if (list.empty()) {
            owners().erase(it);
            gOwnersDirty = true;
        }
        return nullptr;
    }
    return playerByUuid(it->second);
}
} // namespace

void awardOwner(GlobalPos const& pos, std::string_view id) {
    if (auto* p = ownerPlayer(pos, id)) {
        award(*p, id);
        dropOwnerIfComplete(pos);
    }
}

void awardOwnerIfNear(GlobalPos const& pos, std::string_view id, int maxDistance) {
    auto* p = ownerPlayer(pos, id);
    if (!p || static_cast<int>(p->getDimensionId()) != pos.dim) return;
    auto   at = p->getPosition();
    double dx = at.x - (pos.x + .5), dy = at.y - (pos.y + .5), dz = at.z - (pos.z + .5);
    if (dx * dx + dy * dy + dz * dz > double(maxDistance) * maxDistance) return;
    award(*p, id);
    dropOwnerIfComplete(pos);
}

void openTree(Player& player) {
    if (!common::screens::open(player, "advancements.tree")) openForm(player);
    log().info("advancements: {} opened the advancement tree", player.getRealName());
}

void installProgress() {
    static bool done = false;
    if (done) return;
    done = true;
    common::screens::registerType({
        .id     = "advancements.tree",
        .data   = [](Player& p, nlohmann::json const&) -> std::optional<nlohmann::json> { return treeData(p); },
        .action = [](Player&, nlohmann::json const&, nlohmann::json const&) {},
        .form   = [](Player& p, nlohmann::json const&) { openForm(p); },
    });
}

} // namespace lv::feature::advancements

// ---- command ---------------------------------------------------------------------------------------------------------

namespace lv::feature::advancements {
struct AdvancementParams {
    std::string id;
};
} // namespace lv::feature::advancements

namespace {
using lv::feature::advancements::AdvancementParams;

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    using namespace lv;
    namespace adv = lv::feature::advancements;
    cmd.overload().text("advancements").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = commands::playerOf(origin);
        if (!player) return output.error("Only players have advancements.");
        adv::openTree(*player);
        output.success("");
    });
    for (bool grant : {true, false}) {
        cmd.overload<AdvancementParams>().text("advancements").text(grant ? "grant" : "revoke").required("id").execute(
            [grant](CommandOrigin const& origin, CommandOutput& output, AdvancementParams const& params) {
                auto* player = commands::playerOf(origin);
                if (!player) return output.error("Only players have advancements.");
                if (!commands::isAdmin(origin)) return output.error("You need operator permission.");
                if (!lv::core::advancements::find(params.id)) return output.error("Unknown advancement: " + params.id);
                bool changed = grant ? adv::award(*player, params.id) : adv::revoke(*player, params.id);
                output.success(std::string(changed ? "Done: " : "No change: ") + params.id);
            }
        );
    }
});
} // namespace
