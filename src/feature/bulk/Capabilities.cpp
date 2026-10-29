// Bulk unit: kinetic specs, item capability handlers and the airflow provider.

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/dimension/Dimension.h"

#include "feature/bulk/Bulk.h"
#include "feature/common/BulkExt.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"

namespace lv::feature::bulk {

namespace k = core::kinetics;
using kinetics::KineticSpec;

namespace {

/// Create's MillstoneBlock: a small cog (meshes with cogwheels around it) with a shaft below.
KineticSpec millstoneSpec() {
    KineticSpec s;
    s.rotationAxis    = [](k::BlockState const&) -> std::optional<k::Axis> { return k::Axis::Y; };
    s.hasShaftTowards = [](k::BlockState const&, k::Face f) { return f == k::Face::Down; };
    s.cog             = [](k::BlockState const&) { return k::CogType::Small; };
    s.impact          = 4;
    s.rotorState      = "levitate:rotor";
    return kinetics::specs::withModel(std::move(s), "geometry.levitate.millstone", "levitate_millstone");
}

/// Create's CrushingWheelBlock: a pillar with shaft ends on both faces of its axis.
KineticSpec wheelSpec() {
    auto s   = kinetics::specs::shaft("levitate:axis");
    s.impact = 8;
    return kinetics::specs::withModel(std::move(s), "geometry.levitate.crushing_wheel", "levitate_crushing_wheel");
}

/// Create's EncasedFanBlock: the shaft enters at the back; the shaft half turns with the network,
/// the propeller five times faster (EncasedFanRenderer).
KineticSpec fanSpec() {
    KineticSpec s;
    s.states          = {"levitate:facing"};
    s.rotationAxis    = kinetics::specs::axisFromFacing("levitate:facing");
    s.hasShaftTowards = [](k::BlockState const& st, k::Face f) {
        return f == k::opposite(static_cast<k::Face>(std::clamp(st.get("levitate:facing"), 0, 5)));
    };
    s.impact     = 2;
    s.rotorState = "levitate:rotor";
    s.parts      = [](k::RenderView const& v, std::vector<k::RotorPart>& out) {
        auto f   = static_cast<k::Face>(std::clamp(v.state->get("levitate:facing"), 0, 5));
        bool pos = k::isPositive(f);
        out.push_back(k::RotorPart{pos ? "shaft_up" : "shaft_down", k::axisOf(f), 1.f, 0.f, false});
        out.push_back(k::RotorPart{pos ? "blades_up" : "blades_down", k::axisOf(f), 5.f, 0.f, false});
    };
    return kinetics::specs::withModel(std::move(s), "geometry.levitate.encased_fan", "levitate_encased_fan");
}

} // namespace

LV_REGISTER_KINETIC_BLOCK("levitate:millstone", millstoneSpec());
LV_REGISTER_KINETIC_BLOCK("levitate:crushing_wheel", wheelSpec());
LV_REGISTER_KINETIC_BLOCK("levitate:encased_fan", fanSpec());

void registerCapabilities() {
    namespace ci = common::items;
    auto posOf   = [](common::Access const& a) { return toGlobal(static_cast<int>(a.region.getDimensionId()), a.pos); };

    ci::registerBlock(
        std::string(kMillstone),
        ci::Handler{
            [](common::Access const&) { return millstone_cap::slots(); },
            [posOf](common::Access const& a, int slot) { return millstone_cap::getSlot(posOf(a), slot); },
            [posOf](common::Access const& a, ItemStack const& s, bool simulate) { return millstone_cap::insert(posOf(a), s, simulate); },
            [posOf](common::Access const& a, ci::ItemPredicate const& f, int amount, bool simulate) {
                return millstone_cap::extract(posOf(a), f, amount, simulate);
            },
        }
    );
    ci::registerBlock(
        std::string(kController),
        ci::Handler{
            [](common::Access const&) { return controller_cap::slots(); },
            [posOf](common::Access const& a, int slot) { return controller_cap::getSlot(posOf(a), slot); },
            [posOf](common::Access const& a, ItemStack const& s, bool simulate) { return controller_cap::insert(posOf(a), s, simulate); },
            [](common::Access const&, ci::ItemPredicate const&, int, bool) { return common::items::empty(); },
        }
    );

    namespace af = common::airflow;
    af::setProvider(af::Provider{
        [](GlobalPos const& pos) -> std::optional<af::Source> {
            auto* region = regionOf(pos.dim);
            if (!region || idAt(*region, toBlockPos(pos)) != kFan) return std::nullopt;
            af::Source s;
            s.pos    = pos;
            s.facing = stateOf(region->getBlock(toBlockPos(pos)), "levitate:facing");
            s.speed  = kinetics::speedAt(pos);
            if (s.speed != 0) s.pushing = cb::fanPushing(faceOf(s.facing), s.speed);
            if (auto f = fanAt(pos)) s.maxDistance = f->current.maxDistance;
            return s;
        },
        [](BlockSource& region, BlockPos const& p) { return std::string(cb::fanTypeName(catalystAt(region, p))); },
        [](ItemStack const& s, std::string const& type) { return canFanProcess(s, cb::fanTypeByName(type)); },
        [](ItemStack const& s, std::string const& type) { return fanProcess(s, cb::fanTypeByName(type)); },
        [](int count) { return cb::fanProcessingTime(count, config().fanProcessingTime); },
    });
}

} // namespace lv::feature::bulk
