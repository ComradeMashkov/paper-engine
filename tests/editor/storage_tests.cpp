#include "../../editor/project_storage.hpp"
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--crash-save") {
            const fs::path root(argv[2]);
            paper::editor::ProjectStorage storage(root / "test.paperproject");
            storage.save(
                {{root / "a.dcscene", "old A", "new A"}, {root / "b.dcscene", "old B", "new B"}},
                [](size_t) { std::_Exit(73); });
            return 1;
        }
        const auto check = [](bool ok, const char* why) {
            if (!ok)
                throw std::runtime_error(why);
        };
        const auto rejects = [](const auto& operation) {
            try {
                operation();
                return false;
            } catch (const std::exception&) {
                return true;
            }
        };
        const auto write = [](const fs::path& path, const std::string& source) {
            std::ofstream stream(path, std::ios::binary);
            stream << source;
            if (!stream)
                throw std::runtime_error("fixture write failed");
        };
        const auto read = [](const fs::path& path) {
            std::ifstream stream(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(stream), {});
        };
        QTemporaryDir temporary;
        const auto root = fs::path(temporary.path().toStdString());
        const auto project = root / "test.paperproject";
        write(project, "project");
        const auto a = root / "a.dcscene", b = root / "b.dcscene", c = root / "new.dcscene";
        write(a, "old A");
        write(b, "old B");
        paper::editor::ProjectStorage storage(project);
        const std::vector<paper::editor::StoredEdit> batch{
            {a, "old A", "new A"}, {b, "old B", "new B"}, {c, std::nullopt, "new C"}};
        check(rejects(
                  [&] { storage.save(batch, [](size_t) { throw std::runtime_error("crash"); }); }),
              "interrupted save simulated");
        check(read(a) == "new A" && read(b) == "old B" && !fs::exists(c) && storage.pendingSave(),
              "intent survives a partially completed Save All");
        paper::editor::ProjectStorage restarted(project);
        check(restarted.finishSave() && read(b) == "new B" && read(c) == "new C" &&
                  !restarted.pendingSave(),
              "restart completes the entire saved intent");
        check(!restarted.finishSave(), "recovery is idempotent");
        write(a, "old A");
        write(b, "old B");
        fs::remove(c);
        check(rejects(
                  [&] { storage.save(batch, [](size_t) { throw std::runtime_error("crash"); }); }),
              "second partial save");
        write(b, "external B");
        check(rejects([&] { storage.finishSave(); }) && read(b) == "external B" && !fs::exists(c),
              "all files are checked before recovery; external bytes are never overwritten");
        write(b, "old B");
        check(storage.finishSave(), "recovery can resume after conflict resolution");
        const std::vector<paper::editor::StoredEdit> draft{{a, "new A", "draft A"}};
        storage.autosave(draft);
        check(read(a) == "new A" && storage.recovery().at(0).after == "draft A",
              "autosave only writes an isolated recovery snapshot");
        write(a, "external A");
        check(rejects([&] { (void)storage.recovery(); }), "autosave conflict retained");
        write(a, "new A");
        storage.discardRecovery();
        check(storage.recovery().empty(), "explicit discard clears recovery");
        check(rejects([&] { storage.save({{a, "new A", "one"}, {a, "new A", "two"}}); }),
              "duplicate targets rejected");
        check(rejects([&] { storage.save({{c, std::nullopt, "overwrite"}}); }),
              "new file never overwrites existing target");
        check(rejects(
                  [&] { storage.autosave({{root.parent_path() / "escape", std::nullopt, "x"}}); }),
              "paths cannot escape project");
        fs::create_directory_symlink(root, root / "alias");
        check(rejects([&] { storage.autosave({{root / "alias" / "a.dcscene", "new A", "x"}}); }),
              "symlink parents rejected");
        write(storage.directory() / "save.json", "{\"version\":99,\"entries\":[]}");
        check(rejects([&] { storage.finishSave(); }), "unknown journal version rejected");
        fs::remove(storage.directory() / "save.json");
        write(storage.directory() / "recovery.json",
              "{\"version\":1,\"entries\":[{\"path\":\"a.dcscene\",\"before\":\"?\",\"after\":\"?"
              "\"}]}");
        check(rejects([&] { (void)storage.recovery(); }), "corrupt byte encoding rejected");
        storage.discardRecovery();
        // A replacement failure after the journal is durable must retain recovery.
        fs::remove(c);
        check(rejects([&] {
                  storage.save({{a, "new A", "updated A"}, {c, std::nullopt, "C"}},
                               [&](size_t index) {
                                   if (index == 0)
                                       fs::create_directory(c);
                               });
              }),
              "replacement failure injected");
        check(storage.pendingSave() && read(a) == "updated A", "failure keeps durable intent");
        fs::remove(c);
        check(storage.finishSave() && read(c) == "C", "failed replacement retried");
        write(a, "old A");
        write(b, "old B");
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--crash-save", temporary.path()});
        check(child.waitForFinished(10000) && child.exitCode() == 73,
              "writer process actually exits without destructors after its first replacement");
        check(storage.pendingSave() && storage.finishSave() && read(a) == "new A" &&
                  read(b) == "new B",
              "actual process crash recovers the complete Save All");
        std::cout << "Editor storage checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
