---
applyTo: "AlpacaCore/src/vendors/allsky/**,AlpacaCore/include/alpacacore/vendor/allsky/**,AlpacaCore/tests/*allsky*"
---

### Allsky

Devices: ObservingConditions (cloud cover from an allsky camera's latest image) and SafetyMonitor (rules over
OpenWeather and, optionally, the camera).

The image is any JPEG an allsky camera publishes over HTTP (the Allsky software's `image.jpg`); nothing runs on the
camera computer. It is fetched through a seam (`AllskyFetcher`; default libcurl, http/https, 16 MiB cap) and decoded
to grayscale at half size by the vendored `external/stb/stb_image.h`, compiled static into
`allsky_image_analysis.cpp`. Not libjpeg: the server process already holds libjpeg builds of two ABI versions, and a
mixed call aborts the decode ("Wrong JPEG library version: library is 90, caller expects 62", the first fork build).

- Star count (`analyse_allsky_image`): Gaussian smoothing (sigma 1 px) drops single-pixel noise and hot pixels; a grey
  opening (7 px) then a 15 px box give the star-free background; a star is a 7x7 local maximum 8 robust sigma above
  it, inside the sky circle (`roiCenterX`/`roiCenterY` of the width/height, `roiRadius` of the height) and away from
  saturated areas grown by 12 px (the moon and its glare). Tuned on Isartalsternwarte frames (Allsky v2024.12.06,
  2028x1520, 60 s): a clear night 120-260 stars, haze 1-20, overcast or a wet dome 0-1.
- A frame without its JPEG EOI marker (fetched while the camera writes it) is refused, never decoded: libjpeg would
  fill the rest grey, a starless "cloudy" sky. An identical frame keeps the previous reading's time, so a camera that
  stopped publishing ages out instead of looking fresh. The Allsky software writes a frame about every minute
  (60 s exposures): the default poll is 60 s.
- CloudCover = 100 % minus the share of `clearSkyStars` (default 200), measured only while the sun is below -6
  degrees (`sun_altitude_deg`, NOAA low-precision); in daylight and twilight it is ValueNotSet. The count depends
  on the camera, lens, exposure and moon: the operator sets `clearSkyStars` from a clear night's log (debug level).
- The safety monitor is unsafe at once on rain (`rain.1h` > 0), wind, gusts or humidity over their limits, clouds
  over `maxCloudCover` (the camera's at night when an image URL is set, OpenWeather's `clouds.all` otherwise), an
  OpenWeather reading older than 30 minutes or, at night, a camera reading older than 15. It is safe only after every
  rule has held for `holdOffMinutes` (also after a connect). IsSafe is false while disconnected (ISafetyMonitor);
  the `status` Action returns the verdict and its reasons.
- Connect fetches both sources once: a wrong key or image URL fails the connect rather than reading "unsafe".
- Poll threads join in the destructor after `shutdown_connection()`; readings are under a leaf mutex and no lock is
  held across a fetch.
