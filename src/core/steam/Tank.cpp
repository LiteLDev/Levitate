#include "core/steam/Tank.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>

namespace lv::core::steam {

namespace {

struct Ctx {
    TankAccess&       world;
    TankParams const& params;
    std::set<Pos3>    changed;
    std::set<Pos3>    reset; ///< parts made their own controller that still need to (re)connect (Create's updateConnectivity)
};

bool hasFluid(TankPart const& p) { return !p.fluid.empty() && p.amount > 0; }

void clearFluid(TankPart& p) {
    p.fluid.clear();
    p.amount = 0;
}

void put(Ctx& c, Pos3 const& pos, TankPart const& part) {
    c.world.set(pos, part);
    c.changed.insert(pos);
}

/// Pours `fluid` into `target` whose capacity is `capacity` (FluidTank.fill / CreativeSmartFluidTank).
void pour(Ctx& c, TankPart& target, std::string const& fluid, std::int64_t amount, std::int64_t capacity) {
    if (fluid.empty() || amount <= 0) return;
    if (c.params.creative) {
        if (!hasFluid(target)) {
            target.fluid  = fluid;
            target.amount = capacity;
        }
        return;
    }
    if (hasFluid(target) && target.fluid != fluid) return;
    std::int64_t space = std::max<std::int64_t>(0, capacity - (hasFluid(target) ? target.amount : 0));
    std::int64_t moved = std::min(space, amount);
    if (moved <= 0) return;
    target.fluid  = fluid;
    target.amount = (hasFluid(target) ? target.amount : 0) + moved;
}

/// Create's splitMultiAndInvalidate for the structure controlled at `ctrlPos`.
/// `removedCtrl`: the data of the controller when it is the removed block.
void split(Ctx& c, Pos3 const& ctrlPos, std::optional<TankPart> const& removedCtrl, std::optional<Pos3> const& removed, bool reconnect,
           TankPart const* sizeHint = nullptr) {
    std::optional<TankPart> ctrl = removedCtrl;
    bool                    ctrlAlive = false;
    if (!ctrl) {
        ctrl = c.world.get(ctrlPos);
        if (ctrl && !ctrl->isController(ctrlPos)) ctrl.reset();
        ctrlAlive = ctrl.has_value();
    }
    int width  = ctrl ? ctrl->width : sizeHint ? sizeHint->width : 1;
    int height = ctrl ? ctrl->height : sizeHint ? sizeHint->height : 1;
    if (width == 1 && height == 1) return;

    std::int64_t const perBlock = c.params.capacityPerBlock;
    std::string        fluid    = ctrl && hasFluid(*ctrl) ? ctrl->fluid : std::string{};
    std::int64_t       left     = ctrl && hasFluid(*ctrl) ? ctrl->amount : 0;
    bool const         window   = ctrl ? ctrl->window : true;

    if (ctrlAlive) {
        // the controller keeps the first block's worth (its tank shrinks to one block)
        TankPart self = *ctrl;
        if (c.params.creative) {
            if (hasFluid(self)) self.amount = perBlock;
        } else if (hasFluid(self)) {
            self.amount = std::min(self.amount, perBlock);
            left -= self.amount;
        }
        *ctrl = self;
    }

    std::vector<Pos3> frontier;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int z = 0; z < width; ++z) {
                Pos3 pos = ctrlPos.offset(x, y, z);
                if (removed && pos == *removed) continue;
                std::optional<TankPart> part = (ctrlAlive && pos == ctrlPos) ? ctrl : c.world.get(pos);
                if (!part || part->controller != ctrlPos) continue;
                // removeController(keepFluids): a 1x1 tank of its own, taking the structure's window setting
                part->controller = pos;
                part->width      = 1;
                part->height     = 1;
                part->window     = window;
                if (!(ctrlAlive && pos == ctrlPos)) {
                    clearFluid(*part);
                    if (!fluid.empty()) {
                        if (c.params.creative) {
                            part->fluid  = fluid;
                            part->amount = perBlock;
                        } else if (left > 0) {
                            std::int64_t share = std::min(perBlock, left);
                            part->fluid        = fluid;
                            part->amount       = share;
                            left -= share;
                        }
                    }
                }
                put(c, pos, *part);
                c.reset.insert(pos);
                if (reconnect) frontier.push_back(pos);
            }
        }
    }
    if (reconnect && !frontier.empty()) {
        auto more = formMulti(c.world, frontier, c.params);
        c.changed.insert(more.begin(), more.end());
        for (auto const& p : frontier) c.reset.erase(p);
    }
}

