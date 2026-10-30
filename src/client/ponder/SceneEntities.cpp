#ifdef LV_CLIENT

// Entities and particles of Ponder scenes (U46): Create's createEntity / createItemEntity and
// emitParticles.
//
// - Entities (mobs, minecarts, ...) and item entities are drawn by the game's own actor renderer
//   (ActorRenderDispatcher::render, the data-driven models of the resource packs) from "puppet"
//   actors: built by the client level's actor factory, given the local player's dimension, but
//   never added to the level (they don't tick, sync or collide). One puppet per entity type (item
//   entities: per item), drawn at every place the scene shows one, with the scene camera as the
//   world matrix. Puppets are dropped when the scene player closes and when the level goes.
// - Particles are billboards of our own particle sprites (textures/particle/levitate_ponder_particles),
//   simulated from the emitter's age (seeded per particle, so scrubbing the timeline shows the same
//   particles), drawn with the block blend material.

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "glm/gtc/matrix_transform.hpp"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/SupplementaryFieldAutoGenerationMode.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/ecs/gamerefs_entity/OwnerStorageEntity.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorFactory.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/Level.h"

#include "client/Client.h"
#include "client/ponder/Ponder.h"
#include "feature/common/Items.h"
#include "mod/Log.h"

namespace lv::client::ponder {

namespace {

struct Puppet {
    std::unique_ptr<OwnerStorageEntity> owner;
    Actor*                              actor  = nullptr;
    bool                                failed = false;
};

struct Puppets {
    void const*                   level = nullptr;
    std::map<std::string, Puppet> byKey;
    std::set<std::string>         logged;
};

Puppets& puppets() {
    static Puppets p;
    return p;
}

/// The puppet actor of an entity type (`item`: the stack an item entity shows); nullptr when the
/// game can't build one.
Actor* puppet(std::string const& type, std::string const& item = {}) {
    auto* player = currentPlayer();
    if (!player) return nullptr;
    Level& level = player->getLevel();
    auto&  p     = puppets();
    if (p.level != &level) {
        // another level: the old puppets belong to its entity registry, which is gone or going
        for (auto& [k, v] : p.byKey) (void)v.owner.release();
        p.byKey.clear();
        p.level = &level;
    }
    std::string key  = item.empty() ? type : type + "|" + item;
    auto&       slot = p.byKey[key];
    if (slot.actor || slot.failed) return slot.actor;
    slot.failed = true;
    try {
        auto owner = level.getActorFactory().createSpawnedActor(ActorDefinitionIdentifier(type), nullptr, Vec3{0, 0, 0}, Vec2{0, 0});
        if (!owner) {
            log().warn("ponder: no entity of type {} for the scene", type);
            return nullptr;
        }
        slot.owner = std::make_unique<OwnerStorageEntity>(std::move(owner));
        Actor* a   = slot.owner->tryUnwrap().as_ptr();
        if (!a) return nullptr;
        a->setDimension(player->mDimension);
        if (!item.empty()) {
            if (a->getEntityTypeId() != ActorType::ItemEntity) return nullptr;
            auto stack = feature::common::items::make(item);
            if (!stack) return nullptr;
            static_cast<ItemActor*>(a)->item() = *stack;
        }
        slot.actor  = a;
        slot.failed = false;
        log().info("ponder: entity {} ready for scenes", key);
    } catch (std::exception const& e) {
        log().error("ponder: building a {} for the scene failed: {}", type, e.what());
    }
    return slot.actor;
}

// ---- particles ---------------------------------------------------------------------------------

struct ParticleType {
    int   cell;          ///< sprite cell (8 per row, 8x8 pixels)
    float life;          ///< ticks
    float size0, size1;  ///< half size (blocks) at birth / death
    float vx, vy, vz;    ///< base motion (blocks per tick)
    float jitter;        ///< random motion
    float gravity;       ///< blocks per tick per tick
    float r, g, b;       ///< tint
    bool  fade;          ///< alpha fades out
};

ParticleType const& typeOf(std::string const& id) {
    static std::map<std::string, ParticleType> const types{
        {"flame",       {0, 16, .07f, .02f, 0, .015f, 0, .01f, 0, 1, 1, 1, false}  },
        {"soul_flame",  {1, 16, .07f, .02f, 0, .015f, 0, .01f, 0, 1, 1, 1, false}  },
        {"smoke",       {2, 24, .08f, .14f, 0, .035f, 0, .01f, 0, .35f, .35f, .35f, true}},
        {"large_smoke", {2, 30, .16f, .26f, 0, .05f, 0, .015f, 0, .45f, .45f, .45f, true}},
        {"cloud",       {3, 20, .14f, .2f, 0, .025f, 0, .01f, 0, 1, 1, 1, true}    },
        {"splash",      {4, 12, .06f, .05f, 0, .06f, 0, .04f, .012f, 1, 1, 1, false}},
        {"bubble",      {5, 20, .05f, .05f, 0, .03f, 0, .01f, 0, 1, 1, 1, false}   },
        {"snowflake",   {6, 30, .06f, .04f, 0, -.015f, 0, .01f, 0, 1, 1, 1, true}  },
        {"heart",       {7, 20, .1f, .1f, 0, .02f, 0, 0, 0, 1, 1, 1, false}        },
        {"crit",        {8, 10, .06f, .03f, 0, 0, 0, .06f, 0, 1, 1, 1, true}       },
    };
    auto it = types.find(id);
    return it != types.end() ? it->second : types.at("cloud");
}

/// A deterministic random number in [-1, 1) for particle `i` of emitter `seed`, component `k`.
float noise(int seed, int i, int k) {
    std::uint32_t h = static_cast<std::uint32_t>(seed) * 73856093u ^ static_cast<std::uint32_t>(i) * 19349663u ^ static_cast<std::uint32_t>(k) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xffffu) / 32768.f - 1.f;
}

constexpr float kPi = 3.14159265358979f;
/// Scale of data-driven actor models drawn through the UI pass (see drawSceneEntities).
constexpr float kModelScale = 1.f / 16.f;

} // namespace

