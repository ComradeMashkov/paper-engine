#include "../../editor/play_controller.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <fstream>
#include <iostream>

namespace {
namespace fs = std::filesystem;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void waitFor(F condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(condition(), "Timed out waiting for Play lifecycle");
}
std::string read(const fs::path& path) {
    QFile file(QString::fromStdU16String(path.u16string()));
    check(file.open(QIODevice::ReadOnly), "Cannot read fixture");
    return file.readAll().toStdString();
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary;
        check(temporary.isValid(), "Cannot create test directory");
        const fs::path root(temporary.path().toStdU16String());
        fs::create_directories(root / "assets" / "nested folder");
        std::ofstream(root / "assets" / "scene.dcscene") << "saved scene";
        std::ofstream(root / "assets" / "nested folder" / "texture.bin") << "unchanged media";
        paper::editor::PlayInput input;
        input.root = root / "assets";
        input.manifest = "world.dcworld";
        input.expected["scene.dcscene"] = "saved scene";
        input.overrides["scene.dcscene"] = "unsaved scene";
        input.overrides["world.dcworld"] = "[world]\nentrySpawn = 'selected'\n";
        paper::editor::PlayController play;
        QString logs;
        play.output = [&](const QString& message) { logs += message + '\n'; };
        const fs::path runtime(PAPER_PLAY_FIXTURE);
        const auto ready = [&] {
            return !play.sessionDirectory().isEmpty() &&
                   fs::exists(fs::path(play.sessionDirectory().toStdU16String()) / "state" /
                              "ready");
        };
        for (int repeat = 0; repeat < 2; ++repeat) {
            logs.clear();
            play.start(input, runtime, {});
            waitFor(ready);
            check(play.state() == paper::editor::PlayController::State::Running, "Play must run");
            const fs::path snapshot(play.sessionDirectory().toStdU16String());
            play.start(input, runtime, {});
            check(fs::path(play.sessionDirectory().toStdU16String()) == snapshot,
                  "Repeated Play must not create a second process");
            check(read(snapshot / "assets" / "scene.dcscene") == "unsaved scene",
                  "Snapshot must contain unsaved authoring edits");
            check(read(snapshot / "assets" / "nested folder" / "texture.bin") == "unchanged media",
                  "Snapshot must contain external media including paths with spaces");
            check(read(root / "assets" / "scene.dcscene") == "saved scene" &&
                      !fs::exists(root / "state"),
                  "Play must not write to the source project");
            play.stop();
            waitFor([&] { return !play.active(); });
            check(!fs::exists(snapshot), "Stop must remove the snapshot after child exit");
        }
        play.start(input, runtime, {});
        play.stop();
        waitFor([&] { return !play.active(); });
        check(play.sessionDirectory().isEmpty(), "Cancel during preparation must clean up");
        logs.clear();
        play.start(input, runtime, {"--fail"});
        waitFor([&] { return !play.active(); });
        check(logs.contains("Fixture runtime failed") && logs.contains("exit 7"),
              "Runtime failures must be surfaced and permit another Play");
        logs.clear();
        play.start(input, root / "assets" / "scene.dcscene", {});
        waitFor([&] { return !play.active(); });
        check(logs.contains("Runtime error"), "Failed executable launch must restore Edit Mode");
        logs.clear();
        std::ofstream(root / "assets" / "scene.dcscene") << "external edit";
        play.start(input, runtime, {});
        waitFor([&] { return !play.active(); });
        check(logs.contains("changed outside"), "External authoring changes must block Play");
        std::ofstream(root / "assets" / "scene.dcscene") << "saved scene";
        fs::create_symlink(root / "assets" / "scene.dcscene", root / "assets" / "linked-scene");
        logs.clear();
        play.start(input, runtime, {});
        waitFor([&] { return !play.active(); });
        check(logs.contains("symlinks"), "Symlinks must not leak into an isolated snapshot");
        fs::remove(root / "assets" / "linked-scene");
        logs.clear();
        play.start(input, runtime, {"--ignore-stop"});
        waitFor(ready);
        const fs::path forced(play.sessionDirectory().toStdU16String());
        play.stop();
        waitFor([&] { return !play.active(); });
        check(logs.contains("killing") && !fs::exists(forced),
              "Unresponsive runtime must be killed and its snapshot removed");
        std::cout << "Play checks passed: unsaved snapshot/media, source/save isolation, repeat, "
                     "cancel, launch/content failure, external edits, symlinks, forced Stop\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
