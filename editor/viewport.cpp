#include "viewport.hpp"
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

namespace paper::editor {
namespace {
constexpr int frameIntervalMilliseconds = 16;
constexpr float lookRadiansPerPixel = .005f, maximumPitchRadians = 1.5f;
constexpr float metersPerWheelStep = .75f, wheelUnitsPerStep = 120;
constexpr float frameDistanceScale = 3, minimumFrameDistanceMeters = 1;
constexpr float farPlaneMeters = 1000;
} // namespace
Viewport::Viewport(std::filesystem::path shaders, QWidget* parent)
    : QWidget(parent), shaders_(std::move(shaders)) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Трёхмерный вид сцены"));
    setToolTip(tr("Правая кнопка — осмотр; колесо — движение; F — приблизить выбранный объект"));
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
    releaseNavigation();
}
void Viewport::draw() {
    if (!isVisible() || unavailable_ || width() <= 0 || height() <= 0)
        return;
    try {
        initialize();
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
        sdl::check(SDL_RenderPresent(renderer_.get()), "Present editor viewport");
    } catch (const std::exception& error) {
        unavailable_ = true;
        timer_.stop();
        if (failed)
            failed(error.what());
    }
}
void Viewport::scene(std::shared_ptr<ScenePackage> package, std::string scene, bool resetCamera) {
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
    if (event->button() == Qt::RightButton) {
        navigating_ = true;
        grabMouse();
        setCursor(Qt::ClosedHandCursor);
    }
    if (event->button() == Qt::LeftButton && selected && width() > 0 && height() > 0) {
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
        if (!id.empty())
            selected(std::move(id));
    }
}
void Viewport::releaseNavigation() {
    if (navigating_) {
        navigating_ = false;
        releaseMouse();
        unsetCursor();
    }
}
void Viewport::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton)
        releaseNavigation();
}
void Viewport::mouseMoveEvent(QMouseEvent* event) {
    if (!navigating_)
        return;
    const auto delta = event->position() - lastMouse_;
    lastMouse_ = event->position();
    camera_.yaw += static_cast<float>(delta.x()) * lookRadiansPerPixel;
    camera_.pitch = std::clamp(camera_.pitch - static_cast<float>(delta.y()) * lookRadiansPerPixel,
                               -maximumPitchRadians, maximumPitchRadians);
}
void Viewport::wheelEvent(QWheelEvent* event) {
    camera_.position =
        camera_.position +
        camera_.forward() * (event->angleDelta().y() / wheelUnitsPerStep * metersPerWheelStep);
}
void Viewport::focusOutEvent(QFocusEvent*) {
    releaseNavigation();
}
void Viewport::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape)
        releaseNavigation();
    else
        QWidget::keyPressEvent(event);
}
} // namespace paper::editor
