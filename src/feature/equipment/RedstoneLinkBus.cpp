#include "feature/equipment/RedstoneLinkBus.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

namespace lv::feature::equipment::linkbus {

namespace {
struct State {
    std::map<TransmitterId, Transmission>             active;
    std::map<int, ChangeListener>                     listeners;
    std::map<std::string, FrequencySource>            sources;
    int                                               nextListener = 1;
};
State& state() {
    static State s;
    return s;
}
void changed(std::string const& frequency) {
    auto listeners = state().listeners; // copy: listeners may unregister
    for (auto& [id, fn] : listeners) fn(frequency);
}
} // namespace

void transmit(TransmitterId id, std::string const& frequency, core::GlobalPos const& pos, int power) {
    auto& a  = state().active;
    auto  it = a.find(id);
    bool  fresh = it == a.end() || it->second.frequency != frequency || it->second.power != power || it->second.pos != pos;
    std::string old = it != a.end() ? it->second.frequency : std::string{};
    a[id] = Transmission{id, frequency, pos, std::clamp(power, 0, 15)};
    if (!fresh) return;
    if (!old.empty() && old != frequency) changed(old);
    changed(frequency);
}

void release(TransmitterId id) {
    auto& a  = state().active;
    auto  it = a.find(id);
    if (it == a.end()) return;
    auto f = it->second.frequency;
    a.erase(it);
    changed(f);
}

void releaseOwner(std::int64_t owner) {
    std::vector<TransmitterId> ids;
    for (auto const& [id, t] : state().active)
        if (id.owner == owner) ids.push_back(id);
    for (auto const& id : ids) release(id);
}

std::vector<Transmission> transmissions() {
    std::vector<Transmission> out;
    for (auto const& [id, t] : state().active) out.push_back(t);
    return out;
}

int powerAt(std::string const& frequency, core::GlobalPos const& at, int range) {
    int best = 0;
    for (auto const& [id, t] : state().active) {
        if (t.frequency != frequency || t.pos.dim != at.dim) continue;
        if (std::abs(t.pos.x - at.x) > range || std::abs(t.pos.y - at.y) > range || std::abs(t.pos.z - at.z) > range) continue;
        best = std::max(best, t.power);
    }
    return best;
}

int onChanged(ChangeListener fn) {
    int id                  = state().nextListener++;
    state().listeners[id] = std::move(fn);
    return id;
}

void removeListener(int id) { state().listeners.erase(id); }

void registerFrequencySource(std::string const& blockId, FrequencySource fn) { state().sources[blockId] = std::move(fn); }

std::optional<std::pair<std::string, std::string>> frequencyAt(std::string const& blockId, core::GlobalPos const& pos) {
    auto it = state().sources.find(blockId);
    if (it == state().sources.end() || !it->second) return std::nullopt;
    return it->second(pos);
}

void reset() {
    std::set<std::string> freqs;
    for (auto const& [id, t] : state().active) freqs.insert(t.frequency);
    state().active.clear();
    for (auto const& f : freqs) changed(f);
}

} // namespace lv::feature::equipment::linkbus
