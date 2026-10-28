#include "core/tracks/CurveStore.h"

namespace lv::core::tracks {

void CurveStore::index(DimData& d, Key const& key, CurvePtr const& c) {
    d.primaries[key]          = c;
    d.byChunk[chunkOf(key.a)].insert(key);
    d.byEnd[key.a][key.b]     = c;
    d.byEnd[key.b][key.a]     = std::make_shared<Curve const>(c->secondary());
    for (auto const& p : c->occupiedBlocks()) d.byBlock[p].insert(key);
}

void CurveStore::unindex(DimData& d, Key const& key, CurvePtr const& c) {
    d.primaries.erase(key);
    if (auto it = d.byChunk.find(chunkOf(key.a)); it != d.byChunk.end()) {
        it->second.erase(key);
        if (it->second.empty()) d.byChunk.erase(it);
    }
    for (auto const& [from, to] : {std::pair{key.a, key.b}, std::pair{key.b, key.a}}) {
        auto it = d.byEnd.find(from);
        if (it == d.byEnd.end()) continue;
        it->second.erase(to);
        if (it->second.empty()) d.byEnd.erase(it);
    }
    for (auto const& p : c->occupiedBlocks()) {
        auto it = d.byBlock.find(p);
        if (it == d.byBlock.end()) continue;
        it->second.erase(key);
        if (it->second.empty()) d.byBlock.erase(it);
    }
}

std::optional<Curve> CurveStore::add(int dim, Curve const& curve) {
    Curve primary = curve.primary() ? curve : curve.secondary();
    auto  replaced = remove(dim, primary.block(0), primary.block(1));
    auto& d        = mDims[dim];
    Key   key{primary.block(0), primary.block(1)};
    index(d, key, std::make_shared<Curve const>(std::move(primary)));
    return replaced;
}

std::optional<Curve> CurveStore::remove(int dim, BlockPos3 const& a, BlockPos3 const& b) {
    auto dit = mDims.find(dim);
    if (dit == mDims.end()) return std::nullopt;
    auto& d = dit->second;
    for (Key key : {Key{a, b}, Key{b, a}}) {
        auto it = d.primaries.find(key);
        if (it == d.primaries.end()) continue;
        CurvePtr c = it->second;
        unindex(d, key, c);
        return *c;
    }
    return std::nullopt;
}

std::vector<Curve> CurveStore::removeAllAt(int dim, BlockPos3 const& pos) {
    std::vector<Curve> out;
    for (auto const& c : curvesAt(dim, pos))
        if (auto r = remove(dim, c->block(0), c->block(1))) out.push_back(std::move(*r));
    return out;
}

void CurveStore::clear() { mDims.clear(); }

std::vector<CurveStore::CurvePtr> CurveStore::curvesAt(int dim, BlockPos3 const& pos) const {
    std::vector<CurvePtr> out;
    auto                  dit = mDims.find(dim);
    if (dit == mDims.end()) return out;
    auto it = dit->second.byEnd.find(pos);
    if (it == dit->second.byEnd.end()) return out;
    for (auto const& [other, c] : it->second) out.push_back(c);
    return out;
}

CurveStore::CurvePtr CurveStore::between(int dim, BlockPos3 const& a, BlockPos3 const& b) const {
    auto dit = mDims.find(dim);
    if (dit == mDims.end()) return nullptr;
    auto it = dit->second.byEnd.find(a);
    if (it == dit->second.byEnd.end()) return nullptr;
    auto jt = it->second.find(b);
    return jt == it->second.end() ? nullptr : jt->second;
}

bool CurveStore::hasCurvesAt(int dim, BlockPos3 const& pos) const {
    auto dit = mDims.find(dim);
    return dit != mDims.end() && dit->second.byEnd.contains(pos);
}

std::vector<CurveStore::CurvePtr> CurveStore::curvesThrough(int dim, BlockPos3 const& pos) const {
    std::vector<CurvePtr> out;
    auto                  dit = mDims.find(dim);
    if (dit == mDims.end()) return out;
    auto it = dit->second.byBlock.find(pos);
    if (it == dit->second.byBlock.end()) return out;
    for (auto const& key : it->second) {
        auto ct = dit->second.primaries.find(key);
        if (ct != dit->second.primaries.end()) out.push_back(ct->second);
    }
    return out;
}

std::vector<std::pair<int, CurveStore::CurvePtr>> CurveStore::all() const {
    std::vector<std::pair<int, CurvePtr>> out;
    for (auto const& [dim, d] : mDims)
        for (auto const& [key, c] : d.primaries) out.emplace_back(dim, c);
    return out;
}

size_t CurveStore::size() const {
    size_t n = 0;
    for (auto const& [dim, d] : mDims) n += d.primaries.size();
    return n;
}

std::vector<CurveStore::CurvePtr> CurveStore::inChunk(int dim, int cx, int cz) const {
    std::vector<CurvePtr> out;
    auto                  dit = mDims.find(dim);
    if (dit == mDims.end()) return out;
    auto it = dit->second.byChunk.find({cx, cz});
    if (it == dit->second.byChunk.end()) return out;
    for (auto const& key : it->second) {
        auto ct = dit->second.primaries.find(key);
        if (ct != dit->second.primaries.end()) out.push_back(ct->second);
    }
    return out;
}

} // namespace lv::core::tracks