/// Create's tryToFormNewMultiOfWidth.
int formOfWidth(Ctx& c, Pos3 const& origin, int width, bool simulate) {
    auto be = c.world.get(origin);
    if (!be) return 0;
    std::string fluid = hasFluid(*be) ? be->fluid : std::string{};
    int         amount = 0, height = 0;
    int const   maxLength = c.params.maxHeight;

    for (int y = 0; y < maxLength; ++y) {
        bool complete = true;
        for (int x = 0; x < width && complete; ++x) {
            for (int z = 0; z < width && complete; ++z) {
                Pos3 pos  = origin.offset(x, y, z);
                auto part = c.world.get(pos);
                if (!part) {
                    complete = false;
                    break;
                }
                auto controller = c.world.get(part->controller);
                if (!controller) {
                    complete = false;
                    break;
                }
                int otherWidth = controller->width;
                if (otherWidth > width || (otherWidth == width && controller->height == maxLength)) {
                    complete = false;
                    break;
                }
                Pos3 conPos = part->controller;
                if (!(conPos == origin)) {
                    if (conPos.x < origin.x || conPos.z < origin.z || conPos.x + otherWidth > origin.x + width
                        || conPos.z + otherWidth > origin.z + width) {
                        complete = false;
                        break;
                    }
                }
                if (!fluid.empty() && hasFluid(*controller) && controller->fluid != fluid) {
                    complete = false;
                    break;
                }
            }
        }
        if (!complete) break;
        amount += width * width;
        ++height;
    }
    if (simulate) return amount;

    std::int64_t const capacity = static_cast<std::int64_t>(amount) * c.params.capacityPerBlock;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int z = 0; z < width; ++z) {
                Pos3 pos = origin.offset(x, y, z);
                if (pos == origin) continue;
                auto part = c.world.get(pos);
                if (!part) continue;
                if (hasFluid(*part)) {
                    auto self = c.world.get(origin);
                    if (self) {
                        pour(c, *self, part->fluid, part->amount, capacity);
                        put(c, origin, *self);
                    }
                    clearFluid(*part);
                    put(c, pos, *part);
                }
                // the part leaves its old structure (its other parts become 1x1 tanks)
                if (!part->isController(pos) || part->width != 1 || part->height != 1) {
                    split(c, part->controller, std::nullopt, std::nullopt, false, &*part);
                    part = c.world.get(pos);
                    if (!part) continue;
                    if (hasFluid(*part)) { // a share handed out by that split
                        auto self = c.world.get(origin);
                        if (self) {
                            pour(c, *self, part->fluid, part->amount, capacity);
                            put(c, origin, *self);
                        }
                        clearFluid(*part);
                    }
                }
                part->controller = origin;
                part->width      = width;
                part->height     = height;
                put(c, pos, *part);
                c.reset.erase(pos);
            }
        }
    }
    return amount;
}

/// Create's tryToFormNewMulti.
int formNew(Ctx& c, Pos3 const& origin, bool simulate) {
    auto be = c.world.get(origin);
    if (!be || !be->isController(origin)) return 0;
    int bestWidth = 1, bestAmount = -1;
    for (int w = 1; w <= c.params.maxWidth; ++w) {
        int amount = formOfWidth(c, origin, w, true);
        if (amount < bestAmount) continue;
        bestWidth  = w;
        bestAmount = amount;
    }
    if (simulate) return bestAmount;
    if (be->width == bestWidth && be->width * be->width * be->height == bestAmount) return bestAmount;

    split(c, origin, std::nullopt, std::nullopt, false);
    // setTankSize(bestAmount): the controller's fluid is cut to the new capacity
    if (auto self = c.world.get(origin)) {
        std::int64_t capacity = static_cast<std::int64_t>(bestAmount) * c.params.capacityPerBlock;
        if (hasFluid(*self)) self->amount = c.params.creative ? capacity : std::min(self->amount, capacity);
        put(c, origin, *self);
    }
    formOfWidth(c, origin, bestWidth, false);
    if (auto self = c.world.get(origin)) {
        self->controller = origin;
        self->width      = bestWidth;
        self->height     = bestAmount / bestWidth / bestWidth;
        put(c, origin, *self);
    }
    c.reset.erase(origin);
    return bestAmount;
}

