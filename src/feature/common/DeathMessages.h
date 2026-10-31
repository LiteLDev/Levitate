#pragma once

// Create's death messages that name a second party (U59): Bedrock's ActorDamageSource with a custom
// message id gives only the dead entity's name. A unit about to hurt an entity marks it with
// Create's translation key and the extra parameters; when that hurt kills it in the same or the
// next tick, the death message is the marked one (one ActorDamageSource#getDeathMessage hook).
//
//   common::deathmsg::mark(target, "death.attack.create.run_over", {"%entity.create.carriage_contraption"});
//   target.hurt(source, damage, params);
//
// Parameters starting with '%' are translation keys the client resolves.

#include <string>
#include <vector>

class Actor;

namespace lv::feature::common::deathmsg {

/// The next death message of `victim` (this tick and the next) is `key` with the dead entity's
/// name followed by `params`.
void mark(Actor const& victim, std::string key, std::vector<std::string> params = {});
/// The marked message of `victim` (key, params including the name), "" when none (selftests).
[[nodiscard]] std::pair<std::string, std::vector<std::string>> peek(Actor const& victim, std::string const& deadName);

} // namespace lv::feature::common::deathmsg
