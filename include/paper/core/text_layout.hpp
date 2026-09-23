#pragma once
#include "paper/core/utf8.hpp"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace paper {
// Shared measurement/drawing layout. Long words break only between UTF-8 codepoints.
template <class Measure>
std::vector<std::string> wrapText(std::string_view text, float width, Measure measure) {
    std::vector<std::string> lines;
    std::string row, word;
    const auto appendWord = [&]() {
        if (word.empty())
            return;
        if (!row.empty() && measure(row + " " + word) <= width) {
            row += " " + word;
            word.clear();
            return;
        }
        if (!row.empty()) {
            lines.push_back(std::move(row));
            row.clear();
        }
        for (size_t offset = 0; offset < word.size();) {
            const size_t first = offset;
            (void)nextCodepoint(word, offset);
            const auto glyph = std::string_view(word).substr(first, offset - first);
            if (!row.empty() && measure(row + std::string(glyph)) > width) {
                lines.push_back(std::move(row));
                row.clear();
            }
            row += glyph;
        }
        word.clear();
    };
    for (char ch : text) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            appendWord();
            if (ch == '\n') {
                lines.push_back(std::move(row));
                row.clear();
            }
        } else
            word += ch;
    }
    appendWord();
    if (!row.empty())
        lines.push_back(std::move(row));
    return lines;
}
} // namespace paper
