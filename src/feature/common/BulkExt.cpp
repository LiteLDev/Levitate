#include "feature/common/BulkExt.h"

#include "mc/world/item/ItemStack.h"

namespace lv::feature::common::airflow {

namespace {
Provider& provider() {
    static Provider p;
    return p;
}
} // namespace

void setProvider(Provider p) { provider() = std::move(p); }

std::optional<Source> sourceAt(core::GlobalPos const& pos) {
    return provider().sourceAt ? provider().sourceAt(pos) : std::nullopt;
}

std::string typeAt(BlockSource& region, BlockPos const& pos) {
    return provider().typeAt ? provider().typeAt(region, pos) : std::string();
}

bool canProcess(ItemStack const& stack, std::string const& type) {
    return provider().canProcess && provider().canProcess(stack, type);
}

std::optional<std::vector<ItemStack>> process(ItemStack const& stack, std::string const& type) {
    return provider().process ? provider().process(stack, type) : std::nullopt;
}

int processingTime(int count) { return provider().processingTime ? provider().processingTime(count) : 0; }

} // namespace lv::feature::common::airflow
