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

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace alpacacore::vendor::allsky {

/// An 8-bit grayscale image, row-major.
struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

/// The region the stars are counted in: a circle, as fractions of the image
/// (centre x of the width, centre y and radius of the height). The defaults fit
/// a fisheye whose sky circle fills the frame height: they leave out the horizon
/// trees, the overlay text in the corners and the edge of the dome.
struct AllskyRoi {
    double center_x = 0.47;
    double center_y = 0.50;
    double radius = 0.36;
};

struct AllskyAnalysis {
    int stars = 0;              ///< point sources found in the circle
    double sky_median = 0.0;    ///< median sky level in the circle, 0..255 (after the 2x reduction)
    double noise = 0.0;         ///< robust noise of the background-subtracted circle
    double saturated = 0.0;     ///< fraction of the circle that is saturated (moon glare, daylight)
};

/// Decode a JPEG to grayscale, reduced 2x by the decoder (a 2028x1520 frame becomes
/// 1014x760). Throws AlpacaException (DriverException) for data that is not a JPEG.
GrayImage decode_allsky_jpeg(const std::string& jpeg_bytes);

/// Count stars: smooth (Gaussian, sigma 1 px) to drop single-pixel noise and hot pixels,
/// subtract a star-free background (grey opening 7 px, then a 15 px box), and count the
/// local maxima (7x7) that stand 8 robust sigma above it, inside the circle and away
/// from saturated areas (the moon and its glare).
AllskyAnalysis analyse_allsky_image(const GrayImage& image, const AllskyRoi& roi = {});

/// Cloud cover (%) from a star count, relative to the count a clear moonless night gives.
double allsky_cloud_cover(int stars, int clear_sky_stars);

/// The sun's altitude (degrees) at @p when for an observer at @p latitude/@p longitude
/// (degrees, east positive). NOAA low-precision formulas, about 0.1 degrees.
double sun_altitude_deg(std::chrono::system_clock::time_point when, double latitude, double longitude);

/// The sun altitude below which the sky is dark enough to count stars.
inline constexpr double kAllskyNightSunAltitude = -6.0;

}  // namespace alpacacore::vendor::allsky
