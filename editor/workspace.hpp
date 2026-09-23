#pragma once
#include "paper/editor/application.hpp"
#include <QMainWindow>
#include <memory>

namespace paper::editor {
// Internal construction seam for host startup and isolated editor integration tests.
std::unique_ptr<QMainWindow> makeWorkspace(Options options);
} // namespace paper::editor
