#include "paper/core/text_layout.hpp"
#include "paper/core/utf8.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
int main() {
    try {
        const std::string_view text = "AЯ—😀";
        const std::array expected{U'A', U'Я', U'—', U'\U0001f600'};
        std::size_t offset = 0;
        for (const auto code : expected)
            check(paper::nextCodepoint(text, offset) == code,
                  "ASCII, Cyrillic and multibyte decoding");
        check(offset == text.size(), "decoder consumes complete characters");
        for (const std::string_view invalid :
             {"\x80", "\xc0\xaf", "\xe2\x82", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xff"}) {
            offset = 0;
            check(paper::nextCodepoint(invalid, offset) == U'\ufffd' && offset == 1,
                  "invalid or truncated sequence consumes only its first byte");
            while (offset < invalid.size()) {
                const auto before = offset;
                (void)paper::nextCodepoint(invalid, offset);
                check(offset > before && offset <= invalid.size(),
                      "malformed input always advances within bounds");
            }
        }
        const std::string_view interrupted = "\xe2"
                                             "A";
        offset = 0;
        check(paper::nextCodepoint(interrupted, offset) == U'\ufffd',
              "reject interrupted sequence");
        check(paper::nextCodepoint(interrupted, offset) == U'A', "keep the next valid character");
        const auto columns = [](std::string_view value) {
            float count = 0;
            for (size_t i = 0; i < value.size(); ++count)
                (void)paper::nextCodepoint(value, i);
            return count;
        };
        check(paper::wrapText("  Дом  жилой\tещё ", 9, columns) ==
                  std::vector<std::string>{"Дом жилой", "ещё"},
              "word wrapping ignores excess whitespace and fits exact width");
        check(paper::wrapText("дом\n\nя", 10, columns) == std::vector<std::string>{"дом", "", "я"},
              "explicit paragraph breaks preserve empty lines");
        check(paper::wrapText("Я😀дом", 2, columns) == std::vector<std::string>{"Я😀", "до", "м"},
              "long words split between complete UTF-8 codepoints");
        check(paper::wrapText("Я😀", 0, columns) == std::vector<std::string>{"Я", "😀"},
              "a glyph wider than the line still advances");
        check(paper::wrapText(" \t", 10, columns).empty(), "empty text has no occupied lines");
        const auto proportional = [](std::string_view value) {
            float width = 0;
            for (char ch : value)
                width += ch == 'W' ? 3.f : 1.f;
            return width;
        };
        check(paper::wrapText("WW iii", 5, proportional) ==
                  std::vector<std::string>{"W", "W", "iii"},
              "layout uses measured advance widths, not character count");
        std::cout << "UTF-8 and text layout boundary checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
