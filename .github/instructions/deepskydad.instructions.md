---
applyTo: "AlpacaCore/src/vendors/deepskydad/**,AlpacaCore/include/alpacacore/vendor/deepskydad/**,AlpacaCore/tests/*deepskydad*,AlpacaCore/conformu/DeepSkyDad/**"
---

### Deep Sky Dad

Devices: CoverCalibrator (FP1 and FP2 flat panels: a motorized flap with an LED panel).

Protocol: from INDI's `drivers/auxiliary/deepskydad_fp.cpp` (not confirmed against the vendor's Windows ASCOM
driver, and not yet run on hardware or ConformU). The panel is a native USB CDC port (`/dev/ttyACM*`) at a fixed
115200 8N1. Commands are `[XXXX...]` and every reply is framed `(...)`; the wrapper reads to the `)`.
`[GFRM]` returns the firmware banner (FP2 names `DeepSkyDad.FP2`, the model test INDI uses), `[GMOV]` the motor
running flag, `[GLON]` the light, `[GOPS]` the FP2 flap (0 closed, 1 open, anything else part-way) or `[GPOS]` the FP1
flap angle (270 closed, 0 open), `[GLBR]`/`[SLBRnnnn]` the brightness (0..4096), `[SLON0|1]` the light, and
`[STRG0]`/`[STRG270]` then `[SMOV]` the flap target and move, each answered `(OK)`.

- Cover, light and brightness are served from a `util::PolledState` (four exchanges every 500 ms); a getter never
  touches the port. Commands run on the caller under `command_mutex_` (an I/O-level lock, before `mutex_`), each a
  short exchange: `[SMOV]` answers at once and the move is followed by the poll.
- CoverState after Open/Close: `Moving` until a poll measured after the command shows the motor stopped at the wanted
  end. A stop elsewhere after the 3 s start grace, or a move still running after 90 s, is a failed move and reads
  `Error` until the next cover command or a reconnect (the ASCOM async FAQ). The 90 s bound is a guess (TODO:
  measure the FP2 travel time).
- `HaltCover` sends nothing: INDI documents no stop command, so, as the Gemini Rev2 and WandererCover drivers do, it
  stops reporting the move; CoverState then reads the panel's own end, or `Unknown` part-way.
- `CalibratorOn(0)` is `Ready` at zero brightness and sends `[SLON0]` (the panel is dark) rather than an `[SLBR0000]`
  the firmware may refuse. Brightness reads 0 while the calibrator is Off.
- Auto-detect only probes ports whose USB descriptor names Deep Sky Dad (`deepskydad_flatpanel_usb_tokens()`); the
  panel's VID:PID is unconfirmed, so the web form recommends an explicit port.
- The heater (`[GHTT]`, `[GHTM]`, `[SHTMn]`) is not exposed: CoverCalibrator has no member for it.
