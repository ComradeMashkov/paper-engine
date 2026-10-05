#include "project_migration.hpp"
#include "paper/content/document.hpp"
#include "paper/scenes/transforms.hpp"
#include <QTemporaryDir>
#include <fstream>
namespace paper::editor {
namespace {
namespace fs = std::filesystem;
constexpr uintmax_t maximumCopyBytes = uintmax_t{4} * 1024 * 1024 * 1024;
constexpr size_t maximumCopyFiles = 100000;
const std::array<std::string_view, 4> formats{"dcmo.world", "dcmo.scene", "dcmo.templates",
                                              "dcmo.resources"};
bool inside(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}
void write(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.write(text.data(), static_cast<std::streamsize>(text.size())) || !file.flush())
        throw std::runtime_error("Cannot write migrated project copy");
}
void convert(ContentValue& node) {
    if (node.is_object()) {
        if (node.contains("yaw")) {
            const auto t = readTransform(node);
            const auto q = t.rotation;
            node.erase("yaw");
            node["rotation"] = ContentValue::array({q.x, q.y, q.z, q.w});
        }
        if (node.contains("scale") && node.at("scale").is_number()) {
            const auto value = node.at("scale");
            node["scale"] = ContentValue::array({value, value, value});
        }
        if (node.contains("remove"))
            for (auto& key : node["remove"])
                if (key == "yaw")
                    key = "rotation";
        for (auto& [key, child] : node.items())
            convert(child);
    } else if (node.is_array())
        for (auto& child : node)
            convert(child);
}
} // namespace
std::filesystem::path migrateProject(const fs::path& descriptor, const fs::path& destination,
                                     const SceneDocuments& overrides,
                                     const std::function<void(const fs::path&)>& validate) {
    const auto source = fs::canonical(descriptor), target = fs::weakly_canonical(destination);
    auto spec = content::read(source);
    if (spec.at("format") != "paper.project" || spec.at("version") != 1)
        throw std::runtime_error("Unsupported project descriptor");
    ResourceStore project(source.parent_path());
    const auto assets = project.resolve(spec.at("assets").get<std::string>());
    if (fs::exists(target) || inside(target, assets) || inside(target, source.parent_path()))
        throw std::runtime_error("Choose a new project folder outside the original project");
    size_t count = 0;
    uintmax_t bytes = 0;
    for (const auto& entry : fs::recursive_directory_iterator(assets)) {
        if (entry.is_symlink() || (!entry.is_directory() && !entry.is_regular_file()))
            throw std::runtime_error("Project migration does not copy symlinks or special files");
        if (++count > maximumCopyFiles)
            throw std::runtime_error("Project migration exceeds file budget");
        if (entry.is_regular_file()) {
            const auto size = entry.file_size();
            if (size > maximumCopyBytes - bytes)
                throw std::runtime_error("Project migration exceeds byte budget");
            bytes += size;
        }
    }
    fs::create_directories(target.parent_path());
    QTemporaryDir temporary(
        QString::fromStdU16String((target.parent_path() / ".paper-migration-XXXXXX").u16string()));
    if (!temporary.isValid())
        throw std::runtime_error("Cannot prepare project migration");
    const auto staging = fs::path(temporary.path().toStdU16String()) / "project";
    fs::create_directories(staging);
    fs::copy(assets, staging / "assets",
             fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    ResourceStore stagedFiles(staging / "assets");
    for (const auto& [relative, data] : overrides) {
        const auto file = stagedFiles.resolve(relative);
        const auto extension = file.extension();
        if (extension != ".dcscene" && extension != ".dcworld")
            throw std::runtime_error("Unexpected authored migration override");
        fs::create_directories(file.parent_path());
        write(file, content::encode(data));
    }
    for (const auto& entry : fs::recursive_directory_iterator(staging / "assets")) {
        if (entry.is_symlink())
            throw std::runtime_error("Symlink appeared during migration");
        if (!entry.is_regular_file())
            continue;
        const auto extension = entry.path().extension();
        if (extension != ".dcscene" && extension != ".dcworld" && extension != ".dctemplates" &&
            extension != ".dcresources")
            continue;
        const auto relative = entry.path().lexically_relative(staging / "assets");
        auto data =
            overrides.contains(relative) ? overrides.at(relative) : content::read(entry.path());
        if (!data.contains("format") ||
            std::ranges::find(formats, data.at("format").get<std::string>()) == formats.end() ||
            !data.at("version").is_number_integer() ||
            (data.at("version") != content::limits::legacySceneVersion &&
             data.at("version") != content::limits::sceneVersion))
            throw std::runtime_error("Unsupported native document during migration");
        data["version"] = content::limits::sceneVersion;
        if (extension == ".dcscene")
            for (auto& node : data["scene"]["nodes"])
                convert(node);
        if (extension == ".dctemplates")
            convert(data["templates"]);
        write(entry.path(), content::encode(data));
    }
    spec["assets"] = "assets";
    const auto projectFile = staging / source.filename();
    write(projectFile, content::encode(spec));
    if (validate)
        validate(projectFile);
    if (fs::exists(target))
        throw std::runtime_error("Migration destination appeared during preparation");
    fs::rename(staging, target);
    return target / source.filename();
}
} // namespace paper::editor
