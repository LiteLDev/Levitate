// Stock unit: the packagers of U25 (feature/packages/Packages.h) as the stock network's packager
// provider, and U25's "linked" query (a packager with a stock link ignores redstone).

#include <algorithm>

#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/packages/Packages.h"
#include "feature/stock/StockInternal.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::stock {

namespace {
namespace items = common::items;
namespace pk    = feature::packages;

constexpr int kDx[6]{0, 0, 0, 0, -1, 1}, kDy[6]{-1, 1, 0, 0, 0, 0}, kDz[6]{0, 0, -1, 1, 0, 0};

/// The inventory a packager works on (opposite of its facing; Create's InvManipulationBehaviour).
std::optional<std::pair<GlobalPos, int>> targetOf(GlobalPos const& packager) {
    int facing = std::clamp(stateOr(packager, "levitate:facing", 1), 0, 5);
    int back   = facing ^ 1;
    auto t     = offset(packager, kDx[back], kDy[back], kDz[back]);
    auto id    = idAt(t);
    if (id.empty() || id == pk::kPackagerId || id == pk::kRepackagerId || id == "levitate:portable_storage_interface") return std::nullopt;
    return std::pair{t, facing};
}

pk::RequestType toPackages(RequestType t) {
    switch (t) {
    case RequestType::Player: return pk::RequestType::Player;
    case RequestType::Redstone: return pk::RequestType::Redstone;
    case RequestType::Restock: return pk::RequestType::Restock;
    }
    return pk::RequestType::Restock;
}

PackagerProvider packagesProvider() {
    PackagerProvider p;
    // Create: links only use packagers, not repackagers
    p.isPackager = [](BlockSource& r, BlockPos const& pos) { return game::blockIdAt(r, pos) == pk::kPackagerId; };
    p.availableItems = [](BlockSource& r, BlockPos const& pos) -> std::optional<std::vector<BigStack>> {
        auto gp = game::globalPos(r, pos);
        auto t  = targetOf(gp);
        if (!t) return std::nullopt;
        common::Access at{r, game::blockPos(t->first), t->second};
        auto           h = common::items::find(at);
        if (!h) return std::nullopt;
        // counted here (not with packages::availableItems: one ItemStack can't hold a big count)
        std::vector<BigStack> out;
        int                   n = h->slots(at);
        for (int i = 0; i < n; ++i) {
            auto s = h->getSlot(at, i);
            if (items::isEmpty(s)) continue;
            auto key = itemKey(s);
            auto it  = std::find_if(out.begin(), out.end(), [&](BigStack const& b) { return itemKey(b.stack) == key; });
            if (it == out.end()) out.push_back({items::withCount(s, 1), items::countOf(s)});
            else it->count += items::countOf(s);
        }
        return out;
    };
    p.inventoryId = [](BlockSource& r, BlockPos const& pos) -> std::string {
        auto t = targetOf(game::globalPos(r, pos));
        return t ? "inv:" + t->first.key() : std::string{};
    };
    p.tooBusy = [](BlockSource& r, BlockPos const& pos, RequestType type) { return pk::isTooBusyFor(game::globalPos(r, pos), toPackages(type)); };
    p.send    = [](BlockSource& r, BlockPos const& pos, std::vector<PackagingRequest> const& requests) {
        std::vector<pk::PackagingRequest> queue;
        for (auto const& q : requests) {
            pk::PackagingRequest pr;
            pr.item      = q.item;
            pr.count     = q.count;
            pr.address   = q.address;
            pr.orderId   = q.orderId;
            pr.linkIndex = q.linkIndex;
            pr.finalLink = q.finalLink;
            pr.context   = q.orderContext;
            queue.push_back(std::move(pr));
        }
        // Create's performPackageRequests: up to 100 packages at once
        auto gp = game::globalPos(r, pos);
        for (int i = 0; i < 100 && !queue.empty(); ++i) {
            auto before = queue.size();
            auto front  = queue.front().count;
            pk::attemptToSend(gp, queue);
            if (!queue.empty() && queue.size() == before && queue.front().count == front) break; // nothing could be boxed
        }
    };
    return p;
}
} // namespace

class StockPackagesModule {
public:
    void install() {
        registerPackagerProvider(packagesProvider());
        pk::setLinkProvider([](BlockSource& r, BlockPos const& pos) { return linkOfPackager(game::globalPos(r, pos)).has_value(); });
    }
    void uninstall() {
        pk::setLinkProvider(nullptr);
        setPackagerProvider(std::nullopt);
    }
};

} // namespace lv::feature::stock

LV_REGISTER_MODULE(lv::feature::stock::StockPackagesModule);