std::set<Pos3> formMultiImpl(TankAccess& world, std::vector<Pos3> frontier, TankParams const& params, int depth) {
    Ctx c{world, params, {}, {}};
    if (frontier.empty()) return {};
    int minX = INT_MAX, minZ = INT_MAX;
    for (auto const& p : frontier) {
        minX = std::min(minX, p.x);
        minZ = std::min(minZ, p.z);
    }
    minX -= params.maxWidth;
    minZ -= params.maxWidth;
    int const minY = INT_MIN; // vertical multiblocks: the search is not bounded downwards

    struct Candidate {
        int  amount;
        int  order;
        Pos3 pos;
    };
    std::vector<Candidate> queue;
    std::set<Pos3>         visited;
    std::deque<Pos3>       todo(frontier.begin(), frontier.end());
    int                    order = 0;
    while (!todo.empty()) {
        Pos3 pos = todo.front();
        todo.pop_front();
        if (visited.contains(pos)) continue;
        visited.insert(pos);
        int amount = formNew(c, pos, true);
        if (amount > 1) queue.push_back({amount, order++, pos});
        for (Pos3 next : {pos.offset(-1, 0, 0), pos.offset(0, -1, 0), pos.offset(0, 0, -1)}) {
            if (next.x <= minX || next.y <= minY || next.z <= minZ) continue;
            if (visited.contains(next)) continue;
            if (!world.get(next)) continue;
            todo.push_back(next);
        }
    }
    std::stable_sort(queue.begin(), queue.end(), [](Candidate const& a, Candidate const& b) { return a.amount > b.amount; });
    std::set<Pos3> created;
    for (auto const& cand : queue) {
        if (created.contains(cand.pos)) continue;
        created.insert(cand.pos);
        formNew(c, cand.pos, false);
    }
    // parts split off on the way connect again on their own (Create: next tick)
    std::vector<Pos3> again;
    for (auto const& p : c.reset) {
        auto part = world.get(p);
        if (part && part->isController(p) && part->width == 1 && part->height == 1 && !visited.contains(p)) again.push_back(p);
    }
    if (!again.empty() && depth < 8) {
        auto more = formMultiImpl(world, again, params, depth + 1);
        c.changed.insert(more.begin(), more.end());
    }
    return c.changed;
}

} // namespace

std::set<Pos3> formMulti(TankAccess& world, std::vector<Pos3> frontier, TankParams const& params) {
    return formMultiImpl(world, std::move(frontier), params, 0);
}

std::set<Pos3> removePart(TankAccess& world, Pos3 const& removed, TankPart const& removedPart,
                          TankParams const& params) {
    Ctx c{world, params, {}, {}};
    if (removedPart.width == 1 && removedPart.height == 1) return {};
    if (removedPart.isController(removed)) {
        split(c, removed, removedPart, removed, true);
    } else {
        split(c, removedPart.controller, std::nullopt, removed, true, &removedPart);
    }
    return c.changed;
}

std::int64_t capacityOf(TankPart const& controller, TankParams const& params) {
    return static_cast<std::int64_t>(controller.width) * controller.width * controller.height * params.capacityPerBlock;
}

int windowShape(bool window, int width, int xOffset, int zOffset) {
    if (!window) return 0;
    if (width == 1) return 1;
    if (width == 2) return xOffset == 0 ? (zOffset == 0 ? 2 : 3) : (zOffset == 0 ? 4 : 5);
    if (width == 3 && std::abs(std::abs(xOffset) - std::abs(zOffset)) == 1) return 1;
    return 0;
}

int partLight(int fluidLight, std::int64_t amount, std::int64_t capacity, int height, int yOffset, bool window) {
    if (!window) return 0;
    int    luminosity = static_cast<int>(static_cast<float>(fluidLight) / 1.2f);
    double fill       = fillState(amount, capacity);
    int    maxY       = static_cast<int>(fill * height + 1);
    bool   bright     = yOffset < maxY;
    if (amount <= 0) return 0;
    return bright ? luminosity : luminosity > 0 ? 1 : 0;
}

int comparatorLevel(std::int64_t amount, std::int64_t capacity) {
    double f = fillState(amount, capacity);
    return static_cast<int>(std::floor(std::clamp(f * 14 + (f > 0 ? 1 : 0), 0.0, 15.0)));
}

double fillState(std::int64_t amount, std::int64_t capacity) {
    if (capacity <= 0 || amount <= 0) return 0;
    return std::clamp(static_cast<double>(amount) / static_cast<double>(capacity), 0.0, 1.0);
}

} // namespace lv::core::steam
