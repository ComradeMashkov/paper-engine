#include "paper/text/text_renderer.hpp"
#include "paper/core/pixel_format.hpp"
#include "paper/core/text_layout.hpp"
#include "paper/core/utf8.hpp"
#include "paper/sdl/resources.hpp"
#include "paper/text/parameters.hpp"
#include "stb_truetype.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

namespace paper {
using namespace textParameters;
namespace {
constexpr std::pair<int, int> ranges[] = {{32, 95},     {0xa0, 96},  {0x400, 96},
                                          {0x2013, 20}, {0x2116, 1}, {0x2190, 4}};
struct FontPack {
    stbtt_pack_context context{};
    FontPack(std::vector<unsigned char>& bitmap, int extent) {
        if (!stbtt_PackBegin(&context, bitmap.data(), extent, extent, 0, glyphPaddingPixels,
                             nullptr))
            throw std::runtime_error("Cannot create font atlas");
    }
    ~FontPack() noexcept { stbtt_PackEnd(&context); }
};
} // namespace
struct TextRenderer::Face {
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info{};
    int ascent = 0, descent = 0;
    explicit Face(const std::filesystem::path& file) {
        std::ifstream input(file, std::ios::binary);
        bytes = {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (bytes.empty() || !stbtt_InitFont(&info, bytes.data(), 0))
            throw std::runtime_error("Cannot read font: " + file.string());
        stbtt_GetFontVMetrics(&info, &ascent, &descent, nullptr);
    }
    int codepoint(char32_t cp) const {
        for (const auto& [first, count] : ranges)
            if (cp >= static_cast<char32_t>(first) && cp < static_cast<char32_t>(first + count) &&
                stbtt_FindGlyphIndex(&info, static_cast<int>(cp)))
                return static_cast<int>(cp);
        return '?';
    }
};
struct TextRenderer::Font {
    sdl::Texture atlas;
    std::unordered_map<int, stbtt_packedchar> glyphs;
    float scale = 1;
};
TextRenderer::TextRenderer(SDL_Renderer& renderer, const std::filesystem::path& regular,
                           const std::filesystem::path& bold,
                           const std::filesystem::path& handwritten)
    : renderer_(renderer), faces_{std::make_unique<Face>(regular), std::make_unique<Face>(bold),
                                  std::make_unique<Face>(handwritten)} {}
TextRenderer::~TextRenderer() = default;
void TextRenderer::setRasterScale(float scale) {
    // Buckets avoid rebuilding atlases on every pixel of a window drag.
    scale = std::clamp(std::ceil(scale * rasterBucketsPerUnit) / rasterBucketsPerUnit, 1.f,
                       maximumRasterScale);
    if (rasterScale_ != scale) {
        fonts_.clear();
        rasterScale_ = scale;
    }
}
TextRenderer::Font& TextRenderer::font(int size, FontWeight weight) {
    if (size <= 0 || size > maximumLogicalSizePixels)
        throw std::invalid_argument("Font size must be in 1..128");
    const auto key = std::pair{size, weight};
    if (auto found = fonts_.find(key); found != fonts_.end())
        return *found->second;
    const auto& face = *faces_[static_cast<size_t>(weight)];
    const float pixelSize = std::min(maximumRasterSizePixels, size * rasterScale_);
    for (int extent = initialAtlasPixels; extent <= maximumAtlasPixels;
         extent *= atlasGrowthFactor) {
        std::vector<unsigned char> bitmap(extent * extent);
        FontPack pack(bitmap, extent);
        auto f = std::make_unique<Font>();
        f->scale = pixelSize / size;
        bool fits = true;
        for (const auto& [first, count] : ranges) {
            std::vector<stbtt_packedchar> chars(count);
            if (!stbtt_PackFontRange(&pack.context, face.bytes.data(), 0,
                                     STBTT_POINT_SIZE(pixelSize), first, count, chars.data())) {
                fits = false;
                break;
            }
            for (int i = 0; i < count; ++i)
                f->glyphs[first + i] = chars[i];
        }
        if (!fits)
            continue;
        std::vector<unsigned char> rgba(extent * extent * pixelFormat::channels);
        for (int i = 0; i < extent * extent; ++i) {
            rgba[i * pixelFormat::channels + pixelFormat::red] =
                rgba[i * pixelFormat::channels + pixelFormat::green] =
                    rgba[i * pixelFormat::channels + pixelFormat::blue] =
                        pixelFormat::maximumChannel;
            rgba[i * pixelFormat::channels + pixelFormat::alpha] = bitmap[i];
        }
        f->atlas.reset(SDL_CreateTexture(&renderer_, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, extent, extent));
        if (!f->atlas)
            throw std::runtime_error(SDL_GetError());
        sdl::check(
            SDL_UpdateTexture(f->atlas.get(), nullptr, rgba.data(), extent * pixelFormat::channels),
            "Upload glyph atlas");
        sdl::check(SDL_SetTextureBlendMode(f->atlas.get(), SDL_BLENDMODE_BLEND),
                   "Set glyph blending");
        sdl::check(SDL_SetTextureScaleMode(f->atlas.get(), SDL_SCALEMODE_LINEAR),
                   "Set glyph sampling");
        if (fonts_.size() >= cachedFontLimit)
            fonts_.clear();
        auto& result = *f;
        fonts_.emplace(key, std::move(f));
        return result;
    }
    throw std::runtime_error("Font atlas too small");
}
void TextRenderer::text(std::string_view value, float x, float y, int size, Color color,
                        FontWeight weight) {
    if (value.empty())
        return;
    Font& f = font(size, weight);
    const auto& face = *faces_[static_cast<size_t>(weight)];
    const float em = stbtt_ScaleForMappingEmToPixels(&face.info, static_cast<float>(size));
    sdl::check(SDL_SetTextureColorMod(f.atlas.get(), color.r, color.g, color.b), "Set text color");
    sdl::check(SDL_SetTextureAlphaMod(f.atlas.get(), color.a), "Set text opacity");
    float start = x, baseline = y + face.ascent * em;
    int previous = 0;
    for (size_t offset = 0; offset < value.size();) {
        auto cp = nextCodepoint(value, offset);
        if (cp == '\n') {
            x = start;
            baseline += size * newlineLeading;
            previous = 0;
            continue;
        }
        const int current = face.codepoint(cp);
        if (previous)
            x += stbtt_GetCodepointKernAdvance(&face.info, previous, current) * em;
        const auto& g = f.glyphs.at(current);
        SDL_FRect src{static_cast<float>(g.x0), static_cast<float>(g.y0),
                      static_cast<float>(g.x1 - g.x0), static_cast<float>(g.y1 - g.y0)};
        SDL_FRect dst{x + g.xoff / f.scale, baseline + g.yoff / f.scale,
                      (g.xoff2 - g.xoff) / f.scale, (g.yoff2 - g.yoff) / f.scale};
        if (src.w > 0 && src.h > 0)
            sdl::check(SDL_RenderTexture(&renderer_, f.atlas.get(), &src, &dst), "Draw glyph");
        int advance = 0;
        stbtt_GetCodepointHMetrics(&face.info, current, &advance, nullptr);
        x += advance * em;
        previous = current;
    }
}
float TextRenderer::measure(std::string_view value, int size, FontWeight weight) const {
    const auto& face = *faces_[static_cast<size_t>(weight)];
    const float em = stbtt_ScaleForMappingEmToPixels(&face.info, static_cast<float>(size));
    float x = 0, maximum = 0;
    int previous = 0;
    for (size_t offset = 0; offset < value.size();) {
        auto cp = nextCodepoint(value, offset);
        if (cp == '\n') {
            maximum = std::max(maximum, x);
            x = 0;
            previous = 0;
            continue;
        }
        const int current = face.codepoint(cp);
        int advance = 0;
        stbtt_GetCodepointHMetrics(&face.info, current, &advance, nullptr);
        x += advance * em;
        if (previous)
            x += stbtt_GetCodepointKernAdvance(&face.info, previous, current) * em;
        previous = current;
    }
    return std::max(maximum, x);
}
float TextRenderer::paragraphHeight(std::string_view value, float width, int size, float leading,
                                    FontWeight weight) const {
    return wrapText(value, width, [&](std::string_view row) { return measure(row, size, weight); })
               .size() *
           size * leading;
}
float TextRenderer::paragraph(std::string_view value, float x, float y, float width, int size,
                              Color color, float leading, FontWeight weight) {
    for (const auto& row : wrapText(
             value, width, [&](std::string_view text) { return measure(text, size, weight); })) {
        text(row, x, y, size, color, weight);
        y += size * leading;
    }
    return y;
}
} // namespace paper
