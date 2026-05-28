#include "image_processor.hpp"

#include "../utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <sstream>
#include <vector>

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

namespace acecode::image {

namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

void set_error(std::string* error, std::string value) {
    if (error) *error = std::move(value);
}

bool has_real_alpha(const unsigned char* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) return false;
    const std::size_t count = static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < count; ++i) {
        if (pixels[i * 4u + 3u] != 255) return true;
    }
    return false;
}

void write_bytes_callback(void* context, void* data, int size) {
    if (!context || !data || size <= 0) return;
    auto* out = static_cast<std::string*>(context);
    out->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

std::string encode_png(const unsigned char* pixels, int width, int height) {
    std::string out;
    const int ok = stbi_write_png_to_func(
        write_bytes_callback,
        &out,
        width,
        height,
        4,
        pixels,
        width * 4);
    if (!ok) out.clear();
    return out;
}

std::string encode_jpeg(const unsigned char* pixels,
                        int width,
                        int height,
                        int quality,
                        std::size_t target_bytes) {
    const int qualities[] = {
        quality,
        std::min(quality, 80),
        std::min(quality, 75),
        std::min(quality, 70),
    };
    std::string best;
    for (int q : qualities) {
        std::string out;
        const int ok = stbi_write_jpg_to_func(
            write_bytes_callback,
            &out,
            width,
            height,
            4,
            pixels,
            q);
        if (!ok || out.empty()) continue;
        if (best.empty() || out.size() < best.size()) best = out;
        if (out.size() <= target_bytes) return out;
    }
    return best;
}

std::string summarize_dimensions(const ImageInfo& info) {
    if (info.width <= 0 || info.height <= 0) return "unknown";
    std::ostringstream oss;
    oss << info.width << "x" << info.height << " channels=" << info.channels;
    return oss.str();
}

} // namespace

bool is_supported_raster_image_mime(std::string_view mime_type) {
    const std::string mime = ascii_lower(std::string(mime_type));
    if (mime.empty()) return true;
    if (mime == "image/svg+xml") return false;
    if (mime == "image/gif") return false; // Avoid flattening animated GIFs.
    if (mime == "image/webp") return false; // stb_image does not decode WebP.
    return mime.rfind("image/", 0) == 0;
}

std::optional<ImageInfo> probe_image_info(std::string_view bytes,
                                          std::string* error) {
    if (bytes.empty()) {
        set_error(error, "image data is empty");
        return std::nullopt;
    }
    if (bytes.size() > static_cast<std::size_t>(INT_MAX)) {
        set_error(error, "image data too large to decode");
        return std::nullopt;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    const int ok = stbi_info_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()),
        &width,
        &height,
        &channels);
    if (!ok || width <= 0 || height <= 0) {
        const char* reason = stbi_failure_reason();
        set_error(error, reason ? reason : "unsupported image format");
        return std::nullopt;
    }
    ImageInfo info;
    info.width = width;
    info.height = height;
    info.channels = channels;
    info.has_alpha = channels == 2 || channels == 4;
    return info;
}

