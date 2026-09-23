#pragma once
#include "paper/render/gpu_renderer.hpp"
#include "paper/render/spatial.hpp"
#include "paper/scenes/scene.hpp"
#include "paper/sdl/resources.hpp"
#include <QTimer>
#include <QWidget>
#include <functional>

namespace paper::editor {
SDL_Window* wrapNativeWindow(WId id);
class Viewport final : public QWidget {
  public:
    explicit Viewport(std::filesystem::path shaders, QWidget* parent = nullptr);
    ~Viewport() override;
    void scene(std::shared_ptr<ScenePackage> package, std::string scene, bool resetCamera);
    void frame(std::string_view id);
    std::function<void(std::string)> selected, failed;

  protected:
    QPaintEngine* paintEngine() const override { return nullptr; }
    void paintEvent(QPaintEvent*) override {}
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

  private:
    void draw();
    void releaseNavigation();
    void initialize();
    std::filesystem::path shaders_;
    QTimer timer_;
    // Qt owns the native view; only the SDL wrapper is released before QWidget destruction.
    std::unique_ptr<sdl::Session> session_;
    sdl::Window window_;
    sdl::GPUDevice device_;
    sdl::Renderer renderer_;
    std::unique_ptr<GpuRenderer> gpu_;
    std::shared_ptr<ScenePackage> package_;
    std::vector<MeshInstance> instances_;
    std::vector<std::string> instanceIds_;
    MeshQueryCache queries_;
    Camera camera_;
    QPointF lastMouse_;
    bool navigating_ = false, unavailable_ = false;
};
} // namespace paper::editor
