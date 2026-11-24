---
applyTo: "AlpacaCore/src/vendors/scopedome/**,AlpacaCore/include/alpacacore/vendor/scopedome/**,AlpacaCore/tests/*scopedome*,AlpacaCore/conformu/ScopeDome/**"
---

### ScopeDome

Devices: Dome (ScopeDome observatory domes through the USB Card 2.1).

Protocol: from INDI's `drivers/dome/scopedome_usb21.cpp` and `scopedome_dome.cpp` (themselves based on the ScopeDome
Windows ASCOM driver 5.1.30); not yet run on hardware or ConformU. The card is an FTDI serial port at 115200 8N1.
Frames are `0xAA, length, command, CRC-8, payload` with the Dallas/Maxim CRC-8 (reflected 0x8C) over the header,
length, command and payload, values little endian. A command is answered by an echo frame or by an error code
(MotionConflict 3, FunctionNotSupported 2, ParamError 4); a query by the same command carrying its value. A bad CRC
or a timeout is retried once, as INDI does. The connect sends `ConnectionTest` and `StopSafeCommunication` (the
card's watchdog otherwise resets the link after a few seconds of silence) and reads the firmware and `GetImpPerTurn`.

- State (`GetStatus`, `GetCounter`, `GetAllDigitalExt`) is served from a `util::PolledState` every 500 ms; a getter
  never touches the port. The poll body also supervises what is in progress, under `command_mutex_` (the I/O-level
  lock commands take): it refines a slew, finishes a FindHome and stops the shutter motor at its limit switch.
- Azimuth: the counter grows counter-clockwise from the home sensor, so azimuth is
  `-counter * 360 / steps_per_turn + homeAzimuth`. `GetImpPerTurn` 0 (an uncalibrated dome) refuses slews and
  Azimuth with a message asking for the full system calibration in the ScopeDome software.
- A slew goes the short way, waits a 2 s start grace, then re-sends the remaining distance until within
  `azimuthTolerance` (four sends at most). A miss after four sends, or a rotation still running after 5 minutes, is a
  failed rotation: `Slewing` raises it until the next command or a reconnect. A slew into a running move sends `Stop`
  first (the card answers a second move with MotionConflict).
- FindHome sends `FindHome`; when the card stops homing at the home input (or within tolerance of `homeAzimuth`) the
  driver resets both counters, so Azimuth then reads `homeAzimuth`. `AtHome` holds from there until the next move.
- Park rotates to `parkAzimuth`; SetPark changes it until a restart (the configured value is what persists). Park
  does not close the shutter.
- Shutter 1 only: Open/Close switch the card's OPEN1/CLOSE1 outputs. The card does not stop at the limit switch on its
  own (INDI stops it), so the poll clears the output when OPEN1/CLOSED1 closes. A shutter that has not reached its
  limit after `shutterTimeoutSec` is stopped and reads `Error` until the next shutter command. Neither limit closed
  with no move in progress also reads `Error` (ASCOM has no "part-way"). No rotary (radio) link refuses the shutter.
- Connect clears both shutter outputs (an output left on by an earlier session has nobody watching its limit), and a
  disconnect during a shutter move stops it. A disconnect leaves a rotation running: the card ends it itself.
- No altitude, slaving or SyncToAzimuth (set `homeAzimuth` instead); no second shutter, relays, sensors, derotation
  or the inertia table. No port scan: the form takes an explicit port (FTDI serial numbers are not ScopeDome's own).
