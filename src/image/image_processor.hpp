#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace acecode::image {

inline constexpr std::size_t kImageCompressionThresholdBytes = 10u * 1024u * 1024u;
inline constexpr int kImageNormalizeMaxEdge = 2560;
inline constexpr std::size_t kImageNormalizeFinalMaxBytes = 25u * 1024u * 1024u;
inline constexpr std::size_t kImageNormalizeMaxPixels = 80u * 1000u * 1000u;

struct ImageInfo {
    int width = 0;
    int height = 0;
    int channels = 0;
    bool has_alpha = false;
};

struct ImageNormalizeOptions {
    std::size_t compression_threshold_bytes = kImageCompressionThresholdBytes;
    std::size_t final_max_bytes = kImageNormalizeFinalMaxBytes;
    std::size_t max_pixels = kImageNormalizeMaxPixels;
    int max_edge = kImageNormalizeMaxEdge;
    int jpeg_quality = 85;
    bool force = false;
};

struct ImageNormalizeResult {
    bool attempted = false;
    bool ok = false;
    bool changed = false;
    std::string bytes;
    std::string mime_type;
    ImageInfo input;
    ImageInfo output;
    std::string backend;
    std::string reason;
    std::string error;
};

bool is_supported_raster_image_mime(std::string_view mime_type);

std::optional<ImageInfo> probe_image_info(std::string_view bytes,
                                          std::string* error = nullptr);

ImageNormalizeResult normalize_image_bytes(
    std::string_view bytes,
    std::string_view mime_type,
    const ImageNormalizeOptions& options = {});

} // namespace acecode::image
