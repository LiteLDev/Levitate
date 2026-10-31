#include "core/schematics/SafeData.h"

#include <array>
#include <string>

namespace lv::core::schematics {

namespace {

/// Create 6.0.10's blocks tagged `create:safe_nbt` (AllBlocks) that Levitate has.
constexpr std::array<std::string_view, 24> kSafeNbtBlocks{
    "levitate:creative_motor",
    "levitate:creative_fluid_tank",
    "levitate:mechanical_piston",
    "levitate:sticky_mechanical_piston",
    "levitate:windmill_bearing",
    "levitate:mechanical_bearing",
    "levitate:clockwork_bearing",
    "levitate:rope_pulley",
    "levitate:cart_assembler",
    "levitate:minecart_anchor",
    "levitate:linear_chassis",
    "levitate:secondary_linear_chassis",
    "levitate:radial_chassis",
    "levitate:sequenced_gearshift",
    "levitate:rotation_speed_controller",
    "levitate:andesite_funnel",
    "levitate:brass_funnel",
    "levitate:redstone_link",
    "levitate:analog_lever",
    "levitate:placard",
    "levitate:pulse_repeater",
    "levitate:pulse_extender",
    "levitate:pulse_timer",
    "levitate:clipboard",
};

/// Top-level BlockData keys that are never printed: the kinetic network's record (rebuilt when the
/// block is placed) and the advancement owner (the printed block belongs to whoever prints it).
constexpr std::array<std::string_view, 2> kRuntimeKeys{"kinetics", "advancements"};

} // namespace

bool isSafeNbtBlock(std::string_view blockId) {
    for (auto id : kSafeNbtBlocks)
        if (id == blockId) return true;
    return false;
}

std::optional<nbt::Tag> safeBlockEntity(nbt::Tag const& tag) {
    auto const* c = tag.asCompound();
    if (!c) return std::nullopt;
    auto const* id = c->find("id");
    if (!id || !id->asString()) return std::nullopt;
    auto const& type = *id->asString();
    // #minecraft:all_signs: only the text (signs on Bedrock carry no click events, so Create's
    // sign processor has nothing to remove)
    if (type == "Sign" || type == "HangingSign") {
        nbt::Compound safe;
        for (auto const* key : {"id", "FrontText", "BackText", "Text", "IsWaxed"})
            if (auto const* v = c->find(key)) safe.set(key, *v);
        return nbt::Tag(std::move(safe));
    }
    // #minecraft:banners: the whole block entity
    if (type == "Banner") return tag;
    return std::nullopt;
}

nlohmann::json safeBlockData(std::string_view blockId, nlohmann::json const& doc) {
    if (!doc.is_object()) return nullptr;
    nlohmann::json out = nlohmann::json::object();
    if (isSafeNbtBlock(blockId)) {
        // saveWithFullMetadata
        out = doc;
        for (auto key : kRuntimeKeys) out.erase(std::string(key));
    } else {
        // PartialSafeNBT.writeSafe: FilteringBehaviour (every block's filter slots) ...
        if (auto it = doc.find("filter"); it != doc.end()) out["filter"] = *it;
        // ... and ThresholdSwitchBlockEntity's thresholds, inversion and unit
        if (blockId == "levitate:stockpile_switch") {
            if (auto it = doc.find("redstone"); it != doc.end() && it->is_object()) {
                nlohmann::json r = nlohmann::json::object();
                for (auto const* key : {"onAbove", "offBelow", "inverted", "inStacks"})
                    if (auto v = it->find(key); v != it->end()) r[key] = *v;
                if (!r.empty()) out["redstone"] = std::move(r);
            }
        }
        // CopycatBlockEntity.writeSafe: the material (U10 keeps it in data["building"])
        if (blockId.starts_with("levitate:copycat_")) {
            if (auto it = doc.find("building"); it != doc.end()) out["building"] = *it;
        }
        // TrackBlockEntity.writeSafe: the curves leaving the track (U5's data["tracks"])
        if (blockId == "levitate:track") {
            if (auto it = doc.find("tracks"); it != doc.end()) out["tracks"] = *it;
        }
    }
    return out.empty() ? nlohmann::json(nullptr) : out;
}

bool onlyOpCanSetNbt(std::string_view entityId) {
    // Java: MinecartCommandBlock and FallingBlockEntity; Bedrock's NPC runs commands too
    return entityId == "minecraft:command_block_minecart" || entityId == "minecraft:falling_block" || entityId == "minecraft:npc";
}

std::string entityIdOf(nbt::Tag const& tag) {
    auto const* c = tag.asCompound();
    if (!c) return {};
    auto const* id = c->find("identifier");
    return id && id->asString() ? *id->asString() : std::string();
}

} // namespace lv::core::schematics
