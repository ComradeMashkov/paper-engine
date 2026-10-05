#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace paper::editor {
struct StoredEdit {
    std::filesystem::path file;
    // Absence means a new file, never permission to replace an existing file.
    std::optional<std::string> before;
    std::string after;
};
// The project lock must be held by the caller. A durable intent precedes every
// replacement. Recovery verifies the entire batch before completing any file.
class ProjectStorage {
  public:
    explicit ProjectStorage(std::filesystem::path project);
    void save(const std::vector<StoredEdit>& edits,
              const std::function<void(size_t)>& afterReplacement = {});
    bool finishSave();
    void autosave(const std::vector<StoredEdit>& edits);
    [[nodiscard]] std::vector<StoredEdit> recovery() const;
    void discardRecovery();
    [[nodiscard]] bool pendingSave() const;
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

  private:
    std::filesystem::path root_, directory_;
    void checkDirectory() const;
    std::filesystem::path checkedPath(const std::filesystem::path& path) const;
    std::vector<StoredEdit> read(const char* name) const;
    void write(const char* name, const std::vector<StoredEdit>& edits) const;
    void verify(const std::vector<StoredEdit>& edits, bool allowAfter) const;
};
} // namespace paper::editor
