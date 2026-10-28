#include "core/common/ValueSettings.h"

#include <algorithm>
#include <cmath>

namespace lv::core::common {

ValueSettings clampSettings(BoardSpec const& board, ValueSettings s) {
    int rows = std::max<int>(1, static_cast<int>(board.rows.size()));
    s.row    = std::clamp(s.row, 0, rows - 1);
    s.value  = std::clamp(s.value, 0, std::max(0, board.maxValue));
    return s;
}

int valueAtFraction(BoardSpec const& board, double fraction) {
    if (board.maxValue <= 0) return 0;
    fraction = std::clamp(fraction, 0.0, 1.0);
    return static_cast<int>(std::lround(fraction * board.maxValue));
}

int scrollValue(BoardSpec const& board, int value, int steps, bool coarse) {
    int step = coarse ? std::max(1, board.milestoneInterval) : 1;
    long long v = static_cast<long long>(value) + static_cast<long long>(steps) * step;
    if (coarse && steps != 0) {
        // coarse scrolling lands on milestones: round towards the scroll direction
        long long m = step;
        v           = steps > 0 ? (v / m) * m : ((v + m - 1) / m) * m;
    }
    return static_cast<int>(std::clamp<long long>(v, 0, std::max(0, board.maxValue)));
}

BoardSpec plainBoard(int max, int milestoneInterval, std::string rowLabel) {
    return BoardSpec{std::max(0, max), std::max(1, milestoneInterval), {std::move(rowLabel)}};
}

ValueSettings plainToSettings(int value) { return {0, value}; }

int plainFromSettings(ValueSettings s, int min, int max) { return std::clamp(s.value, min, max); }

BoardSpec signedBoard(int max, int milestoneInterval, std::string negativeLabel, std::string positiveLabel) {
    return BoardSpec{std::max(0, max), std::max(1, milestoneInterval), {std::move(negativeLabel), std::move(positiveLabel)}};
}

ValueSettings signedToSettings(int value) { return {value < 0 ? 0 : 1, std::abs(value)}; }

int signedFromSettings(ValueSettings s, int max) {
    int v = std::clamp(std::max(1, s.value), 1, std::max(1, max));
    return s.row == 0 ? -v : v;
}

BoardSpec optionBoard(int count, std::string rowLabel) {
    return BoardSpec{std::max(0, count - 1), 1, {std::move(rowLabel)}};
}

Vec faceLocalPoint(int face, double u, double v, double inset) {
    double const a = u / 16.0, b = v / 16.0, o = inset / 16.0;
    switch (face) {
    case 0: // down: seen from below, u along +x, v along +z
        return {a, -o, b};
    case 1: // up: seen from above, u along +x, v along -z (north is "up" on the screen)
        return {a, 1.0 + o, 1.0 - b};
    case 2: // north (-z): u along -x
        return {1.0 - a, b, -o};
    case 3: // south (+z): u along +x
        return {a, b, 1.0 + o};
    case 4: // west (-x): u along +z
        return {-o, b, a};
    default: // east (+x): u along -z
        return {1.0 + o, b, 1.0 - a};
    }
}

bool testBoxHit(Vec const& localHit, Vec const& boxCentre, double scale) {
    double dx = localHit[0] - boxCentre[0], dy = localHit[1] - boxCentre[1], dz = localHit[2] - boxCentre[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz) < scale / 2.0;
}

} // namespace lv::core::common
