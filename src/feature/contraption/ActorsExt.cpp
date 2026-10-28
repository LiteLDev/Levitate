// Contraption extension added by U22 (actors), see ActorsExt.h.

#include "feature/contraption/ActorsExt.h"

#include "feature/common/BlockData.h"
#include "feature/contraption/Internal.h"
#include "feature/contraption/Registries.h"

namespace lv::feature::contraption::ext {

namespace {

namespace bd = ::lv::feature::common::blockdata;

/// BlockData moves with its block (Create's block entity data on contraptions).
struct BlockDataCarrierRegistrar {
    BlockDataCarrierRegistrar() {
        registerDataCarrier(DataCarrier{
            kBlockDataCarrier,
            [](BlockSource&, core::GlobalPos const& pos) -> std::optional<nlohmann::json> { return bd::take(pos); },
            [](BlockSource&, core::GlobalPos const& pos, nlohmann::json const& doc) { bd::put(pos, doc); },
            nullptr,
        });
    }
};
BlockDataCarrierRegistrar const gBlockDataCarrier;

} // namespace

CarriedBlock const* carriedAt(Contraption const& c, cc::Pos const& local) {
    for (auto const& b : c.blocks())
        if (b.local == local) return &b;
    return nullptr;
}

nlohmann::json const* carriedBlockData(Contraption const& c, cc::Pos const& local) {
    auto const* b = carriedAt(c, local);
    if (!b || !b->data.is_object()) return nullptr;
    auto it = b->data.find(kBlockDataCarrier);
    return it == b->data.end() ? nullptr : &*it;
}

MovementBehaviour const* actorBehaviour(std::string const& blockId) { return detail::actorFor(blockId); }
std::optional<double>    seatHeight(std::string const& blockId) { return detail::seatHeight(blockId); }

bool powered(BlockSource& region, cc::Pos const& p) { return detail::powered(region, p); }
void actionBar(Player& player, std::string const& text) { detail::actionBar(player, text); }
void playSound(int dim, cc::Vec3d const& pos, std::string const& sound, float volume, float pitch) {
    detail::playSound(dim, pos, sound, volume, pitch);
}
void       defer(std::function<void()> fn) { detail::defer(std::move(fn)); }
cc::States statesOf(Block const& block) { return detail::statesOf(block); }
Block const* withStates(Block const& block, cc::States const& states) { return detail::withStates(block, states); }

} // namespace lv::feature::contraption::ext
