#pragma once
#include "paper/render/gpu_renderer.hpp"
#include "paper/render/spatial.hpp"
#include "paper/scenes/scene.hpp"
#include "paper/sdl/resources.hpp"
#include <QElapsedTimer>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <set>

namespace paper::editor {
SDL_Window* wrapNativeWindow(WId id);
class Viewport final : public QWidget {
  public:
    enum class Tool { Select, Move, Rotate, Scale };
    explicit Viewport(std::filesystem::path shaders, QWidget* parent = nullptr);
    ~Viewport() override;
    void scene(std::shared_ptr<ScenePackage> package, std::string scene, bool resetCamera);
    void frame(std::string_view id);
    void selection(std::string id);
    void tool(Tool tool);
    void grid(bool enabled) { grid_ = enabled; }
    void snap(bool enabled) { snap_ = enabled; }
    void editingEnabled(bool enabled) {
        if (!enabled)
            cancelDrag();
        editable_ = enabled;
    }
    size_t renderedFrames() const { return frames_; }
    Vec3 insertionPoint() const;
    // One completed drag becomes one undo command; delta is in world space.
    std::function<void(Tool, Vec3, float)> transformed;
    std::function<void(Tool)> toolRequested;
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
    void keyReleaseEvent(QKeyEvent*) override;

  private:
    void draw();
    void releaseNavigation();
    void initialize();
    void overlay(int pixelWidth, int pixelHeight);
    std::optional<QPointF> project(Vec3 point) const;
    std::optional<Vec3> pivot() const;
    float gizmoLength(Vec3 origin) const;
    int handle(QPointF position) const;
    void cancelDrag();
    void updateDrag(QPointF position);
    void navigate();
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
    QPointF dragStart_;
    Vec3 dragPivot_, dragDelta_;
    float dragValue_ = 0;
    std::vector<MeshInstance> dragInstances_;
    std::set<std::string> dragIds_;
    std::string selectedId_;
    Tool tool_ = Tool::Move;
    int dragAxis_ = -1;
    QSet<int> keys_;
    QElapsedTimer elapsed_;
    bool navigating_ = false, panning_ = false, orbiting_ = false, unavailable_ = false;
    bool grid_ = false, snap_ = false;
    bool editable_ = true;
    size_t frames_ = 0;
};
} // namespace paper::editor
