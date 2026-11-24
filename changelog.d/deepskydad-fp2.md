### Added

- **Deep Sky Dad FP1/FP2 flat panel** (AlpacaCore, new vendor `deepskydad`): a CoverCalibrator driver for the Deep Sky Dad motorized flat panels over their USB CDC port (115200 8N1), from INDI's `deepskydad_fp` protocol. Opens and closes the flap, switches the LED panel and sets its brightness (0-4096); cover, light and brightness are served from a polled state frame, a flap that stops short or runs past 90 s reads `Error` until the next cover command, and `HaltCover` stops reporting the move (the protocol has no stop command). Configured with an explicit port or auto-detect by panel index in the web UI. Not yet validated with ConformU on hardware.

### Added (tests)

- **Deep Sky Dad flat panel cases** (AlpacaCore): `test_deepskydad_covercalibrator.cpp` over a pty fake panel on a fake clock (defaults, metadata, protocol parsing, range checks, state machine, flap moves and failures, halt, calibrator, seeding, FP1, fault and link loss, deferred connect), the `[stress]` registration and the contract sweep tier 1 entry and tier 2 host.
