#include "paper/resources/resource_store.hpp"
#include "paper/core/pixel_format.hpp"
#include "paper/resources/model.hpp"
#include "stb_image.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace paper {
ResourceStore::ResourceStore(std::filesystem::path root, size_t budgetBytes)
    : root_(std::filesystem::weakly_canonical(root)), budgetBytes_(budgetBytes) {
    if (!budgetBytes_)
        throw std::invalid_argument("Resource budget must be positive");
}
std::filesystem::path ResourceStore::resolve(const std::filesystem::path& relative) const {
    if (relative.empty() || relative.is_absolute())
        throw std::runtime_error("Expected a relative asset path: " + relative.string());
    const auto path = std::filesystem::weakly_canonical(root_ / relative);
    const auto inside = path.lexically_relative(root_);
    if (inside.empty() || *inside.begin() == "..")
        throw std::runtime_error("Asset path escapes resource root: " + relative.string());
    return path;
}
void ResourceStore::collectFor(size_t bytes) {
    bool removed = true;
    while (removed && (bytes > budgetBytes_ || ledger_->live > budgetBytes_ - bytes)) {
        removed = false;
        for (auto i = cache_.begin(); i != cache_.end();) {
            if (i->second.use_count() == 1) {
                i = cache_.erase(i);
                removed = true;
            } else
                ++i;
        }
    }
}
void ResourceStore::collect() {
    bool removed = true;
    while (removed) {
        removed = false;
        for (auto i = cache_.begin(); i != cache_.end();) {
            if (i->second.use_count() == 1) {
                i = cache_.erase(i);
                removed = true;
            } else
                ++i;
        }
    }
}
ResourceStats ResourceStore::stats() const {
    return {ledger_->live, ledger_->peak, cache_.size(), loads_, hits_};
}
bool ResourceStore::changed() const {
    for (const auto& [path, fingerprint] : files_) {
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(path, error);
        if (error || stamp != fingerprint.first)
            return true;
        const auto bytes = std::filesystem::file_size(path, error);
        if (error || bytes != fingerprint.second)
            return true;
    }
    return false;
}
std::shared_ptr<const FileResource> ResourceStore::file(const std::filesystem::path& relative) {
    const auto path = resolve(relative);
    const std::string key = "file:" + path.string();
    if (auto handle = cached<FileResource>(key))
        return handle;
    const auto bytes = std::filesystem::file_size(path);
    if (bytes > resourceLimits::fileBytes || bytes > budgetBytes_)
        throw std::runtime_error("Asset exceeds file byte limit: " + path.string());
    collectFor(static_cast<size_t>(bytes));
    if (bytes > budgetBytes_ - ledger_->live)
        throw std::runtime_error("Resource memory budget exceeded before reading: " +
                                 path.string());
    auto value = std::make_unique<FileResource>();
    value->path = path;
    value->modified = std::filesystem::last_write_time(path);
    value->data.resize(static_cast<size_t>(bytes));
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(value->data.data()),
                    static_cast<std::streamsize>(bytes)))
        throw std::runtime_error("Cannot read resource: " + path.string());
    value->byteSize = value->data.size();
    files_[path] = {value->modified, value->data.size()};
    return retain(key, std::move(value));
}
std::shared_ptr<const ImageResource> ResourceStore::image(const std::filesystem::path& relative) {
    const std::string key = "image:" + resolve(relative).string();
    if (auto handle = cached<ImageResource>(key))
        return handle;
    auto source = file(relative);
    int width = 0, height = 0, channels = 0;
    const auto length = static_cast<int>(source->data.size());
    if (!stbi_info_from_memory(source->data.data(), length, &width, &height, &channels) ||
        width <= 0 || height <= 0 ||
        static_cast<size_t>(width) > resourceLimits::imagePixels / height ||
        static_cast<size_t>(width) * height > budgetBytes_ / sizeof(Pixel))
        throw std::runtime_error("Invalid image or pixel budget exceeded: " + relative.string());
    const size_t decodedBytes = static_cast<size_t>(width) * height * sizeof(Pixel);
    collectFor(decodedBytes);
    if (decodedBytes > budgetBytes_ - ledger_->live)
        throw std::runtime_error("Resource memory budget exceeded before decoding: " +
                                 relative.string());
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(source->data.data(), length, &width, &height, &channels,
                              pixelFormat::channels),
        stbi_image_free);
    if (!pixels)
        throw std::runtime_error("Cannot decode image: " + relative.string());
    auto value = std::make_unique<ImageResource>();
    auto& texture = value->texture;
    texture.width = width;
    texture.height = height;
    texture.pixels.resize(static_cast<size_t>(width) * height);
    int first = height, last = -1;
    for (size_t i = 0; i < texture.pixels.size(); ++i) {
        texture.pixels[i] = {pixels.get()[i * pixelFormat::channels + pixelFormat::red],
                             pixels.get()[i * pixelFormat::channels + pixelFormat::green],
                             pixels.get()[i * pixelFormat::channels + pixelFormat::blue],
                             pixels.get()[i * pixelFormat::channels + pixelFormat::alpha]};
        if (texture.pixels[i].a >= pixelFormat::opaqueAlpha) {
            const int row = static_cast<int>(i / width);
            first = std::min(first, row);
            last = std::max(last, row);
        }
    }
    if (last >= first)
        value->layout = {static_cast<float>(first) / height, static_cast<float>(last + 1) / height,
                         static_cast<float>(width) / (last - first + 1)};
    value->byteSize = texture.pixels.size() * sizeof(Pixel);
    value->dependencies.push_back(std::move(source));
    return retain(key, std::move(value));
}
std::shared_ptr<const ModelResource> ResourceStore::model(const std::filesystem::path& relative) {
    const std::string key = "model:" + resolve(relative).string();
    if (auto handle = cached<ModelResource>(key))
        return handle;
    try {
        return retain(key, loadGltf(*this, relative));
    } catch (const std::exception& error) {
        throw std::runtime_error(relative.string() + ": " + error.what());
    }
}
} // namespace paper
