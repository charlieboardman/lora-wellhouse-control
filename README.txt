LoRa well controller
====================
Pico 2 W + Waveshare SX1262 HAT sender; Waveshare ESP32-S3-Relay-6CH
+ SX1262 HAT receiver. Firmware 1.1.1. One PlatformIO project, two environments.

Commands (Linux)
----------------
Run one script; setup and the Python environment are automatic:
  bash deploy.sh                 # Test and build both boards
  bash deploy.sh flash           # Test, build if changed, and flash both boards

To work on just one board:
  bash deploy.sh build sender
  bash deploy.sh flash receiver
  bash deploy.sh test            # Tests only

Builds are cached by SHA-256 of source, settings, build configuration and
installed tool/platform metadata. Unchanged builds are skipped only if all
required output files still match their saved checksums. Missing or modified
outputs trigger a build. Failed/interrupted builds are never cached. Tests
run on every invocation. Flash always writes to the selected board(s).
No environment activation or PlatformIO commands are required.

PlatformIO downloads the board SDKs and compilers into its user cache.
No SDK/tool download archives or manual toolchain installation are needed here.
The sender uses the community RP2350 platform and its Pico SDK integration;
the receiver uses espressif32 6.10.0 / ESP-IDF 5.4.0. Arduino is not required.
The sender platform is pinned to verified revision 5d4561a05e3b212660ac6fdd3fbfb328d1988aa1.

Pico: hold BOOTSEL while connecting USB, then:
  bash deploy.sh flash sender
The default mount is /run/media/YOUR_USER/RP2350; edit upload_port in
platformio.ini if the mounted drive has a different path.
The uploader checks RP2350 identity, but cannot distinguish Pico 2 W from
other RP2350 boards. Installation copies the UF2 to the mounted drive.

Receiver:
  bash deploy.sh flash receiver
The uploader accepts only MAC XX:XX:XX:XX:XX:XX, checks flash offsets,
and requires a complete 16 MiB backup under .backups/ before writing.
The first backup is retained across later uploads. esptool verifies writes.
Do not flash or deliberately exercise relays with an operating pump connected.

Monitor either serial device:
  pio device list
  pio device monitor --port /dev/serial/by-id/DEVICE --baud 115200

Build output: .pio/build/sender/firmware.uf2 and
.pio/build/receiver/firmware.bin (plus bootloader and partition images).

Behavior
--------
GP0 is sampled once every 10 seconds, first sample 10 seconds after startup.
Grounded: send one ON packet. Open: transmit nothing. There is no edge-triggered
transmission, separate debounce, or OFF packet. Delayed loops never catch up
with a burst. The Pico LED shows local TX success, not receiver acknowledgment;
it goes dark at an open-input sample or detected radio fault.

Each exact valid ON packet turns relay 1 on and renews a 31-second deadline.
Silence lets it expire. Invalid and legacy OFF packets never renew or cancel
it. All relays start OFF; detected radio faults drop relay 1 immediately.
RGB green follows the commanded relay state. The VFD determines motor stop
behavior after the relay opens.

Both boards use 8-second watchdogs, bounded radio waits, 2-second initialization
retries, and restart after 8 consecutive initialization failures. Timers are
64-bit monotonic values. There are no recurring application flash writes.
Sender radio refresh occurs hourly after a completed sample/TX; receiver
refresh occurs hourly while the relay is OFF. Silence is not a radio fault.
A watchdog is recovery, not an independent hardware relay cutoff.

Settings and wiring
-------------------
Edit firmware/config.h for shared radio, packet and timing settings; rebuild
and flash BOTH boards when changing shared settings. Defaults: 915 MHz,
SF12, BW125 kHz, CR4/8, LDRO, preamble 8, CRC, sync word 0x1424, +22 dBm TX.
ON packet: 4c 52 31 00 01 02 03 04 05 01; approximately 1.188 seconds airtime.
The identifier is not authentication, encryption, or replay protection.

Sender: float dry contact between GP0 (physical pin 1) and GND (pin 3).
GP0 has an internal pull-up; do not apply external voltage.
HAT SPI1: SCK GP10, MOSI GP11, MISO GP12, CS GP3, BUSY GP2,
RESET GP15, DIO1 GP20. DIO2 controls the RF switch; DIO3 powers the 1.7 V TCXO.
UART stdio must stay disabled because GP0 belongs to the float input.

Receiver HAT mapping: SCK35, MOSI36, MISO37, CS14, BUSY13, RESET12.
Relay1 is GPIO1; RGB is GPIO38. Relay1 COM to VFD DCM, NO to FWD/DI1,
with VFD set for NPN/sink maintained two-wire forward/stop control.
No ESP GPIO drives the VFD directly.

Use an attached 915 MHz antenna. Conducted/radiated power has not been measured;
permitted deployment bandwidth/channel/power still needs review. The controller
cannot detect welded contacts, a stuck-closed float, or all hardware failures.
Independent well/cistern protection, suitable power and enclosure are required.

Layout and verification
-----------------------
platformio.ini       Both board environments and common upload/monitor settings
firmware/config.h    Shared settings and packet format
firmware/sender/     Pico control loop and SX1262 driver
firmware/receiver/   ESP32 control loop and relay deadline
deploy.sh            Automatic setup, tests, cached builds and flashing
scripts/             PlatformIO upload actions
tests/               Timing/protocol tests and simulated sender runtime
CMakeLists.txt       ESP-IDF entry point (used only by the receiver environment)

Host tests cover exact packet validation, lease expiry/renewal, rejected OFF
frames, fault-off behavior, large timestamps, and the actual sender loop with
hourly refresh, retry/reboot, silence, and unchanged sampling cadence.
Historical builds, backups and validation archives were removed. This tooling
migration builds successfully for both boards; host tests pass. A repeated
sender build was verified to use the checksum cache. The migrated firmware
has not been flashed or tested on hardware. Long-term soak/brownout
qualification and a fresh end-to-end hardware check remain outstanding.
