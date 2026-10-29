#include "core/equipment/Toolbox.h"

#include <algorithm>
#include <vector>

namespace lv::core::equipment {

namespace {
nlohmann::json stackJson(TStack const& s) {
    if (s.empty()) return nullptr;
    return {{"m", s.match}, {"p", s.payload}, {"c", s.count}, {"x", s.max}};
}
TStack stackFrom(nlohmann::json const& j) {
    if (!j.is_object()) return {};
    TStack s;
    s.match   = j.value("m", std::string{});
    s.payload = j.value("p", std::string{});
    s.count   = j.value("c", 0);
    s.max     = std::max(1, j.value("x", 64));
    return s.empty() ? TStack{} : s;
}
} // namespace

bool ToolboxInv::accepts(int compartment, TStack const& stack, bool limited) const {
    if (compartment < 0 || compartment >= kCompartments || stack.empty()) return false;
    auto const& f = filters[static_cast<std::size_t>(compartment)];
    if (f.empty()) return !limited;
    return f.match == stack.match;
}

TStack ToolboxInv::insert(TStack stack, bool limited, bool simulate) {
    if (stack.empty()) return {};
    auto work = *this;
    // pass 1: merge into stacks of the same item (stackable items only)
    if (stack.max > 1) {
        for (int i = 0; i < kToolboxSlots && stack.count > 0; ++i) {
            auto& s = work.slots[static_cast<std::size_t>(i)];
            if (s.empty() || s.payload != stack.payload) continue;
            if (!work.accepts(i / kStacksPerCompartment, stack, limited)) continue;
            int room = std::min(s.max, stack.max) - s.count;
            if (room <= 0) continue;
            int n = std::min(room, stack.count);
            s.count += n;
            stack.count -= n;
        }
    }
    // pass 2: empty slots in order
    for (int i = 0; i < kToolboxSlots && stack.count > 0; ++i) {
        auto& s = work.slots[static_cast<std::size_t>(i)];
        if (!s.empty()) continue;
        int c = i / kStacksPerCompartment;
        if (!work.accepts(c, stack, limited)) continue;
        int n = std::min(stack.max, stack.count);
        s     = stack.withCount(n);
        stack.count -= n;
        auto& f = work.filters[static_cast<std::size_t>(c)];
        if (f.empty()) f = stack.withCount(1);
    }
    if (!simulate) *this = std::move(work);
    return stack.count > 0 ? stack : TStack{};
}

TStack ToolboxInv::extract(int compartment, int amount, bool simulate) {
    if (compartment < 0 || compartment >= kCompartments || amount <= 0) return {};
    auto   work = *this;
    TStack out;
    for (int k = kStacksPerCompartment - 1; k >= 0 && amount > 0; --k) {
        auto& s = work.slots[static_cast<std::size_t>(compartment * kStacksPerCompartment + k)];
        if (s.empty()) continue;
        if (!out.empty() && s.payload != out.payload) continue;
        int n = std::min(amount, s.count);
        if (out.count == 0) {
            out       = s;
            out.count = 0;
        }
        out.count += n;
        s.count -= n;
        if (s.count <= 0) s = TStack{};
        amount -= n;
        if (out.max <= 1) break; // unstackables: one item at a time
    }
    work.settle(compartment);
    if (!simulate) *this = std::move(work);
    return out.count > 0 ? out : TStack{};
}

int ToolboxInv::count(int compartment) const {
    if (compartment < 0 || compartment >= kCompartments) return 0;
    int n = 0;
    for (int k = 0; k < kStacksPerCompartment; ++k) {
        auto const& s = slots[static_cast<std::size_t>(compartment * kStacksPerCompartment + k)];
        if (!s.empty()) n += s.count;
    }
    return n;
}

TStack const& ToolboxInv::first(int compartment) const {
    static TStack const none{};
    if (compartment < 0 || compartment >= kCompartments) return none;
    for (int k = 0; k < kStacksPerCompartment; ++k) {
        auto const& s = slots[static_cast<std::size_t>(compartment * kStacksPerCompartment + k)];
        if (!s.empty()) return s;
    }
    return none;
}

void ToolboxInv::settle(int compartment) {
    if (compartment < 0 || compartment >= kCompartments) return;
    std::vector<TStack> items;
    for (int k = 0; k < kStacksPerCompartment; ++k) {
        auto& s = slots[static_cast<std::size_t>(compartment * kStacksPerCompartment + k)];
        if (!s.empty()) items.push_back(s);
        s = TStack{};
    }
    // merge partial stacks of the same item, front first
    std::vector<TStack> merged;
    for (auto& s : items) {
        for (auto& m : merged) {
            if (s.count <= 0) break;
            if (m.payload != s.payload || m.max <= 1) continue;
            int n = std::min(m.max - m.count, s.count);
            m.count += n;
            s.count -= n;
        }
        if (s.count > 0) merged.push_back(s);
    }
    for (std::size_t k = 0; k < merged.size() && k < static_cast<std::size_t>(kStacksPerCompartment); ++k)
        slots[static_cast<std::size_t>(compartment * kStacksPerCompartment) + k] = merged[k];
}

bool ToolboxInv::clearFilter(int compartment) {
    if (compartment < 0 || compartment >= kCompartments || count(compartment) > 0) return false;
    auto& f = filters[static_cast<std::size_t>(compartment)];
    if (f.empty()) return false;
    f = TStack{};
    return true;
}

float ToolboxInv::fillLevel() const {
    // Create: ItemHandlerHelper.calcRedstoneFromInventory over the 32 slots
    float sum = 0;
    for (auto const& s : slots)
        if (!s.empty()) sum += static_cast<float>(s.count) / static_cast<float>(std::max(1, s.max));
    return sum / static_cast<float>(kToolboxSlots);
}

nlohmann::json ToolboxInv::toJson() const {
    nlohmann::json j = {{"slots", nlohmann::json::array()}, {"filters", nlohmann::json::array()}};
    for (auto const& s : slots) j["slots"].push_back(stackJson(s));
    for (auto const& f : filters) j["filters"].push_back(stackJson(f));
    return j;
}

ToolboxInv ToolboxInv::fromJson(nlohmann::json const& j) {
    ToolboxInv inv;
    if (!j.is_object()) return inv;
    auto const s = j.value("slots", nlohmann::json::array());
    for (std::size_t i = 0; i < s.size() && i < inv.slots.size(); ++i) inv.slots[i] = stackFrom(s[i]);
    auto const f = j.value("filters", nlohmann::json::array());
    for (std::size_t i = 0; i < f.size() && i < inv.filters.size(); ++i) inv.filters[i] = stackFrom(f[i]);
    return inv;
}

bool inToolboxRange(double px, double py, double pz, int bx, int by, int bz, double range) {
    double dx = px - (bx + 0.5), dy = py - by, dz = pz - (bz + 0.5);
    return dx * dx + dy * dy + dz * dz < range * range;
}

} // namespace lv::core::equipment
