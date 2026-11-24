// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/allsky/allsky_image_analysis.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

// stb_image, compiled into this file only and with every symbol static: no decoder symbol is
// exported, so no libjpeg build already in the process (gphoto, a camera SDK) can collide with it.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 16384
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace alpacacore::vendor::allsky {

namespace {

using Plane = std::vector<float>;

// Separable 1-D pass along rows (horizontal) or columns, each output pixel from a window of radius r.
template <typename Op>
Plane pass(const Plane& in, int w, int h, int r, bool horizontal, Op op) {
    Plane out(in.size());
    std::vector<float> line;
    const int n = horizontal ? w : h;
    const int lines = horizontal ? h : w;
    line.resize(static_cast<std::size_t>(n));
    std::vector<float> res(static_cast<std::size_t>(n));
    for (int l = 0; l < lines; ++l) {
        for (int i = 0; i < n; ++i) {
            line[static_cast<std::size_t>(i)] =
                in[horizontal ? static_cast<std::size_t>(l) * w + i : static_cast<std::size_t>(i) * w + l];
        }
        op(line, res, r);
        for (int i = 0; i < n; ++i) {
            out[horizontal ? static_cast<std::size_t>(l) * w + i : static_cast<std::size_t>(i) * w + l] =
                res[static_cast<std::size_t>(i)];
        }
    }
    return out;
}

// Edges are clamped (nearest), as scipy.ndimage's default "reflect" nearly does.
void line_min(const std::vector<float>& in, std::vector<float>& out, int r) {
    const int n = static_cast<int>(in.size());
    for (int i = 0; i < n; ++i) {
        float m = in[static_cast<std::size_t>(i)];
        for (int k = std::max(0, i - r); k <= std::min(n - 1, i + r); ++k) m = std::min(m, in[static_cast<std::size_t>(k)]);
        out[static_cast<std::size_t>(i)] = m;
    }
}
void line_max(const std::vector<float>& in, std::vector<float>& out, int r) {
    const int n = static_cast<int>(in.size());
    for (int i = 0; i < n; ++i) {
        float m = in[static_cast<std::size_t>(i)];
        for (int k = std::max(0, i - r); k <= std::min(n - 1, i + r); ++k) m = std::max(m, in[static_cast<std::size_t>(k)]);
        out[static_cast<std::size_t>(i)] = m;
    }
}
void line_mean(const std::vector<float>& in, std::vector<float>& out, int r) {
    const int n = static_cast<int>(in.size());
    double sum = 0.0;
    auto at = [&](int k) { return static_cast<double>(in[static_cast<std::size_t>(std::clamp(k, 0, n - 1))]); };
    for (int k = -r; k <= r; ++k) sum += at(k);
    for (int i = 0; i < n; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<float>(sum / (2 * r + 1));
        sum += at(i + r + 1) - at(i - r);
    }
}
void line_gauss1(const std::vector<float>& in, std::vector<float>& out, int /*r*/) {
    // sigma = 1, truncated at 3: weights for offsets 0..3.
    static const float kW[4] = {0.39905f, 0.24203f, 0.05400f, 0.00443f};
    const int n = static_cast<int>(in.size());
    for (int i = 0; i < n; ++i) {
        float s = kW[0] * in[static_cast<std::size_t>(i)];
        for (int k = 1; k <= 3; ++k) {
            s += kW[k] * (in[static_cast<std::size_t>(std::clamp(i - k, 0, n - 1))] +
                          in[static_cast<std::size_t>(std::clamp(i + k, 0, n - 1))]);
        }
        out[static_cast<std::size_t>(i)] = s;
    }
}

template <typename Op>
Plane filter2d(const Plane& in, int w, int h, int r, Op op) {
    return pass(pass(in, w, h, r, true, op), w, h, r, false, op);
}

double median_of(std::vector<float> v) {
    if (v.empty()) return 0.0;
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

}  // namespace

GrayImage decode_allsky_jpeg(const std::string& jpeg_bytes) {
    // A frame fetched while the camera is still writing it ends before its EOI marker. A decoder may
    // fill the rest grey, which would read as a starless (cloudy) sky: refuse it instead.
    std::size_t end = jpeg_bytes.size();
    while (end > 0 && (jpeg_bytes[end - 1] == '\0' || jpeg_bytes[end - 1] == '\n' || jpeg_bytes[end - 1] == '\r')) --end;
    if (end < 4 || static_cast<unsigned char>(jpeg_bytes[0]) != 0xFF || static_cast<unsigned char>(jpeg_bytes[1]) != 0xD8 ||
        static_cast<unsigned char>(jpeg_bytes[end - 2]) != 0xFF || static_cast<unsigned char>(jpeg_bytes[end - 1]) != 0xD9) {
        throw AlpacaException("Allsky image is incomplete or not a JPEG (caught mid-write?)", AlpacaError::DriverException);
    }
    int w = 0;
    int h = 0;
    int components = 0;
    std::unique_ptr<stbi_uc, void (*)(void*)> full(
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(jpeg_bytes.data()), static_cast<int>(end), &w, &h,
                              &components, 1),
        stbi_image_free);
    if (!full) {
        throw AlpacaException(std::string("Allsky image is not a readable JPEG: ") + stbi_failure_reason(),
                              AlpacaError::DriverException);
    }
    // Half size, each output pixel the mean of a 2x2 block (as the libjpeg 1/2 DCT scaling did).
    GrayImage image;
    image.width = w / 2;
    image.height = h / 2;
    image.pixels.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height));
    const stbi_uc* src = full.get();
    for (int y = 0; y < image.height; ++y) {
        const stbi_uc* r0 = src + static_cast<std::size_t>(2 * y) * w;
        const stbi_uc* r1 = r0 + w;
        std::uint8_t* out = image.pixels.data() + static_cast<std::size_t>(y) * image.width;
        for (int x = 0; x < image.width; ++x) {
            out[x] = static_cast<std::uint8_t>((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) / 4);
        }
    }
    return image;
}

