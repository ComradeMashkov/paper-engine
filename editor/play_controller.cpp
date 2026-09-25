#include "play_controller.hpp"
#include "paper/content/document.hpp"
#include "paper/resources/resource_store.hpp"
#include <QDir>
#include <QFile>
#include <QtConcurrentRun>
#include <fstream>

namespace paper::editor {
namespace {
namespace fs = std::filesystem;
// Editor-owned limits for copying and stopping a child, independent of gameplay.
constexpr uintmax_t snapshotBytesLimit = 4ULL * 1024 * 1024 * 1024;
constexpr size_t snapshotFilesLimit = 100000;
constexpr int gracefulStopMilliseconds = 3000, terminateMilliseconds = 1000;
constexpr qint64 logChunkBytes = 16 * 1024;
QString pathText(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}
void write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file)
        throw std::runtime_error("Cannot write Play snapshot: " + path.string());
}
void verify(const PlayInput& input) {
    ResourceStore files(input.root);
    for (const auto& [relative, expected] : input.expected) {
        QFile file(pathText(files.resolve(relative)));
        if (!file.open(QIODevice::ReadOnly) ||
            file.size() != static_cast<qint64>(expected.size()) ||
            file.readAll().toStdString() != expected)
            throw std::runtime_error(
                "Project files changed outside the editor. Reopen before Play.");
    }
}
using Stamp = std::pair<uintmax_t, fs::file_time_type>;
std::map<fs::path, Stamp> inventory(const fs::path& root,
                                    const std::shared_ptr<std::atomic_bool>& canceled) {
    std::map<fs::path, Stamp> result;
    uintmax_t bytes = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (*canceled)
            throw std::runtime_error("Play canceled.");
        if (entry.is_symlink())
            throw std::runtime_error("Play snapshots require regular files, not symlinks: " +
                                     entry.path().string());
        if (entry.is_directory())
            continue;
        if (!entry.is_regular_file())
            throw std::runtime_error("Unsupported file in Play snapshot: " + entry.path().string());
        const auto size = entry.file_size();
        if (size > snapshotBytesLimit - bytes || result.size() >= snapshotFilesLimit)
            throw std::runtime_error("Play snapshot exceeds the 4 GiB / 100,000 file limit.");
        bytes += size;
        result.emplace(entry.path().lexically_relative(root), Stamp{size, entry.last_write_time()});
    }
    return result;
}
PlaySnapshot prepare(PlayInput input, std::shared_ptr<std::atomic_bool> canceled) {
    PlaySnapshot result;
    try {
        result.directory = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/paper-play-XXXXXX");
        if (!result.directory->isValid())
            throw std::runtime_error("Cannot create temporary Play directory.");
        const fs::path directory(result.directory->path().toStdU16String());
        const auto assets = directory / "assets";
        fs::create_directories(assets);
        fs::create_directory(directory / "state");
        verify(input);
        const auto before = inventory(input.root, canceled);
        ResourceStore source(input.root), destination(assets);
        for (const auto& [relative, stamp] : before) {
            if (*canceled)
                throw std::runtime_error("Play canceled.");
            const auto target = destination.resolve(relative);
            fs::create_directories(target.parent_path());
            fs::copy_file(source.resolve(relative), target);
        }
        if (inventory(input.root, canceled) != before)
            throw std::runtime_error("Assets changed while preparing Play. Try again.");
        verify(input);
        for (const auto& [relative, bytes] : input.overrides)
            write(destination.resolve(relative), bytes);
        // Fixed child paths keep temporary persistence separate from authored assets.
        write(directory / "session.paperplay",
              content::encode(ContentValue{{"format", "paper.play"},
                                           {"version", 1},
                                           {"world", input.manifest.generic_string()}}));
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
        result.directory.reset();
    }
    return result;
}
} // namespace
PlayController::PlayController(QObject* parent) : QObject(parent) {
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        if (process_.state() == QProcess::NotRunning)
            return;
        if (!terminated_) {
            log("Runtime did not respond to Stop; requesting process termination.");
            terminated_ = true;
            process_.terminate();
            deadline_.start(terminateMilliseconds);
        } else {
            log("Runtime did not terminate; killing the owned Play process.");
            process_.kill();
        }
    });
    connect(&process_, &QProcess::started, this, [this] {
        if (state_ == State::Stopping)
            stop();
        else {
            log("Play started. Escape releases the game cursor; return to the editor for Stop.");
            state(State::Running);
        }
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { drain(); });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        drain();
        log(QString("Play ended: %1 (exit %2).")
                .arg(status == QProcess::NormalExit ? "normal exit" : "process terminated")
                .arg(code));
        finish();
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        log("Runtime error: " + process_.errorString());
        if (error == QProcess::FailedToStart)
            finish();
    });
    connect(&worker_, &QFutureWatcher<PlaySnapshot>::finished, this, [this] {
        snapshot_ = worker_.result();
        if (*canceled_ || !snapshot_.error.isEmpty()) {
            log(*canceled_ ? "Play canceled." : snapshot_.error);
            finish();
            return;
        }
        process_.setWorkingDirectory(snapshot_.directory->path());
        process_.setProcessChannelMode(QProcess::MergedChannels);
        state(State::Starting);
        auto args = arguments_;
        args << "--paper-play" << snapshot_.directory->filePath("session.paperplay");
        process_.start(executable_, args);
    });
}
PlayController::~PlayController() {
    if (canceled_)
        *canceled_ = true;
    // Normal close is asynchronous. This fallback handles direct owner destruction.
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(terminateMilliseconds);
    }
}
void PlayController::start(PlayInput input, const fs::path& executable,
                           const std::vector<std::string>& arguments) {
    if (active())
        return;
    if (!executable.is_absolute() || !fs::is_regular_file(executable)) {
        log("Play runtime is missing. Build the host game's executable first.");
        return;
    }
    executable_ = pathText(executable);
    arguments_.clear();
    for (const auto& argument : arguments)
        arguments_.push_back(QString::fromStdString(argument));
    canceled_ = std::make_shared<std::atomic_bool>(false);
    terminated_ = false;
    log("Preparing an isolated copy with all unsaved scene edits…");
    state(State::Preparing);
    worker_.setFuture(QtConcurrent::run(prepare, std::move(input), canceled_));
}
void PlayController::stop() {
    if (!active())
        return;
    *canceled_ = true;
    state(State::Stopping);
    if (snapshot_.directory && process_.state() != QProcess::NotRunning) {
        QFile request(snapshot_.directory->filePath("stop.request"));
        if (!request.open(QIODevice::WriteOnly))
            log("Cannot write Stop request; process termination will be used.");
        request.close();
        deadline_.start(gracefulStopMilliseconds);
    }
}
void PlayController::finish() {
    deadline_.stop();
    if (snapshot_.directory && !snapshot_.directory->remove())
        log("Could not remove the temporary Play directory: " + snapshot_.directory->path());
    snapshot_ = {};
    state(State::Idle);
}
QString PlayController::sessionDirectory() const {
    return snapshot_.directory ? snapshot_.directory->path() : QString{};
}
void PlayController::state(State value) {
    state_ = value;
    if (changed)
        changed(value);
}
void PlayController::log(const QString& value) {
    if (output)
        output(value);
}
void PlayController::drain() {
    while (process_.bytesAvailable())
        log(QString::fromUtf8(process_.read(logChunkBytes)));
}
} // namespace paper::editor
