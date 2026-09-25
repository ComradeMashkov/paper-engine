#pragma once
#include <QFutureWatcher>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace paper::editor {
struct PlayInput {
    std::filesystem::path root, manifest;
    // Paths relative to root. Expected bytes detect external authoring changes;
    // overrides include unsaved scenes and the selected entry spawn.
    std::map<std::filesystem::path, std::string> expected, overrides;
};
struct PlaySnapshot {
    std::shared_ptr<QTemporaryDir> directory;
    QString error;
};
class PlayController final : public QObject {
  public:
    enum class State { Idle, Preparing, Starting, Running, Stopping };
    explicit PlayController(QObject* parent = nullptr);
    ~PlayController() override;
    void start(PlayInput input, const std::filesystem::path& executable,
               const std::vector<std::string>& arguments);
    void stop();
    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] bool active() const { return state_ != State::Idle; }
    [[nodiscard]] QString sessionDirectory() const;
    std::function<void(State)> changed;
    std::function<void(QString)> output;

  private:
    void state(State value);
    void finish();
    void log(const QString& value);
    void drain();
    State state_ = State::Idle;
    QProcess process_;
    QTimer deadline_;
    QFutureWatcher<PlaySnapshot> worker_;
    PlaySnapshot snapshot_;
    QString executable_;
    QStringList arguments_;
    std::shared_ptr<std::atomic_bool> canceled_;
    bool terminated_ = false;
};
} // namespace paper::editor