AllskyAnalysis analyse_allsky_image(const GrayImage& image, const AllskyRoi& roi) {
    AllskyAnalysis result;
    const int w = image.width;
    const int h = image.height;
    if (w < 32 || h < 32 || image.pixels.size() != static_cast<std::size_t>(w) * h) return result;

    Plane raw(image.pixels.begin(), image.pixels.end());
    const Plane im = filter2d(raw, w, h, 3, line_gauss1);
    // Grey opening (erosion then dilation, 7 px) removes the stars; the box smooths what is left.
    const Plane opened = filter2d(filter2d(im, w, h, 3, line_min), w, h, 3, line_max);
    const Plane bg = filter2d(opened, w, h, 7, line_mean);
    Plane th(im.size());
    for (std::size_t i = 0; i < im.size(); ++i) th[i] = im[i] - bg[i];

    // Saturated areas (moon, glare, daylight), grown by 12 px each way.
    Plane sat(im.size());
    for (std::size_t i = 0; i < im.size(); ++i) sat[i] = im[i] > 240.0f ? 1.0f : 0.0f;
    const Plane sat_grown = filter2d(sat, w, h, 12, line_max);
    const Plane local_max = filter2d(th, w, h, 3, line_max);

    const double cx = roi.center_x * w;
    const double cy = roi.center_y * h;
    const double r = roi.radius * h;
    std::vector<float> roi_th;
    std::vector<float> roi_sky;
    std::size_t roi_n = 0;
    std::size_t roi_sat = 0;
    std::vector<std::size_t> roi_index;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double dx = x - cx;
            const double dy = y - cy;
            if (dx * dx + dy * dy >= r * r) continue;
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            roi_index.push_back(i);
            roi_th.push_back(th[i]);
            roi_sky.push_back(im[i]);
            ++roi_n;
            if (sat[i] > 0.0f) ++roi_sat;
        }
    }
    if (roi_n == 0) return result;
    const double med = median_of(roi_th);
    std::vector<float> dev(roi_th.size());
    for (std::size_t k = 0; k < roi_th.size(); ++k) dev[k] = static_cast<float>(std::fabs(roi_th[k] - med));
    const double sigma = 1.4826 * median_of(dev) + 0.2;
    const double threshold = std::max(8.0 * sigma, 3.0);
    int stars = 0;
    for (std::size_t i : roi_index) {
        if (sat_grown[i] > 0.0f) continue;
        if (th[i] > threshold && th[i] >= local_max[i]) ++stars;
    }
    result.stars = stars;
    result.sky_median = median_of(roi_sky);
    result.noise = sigma;
    result.saturated = static_cast<double>(roi_sat) / static_cast<double>(roi_n);
    return result;
}

double allsky_cloud_cover(int stars, int clear_sky_stars) {
    if (clear_sky_stars <= 0) return 100.0;
    const double clear = std::clamp(static_cast<double>(stars) / clear_sky_stars, 0.0, 1.0);
    return std::round(100.0 * (1.0 - clear));
}

double sun_altitude_deg(std::chrono::system_clock::time_point when, double latitude, double longitude) {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kDeg = kPi / 180.0;
    const double unix_s = std::chrono::duration<double>(when.time_since_epoch()).count();
    const double jd = unix_s / 86400.0 + 2440587.5;
    const double n = jd - 2451545.0;
    const double L = std::fmod(280.460 + 0.9856474 * n, 360.0);
    const double g = std::fmod(357.528 + 0.9856003 * n, 360.0) * kDeg;
    const double lambda = (L + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * kDeg;
    const double eps = (23.439 - 0.0000004 * n) * kDeg;
    const double ra = std::atan2(std::cos(eps) * std::sin(lambda), std::cos(lambda));
    const double dec = std::asin(std::sin(eps) * std::sin(lambda));
    const double gmst_h = std::fmod(18.697374558 + 24.06570982441908 * n, 24.0);
    const double lst = (gmst_h * 15.0 + longitude) * kDeg;
    const double ha = lst - ra;
    const double lat = latitude * kDeg;
    const double alt = std::asin(std::sin(lat) * std::sin(dec) + std::cos(lat) * std::cos(dec) * std::cos(ha));
    return alt / kDeg;
}

}  // namespace alpacacore::vendor::allsky
