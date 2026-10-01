#pragma once
#include "paper/render/scene.hpp"
#include <memory>
#include <optional>
namespace paper {
namespace physicsLimits {
inline constexpr size_t triangles = 50000;
}
using ColliderId = std::uint32_t;
struct CollisionFilter {
    std::uint32_t category = 1, mask = ~std::uint32_t{0};
    bool accepts(CollisionFilter other) const noexcept {
        return (category & other.mask) && (mask & other.category);
    }
};
struct CollisionHit {
    ColliderId collider;
    Vec3 point, normal;
    float distance;
};
struct PhysicsLimits {
    unsigned bodies = 4096, bodyPairs = 4096, contacts = 4096;
};
class CharacterController;
// A single owner thread performs all mutations/queries. No SDL, Qt or host dependencies.
class PhysicsWorld {
  public:
    explicit PhysicsWorld(PhysicsLimits limits = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    ColliderId addBox(const Box3& local, const MeshTransform& transform = {},
                      CollisionFilter filter = {});
    ColliderId addMesh(std::span<const Triangle3> mesh, const MeshTransform& transform = {},
                       CollisionFilter filter = {});
    // Transform updates are rigid; scale/shear is baked into the shape at creation.
    void move(ColliderId id, Vec3 position, Rotation3 rotation = {});
    void remove(ColliderId id);
    [[nodiscard]] std::optional<CollisionHit> raycast(Vec3 origin, Vec3 direction, float distance,
                                                      CollisionFilter filter = {}) const;

  private:
    struct State;
    std::shared_ptr<State> state_;
    friend class CharacterController;
};
struct CharacterSettings {
    float radiusMeters = .3f, standingHeightMeters = 1.8f, crouchingHeightMeters = 1;
    float maximumSlopeRadians = units::radians(45), stepHeightMeters = .35f, snapDownMeters = .25f;
    float gravityMetersPerSecondSquared = 9.81f, jumpSpeedMetersPerSecond = 5;
    float paddingMeters = .02f, maximumSubstepSeconds = 1.f / 60.f, maximumElapsedSeconds = .25f;
    CollisionFilter filter;
};
struct CharacterInput {
    Vec3 horizontalVelocity;
    bool crouch = false, jump = false;
};
struct CharacterState {
    Vec3 feet, velocity, groundNormal{0, 1, 0};
    bool grounded = false, crouched = false, standingBlocked = false;
};
class CharacterController {
  public:
    CharacterController(PhysicsWorld& world, Vec3 feet, CharacterSettings settings = {});
    ~CharacterController();
    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;
    void update(const CharacterInput& input, float seconds);
    [[nodiscard]] CharacterState state() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace paper
