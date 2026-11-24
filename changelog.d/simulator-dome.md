### Added

- **Dome simulator** (AlpacaCore, new vendor `simulator`): a simulated observatory dome (IDomeV3) with no hardware, for trying a client before a real dome is connected. It rotates the short way round at a configurable rate, opens and closes its shutter in a configurable time, parks, finds home, syncs and aborts; `Slewing` covers rotation and shutter, `AtPark` holds only after a finished Park. Configured in the web UI (rotation speed, shutter time, home and park azimuth).

### Added (tests)

- **Dome simulator cases** (AlpacaCore): `test_simulator_dome.cpp` on a FakeTaskClock (defaults, metadata, capabilities, range checks, unsupported members, state machine, short-way slews, shutter timing and reversal, park and SetPark, FindHome, AbortSlew, sync), the `[stress]` registration, the contract sweep entry and the routing config round-trip.