void releaseScenePuppets() {
    auto& p = puppets();
    p.byKey.clear(); // the actors were never added to the level: their entity contexts go with them
}

int drawSceneEntities(ScreenContext& sc, BaseActorRenderContext& rc, glm::mat4 const& camera, cp::SceneState const& st) {
    std::shared_ptr<ActorRenderDispatcher>& dispatcher = rc.mEntityRenderDispatcher;
    if (!dispatcher) return 0;
    auto& worldMat = sc.camera.worldMatrixStack.get();
    int   drawn    = 0;
    auto  draw     = [&](Actor& a, cp::Vec3f const& pos, float yaw) {
        auto ref = worldMat.push(false);
        // data-driven actor models come out in model pixels here (the world pass scales them by
        // its own projection): shrink them to blocks about the entity's position
        glm::vec3 at{pos.x, pos.y, pos.z};
        ref.mat->_m.get() = ref.mat->_m.get() * camera * glm::translate(glm::mat4(1.f), at) * glm::scale(glm::mat4(1.f), glm::vec3(kModelScale))
                          * glm::translate(glm::mat4(1.f), -at);
        dispatcher->render(rc, a, Vec3{0, 0, 0}, Vec3{pos.x, pos.y, pos.z}, Vec2{0, yaw}, true);
        ++drawn;
    };
    for (auto const& e : st.entities) {
        if (auto* a = puppet(e.id)) draw(*a, e.pos, e.yaw);
    }
    return drawn;
}

bool drawSceneItemEntity(ScreenContext& sc, BaseActorRenderContext& rc, glm::mat4 const& camera, std::string const& item, cp::Vec3f const& pos, float time) {
    std::shared_ptr<ActorRenderDispatcher>& dispatcher = rc.mEntityRenderDispatcher;
    if (!dispatcher) return false;
    auto* a = puppet("minecraft:item", item);
    if (!a) return false;
    auto& worldMat    = sc.camera.worldMatrixStack.get();
    auto  ref         = worldMat.push(false);
    ref.mat->_m.get() = ref.mat->_m.get() * camera;
    // the item entity's slow spin (dropped items turn as they lie)
    float yaw = std::fmod(time * 4.f, 360.f);
    // item entities are drawn from a quarter block below their centre
    dispatcher->render(rc, *a, Vec3{0, 0, 0}, Vec3{pos.x, pos.y - .125f, pos.z}, Vec2{0, yaw}, true);
    return true;
}