ImageNormalizeResult normalize_image_bytes(
    std::string_view bytes,
    std::string_view mime_type,
    const ImageNormalizeOptions& options) {
    ImageNormalizeResult result;
    result.backend = "stb";
    result.mime_type = std::string(mime_type);

    if (!is_supported_raster_image_mime(mime_type)) {
        result.attempted = bytes.size() >= options.compression_threshold_bytes ||
            options.force;
        result.reason = "unsupported mime";
        result.error = "unsupported image mime for stb normalization";
        if (result.attempted) {
            LOG_WARN("[image-normalize] unsupported mime"
                     " size=" + std::to_string(bytes.size()) +
                     " mime=" + std::string(mime_type));
        }
        return result;
    }

    std::string probe_error;
    auto info = probe_image_info(bytes, &probe_error);
    if (!info.has_value()) {
        result.attempted = bytes.size() >= options.compression_threshold_bytes ||
            options.force;
        result.error = probe_error.empty() ? "unsupported image format" : probe_error;
        if (result.attempted) {
            LOG_WARN("[image-normalize] probe failed"
                     " size=" + std::to_string(bytes.size()) +
                     " mime=" + std::string(mime_type) +
                     " error=" + result.error);
        }
        return result;
    }
    result.input = *info;

    const int max_dim = std::max(info->width, info->height);
    const bool size_trigger = bytes.size() >= options.compression_threshold_bytes;
    const bool dimension_trigger = options.max_edge > 0 && max_dim > options.max_edge;
    if (!options.force && !size_trigger && !dimension_trigger) {
        result.ok = true;
        result.reason = "below thresholds";
        result.output = result.input;
        return result;
    }

    result.attempted = true;

    const std::size_t pixel_count = static_cast<std::size_t>(info->width) *
        static_cast<std::size_t>(info->height);
    if (pixel_count > options.max_pixels) {
        result.error = "image pixel count exceeds normalization cap";
        LOG_WARN("[image-normalize] reject huge image"
                 " size=" + std::to_string(bytes.size()) +
                 " dims=" + summarize_dimensions(*info) +
                 " max_pixels=" + std::to_string(options.max_pixels));
        return result;
    }

    LOG_INFO("[image-normalize] start"
             " backend=stb"
             " size=" + std::to_string(bytes.size()) +
             " threshold=" + std::to_string(options.compression_threshold_bytes) +
             " mime=" + std::string(mime_type) +
             " dims=" + summarize_dimensions(*info) +
             " size_trigger=" + std::string(size_trigger ? "1" : "0") +
             " dimension_trigger=" + std::string(dimension_trigger ? "1" : "0"));

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()),
        &width,
        &height,
        &channels,
        4);
    if (!decoded || width <= 0 || height <= 0) {
        const char* reason = stbi_failure_reason();
        result.error = reason ? reason : "image decode failed";
        if (decoded) stbi_image_free(decoded);
        LOG_WARN("[image-normalize] decode failed error=" + result.error);
        return result;
    }

    const bool alpha = has_real_alpha(decoded, width, height);
    int out_width = width;
    int out_height = height;
    if (dimension_trigger) {
        const double scale = static_cast<double>(options.max_edge) /
            static_cast<double>(std::max(width, height));
        out_width = std::max(1, static_cast<int>(std::round(width * scale)));
        out_height = std::max(1, static_cast<int>(std::round(height * scale)));
    }

    std::vector<unsigned char> resized;
    const unsigned char* output_pixels = decoded;
    if (out_width != width || out_height != height) {
        resized.resize(static_cast<std::size_t>(out_width) *
                       static_cast<std::size_t>(out_height) * 4u);
        unsigned char* resize_ok = stbir_resize_uint8_srgb(
            decoded,
            width,
            height,
            0,
            resized.data(),
            out_width,
            out_height,
            0,
            STBIR_RGBA);
        if (!resize_ok) {
            stbi_image_free(decoded);
            result.error = "image resize failed";
            LOG_WARN("[image-normalize] resize failed");
            return result;
        }
        output_pixels = resized.data();
    }

    std::string out_mime;
    std::string encoded;
    if (alpha) {
        out_mime = "image/png";
        encoded = encode_png(output_pixels, out_width, out_height);
    } else {
        out_mime = "image/jpeg";
        encoded = encode_jpeg(output_pixels,
                              out_width,
                              out_height,
                              options.jpeg_quality,
                              options.compression_threshold_bytes);
    }
    stbi_image_free(decoded);

    if (encoded.empty()) {
        result.error = "image encode failed";
        LOG_WARN("[image-normalize] encode failed");
        return result;
    }
    if (encoded.size() > options.final_max_bytes) {
        result.error = "normalized image still exceeds hard cap";
        LOG_WARN("[image-normalize] normalized image too large"
                 " output_size=" + std::to_string(encoded.size()) +
                 " final_max=" + std::to_string(options.final_max_bytes));
        return result;
    }
    if (!dimension_trigger &&
        encoded.size() >= bytes.size() &&
        bytes.size() <= options.final_max_bytes) {
        result.ok = true;
        result.changed = false;
        result.reason = "normalized output was not smaller";
        result.output = {out_width, out_height, 4, alpha};
        LOG_INFO("[image-normalize] keep original"
                 " reason=encoded-not-smaller"
                 " original_size=" + std::to_string(bytes.size()) +
                 " encoded_size=" + std::to_string(encoded.size()) +
                 " output_dims=" + summarize_dimensions(result.output));
        return result;
    }

    result.ok = true;
    result.changed = true;
    result.bytes = std::move(encoded);
    result.mime_type = out_mime;
    result.output = {out_width, out_height, 4, alpha};
    result.reason = size_trigger && dimension_trigger
        ? "size and dimensions exceeded thresholds"
        : (size_trigger ? "size exceeded threshold" : "dimensions exceeded threshold");

    LOG_INFO("[image-normalize] changed"
             " backend=stb"
             " original_size=" + std::to_string(bytes.size()) +
             " output_size=" + std::to_string(result.bytes.size()) +
             " original_mime=" + std::string(mime_type) +
             " output_mime=" + result.mime_type +
             " input_dims=" + summarize_dimensions(result.input) +
             " output_dims=" + summarize_dimensions(result.output));
    return result;
}

} // namespace acecode::image
