### Added

- **ScopeDome dome** (AlpacaCore, new vendor `scopedome`): ScopeDome observatory domes through the USB Card 2.1 (IDomeV3), from the INDI ScopeDome protocol. Rotation the short way with refinement into a configurable tolerance, FindHome (resets the counter at the home sensor), Park and SetPark, and the main shutter, stopped at its limit switches with a configurable timeout. Configured in the web UI (port, home and park azimuth, tolerance, shutter timeout). Not yet run on hardware.

### Added (tests)

- **ScopeDome cases** (AlpacaCore): `test_scopedome_dome.cpp` over a pty `FakeScopeDomeCard` on a FakeTaskClock (defaults, metadata, frame encoding and CRC, connect and a silent port, azimuth from the counter, short-way slews, refinement and a slew that misses its tolerance, a slew into a running move, FindHome, Park and SetPark, the shutter on its limits, a jammed shutter, no shutter link, AbortSlew, a disconnect during a shutter move, a noisy line and an unplug, range checks, state machine, unsupported members), the `[stress]` registration, the contract sweep tier 1 and tier 2 entries, the snapshot hook and the routing config round-trip.
