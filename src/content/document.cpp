#include "paper/content/document.hpp"
#include <fstream>
#include <sstream>
#include <tomlplusplus/toml.hpp>

namespace paper::content {
static_assert(TOML_MAX_NESTED_VALUES == limits::nesting);
namespace {
void count(size_t depth, size_t& values) {
    if (depth >= limits::nesting || ++values > limits::values)
        throw std::runtime_error("Content nesting/value limit exceeded");
}
ContentValue convert(const toml::node& node, size_t depth, size_t& values) {
    count(depth, values);
    if (const auto* table = node.as_table()) {
        auto result = ContentValue::object();
        for (const auto& [key, child] : *table)
            result[key.str()] = convert(child, depth + 1, values);
        return result;
    }
    if (const auto* array = node.as_array()) {
        auto result = ContentValue::array();
        for (const auto& child : *array)
            result.push_back(convert(child, depth + 1, values));
        return result;
    }
    if (const auto* value = node.as_string())
        return value->get();
    if (const auto* value = node.as_integer())
        return value->get();
    if (const auto* value = node.as_boolean())
        return value->get();
    if (const auto* value = node.as_floating_point()) {
        if (!std::isfinite(value->get()))
            throw std::runtime_error("Non-finite content number");
        return value->get();
    }
    throw std::runtime_error("Dates and times are not supported content values");
}
std::unique_ptr<toml::node> convert(const ContentValue& value, size_t depth, size_t& values) {
    count(depth, values);
    if (value.is_object()) {
        auto result = std::make_unique<toml::table>();
        for (const auto& [key, child] : value.items()) {
            auto node = convert(child, depth + 1, values);
            node->visit([&](auto&& typed) { result->insert(key, std::move(typed)); });
        }
        return result;
    }
    if (value.is_array()) {
        auto result = std::make_unique<toml::array>();
        for (const auto& child : value) {
            auto node = convert(child, depth + 1, values);
            node->visit([&](auto&& typed) { result->push_back(std::move(typed)); });
        }
        return result;
    }
    if (value.is_string())
        return std::make_unique<toml::value<std::string>>(value.get<std::string>());
    if (value.is_boolean())
        return std::make_unique<toml::value<bool>>(value.get<bool>());
    if (value.is_number_integer())
        return std::make_unique<toml::value<std::int64_t>>(value.get<std::int64_t>());
    if (value.is_number()) {
        const auto number = value.get<double>();
        if (!std::isfinite(number))
            throw std::runtime_error("Non-finite content number");
        return std::make_unique<toml::value<double>>(number);
    }
    throw std::runtime_error("Null cannot be serialized as TOML");
}
} // namespace
ContentValue parse(std::string_view source, std::string_view name) {
    if (source.size() > limits::fileBytes)
        throw std::runtime_error("Content file byte limit exceeded");
    try {
        const auto table = toml::parse(source, name);
        size_t values = 0;
        return convert(table, 0, values);
    } catch (const toml::parse_error& error) {
        std::ostringstream message;
        message << error;
        throw std::runtime_error(message.str());
    }
}
ContentValue read(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > limits::fileBytes)
        throw std::runtime_error("Content file byte limit exceeded: " + path.string());
    std::ifstream stream(path, std::ios::binary);
    std::string source(static_cast<size_t>(size), '\0');
    if (!stream.read(source.data(), static_cast<std::streamsize>(size)) ||
        stream.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Cannot read stable content file: " + path.string());
    return parse(source, path.string());
}
std::string encode(const ContentValue& value) {
    if (!value.is_object())
        throw std::invalid_argument("TOML document root must be a table");
    size_t values = 0;
    const auto document = convert(value, 0, values);
    std::ostringstream stream;
    stream << toml::toml_formatter(*document->as_table()) << '\n';
    auto result = stream.str();
    if (result.size() > limits::fileBytes)
        throw std::runtime_error("Encoded content byte limit exceeded");
    if (parse(result, "<encoded content>") != value)
        throw std::runtime_error("TOML encoding did not preserve content values");
    return result;
}
} // namespace paper::content
