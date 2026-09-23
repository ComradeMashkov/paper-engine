#pragma once
#include <cstddef>
#include <string_view>

namespace paper {
namespace utf8Encoding {
// Unicode scalar ranges and UTF-8 continuation-byte bit layout.
inline constexpr unsigned asciiLimit = 0x80, continuationMask = 0xc0, continuationPrefix = 0x80,
                          continuationPayloadMask = 0x3f;
inline constexpr int continuationPayloadBits = 6;
inline constexpr char32_t twoByteMinimum = 0x80, threeByteMinimum = 0x800,
                          fourByteMinimum = 0x10000, surrogateFirst = 0xd800,
                          surrogateLast = 0xdfff, scalarMaximum = 0x10ffff;
inline constexpr unsigned twoByteFirst = 0xc2, twoByteLast = 0xdf, threeByteFirst = 0xe0,
                          threeByteLast = 0xef, fourByteFirst = 0xf0, fourByteLast = 0xf4;
} // namespace utf8Encoding
// Invalid input consumes one byte and uses the font's replacement glyph.
[[nodiscard]] inline char32_t nextCodepoint(std::string_view text, std::size_t& offset) noexcept {
    using namespace utf8Encoding;
    const auto first = static_cast<unsigned char>(text[offset++]);
    if (first < asciiLimit)
        return first;
    const int count = first >= twoByteFirst && first <= twoByteLast       ? 1
                      : first >= threeByteFirst && first <= threeByteLast ? 2
                      : first >= fourByteFirst && first <= fourByteLast   ? 3
                                                                          : 0;
    if (count == 0 || text.size() - offset < static_cast<std::size_t>(count))
        return U'\ufffd';
    char32_t code = first & ((1u << (continuationPayloadBits - count)) - 1u);
    for (int i = 0; i < count; ++i) {
        const auto byte = static_cast<unsigned char>(text[offset + i]);
        if ((byte & continuationMask) != continuationPrefix)
            return U'\ufffd';
        code = (code << continuationPayloadBits) | (byte & continuationPayloadMask);
    }
    // numbers: count is the number of continuation bytes (one, two or three).
    if ((count == 1 && code < twoByteMinimum) ||
        (count == 2 &&
         code < threeByteMinimum) || // numbers: UTF-8 uses one to three continuation bytes.
        (count == 3 && code < fourByteMinimum) ||
        (code >= surrogateFirst && code <= surrogateLast) ||
        code > scalarMaximum) // numbers: UTF-8 uses one to three continuation bytes.
        return U'\ufffd';
    offset += count;
    return code;
}
} // namespace paper
