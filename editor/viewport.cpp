#include "viewport.hpp"
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <array>

namespace paper::editor {
namespace {
constexpr int frameIntervalMilliseconds = 16;
constexpr float lookRadiansPerPixel = .005f, maximumPitchRadians = 1.5f;
constexpr float metersPerWheelStep = .75f, wheelUnitsPerStep = 120;
constexpr float frameDistanceScale = 3, minimumFrameDistanceMeters = 1;
constexpr float farPlaneMeters = 1000;
constexpr float cameraSpeedMetersPerSecond = 4, fastCameraMultiplier = 3;
constexpr float maximumFrameSeconds = .1f, millisecondsPerSecond = 1000;
constexpr float handlePixels = 86, handleHitPixels = 10, rotationRingPixels = 48;
constexpr float rotationRadiansPerPixel = .01f, scalePerPixel = .01f;
constexpr float snapMeters = .5f, snapRadians = pi3 / 12, snapScale = .1f;
constexpr float gridSpacingMeters = 1, gridHalfExtentMeters = 20, nearOverlayMeters = .05f;
constexpr float insertDistanceMeters = 3;
constexpr std::array<Vec3, 3> axes{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
float distance(QPointF a, QPointF b) {
    return static_cast<float>(std::hypot(a.x() - b.x(), a.y() - b.y()));
}
float segmentDistance(QPointF p, QPointF a, QPointF b) {
    const auto d = b - a;
    const auto squared = QPointF::dotProduct(d, d);
    if (squared <= 1)
        return distance(p, a);
    const auto t = std::clamp(QPointF::dotProduct(p - a, d) / squared, 0.0, 1.0);
    return distance(p, a + d * t);
}
} // namespace
Viewport::Viewport(std::filesystem::path shaders, QWidget* parent)
    : QWidget(parent), shaders_(std::move(shaders)) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    elapsed_.start();
    setAccessibleName(tr("3D Scene View"));
    setToolTip(tr("Right mouse: Look; Wheel: Move; F: Frame Selected"));
    connect(&timer_, &QTimer::timeout, this, [this] { draw(); });
}
Viewport::~Viewport() {
    timer_.stop();
    releaseNavigation();
    gpu_.reset();
    renderer_.reset();
    device_.reset();
    window_.reset();
    session_.reset();
}
void Viewport::initialize() {
    if (gpu_ || unavailable_)
        return;
    // Qt owns lifecycle and CLI signals, including stopping a development run.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    session_ = std::make_unique<sdl::Session>(false);
    window_.reset(wrapNativeWindow(winId()));
    sdl::check(bool(window_), "Wrap Qt viewport");
#if defined(__APPLE__)
    constexpr auto formats = SDL_GPU_SHADERFORMAT_MSL;
#elif defined(_WIN32)
    constexpr auto formats = SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV;
#else
    constexpr auto formats = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
    device_.reset(SDL_CreateGPUDevice(formats, false, nullptr));
    sdl::check(bool(device_), "Create editor GPU device");
    renderer_.reset(SDL_CreateGPURenderer(device_.get(), window_.get()));
    sdl::check(bool(renderer_), "Create editor GPU renderer");
    RenderConfig config;
    config.farPlaneMeters = farPlaneMeters;
    gpu_ = std::make_unique<GpuRenderer>(*device_, *renderer_, shaders_, config);
}
void Viewport::showEvent(QShowEvent*) {
    timer_.start(frameIntervalMilliseconds);
}
void Viewport::hideEvent(QHideEvent*) {
    timer_.stop();
    cancelDrag();
    releaseNavigation();
}
void Viewport::draw() {
    if (!isVisible() || unavailable_ || width() <= 0 || height() <= 0)
        return;
    try {
        initialize();
        navigate();
        // Qt dispatches native events; pumping Cocoa again would reenter its event loop.
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        int pixelWidth = 0, pixelHeight = 0;
        sdl::check(SDL_GetCurrentRenderOutputSize(renderer_.get(), &pixelWidth, &pixelHeight),
                   "Viewport pixel size");
        if (pixelWidth <= 0 || pixelHeight <= 0)
            return;
        const RenderSize size{static_cast<std::uint32_t>(pixelWidth),
                              static_cast<std::uint32_t>(pixelHeight)};
        if (gpu_->size() != size)
            gpu_->resize(size);
        constexpr Pixel background{37, 40, 46};
        SDL_SetRenderDrawColor(renderer_.get(), background.r, background.g, background.b,
                               background.a);
        sdl::check(SDL_RenderClear(renderer_.get()), "Clear editor viewport");
        if (package_) {
            RenderOptions options;
            options.aspect = static_cast<float>(pixelWidth) / pixelHeight;
            options.shadows.quality = ShadowQuality::Off;
            RenderStyle style;
            style.minimumLight = style.maximumLight = style.ambient = 1;
            style.fogMaximum = 0;
            gpu_->render(instances_, package_->materials, camera_, options, style);
            sdl::check(SDL_RenderTexture(renderer_.get(), gpu_->image(), nullptr, nullptr),
                       "Compose editor viewport");
        }
        overlay(pixelWidth, pixelHeight);
        sdl::check(SDL_RenderPresent(renderer_.get()), "Present editor viewport");
        ++frames_;
    } catch (const std::exception& error) {
        unavailable_ = true;
        timer_.stop();
        if (failed)
            failed(error.what());
    }
}
void Viewport::scene(std::shared_ptr<ScenePackage> package, std::string scene, bool resetCamera) {
    cancelDrag();
    package_ = std::move(package);
    instances_.clear();
    instanceIds_.clear();
    queries_ = {};
    if (resetCamera)
        for (const auto& spawn : package_->spawns)
            if (spawn.scene == scene) {
                camera_ = spawn.camera;
                break;
            }
    for (const auto& node : package_->nodes) {
        if (node.scene != scene || !node.asset)
            continue;
        if (node.asset->mesh) {
            instances_.push_back({node.asset->mesh, node.transform, false, node.room});
            instanceIds_.push_back(node.id);
        }
        if (node.asset->model)
            for (auto instance : node.asset->model->sample()) {
                instance.transform = compose(node.transform, instance.transform);
                instance.castsShadow = false;
                instances_.push_back(std::move(instance));
                instanceIds_.push_back(node.id);
            }
    }
}
void Viewport::frame(std::string_view id) {
    if (!package_)
        return;
    Box3 bounds{};
    bool found = false;
    for (size_t i = 0; i < instances_.size(); ++i)
        if (instanceIds_[i] == id) {
            auto box = worldBounds(meshBounds(*instances_[i].mesh), instances_[i].transform);
            bounds = found ? unionBounds(bounds, box) : box;
            found = true;
        }
    if (!found)
        for (const auto& node : package_->nodes)
            if (node.id == id) {
                bounds = node.bounds;
                bounds.center = node.transform.position;
                found = true;
                break;
            }
    if (found)
        camera_.position =
            bounds.center - camera_.forward() * std::max(minimumFrameDistanceMeters,
                                                         length(bounds.half) * frameDistanceScale);
}
void Viewport::mousePressEvent(QMouseEvent* event) {
    setFocus();
    lastMouse_ = event->position();
    if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier))) {
        cancelDrag();
        panning_ = event->button() == Qt::MiddleButton;
        orbiting_ = event->button() == Qt::LeftButton;
        navigating_ = true;
        grabMouse();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() == Qt::LeftButton && selected && width() > 0 && height() > 0) {
        dragAxis_ = handle(event->position());
        if (dragAxis_ >= 0) {
            dragStart_ = event->position();
            dragPivot_ = *pivot();
            dragInstances_ = instances_;
            dragDelta_ = {};
            dragValue_ = tool_ == Tool::Scale ? 1 : 0;
            dragIds_ = {selectedId_};
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& node : package_->nodes)
                    if (dragIds_.contains(node.parent) && dragIds_.insert(node.id).second)
                        changed = true;
            }
            grabMouse();
            setCursor(Qt::SizeAllCursor);
            return;
        }
        constexpr float clipSpan = 2;
        const float nx = static_cast<float>(event->position().x()) / width() * clipSpan - 1;
        const float ny = 1 - static_cast<float>(event->position().y()) / height() * clipSpan;
        const float vertical = std::tan(camera_.fov / clipSpan);
        const auto ray =
            normalized(camera_.forward() + camera_.right() * (nx * vertical * width() / height()) +
                       camera_.up() * (ny * vertical));
        float nearest = std::numeric_limits<float>::infinity();
        std::string id;
        for (size_t i = 0; i < instances_.size(); ++i) {
            const auto hit = queries_.pick(instances_[i], camera_.position, ray);
            if (hit.distance < nearest) {
                nearest = hit.distance;
                id = instanceIds_[i];
            }
        }
        selected(std::move(id));
    }
}
void Viewport::releaseNavigation() {
    if (navigating_) {
        navigating_ = false;
        panning_ = false;
        orbiting_ = false;
        keys_.clear();
        releaseMouse();
        unsetCursor();
    }
}
void Viewport::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton || orbiting_)
        releaseNavigation();
    if (event->button() == Qt::LeftButton && dragAxis_ >= 0) {
        const auto delta = dragDelta_;
        const auto value = dragValue_;
        cancelDrag();
        if (transformed && (length(delta) > 0 || value != (tool_ == Tool::Scale ? 1 : 0)))
            transformed(tool_, delta, value);
    }
}
void Viewport::mouseMoveEvent(QMouseEvent* event) {
    if (dragAxis_ >= 0) {
        updateDrag(event->position());
        return;
    }
    if (!navigating_) {
        setCursor(handle(event->position()) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        return;
    }
    const auto delta = event->position() - lastMouse_;
    lastMouse_ = event->position();
    if (panning_) {
        const auto depth =
            pivot() ? std::max(minimumFrameDistanceMeters, length(*pivot() - camera_.position))
                    : insertDistanceMeters;
        const auto metersPerPixel = depth * std::tan(camera_.fov * .5f) / std::max(1, height()) *
                                    2; // numbers: vertical perspective spans twice the half-FOV.
        camera_.position = camera_.position -
                           camera_.right() * static_cast<float>(delta.x() * metersPerPixel) +
                           camera_.up() * static_cast<float>(delta.y() * metersPerPixel);
        return;
    }
    const auto orbitCenter = pivot().value_or(insertionPoint());
    const auto orbitDistance =
        std::max(minimumFrameDistanceMeters, length(orbitCenter - camera_.position));
    camera_.yaw += static_cast<float>(delta.x()) * lookRadiansPerPixel;
    camera_.pitch = std::clamp(camera_.pitch - static_cast<float>(delta.y()) * lookRadiansPerPixel,
                               -maximumPitchRadians, maximumPitchRadians);
    if (orbiting_)
        camera_.position = orbitCenter - camera_.forward() * orbitDistance;
}
void Viewport::wheelEvent(QWheelEvent* event) {
    camera_.position =
        camera_.position +
        camera_.forward() * (event->angleDelta().y() / wheelUnitsPerStep * metersPerWheelStep);
}
void Viewport::focusOutEvent(QFocusEvent*) {
    cancelDrag();
    releaseNavigation();
}
void Viewport::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        cancelDrag();
        releaseNavigation();
    } else if (navigating_) {
        keys_.insert(event->key());
        event->accept();
    } else if (toolRequested && (event->key() == Qt::Key_Q || event->key() == Qt::Key_W ||
                                 event->key() == Qt::Key_E || event->key() == Qt::Key_R)) {
        const auto requested = event->key() == Qt::Key_Q   ? Tool::Select
                               : event->key() == Qt::Key_W ? Tool::Move
                               : event->key() == Qt::Key_E ? Tool::Rotate
                                                           : Tool::Scale;
        toolRequested(requested);
    } else
        QWidget::keyPressEvent(event);
}
void Viewport::keyReleaseEvent(QKeyEvent* event) {
    if (!event->isAutoRepeat())
        keys_.remove(event->key());
    QWidget::keyReleaseEvent(event);
}
void Viewport::navigate() {
    const float seconds = std::min(maximumFrameSeconds, elapsed_.restart() / millisecondsPerSecond);
    if (!navigating_ || panning_ || orbiting_)
        return;
    Vec3 direction{};
    if (keys_.contains(Qt::Key_W))
        direction = direction + camera_.forward();
    if (keys_.contains(Qt::Key_S))
        direction = direction - camera_.forward();
    if (keys_.contains(Qt::Key_D))
        direction = direction + camera_.right();
    if (keys_.contains(Qt::Key_A))
        direction = direction - camera_.right();
    if (keys_.contains(Qt::Key_E))
        direction.y += 1;
    if (keys_.contains(Qt::Key_Q))
        direction.y -= 1;
    camera_.position =
        camera_.position + normalized(direction) * cameraSpeedMetersPerSecond * seconds *
                               (keys_.contains(Qt::Key_Shift) ? fastCameraMultiplier : 1);
}
void Viewport::selection(std::string id) {
    if (id != selectedId_)
        cancelDrag();
    selectedId_ = std::move(id);
}
void Viewport::tool(Tool tool) {
    cancelDrag();
    tool_ = tool;
}
Vec3 Viewport::insertionPoint() const {
    return camera_.position + camera_.forward() * insertDistanceMeters;
}
std::optional<Vec3> Viewport::pivot() const {
    if (package_)
        for (const auto& node : package_->nodes)
            if (node.id == selectedId_)
                return node.transform.position;
    return {};
}
std::optional<QPointF> Viewport::project(Vec3 point) const {
    const auto relative = point - camera_.position;
    const auto depth = dot(relative, camera_.forward());
    if (depth <= nearOverlayMeters || height() <= 0 || width() <= 0)
        return {};
    const auto factor =
        height() / (2 * std::tan(camera_.fov * .5f) *
                    depth); // numbers: full perspective height uses twice the half-FOV tangent.
    return QPointF(width() * .5f + dot(relative, camera_.right()) * factor,
                   height() * .5f - dot(relative, camera_.up()) *
                                        factor); // numbers: NDC origin is the viewport centre.
}
float Viewport::gizmoLength(Vec3 origin) const {
    return std::max(nearOverlayMeters, dot(origin - camera_.position, camera_.forward())) * 2 *
           std::tan(camera_.fov * .5f) * handlePixels /
           std::max(1, height()); // numbers: inverse perspective converts the fixed screen handle
                                  // length to metres.
}
int Viewport::handle(QPointF position) const {
    const auto origin = pivot();
    if (!origin || tool_ == Tool::Select || !editable_)
        return -1;
    const auto start = project(*origin);
    if (!start)
        return -1;
    if (tool_ == Tool::Rotate)
        return std::abs(distance(position, *start) - rotationRingPixels) <= handleHitPixels ? 0
                                                                                            : -1;
    if (tool_ == Tool::Scale)
        return segmentDistance(position, *start, *start + QPointF(handlePixels, -handlePixels)) <=
                       handleHitPixels
                   ? 0
                   : -1;
    int result = -1;
    float nearest = handleHitPixels;
    for (size_t i = 0; i < axes.size(); ++i) {
        const auto end = project(*origin + axes[i] * gizmoLength(*origin));
        if (end && distance(*start, *end) > handleHitPixels) {
            const auto d = segmentDistance(position, *start, *end);
            if (d < nearest) {
                nearest = d;
                result = static_cast<int>(i);
            }
        }
    }
    return result;
}
void Viewport::cancelDrag() {
    if (dragAxis_ < 0)
        return;
    instances_ = std::move(dragInstances_);
    dragAxis_ = -1;
    dragIds_.clear();
    releaseMouse();
    unsetCursor();
}
void Viewport::updateDrag(QPointF position) {
    const auto delta = position - dragStart_;
    dragDelta_ = {};
    if (tool_ == Tool::Move) {
        const auto start = project(dragPivot_);
        const auto end =
            project(dragPivot_ + axes.at(static_cast<size_t>(dragAxis_)) * gizmoLength(dragPivot_));
        if (!start || !end)
            return;
        const auto axis = *end - *start;
        const auto squared = QPointF::dotProduct(axis, axis);
        if (squared <= 1)
            return;
        auto amount = static_cast<float>(QPointF::dotProduct(delta, axis) / squared) *
                      gizmoLength(dragPivot_);
        if (snap_)
            amount = std::round(amount / snapMeters) * snapMeters;
        dragDelta_ = axes.at(static_cast<size_t>(dragAxis_)) * amount;
    } else if (tool_ == Tool::Rotate) {
        dragValue_ = static_cast<float>(delta.x()) * rotationRadiansPerPixel;
        if (snap_)
            dragValue_ = std::round(dragValue_ / snapRadians) * snapRadians;
    } else if (tool_ == Tool::Scale) {
        dragValue_ = std::exp(static_cast<float>(delta.x() - delta.y()) * scalePerPixel);
        if (snap_)
            dragValue_ = std::round(dragValue_ / snapScale) * snapScale;
        dragValue_ = std::clamp(dragValue_, sceneLimits::scaleMinimum, sceneLimits::scaleMaximum);
    }
    instances_ = dragInstances_;
    for (size_t i = 0; i < instances_.size(); ++i) {
        if (!dragIds_.contains(instanceIds_[i]))
            continue;
        auto& transform = instances_[i].transform;
        if (tool_ == Tool::Move)
            transform.position = transform.position + dragDelta_;
        else if (tool_ == Tool::Rotate) {
            transform.position = dragPivot_ + rotateY(transform.position - dragPivot_, dragValue_);
            transform.rotation = Rotation3::axisAngle({0, 1, 0}, dragValue_) * transform.rotation;
        } else if (tool_ == Tool::Scale) {
            transform.position = dragPivot_ + (transform.position - dragPivot_) * dragValue_;
            transform.scale *= dragValue_;
        }
    }
}
void Viewport::overlay(int pixelWidth, int pixelHeight) {
    const auto line2 = [&](QPointF a, QPointF b) {
        SDL_RenderLine(renderer_.get(), static_cast<float>(a.x() * pixelWidth / width()),
                       static_cast<float>(a.y() * pixelHeight / height()),
                       static_cast<float>(b.x() * pixelWidth / width()),
                       static_cast<float>(b.y() * pixelHeight / height()));
    };
    const auto line = [&](Vec3 a, Vec3 b) {
        const auto da = dot(a - camera_.position, camera_.forward()),
                   db = dot(b - camera_.position, camera_.forward());
        if (da <= nearOverlayMeters && db <= nearOverlayMeters)
            return;
        constexpr float clippingMargin = 1.01f;
        if (da <= nearOverlayMeters)
            a = a + (b - a) * ((nearOverlayMeters * clippingMargin - da) / (db - da));
        else if (db <= nearOverlayMeters)
            b = b + (a - b) * ((nearOverlayMeters * clippingMargin - db) / (da - db));
        const auto pa = project(a), pb = project(b);
        if (pa && pb)
            line2(*pa, *pb);
    };
    if (grid_) {
        constexpr Pixel color{80, 86, 95, 120};
        SDL_SetRenderDrawBlendMode(renderer_.get(), SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_.get(), color.r, color.g, color.b, color.a);
        const auto x = std::floor(camera_.position.x / gridSpacingMeters) * gridSpacingMeters;
        const auto z = std::floor(camera_.position.z / gridSpacingMeters) * gridSpacingMeters;
        for (float d = -gridHalfExtentMeters; d <= gridHalfExtentMeters; d += gridSpacingMeters) {
            line({x + d, 0, z - gridHalfExtentMeters}, {x + d, 0, z + gridHalfExtentMeters});
            line({x - gridHalfExtentMeters, 0, z + d}, {x + gridHalfExtentMeters, 0, z + d});
        }
    }
    Box3 bounds{};
    bool found = false;
    for (size_t i = 0; i < instances_.size(); ++i)
        if (instanceIds_[i] == selectedId_) {
            const auto box = worldBounds(meshBounds(*instances_[i].mesh), instances_[i].transform);
            bounds = found ? unionBounds(bounds, box) : box;
            found = true;
        }
    constexpr Pixel highlight{255, 194, 92};
    SDL_SetRenderDrawColor(renderer_.get(), highlight.r, highlight.g, highlight.b, highlight.a);
    if (found) {
        constexpr size_t corners = 8;
        std::array<Vec3, corners> points;
        for (size_t i = 0; i < corners; ++i)
            points[i] =
                bounds.center +
                Vec3{(i & 1) ? bounds.half.x : -bounds.half.x,
                     (i & 2) ? bounds.half.y : -bounds.half.y,
                     (i & 4) ? bounds.half.z
                             : -bounds.half
                                    .z}; // numbers: the three bit positions enumerate box corners.
        for (size_t i = 0; i < corners; ++i)
            for (const size_t bit : {1, 2, 4})
                if (!(i & bit))
                    line(points[i], points[i | bit]); // numbers: connect each box corner along its
                                                      // three positive edges.
    }
    const auto origin = pivot();
    if (!origin || tool_ == Tool::Select)
        return;
    const auto shifted = *origin + (dragAxis_ >= 0 ? dragDelta_ : Vec3{});
    const auto start = project(shifted);
    if (!start)
        return;
    if (tool_ == Tool::Move) {
        constexpr std::array<Pixel, 3> colors{Pixel{242, 97, 97}, Pixel{118, 217, 131},
                                              Pixel{98, 170, 255}};
        for (size_t i = 0; i < axes.size(); ++i) {
            const auto c = colors[i];
            SDL_SetRenderDrawColor(renderer_.get(), c.r, c.g, c.b, c.a);
            const auto end = project(shifted + axes[i] * gizmoLength(shifted));
            if (end) {
                line2(*start, *end);
                constexpr float markerPixels = 4;
                line2(*end - QPointF(markerPixels, 0), *end + QPointF(markerPixels, 0));
                line2(*end - QPointF(0, markerPixels), *end + QPointF(0, markerPixels));
            }
        }
    } else if (tool_ == Tool::Rotate) {
        constexpr int segments = 64;
        for (int i = 0; i < segments; ++i) {
            const float a = 2 * pi3 * i / segments,
                        b = 2 * pi3 * (i + 1) / segments; // numbers: sample one full revolution.
            line2(*start + QPointF(std::cos(a), std::sin(a)) * rotationRingPixels,
                  *start + QPointF(std::cos(b), std::sin(b)) * rotationRingPixels);
        }
    } else
        line2(*start, *start + QPointF(handlePixels, -handlePixels));
}
} // namespace paper::editor
