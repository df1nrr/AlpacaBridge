### Added

- **OpenWeather** (AlpacaCore, new vendor `openweather`): ObservingConditions from openweathermap.org's current weather for a site (temperature, humidity, computed dew point, pressure, wind, gusts, cloud cover, rain). Configured with an API key, latitude, longitude and an update interval (default 10 minutes).
- **Allsky camera cloud cover** (AlpacaCore, new vendor `allsky`): ObservingConditions CloudCover from the star count of an allsky camera's latest image (e.g. the Allsky software's `image.jpg`), measured at night; the sky circle and the clear-sky star count are configurable.
- **Weather safety monitor** (vendor `allsky`, SafetyMonitor): unsafe at once on rain, wind, gusts, humidity or clouds over their limits (the allsky count at night, OpenWeather by day) or stale data; safe again after a configurable hold-off. A `status` Action tells why. The web UI now offers the Safety Monitor device type.

### Added (tests)

- **OpenWeather, Allsky and weather safety cases** (AlpacaCore): `test_openweather_observingconditions.cpp` (reply parsing, URL, values, connect failures, Refresh, age, ranges, state machine, unsupported sensors), `test_allsky.cpp` (star counting on synthetic JPEG fields, the moon mask, sun altitude, cloud cover night and day, connect failures, safety rules one by one, hold-off, stale readings, OpenWeather-only), the `[stress]` registrations, contract sweep entries (with SafetyMonitor support in the sweep) and the routing config round-trips; the web tests now cover the new vendors and the Safety Monitor type.
