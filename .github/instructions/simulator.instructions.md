---
applyTo: "AlpacaCore/src/vendors/simulator/**,AlpacaCore/include/alpacacore/vendor/simulator/**,AlpacaCore/tests/*simulator*"
---

### Simulator

Devices: Dome (`create_simulator_dome`). No hardware and no SDK: for trying a client (ARA, NINA) or the Dome
HTTP surface before real hardware is connected. Built with `ALPACACORE_ENABLE_SIMULATOR`.

- The motion is `DomeModel` (`simulator_dome_model.h`), a pure function of time: azimuth rotates the short way round
  at `slewRateDegPerSec`, the shutter moves between closed and open in `shutterTimeSec`. The driver reads it on its
  `util::TaskClock` under its state lock, so no thread runs and a getter never waits; tests step a `FakeTaskClock`.
- IDomeV3 choices: `Slewing` is true while the dome rotates or the shutter moves ("any part of the dome"). `AtPark` is
  true only after a `Park` has finished, and any later slew, sync, FindHome or AbortSlew clears it; `Park` while
  parked is a no-op. `AtHome` is true whenever the dome stands within 0.5 degrees of `homeAzimuth`. `AbortSlew` stops
  rotation and shutter; a shutter stopped part-way reads `shutterError` (4) until the next shutter command.
- `CanSetAltitude` and `CanSlave` are false: `Altitude` and `Slaved=true` throw `PropertyNotImplemented`,
  `SlewToAltitude` throws `MethodNotImplemented`; `Slaved=false` is accepted.
- The position and shutter survive a disconnect (a real dome does not move when the client lets go); a new process
  starts at `parkAzimuth` with the shutter closed.
