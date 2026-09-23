#include "paper/content/document.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace paper;
namespace {
void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void rejects(F&& operation, const char* message) {
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}
void documents() {
    const auto value = content::parse(R"toml(
# Comments, quoted dotted IDs, nested tables and arrays retain their meanings.
locale = "ru"
flag = true
integer = 1
floating = 1.0
empty = []
[blank]
[strings]
"dialogue.key" = "Ключ\n\"номер\" \\ путь"
[[nodes]]
id = "parent"
position = [0.0, 1.0, -1.0]
[nodes.bounds]
center = [0, 0, 0]
[[nodes]]
id = "child"
parent = "parent"
)toml",
                                      "fixture.toml");
    check(value.at("strings").at("dialogue.key").get<std::string>() == "Ключ\n\"номер\" \\ путь",
          "Unicode, escapes and dotted IDs must survive decoding");
    check(value.at("integer").is_number_integer() && !value.at("floating").is_number_integer(),
          "integer and floating-point values must remain distinct");
    check(value.at("blank").is_object() && value.at("empty").is_array(),
          "empty tables must not become arrays");
    check(content::parse(content::encode(value), "roundtrip.toml") == value,
          "canonical encoding must preserve every typed value and nested array table");
    for (const auto source : {R"({"version":1})", "a=1\na=1", "a=nan", "a=inf", "a=1979-05-27",
                              "a=07:32:00", "a=9223372036854775808", "a=[1,", "[a]\n[a]"})
        rejects([&] { (void)content::parse(source, "bad.toml"); }, "invalid content accepted");
    constexpr unsigned char invalidUtf8 = 0xff;
    const auto invalid = "a=\"" + std::string(1, static_cast<char>(invalidUtf8)) + "\"";
    rejects([&] { (void)content::parse(invalid, "utf8.toml"); }, "invalid UTF-8 accepted");
    const auto deep = "a=" + std::string(content::limits::nesting, '[') + "0" +
                      std::string(content::limits::nesting, ']');
    rejects([&] { (void)content::parse(deep, "deep.toml"); }, "excessive nesting accepted");
    rejects([] { (void)content::encode(ContentValue{{"missing", nullptr}}); },
            "null cannot be written to TOML");
    rejects(
        [] {
            (void)content::encode(ContentValue{{"bad", std::numeric_limits<double>::infinity()}});
        },
        "infinity cannot be written to TOML");
    try {
        (void)content::parse("value=", "diagnostic.toml");
        throw std::logic_error("malformed document accepted");
    } catch (const std::runtime_error& error) {
        check(std::string(error.what()).find("diagnostic.toml") != std::string::npos,
              "syntax diagnostic must identify the source file");
    }
}
void typedValues() {
    rejects([] { (void)ContentValue(true).get<int>(); }, "bool is not an integer");
    rejects([] { (void)ContentValue(1.0).get<int>(); }, "float must not silently truncate");
    rejects([] { (void)ContentValue(-1).get<unsigned>(); }, "negative integer must not wrap");
    rejects([] { (void)ContentValue(std::numeric_limits<std::uint64_t>::max()); },
            "unsigned integer overflow must be rejected");
    rejects([] { (void)ContentValue(std::numeric_limits<double>::max()).get<float>(); },
            "floating-point narrowing must not overflow");
    ContentValue base{{"table", {{"keep", true}, {"change", false}}},
                      {"array", ContentValue::array({"old", "second"})}};
    const auto original = base;
    base.overlay(ContentValue{{"table", {{"change", true}}}, {"array", ContentValue::array()}});
    check(base.at("table").at("keep").get<bool>() && base.at("table").at("change").get<bool>() &&
              base.at("array").empty() && !original.at("table").at("change").get<bool>(),
          "overrides must recursively merge tables, replace arrays and leave snapshots intact");
}
} // namespace
int main() {
    try {
        documents();
        typedValues();
        std::cout << "Native document checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
