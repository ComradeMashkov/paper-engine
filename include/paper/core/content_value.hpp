#pragma once
#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace paper {
// Format-independent, owning data exchanged with Lua and content validators.
// There is deliberately no text parser or serializer on this value type.
class ContentValue {
  public:
    using Array = std::vector<ContentValue>;
    using Table = std::map<std::string, ContentValue, std::less<>>;
    ContentValue() = default;
    ContentValue(std::nullptr_t) {}
    ContentValue(bool value) : data_(value) {}
    ContentValue(const char* value) : data_(std::string(value)) {}
    ContentValue(std::string value) : data_(std::move(value)) {}
    ContentValue(std::string_view value) : data_(std::string(value)) {}
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    ContentValue(T value) {
        if (!std::in_range<std::int64_t>(value))
            throw std::out_of_range("Content integer outside signed 64-bit range");
        data_ = static_cast<std::int64_t>(value);
    }
    template <std::floating_point T> ContentValue(T value) : data_(static_cast<double>(value)) {}
    explicit ContentValue(Array value) : data_(std::move(value)) {}
    explicit ContentValue(Table value) : data_(std::move(value)) {}
    // Convenience for authored fixtures: pairs form a table; other lists form arrays.
    ContentValue(std::initializer_list<ContentValue> values) {
        constexpr size_t pairSize = 2;
        if (std::ranges::all_of(values, [](const auto& entry) {
                return entry.is_array() && entry.size() == pairSize && entry.at(0).is_string();
            })) {
            Table table;
            for (const auto& entry : values)
                if (!table.emplace(entry.at(0).get<std::string>(), entry.at(1)).second)
                    throw std::invalid_argument("Duplicate content table key");
            data_ = std::move(table);
        } else
            data_ = Array(values);
    }
    static ContentValue array(std::initializer_list<ContentValue> values = {}) {
        return ContentValue(Array(values));
    }
    static ContentValue object() { return ContentValue(Table{}); }
    bool is_null() const { return std::holds_alternative<std::monostate>(data_); }
    bool is_object() const { return std::holds_alternative<Table>(data_); }
    bool is_array() const { return std::holds_alternative<Array>(data_); }
    bool is_string() const { return std::holds_alternative<std::string>(data_); }
    bool is_boolean() const { return std::holds_alternative<bool>(data_); }
    bool is_number_integer() const { return std::holds_alternative<std::int64_t>(data_); }
    bool is_number() const { return is_number_integer() || std::holds_alternative<double>(data_); }
    size_t size() const {
        if (is_object())
            return items().size();
        if (is_array())
            return elements().size();
        if (is_string())
            return std::get<std::string>(data_).size();
        return is_null() ? 0 : 1;
    }
    bool empty() const { return size() == 0; }
    bool contains(std::string_view key) const { return is_object() && items().contains(key); }
    Table& items() { return storage<Table>(); }
    const Table& items() const { return storage<Table>(); }
    Array& elements() { return storage<Array>(); }
    const Array& elements() const { return storage<Array>(); }
    auto begin() { return elements().begin(); }
    auto end() { return elements().end(); }
    auto begin() const { return elements().begin(); }
    auto end() const { return elements().end(); }
    ContentValue& at(std::string_view key) {
        const auto it = items().find(key);
        if (it == items().end())
            throw std::out_of_range("Missing content field: " + std::string(key));
        return it->second;
    }
    const ContentValue& at(std::string_view key) const {
        const auto it = items().find(key);
        if (it == items().end())
            throw std::out_of_range("Missing content field: " + std::string(key));
        return it->second;
    }
    ContentValue& at(size_t index) { return elements().at(index); }
    const ContentValue& at(size_t index) const { return elements().at(index); }
    ContentValue& operator[](std::string_view key) {
        if (is_null())
            data_ = Table{};
        return items()[std::string(key)];
    }
    const ContentValue& operator[](std::string_view key) const { return at(key); }
    ContentValue& operator[](size_t index) { return at(index); }
    const ContentValue& operator[](size_t index) const { return at(index); }
    ContentValue& front() { return elements().at(0); }
    const ContentValue& front() const { return elements().at(0); }
    ContentValue& back() { return elements().at(size() - 1); }
    const ContentValue& back() const { return elements().at(size() - 1); }
    void push_back(ContentValue value) { elements().push_back(std::move(value)); }
    void erase(std::string_view key) { items().erase(std::string(key)); }
    void erase(size_t index) {
        (void)elements().at(index);
        elements().erase(elements().begin() + static_cast<Array::difference_type>(index));
    }
    template <class T> T get() const {
        if constexpr (std::same_as<T, ContentValue>)
            return *this;
        else if constexpr (std::same_as<T, std::string> || std::same_as<T, bool>)
            return storage<T>();
        else if constexpr (std::integral<T>) {
            const auto value = storage<std::int64_t>();
            if (!std::in_range<T>(value))
                throw std::out_of_range("Content integer narrowing");
            return static_cast<T>(value);
        } else if constexpr (std::floating_point<T>) {
            if (!is_number())
                throw std::invalid_argument("Expected content number");
            const double value = is_number_integer()
                                     ? static_cast<double>(std::get<std::int64_t>(data_))
                                     : std::get<double>(data_);
            if (std::isfinite(value) && std::abs(value) > std::numeric_limits<T>::max())
                throw std::out_of_range("Content floating-point narrowing");
            return static_cast<T>(value);
        } else
            static_assert(!sizeof(T), "Unsupported content conversion");
    }
    template <class T> T value(std::string_view key, T fallback) const {
        (void)items();
        return contains(key) ? at(key).get<T>() : fallback;
    }
    std::string value(std::string_view key, const char* fallback) const {
        return value(key, std::string(fallback));
    }
    // Tables overlay recursively; arrays/scalars replace. Deletion is an explicit
    // scene-schema operation, never an implicit special null value.
    void overlay(const ContentValue& patch) {
        if (!patch.is_object()) {
            *this = patch;
            return;
        }
        if (!is_object())
            data_ = Table{};
        for (const auto& [key, value] : patch.items()) {
            if (value.is_null())
                throw std::invalid_argument("Null is not a content override");
            (*this)[key].overlay(value);
        }
    }
    friend bool operator==(const ContentValue&, const ContentValue&) = default;
    template <class T>
        requires std::is_arithmetic_v<T>
    bool operator<(T value) const {
        return numeric() < static_cast<long double>(value);
    }
    template <class T>
        requires std::is_arithmetic_v<T>
    bool operator<=(T value) const {
        return numeric() <= static_cast<long double>(value);
    }
    template <class T>
        requires std::is_arithmetic_v<T>
    bool operator>(T value) const {
        return numeric() > static_cast<long double>(value);
    }
    template <class T>
        requires std::is_arithmetic_v<T>
    bool operator>=(T value) const {
        return numeric() >= static_cast<long double>(value);
    }

  private:
    std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Table> data_;
    template <class T> T& storage() {
        if (auto* value = std::get_if<T>(&data_))
            return *value;
        throw std::invalid_argument("Unexpected content value type");
    }
    template <class T> const T& storage() const {
        if (const auto* value = std::get_if<T>(&data_))
            return *value;
        throw std::invalid_argument("Unexpected content value type");
    }
    long double numeric() const {
        if (is_number_integer())
            return static_cast<long double>(std::get<std::int64_t>(data_));
        if (std::holds_alternative<double>(data_))
            return std::get<double>(data_);
        throw std::invalid_argument("Expected content number");
    }
};
} // namespace paper
