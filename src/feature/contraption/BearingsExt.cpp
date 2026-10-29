// U21 extension of the contraption engine (see BearingsExt.h).

#include "feature/contraption/BearingsExt.h"

#include <string>

#include "feature/contraption/Internal.h"
#include "game/WorldDb.h"
#include "mod/Log.h"

namespace lv::feature::contraption::ext {

namespace {
// the engine's persistence keys (Engine.cpp)
constexpr char kDbPrefix[] = "lv:contraption:c:";
constexpr char kDbNextId[] = "lv:contraption:next_id";

nlohmann::json vec(cc::Vec3d const& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
} // namespace

std::optional<nlohmann::json> snapshot(Id id) {
    if (!game::db::available() || !find(id)) return std::nullopt;
    detail::saveAll();
    return game::db::load(kDbPrefix + std::to_string(id));
}

std::optional<Id> restore(nlohmann::json record, core::GlobalPos const& anchor, std::string const& kind, cc::Transform const& t) {
    if (!game::db::available() || !record.is_object()) return std::nullopt;
    detail::saveAll(); // writes the engine's next id
    Id next = 1;
    if (auto n = game::db::load(kDbNextId)) next = std::max<Id>(1, n->value("next", Id{1}));
    while (find(next)) ++next;
    record["id"]     = next;
    record["dim"]    = anchor.dim;
    record["kind"]   = kind;
    record["anchor"] = nlohmann::json::array({anchor.x, anchor.y, anchor.z});
    record["transform"] = {
        {"origin", vec(t.origin)},
        {"offset", vec(t.offset)},
        {"axis",   int(t.axis)  },
        {"angle",  t.angle      },
        {"pivot",  vec(t.pivot) }
    };
    record["motion"] = {
        {"velocity",     nlohmann::json::array({0, 0, 0})},
        {"angular",      0.0                             },
        {"follow",       0                               },
        {"followOffset", nlohmann::json::array({0, 0, 0})},
        {"ticks",        -1                              }
    };
    record["seated"] = nlohmann::json::array();
    if (record.contains("blocks") && record["blocks"].is_array()) {
        for (auto& b : record["blocks"]) b["e"] = 0; // new display entities
    }
    game::db::save(kDbPrefix + std::to_string(next), record);
    game::db::save(kDbNextId, nlohmann::json{{"next", next + 1}});
    detail::reloadAll();
    if (!find(next)) {
        log().error("contraption #{} could not be restored", next);
        return std::nullopt;
    }
    return next;
}

} // namespace lv::feature::contraption::ext
