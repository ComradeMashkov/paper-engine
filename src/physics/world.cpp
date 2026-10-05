#include "paper/physics/world.hpp"
// Jolt must precede its other headers; it defines their configuration macros.
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <map>
#include <thread>
namespace paper {
namespace {
JPH::Vec3 j(Vec3 p) {
    return {p.x, p.y, p.z};
}
Vec3 p(JPH::Vec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}
JPH::Quat j(Rotation3 q) {
    q = q.unit();
    return {q.x, q.y, q.z, q.w};
}
constexpr unsigned categoryBits = 32;
constexpr float capsuleCentreFraction = .5f;
constexpr unsigned maximumSubsteps = 256, maximumBodies = 65536, maximumPairs = 262144;

constexpr float maximumCoordinate = 10000, maximumVelocity = 1000, maximumHeight = 1000,
                maximumFrameSeconds = 1;
bool coordinate(Vec3 v) {
    return finite3(v) &&
           std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)}) <= maximumCoordinate;
}
std::uint64_t packed(CollisionFilter f) {
    return (std::uint64_t(f.category) << categoryBits) | f.mask;
}
CollisionFilter unpacked(std::uint64_t f) {
    return {std::uint32_t(f >> categoryBits), std::uint32_t(f)};
}
struct Runtime {
    Runtime() {
        if (JPH::Factory::sInstance)
            throw std::runtime_error("Paper physics requires ownership of Jolt initialization");
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory;
        JPH::RegisterTypes();
    }
    ~Runtime() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};
