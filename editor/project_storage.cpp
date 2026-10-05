#include "project_storage.hpp"
#include "paper/content/document.hpp"
#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <set>

namespace paper::editor {
namespace {
namespace fs = std::filesystem;
// Bound decoding and retained source bytes independently of the content parser.
constexpr qint64 maximumJournalBytes = 192 * 1024 * 1024;
constexpr size_t maximumStoredBytes = 128 * 1024 * 1024, maximumEdits = 256;
constexpr int storageVersion = 1;
constexpr qsizetype journalFields = 2, entryFields = 3;
QString text(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}
std::optional<std::string> disk(const fs::path& path, qint64 limit = content::limits::fileBytes) {
    if (!fs::exists(path))
        return std::nullopt;
    if (!fs::is_regular_file(fs::symlink_status(path)))
        throw std::runtime_error("Editor storage requires regular files");
    QFile file(text(path));
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit)
        throw std::runtime_error("Cannot read bounded editor storage: " + path.string());
    const auto value = file.readAll();
    if (file.error() != QFileDevice::NoError)
        throw std::runtime_error(file.errorString().toStdString());
    return value.toStdString();
}
void replace(const fs::path& path, const std::string& value) {
    QSaveFile file(text(path));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(value.data(), static_cast<qint64>(value.size())) !=
            static_cast<qint64>(value.size()) ||
        !file.commit())
        throw std::runtime_error("Cannot atomically write " + path.string() + ": " +
                                 file.errorString().toStdString());
}
QString encode(const std::string& value) {
    return QString::fromLatin1(QByteArray::fromStdString(value).toBase64());
}
std::string decode(const QJsonValue& value) {
    if (!value.isString())
        throw std::runtime_error("Malformed recovery bytes");
    const auto encoded = value.toString().toLatin1();
    const auto decoded =
        QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > static_cast<qint64>(content::limits::fileBytes) ||
        decoded.decoded.toBase64() != encoded)
        throw std::runtime_error("Malformed or oversized recovery bytes");
    return decoded.decoded.toStdString();
}
void clearFile(const fs::path& path) {
    if (fs::exists(path) && !QFile::remove(text(path)))
        throw std::runtime_error("Cannot clear editor recovery: " + path.string());
}
} // namespace
ProjectStorage::ProjectStorage(fs::path project) {
    project = fs::canonical(project);
    root_ = project.parent_path();
    const auto key = QCryptographicHash::hash(text(project).toUtf8(), QCryptographicHash::Sha256);
    directory_ = root_ / ".paper-editor" / key.toHex().toStdString();
    checkDirectory();
}
void ProjectStorage::checkDirectory() const {
    for (const auto& path : {root_ / ".paper-editor", directory_}) {
        const auto status = fs::symlink_status(path);
        if (fs::exists(status) && !fs::is_directory(status))
            throw std::runtime_error("Unsafe editor recovery directory");
    }
}
fs::path ProjectStorage::checkedPath(const fs::path& file) const {
    auto path = file.is_absolute() ? file.lexically_normal() : (root_ / file).lexically_normal();
    const auto relative = path.lexically_relative(root_);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
        *relative.begin() == ".paper-editor")
        throw std::runtime_error("Recovery file escapes project");
    auto walk = root_;
    for (const auto& part : relative) {
        if (part.string().starts_with('.'))
            throw std::runtime_error("Recovery cannot target hidden project state");
        walk /= part;
        if (fs::is_symlink(fs::symlink_status(walk)))
            throw std::runtime_error("Recovery cannot follow symlinks");
    }
    if (!fs::is_directory(path.parent_path()))
        throw std::runtime_error("Recovery parent directory is missing");
    return path;
}
void ProjectStorage::verify(const std::vector<StoredEdit>& edits, bool allowAfter) const {
    if (edits.size() > maximumEdits)
        throw std::runtime_error("Too many recovery files");
    size_t total = 0;
    std::set<fs::path> paths;
    for (const auto& edit : edits) {
        const auto path = checkedPath(edit.file);
        if (!paths.insert(path).second)
            throw std::runtime_error("Duplicate recovery file");
        total += edit.after.size() + (edit.before ? edit.before->size() : 0);
        if (total > maximumStoredBytes || edit.after.size() > content::limits::fileBytes ||
            (edit.before && edit.before->size() > content::limits::fileBytes))
            throw std::runtime_error("Recovery byte budget exceeded");
        const auto current = disk(path);
        if (current != edit.before && !(allowAfter && current == edit.after))
            throw std::runtime_error("File changed outside the editor: " + path.string() +
                                     "; recovery has been retained");
    }
}
void ProjectStorage::write(const char* name, const std::vector<StoredEdit>& edits) const {
    checkDirectory();
    QJsonArray entries;
    for (const auto& edit : edits) {
        QJsonObject entry;
        entry["path"] = text(checkedPath(edit.file).lexically_relative(root_));
        entry["before"] = edit.before ? QJsonValue(encode(*edit.before)) : QJsonValue::Null;
        entry["after"] = encode(edit.after);
        entries.push_back(entry);
    }
    const auto encoded =
        QJsonDocument(QJsonObject{{"version", storageVersion}, {"entries", entries}})
            .toJson(QJsonDocument::Compact);
    if (encoded.size() > maximumJournalBytes)
        throw std::runtime_error("Editor journal byte budget exceeded");
    fs::create_directories(directory_);
    checkDirectory();
    replace(directory_ / name, encoded.toStdString());
}
std::vector<StoredEdit> ProjectStorage::read(const char* name) const {
    checkDirectory();
    const auto source = disk(directory_ / name, maximumJournalBytes);
    if (!source)
        return {};
    QJsonParseError error;
    const auto parsed = QJsonDocument::fromJson(QByteArray::fromStdString(*source), &error);
    const auto object = parsed.object();
    if (error.error != QJsonParseError::NoError || !parsed.isObject() ||
        object.size() != journalFields || object.value("version") != storageVersion ||
        !object.value("entries").isArray())
        throw std::runtime_error("Unsupported or malformed editor journal");
    const auto entries = object.value("entries").toArray();
    if (entries.size() > static_cast<qsizetype>(maximumEdits))
        throw std::runtime_error("Too many recovery entries");
    std::vector<StoredEdit> result;
    size_t total = 0;
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        if (!value.isObject() || entry.size() != entryFields || !entry.value("path").isString() ||
            !entry.contains("before") || !entry.contains("after"))
            throw std::runtime_error("Malformed recovery entry");
        const fs::path relative(entry.value("path").toString().toStdU16String());
        if (relative.is_absolute())
            throw std::runtime_error("Absolute path in recovery journal");
        StoredEdit edit{checkedPath(relative), std::nullopt, decode(entry.value("after"))};
        if (!entry.value("before").isNull())
            edit.before = decode(entry.value("before"));
        total += edit.after.size() + (edit.before ? edit.before->size() : 0);
        if (total > maximumStoredBytes)
            throw std::runtime_error("Recovery byte budget exceeded");
        result.push_back(std::move(edit));
    }
    return result;
}
bool ProjectStorage::pendingSave() const {
    checkDirectory();
    return fs::exists(directory_ / "save.json");
}
bool ProjectStorage::finishSave() {
    if (!pendingSave())
        return false;
    const auto edits = read("save.json");
    verify(edits, true);
    for (const auto& edit : edits) {
        const auto path = checkedPath(edit.file);
        if (disk(path) != edit.after)
            replace(path, edit.after);
    }
    clearFile(directory_ / "save.json");
    return true;
}
void ProjectStorage::save(const std::vector<StoredEdit>& edits,
                          const std::function<void(size_t)>& afterReplacement) {
    if (pendingSave())
        throw std::runtime_error("A pending Save All must be recovered before saving");
    verify(edits, false);
    if (edits.empty())
        return;
    write("save.json", edits);
    // Recheck every dependency after persisting intent, before the first replacement.
    verify(edits, false);
    for (size_t i = 0; i < edits.size(); ++i) {
        const auto& edit = edits[i];
        const auto path = checkedPath(edit.file);
        if (disk(path) != edit.before)
            throw std::runtime_error("File changed during Save All; journal retained");
        if (edit.before != edit.after)
            replace(path, edit.after);
        if (afterReplacement)
            afterReplacement(i);
    }
    clearFile(directory_ / "save.json");
}
void ProjectStorage::autosave(const std::vector<StoredEdit>& edits) {
    verify(edits, false);
    if (edits.empty()) {
        discardRecovery();
        return;
    }
    write("recovery.json", edits);
}
std::vector<StoredEdit> ProjectStorage::recovery() const {
    auto edits = read("recovery.json");
    verify(edits, true);
    std::erase_if(edits, [](const auto& edit) {
        return edit.before != edit.after && disk(edit.file) == edit.after;
    });
    if (std::ranges::none_of(edits, [](const auto& edit) { return edit.before != edit.after; }))
        return {};
    return edits;
}
void ProjectStorage::discardRecovery() {
    checkDirectory();
    clearFile(directory_ / "recovery.json");
}
} // namespace paper::editor
