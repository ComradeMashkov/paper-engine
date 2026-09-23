#pragma once
#include "paper/render/draw_types.hpp"
#include "paper/text/parameters.hpp"
#include <SDL3/SDL_render.h>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace paper {
enum class FontWeight { Regular, Bold, Handwritten, Count };
class TextRenderer {
  public:
    TextRenderer(SDL_Renderer& renderer, const std::filesystem::path& regular,
                 const std::filesystem::path& bold, const std::filesystem::path& handwritten);
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;
    TextRenderer(TextRenderer&&) = delete;
    TextRenderer& operator=(TextRenderer&&) = delete;
    void setRasterScale(float scale);
    void text(std::string_view value, float x, float y, int size, Color color,
              FontWeight weight = FontWeight::Regular);
    [[nodiscard]] float measure(std::string_view value, int size,
                                FontWeight weight = FontWeight::Regular) const;
    [[nodiscard]] float paragraphHeight(std::string_view value, float width, int size,
                                        float leading,
                                        FontWeight weight = FontWeight::Regular) const;
    float paragraph(std::string_view value, float x, float y, float width, int size, Color color,
                    float leading, FontWeight weight = FontWeight::Regular);

  private:
    struct Font;
    struct Face;
    Font& font(int size, FontWeight weight);
    SDL_Renderer& renderer_; // Borrowed; Engine destroys this cache before the renderer.
    std::array<std::unique_ptr<Face>, static_cast<size_t>(FontWeight::Count)> faces_;
    std::map<std::pair<int, FontWeight>, std::unique_ptr<Font>> fonts_;
    float rasterScale_ = 1;
};
} // namespace paper
