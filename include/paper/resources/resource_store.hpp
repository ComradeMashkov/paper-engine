#pragma once
#include "paper/render/scene.hpp"
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>

namespace paper {
namespace resourceLimits {
inline constexpr size_t cacheBytes = 256 * 1024 * 1024;
inline constexpr size_t fileBytes = 64 * 1024 * 1024;
inline constexpr size_t imagePixels = 16 * 1024 * 1024;
inline constexpr size_t modelTriangles = 500'000;
inline constexpr size_t modelNodes = 4096;
inline constexpr size_t skinJoints = 256, skinBindings = 4096;
inline constexpr size_t animationKeys = 100'000;
} // namespace resourceLimits
struct Resource {
    virtual ~Resource() = default;
    size_t byteSize = 0;
    std::vector<std::shared_ptr<const Resource>> dependencies;
};
struct FileResource final : Resource {
    std::filesystem::path path;
    std::filesystem::file_time_type modified;
    std::vector<std::uint8_t> data;
};
struct ImageResource final : Resource {
    PaintedTexture texture;
    SpriteLayout layout;
};
struct ModelResource;
struct ResourceStats {
    size_t liveBytes = 0, peakBytes = 0, cachedEntries = 0, loads = 0, hits = 0;
};
// Main-thread asset cache. Handles keep data/dependencies alive after eviction or
// store destruction. The shared ledger accounts for those outstanding handles.
class ResourceStore {
  public:
    explicit ResourceStore(std::filesystem::path root,
                           size_t budgetBytes = resourceLimits::cacheBytes);
    [[nodiscard]] std::filesystem::path resolve(const std::filesystem::path& relative) const;
    [[nodiscard]] std::shared_ptr<const FileResource> file(const std::filesystem::path& relative);
    [[nodiscard]] std::shared_ptr<const ImageResource> image(const std::filesystem::path& relative);
    [[nodiscard]] std::shared_ptr<const ModelResource> model(const std::filesystem::path& relative);
    [[nodiscard]] bool changed() const;
    [[nodiscard]] ResourceStats stats() const;
    void collect();
    template <class T> std::shared_ptr<const T> publish(std::string id, std::unique_ptr<T> value) {
        if (auto handle = cached<T>(id))
            return handle;
        return retain(std::move(id), std::move(value));
    }
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

  private:
    template <class T> std::shared_ptr<const T> cached(const std::string& key) {
        const auto i = cache_.find(key);
        if (i == cache_.end())
            return {};
        ++hits_;
        return std::dynamic_pointer_cast<const T>(i->second);
    }
    template <class T> std::shared_ptr<const T> retain(std::string key, std::unique_ptr<T> value) {
        collectFor(value->byteSize);
        if (value->byteSize > budgetBytes_ - ledger_->live)
            throw std::runtime_error("Resource memory budget exceeded: " + key);
        const auto bytes = value->byteSize;
        ledger_->live += bytes;
        ledger_->peak = std::max(ledger_->peak, ledger_->live);
        std::shared_ptr<const T> handle(value.release(), [ledger = ledger_, bytes](const T* data) {
            delete data;
            ledger->live -= bytes;
        });
        cache_.emplace(std::move(key), handle);
        ++loads_;
        return handle;
    }
    void collectFor(size_t bytes);
    struct Ledger {
        size_t live = 0, peak = 0;
    };
    std::filesystem::path root_;
    size_t budgetBytes_, loads_ = 0, hits_ = 0;
    std::shared_ptr<Ledger> ledger_ = std::make_shared<Ledger>();
    std::map<std::string, std::shared_ptr<const Resource>, std::less<>> cache_;
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, size_t>> files_;
};
} // namespace paper