Runtime& runtime() {
    static Runtime instance;
    return instance;
}
class BroadPhase final : public JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override {
        return JPH::BroadPhaseLayer(0);
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "Paper"; }
#endif
};
class BroadFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
};
class PairFilter final : public JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override { return true; }
};
class QueryFilter final : public JPH::BodyFilter {
  public:
    explicit QueryFilter(CollisionFilter f, std::optional<JPH::BodyID> body = {})
        : filter(f), only(body) {}
    bool ShouldCollide(const JPH::BodyID& body) const override { return !only || body == *only; }
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        return filter.accepts(unpacked(body.GetUserData()));
    }
    CollisionFilter filter;
    std::optional<JPH::BodyID> only;
};
JPH::RefConst<JPH::Shape> checked(const JPH::Shape::ShapeResult& result) {
    if (result.HasError())
        throw std::invalid_argument(result.GetError().c_str());
    return result.Get();
}
void validFilter(CollisionFilter filter) {
    if (!filter.category)
        throw std::invalid_argument("Collider category cannot be empty");
}
} // namespace
struct PhysicsWorld::State {
    // Dependencies outlive PhysicsSystem and characters. Characters hold this shared state.
    BroadPhase broad;
    BroadFilter broadFilter;
    PairFilter pairFilter;
    JPH::TempAllocatorMalloc allocator;
    JPH::PhysicsSystem physics;
    std::map<ColliderId, JPH::BodyID> bodies;
    ColliderId next = 1;
    std::thread::id owner = std::this_thread::get_id();
    explicit State(PhysicsLimits limits) {
        (void)runtime();
        if (!limits.bodies || !limits.bodyPairs || !limits.contacts ||
            limits.bodies > maximumBodies || limits.bodyPairs > maximumPairs ||
            limits.contacts > maximumPairs)
            throw std::invalid_argument("Physics capacities must be positive");
        physics.Init(limits.bodies, 0, limits.bodyPairs, limits.contacts, broad, broadFilter,
                     pairFilter);
    }
    ~State() {
        for (const auto& [id, body] : bodies) {
            physics.GetBodyInterface().RemoveBody(body);
            physics.GetBodyInterface().DestroyBody(body);
        }
    }
    void check() const {
        if (owner != std::this_thread::get_id())
            throw std::logic_error("Physics world accessed from a non-owner thread");
    }
    ColliderId add(JPH::RefConst<JPH::Shape> shape, CollisionFilter filter, Vec3 position,
                   Rotation3 rotation) {
        check();
        validFilter(filter);
        if (next == 0)
            throw std::overflow_error("Collider IDs exhausted");
        JPH::BodyCreationSettings settings(shape, j(position), j(rotation),
                                           JPH::EMotionType::Static, 0);
        settings.mUserData = packed(filter);
        const auto body =
            physics.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (body.IsInvalid())
            throw std::runtime_error("Physics body capacity exhausted");
        const auto id = next++;
        try {
            bodies.emplace(id, body);
        } catch (...) {
            physics.GetBodyInterface().RemoveBody(body);
            physics.GetBodyInterface().DestroyBody(body);
            throw;
        }
        return id;
    }
};
PhysicsWorld::PhysicsWorld(PhysicsLimits limits) : state_(std::make_shared<State>(limits)) {}
PhysicsWorld::~PhysicsWorld() = default;
ColliderId PhysicsWorld::addBox(const Box3& local, const MeshTransform& transform,
                                CollisionFilter filter) {
    state_->check();
    if (!transform.valid() || !MeshTransform{{}, local.rotation}.valid() ||
        !std::isfinite(local.yaw) || !coordinate(transform.position) || !finite3(local.center) ||
        !finite3(local.half) || local.half.x <= 0 || local.half.y <= 0 || local.half.z <= 0)
        throw std::invalid_argument("Invalid box collider");
    JPH::Array<JPH::Vec3> points;
    for (float x : {-1.f, 1.f})
        for (float y : {-1.f, 1.f})
            for (float z : {-1.f, 1.f}) {
                const auto baked = transform.rotation.inverse().apply(transform.vector(
                    local.center + local.orientation().apply(
                                       {x * local.half.x, y * local.half.y, z * local.half.z})));
                if (!coordinate(baked) ||
                    !coordinate(transform.position + transform.rotation.apply(baked)))
                    throw std::invalid_argument("Box collider exceeds coordinate budget");
                points.push_back(j(baked));
            }
    JPH::ConvexHullShapeSettings settings(points);
    settings.mMaxConvexRadius = 0;
    return state_->add(checked(settings.Create()), filter, transform.position, transform.rotation);
}
ColliderId PhysicsWorld::addMesh(std::span<const Triangle3> mesh, const MeshTransform& transform,
                                 CollisionFilter filter) {
    state_->check();
    if (mesh.empty() || mesh.size() > physicsLimits::triangles || !transform.valid() ||
        !coordinate(transform.position))
        throw std::invalid_argument("Invalid mesh collider");
    JPH::TriangleList triangles;
    for (const auto& triangle : mesh) {
        const auto a = transform.rotation.inverse().apply(transform.vector(triangle.v[0].p)),
                   b = transform.rotation.inverse().apply(transform.vector(triangle.v[1].p)),
                   c = transform.rotation.inverse().apply(transform.vector(triangle.v[2].p));
        if (!coordinate(a) || !coordinate(b) || !coordinate(c) ||
            !coordinate(transform.point(triangle.v[0].p)) ||
            !coordinate(transform.point(triangle.v[1].p)) ||
            !coordinate(transform.point(triangle.v[2].p)) ||
            length(cross(b - a, c - a)) ==
                0) // numbers: validate the three stored triangle vertices.
            throw std::invalid_argument("Degenerate collider triangle");
        triangles.emplace_back(j(a), j(b), j(c));
    }
    return state_->add(checked(JPH::MeshShapeSettings(triangles).Create()), filter,
                       transform.position, transform.rotation);
}
void PhysicsWorld::remove(ColliderId id) {
    state_->check();
    const auto body = state_->bodies.at(id);
    state_->physics.GetBodyInterface().RemoveBody(body);
    state_->physics.GetBodyInterface().DestroyBody(body);
    state_->bodies.erase(id);
}
void PhysicsWorld::move(ColliderId id, Vec3 position, Rotation3 rotation) {
    state_->check();
    MeshTransform t{position, rotation};
    if (!t.valid() || !coordinate(position))
        throw std::invalid_argument("Invalid collider pose");
    state_->physics.GetBodyInterface().SetPositionAndRotation(
        state_->bodies.at(id), j(position), j(rotation), JPH::EActivation::DontActivate);
}
std::optional<CollisionHit> PhysicsWorld::raycast(Vec3 origin, Vec3 direction, float distance,
                                                  CollisionFilter filter) const {
    state_->check();
    validFilter(filter);
    const float magnitude = length(direction);
    if (!coordinate(origin) || !finite3(direction) || !std::isfinite(magnitude) || magnitude == 0 ||
        !std::isfinite(distance) || distance <= 0 || distance > maximumCoordinate)
        throw std::invalid_argument("Invalid ray");
    direction = direction / magnitude;
    JPH::RayCastResult hit;
    QueryFilter query(filter);
    if (!state_->physics.GetNarrowPhaseQuery().CastRay(
            JPH::RRayCast(j(origin), j(direction * distance)), hit, {}, {}, query))
        return {};
    JPH::BodyLockRead lock(state_->physics.GetBodyLockInterface(), hit.mBodyID);
    if (!lock.Succeeded())
        throw std::runtime_error("Collider disappeared during owner-thread query");
    const auto point = origin + direction * (distance * hit.mFraction);
    for (const auto& [id, body] : state_->bodies)
        if (body == hit.mBodyID)
            return CollisionHit{
                id, point, p(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, j(point))),
                distance * hit.mFraction};
    throw std::logic_error("Ray hit an unregistered collider");
}
bool PhysicsWorld::overlapsCapsule(Vec3 feet, float radius, float height, CollisionFilter filter,
                                   std::optional<ColliderId> only, float tolerance) const {
    state_->check();
    validFilter(filter);
    if (!coordinate(feet) || !std::isfinite(radius) || !std::isfinite(height) ||
        !std::isfinite(tolerance) || radius <= 0 || height * capsuleCentreFraction < radius ||
        height > maximumHeight || tolerance < 0 || tolerance > radius ||
        !coordinate(feet + Vec3{0, height, 0}))
        throw std::invalid_argument("Invalid capsule placement query");
    // numbers: total capsule height contains two hemispheres; its centre is half above feet.
    JPH::CapsuleShape shape(height * capsuleCentreFraction - radius, radius);
    class Collector final : public JPH::CollideShapeCollector {
      public:
        explicit Collector(float threshold) : tolerance(threshold) {}
        void AddHit(const JPH::CollideShapeResult& hit) override {
            if (hit.mPenetrationDepth > tolerance) {
                blocked = true;
                ForceEarlyOut();
            }
        }
        float tolerance;
        bool blocked = false;
    } collector(tolerance);
    QueryFilter query(filter, only ? std::optional{state_->bodies.at(*only)} : std::nullopt);
    const auto centre = j(feet + Vec3{0, height * capsuleCentreFraction, 0});
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    state_->physics.GetNarrowPhaseQuery().CollideShape(&shape, JPH::Vec3::sReplicate(1),
                                                       JPH::RMat44::sTranslation(centre), settings,
                                                       centre, collector, {}, {}, query);
    return collector.blocked;
}
struct CharacterController::Impl {
    std::shared_ptr<PhysicsWorld::State> world;
    CharacterSettings config;
    JPH::RefConst<JPH::Shape> standing, crouching;
    std::unique_ptr<JPH::CharacterVirtual> character;
    bool crouched = false, blocked = false;
    Impl(std::shared_ptr<PhysicsWorld::State> state, Vec3 feet, CharacterSettings settings)
        : world(std::move(state)), config(settings) {
        world->check();
        validFilter(config.filter);
        const std::array values{config.radiusMeters,
                                config.standingHeightMeters,
                                config.crouchingHeightMeters,
                                config.maximumSlopeRadians,
                                config.stepHeightMeters,
                                config.snapDownMeters,
                                config.gravityMetersPerSecondSquared,
                                config.jumpSpeedMetersPerSecond,
                                config.paddingMeters,
                                config.maximumSubstepSeconds,
                                config.maximumElapsedSeconds};
        if (!coordinate(feet) ||
            !std::ranges::all_of(values, [](float v) { return std::isfinite(v); }) ||
            config.radiusMeters <= 0 || config.crouchingHeightMeters < 2 * config.radiusMeters ||
            config.standingHeightMeters < config.crouchingHeightMeters ||
            config.maximumSlopeRadians <= 0 || config.maximumSlopeRadians >= pi3 / 2 ||
            config.stepHeightMeters < 0 || config.snapDownMeters < 0 ||
            config.gravityMetersPerSecondSquared < 0 || config.jumpSpeedMetersPerSecond < 0 ||
            config.paddingMeters <= 0 || config.maximumSubstepSeconds <= 0 ||
            config.maximumElapsedSeconds < config.maximumSubstepSeconds ||
            config.maximumElapsedSeconds / config.maximumSubstepSeconds > maximumSubsteps ||
            config.maximumElapsedSeconds > maximumFrameSeconds ||
            config.standingHeightMeters > maximumHeight ||
            config.paddingMeters > config.radiusMeters ||
            config.gravityMetersPerSecondSquared > maximumVelocity ||
            config.jumpSpeedMetersPerSecond > maximumVelocity ||
            config.stepHeightMeters > maximumHeight || config.snapDownMeters > maximumHeight)
            throw std::invalid_argument(
                "Invalid character settings"); // numbers: capsule diameter, half-turn slope limits.
        standing = new JPH::CapsuleShape(config.standingHeightMeters * capsuleCentreFraction -
                                             config.radiusMeters,
                                         config.radiusMeters);
        crouching = new JPH::CapsuleShape(
            config.crouchingHeightMeters * capsuleCentreFraction - config.radiusMeters,
            config.radiusMeters); // numbers: capsule half height excludes its two hemispheres.
        JPH::CharacterVirtualSettings s;
        s.mShape = standing;
        s.mShapeOffset = JPH::Vec3(0, config.standingHeightMeters * capsuleCentreFraction,
                                   0); // numbers: feet-origin capsule centre.
        s.mMaxSlopeAngle = config.maximumSlopeRadians;
        s.mCharacterPadding = config.paddingMeters;
        s.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -config.radiusMeters);
        character = std::make_unique<JPH::CharacterVirtual>(&s, j(feet), JPH::Quat::sIdentity(), 0,
                                                            &world->physics);
        refresh();
    }
    void refresh() {
        QueryFilter filter(config.filter);
        character->RefreshContacts({}, {}, filter, {}, world->allocator);
    }
    bool grounded() const {
        return character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    }
};
CharacterController::CharacterController(PhysicsWorld& world, Vec3 feet, CharacterSettings settings)
    : impl_(std::make_unique<Impl>(world.state_, feet, settings)) {}