int drawSceneParticles(
    MinecraftUIRenderContext& ui,
    ScreenContext&            sc,
    glm::mat4 const&          camera,
    mce::MaterialPtr const&   material,
    cp::SceneState const&     st,
    cp::View const&           view
) {
    if (st.particles.empty()) return 0;
    // loaded once and kept (never destroyed after the renderer at exit)
    static auto* texture = new mce::TexturePtr(ui.getTexture(ResourceLocation(Core::PathView("textures/particle/levitate_ponder_particles")), false));
    // the camera's right and up in scene coordinates (rows of View's rotation)
    float     yr = view.yaw * kPi / 180.f, pr = view.pitch * kPi / 180.f;
    glm::vec3 right{std::cos(yr), 0.f, std::sin(yr)};
    glm::vec3 up{std::sin(pr) * std::sin(yr), std::cos(pr), -std::sin(pr) * std::cos(yr)};

    Tessellator& tess = sc.tessellator;
    tess.begin(Tessellator::DebugContextCallback{}, mce::PrimitiveMode::QuadList, 0, false);
    int count = 0;
    for (auto const& em : st.particles) {
        auto const& t     = typeOf(em.type);
        int         first = std::max(0, static_cast<int>(std::floor((em.age - t.life) * em.rate)));
        int         last  = static_cast<int>(std::floor(em.age * em.rate));
        for (int i = first; i <= last && count < 2000; ++i) {
            float born = static_cast<float>(i) / em.rate;
            float a    = em.age - born;
            if (a < 0 || a >= t.life) continue;
            float k = a / t.life;
            glm::vec3 p{em.point.x + noise(em.instance, i, 0) * em.spread.x, em.point.y + noise(em.instance, i, 1) * em.spread.y,
                        em.point.z + noise(em.instance, i, 2) * em.spread.z};
            glm::vec3 v{t.vx + em.motion.x + noise(em.instance, i, 3) * t.jitter, t.vy + em.motion.y + std::abs(noise(em.instance, i, 4)) * t.jitter,
                        t.vz + em.motion.z + noise(em.instance, i, 5) * t.jitter};
            p += v * a;
            p.y -= t.gravity * a * a * .5f;
            float h     = t.size0 + (t.size1 - t.size0) * k;
            float alpha = t.fade ? 1.f - k : 1.f;
            float u0 = static_cast<float>(t.cell % 8) / 8.f, v0 = static_cast<float>(t.cell / 8) / 2.f;
            float u1 = u0 + 1.f / 8.f, v1 = v0 + 1.f / 2.f;
            glm::vec3 c0 = p - right * h - up * h, c1 = p + right * h - up * h, c2 = p + right * h + up * h, c3 = p - right * h + up * h;
            tess.color(t.r, t.g, t.b, alpha);
            tess.simpleQuad(Vec3{c0.x, c0.y, c0.z}, Vec2{u0, v1}, Vec3{c1.x, c1.y, c1.z}, Vec2{u1, v1}, Vec3{c2.x, c2.y, c2.z}, Vec2{u1, v0}, Vec3{c3.x, c3.y, c3.z}, Vec2{u0, v0});
            tess.simpleQuad(Vec3{c3.x, c3.y, c3.z}, Vec2{u0, v0}, Vec3{c2.x, c2.y, c2.z}, Vec2{u1, v0}, Vec3{c1.x, c1.y, c1.z}, Vec2{u1, v1}, Vec3{c0.x, c0.y, c0.z}, Vec2{u0, v1});
            ++count;
        }
    }
    auto mesh = tess.end(Tessellator::UploadMode::Buffered, "levitate_ponder_particles", SupplementaryFieldAutoGenerationMode::None);
    if (count == 0) return 0;
    auto& worldMat    = sc.camera.worldMatrixStack.get();
    auto  ref         = worldMat.push(false);
    ref.mat->_m.get() = ref.mat->_m.get() * camera;
    std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture> tex{*texture};
    mesh.renderMesh(sc, material, tex, 0, 0, OffscreenCaptureDescription{}, nullptr);
    return count;
}

} // namespace lv::client::ponder

#endif // LV_CLIENT
