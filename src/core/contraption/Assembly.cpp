#include "core/contraption/Assembly.h"

#include <deque>

namespace lv::core::contraption {

DirMask glueableFaces(ChassisKind kind, Axis axis) {
    switch (kind) {
    case ChassisKind::Linear:
    case ChassisKind::SecondaryLinear:
        return bit(dirOf(axis, true)) | bit(dirOf(axis, false));
    case ChassisKind::Radial: {
        DirMask m = 0;
        for (auto d : kAllDirs)
            if (axisOf(d) != axis) m |= bit(d);
        return m;
    }
    default:
        return 0;
    }
}

bool canStick(BlockInfo const& a, BlockInfo const& b, Dir d) {
    if (b.brittle || a.brittle) return false;
    if (a.sticky != StickyKind::None && b.sticky != StickyKind::None && a.sticky != b.sticky) return false;
    if (a.pushOnly || b.pushOnly) return false;
    if (has(a.notSupportive, d)) return false;
    if (has(b.notSupportive, opposite(d))) return false;
    return true;
}

std::vector<Pos> chassisIncluded(Pos const& pos, BlockInfo const& chassis, WorldQuery const& world, std::optional<Dir> forced) {
    std::vector<Pos> out;
    int              range = chassis.chassisRange;
    if (chassis.chassis == ChassisKind::Linear || chassis.chassis == ChassisKind::SecondaryLinear) {
        for (bool pos_ : {true, false}) {
            Dir  facing = dirOf(chassis.chassisAxis, pos_);
            bool sticky = has(chassis.stickyFaces, facing);
            for (int i = 1; i <= range; ++i) {
                if (forced != facing && !sticky) break;
                Pos  current = pos + offset(facing) * i;
                auto info    = world(current);
                if (!info.necessary) break;
                if (info.brittle) break;
                out.push_back(current);
                if (has(info.notSupportive, facing)) break;
            }
        }
    } else if (chassis.chassis == ChassisKind::Radial) {
        double limit = (range + 0.5) * (range + 0.5);
        for (auto facing : kAllDirs) {
            if (axisOf(facing) == chassis.chassisAxis) continue;
            if (!has(chassis.stickyFaces, facing)) continue;
            std::deque<Pos>                       frontier{pos + offset(facing)};
            std::unordered_set<Pos, PosHash>      visited;
            while (!frontier.empty()) {
                Pos search = frontier.front();
                frontier.pop_front();
                if (visited.contains(search)) continue;
                Pos d = search - pos;
                if (double(d.x * d.x + d.y * d.y + d.z * d.z) >= limit) continue;
                auto info = search == pos ? chassis : world(search);
                if (!info.necessary) continue;
                if (info.brittle) continue;
                visited.insert(search);
                if (search != pos) out.push_back(search);
                for (auto o : kAllDirs) {
                    if (axisOf(o) == chassis.chassisAxis) continue;
                    if (search == pos && o != facing) continue;
                    if (has(info.notSupportive, o)) continue;
                    frontier.push_back(search + offset(o));
                }
            }
        }
    }
    return out;
}

std::vector<Pos> attachedChassis(Pos const& pos, BlockInfo const& chassis, WorldQuery const& world) {
    std::vector<Pos> out;
    if (chassis.chassis == ChassisKind::Radial) {
        for (bool p : {false, true}) {
            Pos  current = pos + offset(dirOf(chassis.chassisAxis, p));
            auto info    = world(current);
            if (info.chassis != ChassisKind::Radial || info.chassisAxis != chassis.chassisAxis) continue;
            out.push_back(current);
        }
    } else if (chassis.chassis != ChassisKind::None) {
        for (auto d : kAllDirs) {
            Pos  current = pos + offset(d);
            auto info    = world(current);
            if (info.chassis != chassis.chassis) continue; // same kind: linear with linear, secondary with secondary
            if (info.chassisAxis != chassis.chassisAxis) continue;
            out.push_back(current);
        }
    }
    return out;
}

AssemblyResult
assemble(std::vector<Pos> const& start, WorldQuery const& world, GlueQuery const& glue, AssemblyLimits const& limits) {
    AssemblyResult                   result;
    std::deque<Pos>                  frontier(start.begin(), start.end());
    std::unordered_set<Pos, PosHash> visited;
    std::unordered_set<Pos, PosHash> added;
    auto fail = [&](AssemblyError e, Pos p, std::string id) {
        result.error      = e;
        result.errorPos   = p;
        result.errorBlock = std::move(id);
        result.blocks.clear();
        return result;
    };

    for (int guard = 0; guard < 1000000 && !frontier.empty(); ++guard) {
        Pos pos = frontier.front();
        frontier.pop_front();
        if (visited.contains(pos)) continue;
        visited.insert(pos);
        if (pos.y < limits.minY || pos.y >= limits.maxY) continue;
        auto info = world(pos);
        if (!info.loaded) return fail(AssemblyError::UnloadedChunk, pos, info.id);
        if (info.anchor) continue;
        if (!info.necessary) continue;
        if (!info.movable) return fail(AssemblyError::Unmovable, pos, info.id);

        auto push = [&](Pos const& p) {
            if (!visited.contains(p)) frontier.push_back(p);
        };

        if (info.chassis != ChassisKind::None) {
            for (auto const& p : attachedChassis(pos, info, world)) push(p);
            for (auto const& p : chassisIncluded(pos, info, world, limits.forced)) push(p);
        }
        if (info.stickerFacing) {
            Pos  target = pos + offset(*info.stickerFacing);
            auto t      = world(target);
            if (!has(t.notSupportive, opposite(*info.stickerFacing))) push(target);
        }
        for (auto const& p : info.linked) push(p);

        for (auto d : kAllDirs) {
            Pos  np = pos + offset(d);
            auto n  = world(np);
            if (n.anchor) continue;
            if (!n.movable) {
                if (limits.forced == d) return fail(AssemblyError::Unmovable, np, n.id);
                continue;
            }
            if (visited.contains(np)) continue;
            bool faceHasGlue  = glue && glue(pos, d);
            bool attachedHere = has(n.attachedTowards, opposite(d));
            bool stick        = info.sticky != StickyKind::None && canStick(info, n, d);
            bool forced       = limits.forced == d && !has(info.notSupportive, d);
            if (faceHasGlue || attachedHere || stick || forced) frontier.push_back(np);
        }

        if (added.insert(pos).second) result.blocks.push_back(pos);
        if (static_cast<int>(result.blocks.size()) > limits.maxBlocks) {
            return fail(AssemblyError::TooLarge, pos, info.id);
        }
    }
    return result;
}

} // namespace lv::core::contraption