CharacterController::~CharacterController() = default;
CharacterState CharacterController::state() const {
    impl_->world->check();
    return {p(impl_->character->GetPosition()),
            p(impl_->character->GetLinearVelocity()),
            p(impl_->character->GetGroundNormal()),
            impl_->grounded(),
            impl_->crouched,
            impl_->blocked};
}
void CharacterController::update(const CharacterInput& input, float seconds) {
    auto& i = *impl_;
    i.world->check();
    if (!finite3(input.horizontalVelocity) || input.horizontalVelocity.y != 0 ||
        std::max(std::abs(input.horizontalVelocity.x), std::abs(input.horizontalVelocity.z)) >
            maximumVelocity ||
        !std::isfinite(seconds) || seconds < 0 || seconds > i.config.maximumElapsedSeconds)
        throw std::invalid_argument("Invalid character update");
    if (seconds == 0)
        return;
    QueryFilter filter(i.config.filter);
    i.blocked = false;
    if (input.crouch != i.crouched) {
        const float desired =
            input.crouch ? i.config.crouchingHeightMeters : i.config.standingHeightMeters;
        const float old =
            i.crouched ? i.config.crouchingHeightMeters : i.config.standingHeightMeters;
        i.character->SetShapeOffset(JPH::Vec3(0, desired * capsuleCentreFraction,
                                              0)); // numbers: capsule centre above feet.
        if (i.character->SetShape(input.crouch ? i.crouching.GetPtr() : i.standing.GetPtr(),
                                  i.config.paddingMeters, {}, {}, filter, {}, i.world->allocator))
            i.crouched = input.crouch;
        else {
            i.character->SetShapeOffset(JPH::Vec3(0, old * capsuleCentreFraction, 0));
            i.blocked = true;
        } // numbers: restore capsule centre on failed clearance query.
    }
    const auto steps = static_cast<unsigned>(std::ceil(seconds / i.config.maximumSubstepSeconds));
    const float dt = seconds / steps;
    for (unsigned step = 0; step < steps; ++step) {
        i.refresh();
        auto velocity = p(i.character->GetLinearVelocity());
        if (i.grounded() && velocity.y <= 0)
            velocity.y = 0;
        if (step == 0 && input.jump && i.grounded())
            velocity.y = i.config.jumpSpeedMetersPerSecond;
        velocity.x = input.horizontalVelocity.x;
        velocity.z = input.horizontalVelocity.z;
        velocity.y -= i.config.gravityMetersPerSecondSquared * dt;
        i.character->SetLinearVelocity(j(velocity));
        JPH::CharacterVirtual::ExtendedUpdateSettings s;
        s.mWalkStairsStepUp = JPH::Vec3(0, i.config.stepHeightMeters, 0);
        s.mStickToFloorStepDown = JPH::Vec3(0, velocity.y > 0 ? 0 : -i.config.snapDownMeters, 0);
        i.character->ExtendedUpdate(dt, JPH::Vec3(0, -i.config.gravityMetersPerSecondSquared, 0), s,
                                    {}, {}, filter, {}, i.world->allocator);
    }
}
} // namespace paper
