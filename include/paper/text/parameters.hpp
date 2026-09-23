#pragma once

namespace paper::textParameters {
inline constexpr int glyphPaddingPixels = 2;
inline constexpr float rasterBucketsPerUnit = 2, maximumRasterScale = 4;
inline constexpr int maximumLogicalSizePixels = 128;
inline constexpr float maximumRasterSizePixels = 192;
inline constexpr int initialAtlasPixels = 512, maximumAtlasPixels = 4096, atlasGrowthFactor = 2;
inline constexpr unsigned cachedFontLimit = 16;
inline constexpr float newlineLeading = 1.35f, paragraphLeading = 1.5f;
} // namespace paper::textParameters
