---
applyTo: "AlpacaCore/src/vendors/openweather/**,AlpacaCore/include/alpacacore/vendor/openweather/**,AlpacaCore/tests/*openweather*"
---

### OpenWeather

Devices: ObservingConditions (current weather for a site from openweathermap.org).

Source: the `GET https://api.openweathermap.org/data/2.5/weather?lat=..&lon=..&units=metric&appid=KEY` reply
(`main.temp`, `main.humidity`, `main.pressure`, `wind.speed`/`deg`/`gust`, `clouds.all`, `rain.1h`, `dt`), read
with a small field extractor, not a JSON library. The fetch is a seam (`OpenWeatherFetcher`); the default is libcurl,
http/https only, 256 KiB cap. Not yet run against the live service by this project's tests.

- The API key is a Plain catalog field, not `Role::Secret`: a Secret field is never persisted and the driver needs
  the key after a restart. The web form masks it; the log redacts it (`appid=***`). Only the key's alphanumeric
  characters reach the URL.
- Connect fetches once: a wrong key (HTTP 401, which also answers a key not yet activated) or no network is a
  failed connect with OpenWeather's reason. A poll thread fetches every `pollIntervalSeconds` (120..3600, default
  600: the free plan allows 1000 calls a day); a failed poll keeps the last reading, and TimeSinceLastUpdate (from
  `dt`) shows its age.
- DewPoint is computed (Magnus-Tetens) from temperature and humidity: the API sends none. RainRate is `rain.1h`, and
  0 when the reply has no rain block (OpenWeather omits it while dry). AveragePeriod is 0 only.
- SkyBrightness, SkyQuality, SkyTemperature and StarFWHM are PropertyNotImplemented.
- The weather safety monitor (vendor `allsky`) uses `fetch_openweather_current()` from this vendor; see
  [Allsky](allsky.instructions.md).
