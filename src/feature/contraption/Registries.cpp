#include "feature/contraption/Registries.h"

#include <algorithm>
#include <unordered_map>

namespace lv::feature::contraption {

namespace {
template <class T>
std::unordered_map<std::string, T>& table() {
    static std::unordered_map<std::string, T> t;
    return t;
}
std::vector<DataCarrier>& carrierList() {
    static std::vector<DataCarrier> v;
    return v;
}
template <class T>
T const* lookup(std::string const& id) {
    auto& t  = table<T>();
    auto  it = t.find(id);
    return it == t.end() ? nullptr : &it->second;
}
} // namespace

void registerDataCarrier(DataCarrier carrier) {
    auto& v = carrierList();
    for (auto& c : v) {
        if (c.name == carrier.name) {
            c = std::move(carrier);
            return;
        }
    }
    v.push_back(std::move(carrier));
    // the generic BlockData carrier (CarriedData.cpp) runs last: units' own carriers capture what
    // they claim first (their mounted storage reads it under their name), it takes the rest
    std::stable_partition(v.begin(), v.end(), [](DataCarrier const& c) { return c.name != "common:blockdata"; });
}
void registerRotationHandler(std::string blockId, RotationHandler handler) {
    table<RotationHandler>()[std::move(blockId)] = std::move(handler);
}
void registerMovementRules(std::string blockId, MovementRules rules) {
    table<MovementRules>()[std::move(blockId)] = std::move(rules);
}
void registerActor(std::string blockId, MovementBehaviour behaviour) {
    table<MovementBehaviour>()[std::move(blockId)] = std::move(behaviour);
}
void registerSeat(std::string blockId, double sittingHeight) { table<double>()[std::move(blockId)] = sittingHeight; }
void registerFluidStorage(std::string blockId, FluidStorageAdapter adapter) {
    table<FluidStorageAdapter>()[std::move(blockId)] = std::move(adapter);
}

namespace detail {
MovementRules const*       movementRules(std::string const& id) { return lookup<MovementRules>(id); }
RotationHandler const*     rotationHandler(std::string const& id) { return lookup<RotationHandler>(id); }
MovementBehaviour const*   actorFor(std::string const& id) { return lookup<MovementBehaviour>(id); }
FluidStorageAdapter const* fluidAdapter(std::string const& id) { return lookup<FluidStorageAdapter>(id); }
std::optional<double>      seatHeight(std::string const& id) {
    auto const* h = lookup<double>(id);
    return h ? std::optional<double>(*h) : std::nullopt;
}
std::vector<DataCarrier> const& carriers() { return carrierList(); }
} // namespace detail

} // namespace lv::feature::contraption
