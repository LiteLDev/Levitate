#pragma once

// Client side of the kinetic network (client build only): the speeds the server syncs to the
// client mod (channel "kinetics.sync" of the companion protocol). Other client units read them
// here instead of listening to the channel themselves (a channel has one client handler).

#ifdef LV_CLIENT

#include <functional>
#include <optional>

#include "core/kinetics/SyncCodec.h"

namespace lv::client::kinetics {

struct SyncedSpeed {
    float speed      = 0; ///< effective speed (RPM, signed)
    int   sourceFace = -1; ///< face towards the source (0..5, Create's Direction order) or -1
};

/// The last synced speed of the kinetic block at (dim, x, y, z) (thread safe).
[[nodiscard]] std::optional<SyncedSpeed> syncedSpeed(int dim, int x, int y, int z);

/// Called on the client thread with every batch received (after it was applied). A batch with
/// `reset` starts a full sync of its dimension.
using SyncListener = std::function<void(core::kinetics::sync::Batch const&)>;
void onSync(SyncListener fn);

} // namespace lv::client::kinetics

#endif // LV_CLIENT
