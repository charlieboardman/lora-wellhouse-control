LoRa wellhouse controller
========================

This project runs two boards that control a well pump and expose the wellhouse's
RS-485 devices to ordinary Modbus TCP clients over Wi-Fi. A LoRa link connects
the boards. Pump RUN control uses a relay; Modbus provides a separate path for
reading device information and issuing device commands.

The firmware is version 2.0.0. The implementation and host tests are complete,
and both firmware targets build. This version has not yet been validated on the
physical radio/VFD installation. See "First hardware checkout" before deployment.

This guide assumes you know basic programming and LoRa, but have not seen this
implementation. Start with the system overview and build instructions; the code
map and protocol sections explain where to make changes.

Contents
--------
  1. System overview
  2. Repository and code map
  3. Configure and build
  4. Physical bill of materials, wiring and flashing
  5. Pump behavior and HTTP interface
  6. Using Modbus TCP
  7. How the queue and radio protocol work
  8. Clock synchronization and result timestamps
  9. Tests and first hardware checkout
 10. Troubleshooting
 11. Making changes


1. System overview
------------------

  Modbus TCP client -------- Wi-Fi:502 -----+
  HTTP client -------------- Wi-Fi:80 ------+
                                           |
  Float switch ----------------------> Pico 2 W
                                       "sender"
                                           |
                              SX1262 <--- LoRa ---> SX1262
                                                       |
                                             ESP32-S3-Relay-6CH
                                                 "receiver"
                                                 /         \
                                           Relay 1         RS-485
                                              |               |
                                         VFD RUN input    VFD / other devices

The sender is a Raspberry Pi Pico 2 W with a Waveshare SX1262 HAT. It reads the
float switch, connects to Wi-Fi, hosts the TCP/HTTP interfaces, and schedules
all radio traffic. "Sender" means it initiates exchanges; it also receives replies.

The receiver is a Waveshare ESP32-S3-Relay-6CH with an SX1262 radio. It controls
relay 1 and acts as the Modbus RTU controller on its built-in RS-485 terminals.
It replies only when asked by the sender. It does not initiate radio traffic.

There are two independent uses of the link:

  * Pump control: a grounded float causes periodic ON messages. Each fresh ON
    renews the receiver's relay timeout. Without renewals, the relay opens.
  * Modbus gateway: a TCP request reaches the Pico, crosses LoRa, runs on RS-485,
    and returns over LoRa to the original TCP client.

Both share one scheduled link. An exchange must finish or time out before the
next starts. Pump traffic has priority between exchanges. This preserves float
sampling and relay timers while slow radio transmissions are in progress.

Wi-Fi is needed for client access and Internet time, but not for float-driven
pump control. A loss of Wi-Fi does not deliberately stop the radio heartbeat.
A loss of radio renewals eventually opens the receiver's relay.

Modbus addresses select devices on the RS-485 bus; the Pico has no VFD-specific
register API. Register meanings, scaling, and device limits belong to the device
manual. Sending a Modbus command does not bypass the float/relay logic, although
the command itself can change whatever behavior the addressed device permits.


2. Repository and code map
--------------------------

  deploy.sh                     Setup, tests, cached builds, optional flashing
  platformio.ini                Board environments and frameworks
  upload.example.json           Upload port and receiver MAC template
  upload.local.json             Your local upload ports/MAC; Git ignores it
  CMakeLists.txt                ESP-IDF project entry point
  firmware/CMakeLists.txt       Receiver component sources
  firmware/config.h             Shared radio, timing, serial, and pin settings
  firmware/wifi.example.h       Wi-Fi configuration template
  firmware/wifi.local.h         Your credentials; create locally, Git ignores it

  firmware/sender/main.c        Pico hardware setup and main radio/control loop
  firmware/sender/network.c     Wi-Fi, lwIP TCP/HTTP servers, NTP client
  firmware/sender/network.h     Interface between main loop and network layer
  firmware/sender/lwipopts.h    Network stack configuration and memory limits

  firmware/receiver/main.cpp    ESP32 hardware, relay, UART, radio and watchdog
  firmware/receiver/sdkconfig*  ESP-IDF build configuration

  firmware/gateway/sender.c     Queue, priorities, retries, result assembly, sync
  firmware/gateway/sender.h     Sender state and platform callback interfaces
  firmware/gateway/receiver.c   Commands, relay deadlines, duplicate/result cache
  firmware/gateway/receiver.h   Receiver state and interfaces
  firmware/gateway/protocol.h   Radio encoding, CRC, packet/cache limits, RTU checks
  firmware/gateway/radio.c      Shared SX1262 driver and airtime calculation
  firmware/gateway/radio.h      Driver interface and board-provided hardware hooks
  firmware/gateway/clock.h      UTC offset from a monotonic clock
  firmware/gateway/frontend.h   Modbus TCP framing, HTTP routing, daily sync helper

  scripts/upload.py            PlatformIO uploader with identity/backup checks
  tests/                       Host state-machine, driver and interoperability tests

The gateway directory contains portable C logic used by both real firmware and
host tests. Board entry points supply SPI, GPIO, time, and UART access. The Pico
network layer translates client traffic into gateway jobs and delivers completed
responses through callbacks. There is no server process to install elsewhere.

A useful reading order is config.h, gateway/protocol.h, gateway/sender.c,
gateway/receiver.c, then the two board entry points and sender/network.c.

Generated directories are deliberately separate from source:

  .venv/       Python tools installed by deploy.sh
  .build/      Test binaries, compiler caches, build receipts and deployment lock
  .pio/        PlatformIO build products and project dependencies
  .backups/    Receiver flash backup and identity/hash files from the uploader

PlatformIO also downloads platforms/toolchains into its user cache, normally
~/.platformio. SDKs are dependencies managed by PlatformIO, not source that you
need to maintain in this repository. scripts/upload.py is called automatically
by PlatformIO through deploy.sh; it is not another entry point to run manually.


3. Configure and build
----------------------

The current workflow targets a Linux host. You need Git, Bash, Python 3 with venv
and pip support, flock (normally supplied by util-linux), and Internet access for
the first setup. Flashing also needs access to the USB device or BOOTSEL mount.

From the repository root, run:

  bash deploy.sh

In an interactive terminal, this opens a menu to flash the sender, flash the
receiver, flash both, test and build without flashing, or run host tests only.
It creates the local Python environment, installs pinned tools, and runs host
checks automatically. The script is invoked with bash; no executable permission
or manual virtual-environment activation is needed. Subsequent runs reuse the
environment and unchanged firmware images.

Direct CLI commands (skipping the menu):

  bash deploy.sh                 Interactive menu (or build all if non-interactive)
  bash deploy.sh build           Set up, test, build both boards
  bash deploy.sh build sender    Set up, test, build the Pico only
  bash deploy.sh build receiver  Set up, test, build the ESP32 only
  bash deploy.sh test            Run host checks without building board firmware
  bash deploy.sh flash           Test/build, then flash sender and receiver
  bash deploy.sh flash sender    Test/build, then flash only the Pico
  bash deploy.sh flash receiver  Test/build, then flash only the ESP32
  bash deploy.sh help            Show command syntax

Tests run even when selecting one board or reusing an unchanged build. Concurrent
invocations are serialized by a lock so they do not modify builds simultaneously.

Wi-Fi configuration
~~~~~~~~~~~~~~~~~~~

Copy the template once:

  cp firmware/wifi.example.h firmware/wifi.local.h

Edit firmware/wifi.local.h:

  #pragma once
  #define WIFI_SSID "your-network"
  #define WIFI_PASSWORD "your-password"
  #define NTP_SERVER "pool.ntp.org"

Do not commit credentials. Git ignores wifi.local.h. If it is absent, the empty
example settings are used: the firmware builds and float/radio operation works,
but there is no Wi-Fi client access. Wi-Fi settings are compiled into the image,
so changing them requires rebuilding and flashing the Pico.

The Pico uses DHCP, requests hostname wellhouse-modbus-bridge, advertises
wellhouse-modbus-bridge.local over mDNS (_http._tcp:80 and _modbus._tcp:502),
and logs its assigned IP on USB serial. Clients on the same LAN can connect to
wellhouse-modbus-bridge.local without router DNS or static IP configuration, or
use the logged IP directly.

Shared configuration
~~~~~~~~~~~~~~~~~~~~

Most operational settings are in firmware/config.h:

  Float sampling                 1 second
  ON interval                    30 seconds
  Receiver relay hold            130 seconds
  Pause duration                 1 hour
  RS-485                         9600 baud, 8 data bits, odd parity, 1 stop bit
  Serial response deadline       1 second
  Serial end-of-response gap     20 milliseconds
  Radio                          915 MHz, SF12, BW500, CR4/8, LDRO disabled
  Radio preamble / sync word     8 symbols / 0x1424
  Transmit power                 +22 dBm
  Watchdog                       8 seconds on each board

SERIAL_PARITY uses 0=none, 1=odd, 2=even; SERIAL_STOP_BITS uses 1 or 2. All bus
devices must use the configured serial settings and have distinct addresses.
GW_LINK_ID is not hardcoded in the repository: bash deploy.sh flash prompts you
to enter a shared 64-bit hex pair ID (or press Enter to generate one), or you
can pass GW_LINK_ID=0x... in the environment. Both boards must be flashed with
the same ID. It is a pair identifier, not cryptographic authentication.
Frequency, modulation, sync word, and protocol version must also agree.
See "Making changes" before altering radio/timing settings.

Build tools and caching
~~~~~~~~~~~~~~~~~~~~~~~

PlatformIO is the common build/upload interface. The sender uses the Pico SDK
framework; the receiver uses ESP-IDF. A common CLI does not mean a common SDK.
The Pico community platform is pinned to commit
5d4561a05e3b212660ac6fdd3fbfb328d1988aa1 of maxgerhardt/platform-raspberrypi.
The receiver uses espressif32 6.10.0 / ESP-IDF 5.4.0. deploy.sh pins its Python
tools, including PlatformIO, esptool, Zig for host tests, and PyModbus.

The cache uses SHA-256 of relevant sources, configuration, installed tool/platform
metadata, and required output files. A missing or changed image triggers a build.
A failed/interrupted build does not leave a valid receipt. The script also rejects
a build if its source inputs changed while it was running.

Primary outputs:

  .pio/build/sender/firmware.uf2       Pico BOOTSEL image
  .pio/build/sender/firmware.elf       Pico symbols/debug image
  .pio/build/receiver/firmware.bin     ESP32 application
  .pio/build/receiver/firmware.elf     ESP32 symbols/debug image
  .pio/build/receiver/bootloader.bin  ESP32 bootloader
  .pio/build/receiver/partitions.bin  ESP32 partition table

Use deploy.sh for uploads so the right set of images and checks is used.


4. Physical bill of materials, wiring and flashing
--------------------------------------------------

Physical bill of materials (BOM)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Quantities below are for one sender/receiver pair. The board types and interfaces
come from this implementation; unspecified field parts are requirements to select,
not an inventory of hardware already purchased. Reuse existing equipment where
appropriate. Power-supply models, cable lengths, enclosure sizes and the float
model are not recorded in the repository.

Core electronics

  Qty  Item                                  Location / purpose
  ---  ------------------------------------  -----------------------------------
   1   Raspberry Pi Pico 2 W                 Sender controller and Wi-Fi gateway
   1   Waveshare ESP32-S3-Relay-6CH           Receiver controller, relay and RS-485
   2   Waveshare Pico-LoRa-SX1262, 915 MHz    One SPI radio module per board
   2   Antennas suitable for 915 MHz         One per radio; check kit contents
   1   Dry-contact float switch             Cistern demand input to sender GP0/GND

Use the Pico-compatible SPI radio module matching the pin mappings below. An
SX1262 product with a UART interface is not a drop-in replacement for this driver.
Check the actual radio variant and antenna band when ordering; the product family
also includes other frequency bands. Count bundled antennas before buying extras.
The receiver already provides the RS-485 transceiver and relay contacts used here.

The Pico needs populated GPIO headers that mate with its radio module. If buying
a bare Pico 2 W, include two 20-pin header strips and soldering supplies, or choose
the board with headers fitted. Verify connector orientation and clearance for both
radio assemblies before selecting enclosures. Include mating headers/connectors
if they are not supplied with the selected kits.

Power and service connections

  Qty  Item                                  Selection notes
  ---  ------------------------------------  -----------------------------------
   1   Sender regulated power supply         5 V USB supply for the Pico assembly;
                                             size for Pico Wi-Fi plus radio TX
   1   Receiver regulated DC power supply    Board terminal input supports 7-36 V;
                                             select voltage/current for assembly
   1   USB data cable, Micro-B device end    Pico flashing, logs and USB power
   1   USB data cable, Type-C device end     Receiver flashing and logs

Match cable host ends to the workstation. Charge-only cables cannot flash or carry
logs. The supplies above describe the two installed locations; USB host power can
serve during bench work where supported. Record the final supply models and current
ratings after checking board/module requirements. Do not assume the VFD provides a
suitable auxiliary power output. Follow board documentation if using more than one
power connection; this BOM does not specify a dual-source power arrangement.

Field wiring and mounting

  Qty / length         Item                         Purpose / sizing basis
  -------------------  ---------------------------  ----------------------------
  1 measured run       Two-conductor float cable    Dry contact to GP0 and GND;
                                                    may be integral to the float
  1 measured bus run   RS-485 twisted-pair cable    Receiver A/B to VFD and other
                                                    devices; reference/shield as
                                                    required by device manuals
  1 measured run       Two-conductor control cable Relay COM/NO to VFD RUN inputs
  As required          Power leads and connectors  Match supplies and terminals
  Up to 2 enclosures   Suitable protected housings Sender and receiver assemblies;
                                                    reuse an existing cabinet
  As required          Standoffs / mounting parts  Secure boards and radio modules
  Per cable entry      Cable glands / strain relief Match cable diameter and housing
  Per termination      Ferrules / terminal hardware Match wire and terminal type
  As required          Labels                      Unit IDs, wires, ports, supplies

Cable lengths, wire size, enclosure rating, power protection and installation
hardware depend on the site. Select a float suitable for the liquid, mounting
method and required contact behavior: demand must close the contact to ground.

RS-485 termination/biasing must follow the actual bus topology and device manuals.
Check what is already fitted before adding resistors; do not blindly add a
termination at every device. If external antenna mounting is needed, include the
matching coax/bulkhead parts and account for cable loss. Those parts are optional
and their connector types/lengths have not been specified for this installation.

Existing installation and shared equipment

  Qty  Item                                  Status / role
  ---  ------------------------------------  -----------------------------------
   1   VFD (Modbus RTU capable)              Existing installed drive;
                                             verify nameplate and bus settings
   1   Pump / motor and cistern              Existing controlled equipment
   1   Wi-Fi access point / LAN with DHCP    At sender; Internet/DNS for NTP
   1   Linux workstation                     Build, flash and service both boards

The VFD, motor, mains wiring and independent protection are installation equipment,
not new purchases implied by the controller BOM. Additional Modbus devices are
optional; each needs its own bus address and compatible serial settings.

Useful bench equipment: a multimeter, suitable terminal screwdriver, and a
USB-to-RS-485 adapter for checking a device directly before testing through LoRa.
The adapter is a diagnostic tool, not part of the deployed gateway.

Manufacturer references for selecting the exact variants and checking connectors,
power inputs and supplied accessories (checked November 2026):

  Pico 2 W board / USB / header documentation:
  https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html

  Waveshare Pico-LoRa-SX1262 radio family; select the 915 MHz variant:
  https://www.waveshare.com/wiki/Pico-LoRa-SX1262

  Waveshare ESP32-S3-Relay-6CH hardware and power input:
  https://docs.waveshare.com/ESP32-S3-Relay-6CH

Wiring
~~~~~~

The intended hardware is a Pico 2 W (RP2350), not an original Pico/Pico W, and a
Waveshare ESP32-S3-Relay-6CH. Both radios need attached antennas suitable for the
configured band: both boards transmit.

Sender connections (GP numbers unless physical pin is stated):

  Float contact   GP0 (physical pin 1) to GND (physical pin 3)
  Radio SPI1      SCK GP10, MOSI GP11, MISO GP12
  Radio control   CS GP3, BUSY GP2, RESET GP15, DIO1 GP20

The float uses the internal pull-up: grounded means demand, open means no demand.
It must be a dry contact; do not apply external voltage. UART stdio is disabled
to keep GP0 available. Radio DIO2 controls antenna switching and DIO3 supplies
the HAT's 1.7 V TCXO control.

Receiver connections:

  Radio SPI       SCK35, MOSI36, MISO37
  Radio control   CS14, BUSY13, RESET12
  Onboard RS-485  TX17, RX18; automatic direction, no RTS GPIO
  Relay 1         GPIO1
  RGB indicator   GPIO38; green follows the commanded relay state

Use the receiver's A/B terminals for the RS-485 bus and the device's documented
bus wiring, grounding and termination arrangement. The existing RUN arrangement
uses relay COM to VFD DCM and NO to FWD/DI1, with maintained NPN/sink two-wire
forward/stop control. Confirm the VFD setup before connecting an actual load.

Flashing is an explicit operation. Isolate the pump RUN circuit for initial
checkout; do not flash or exercise relays with an operating pump connected.

By default, flashing auto-detects a single connected RP2350 BOOTSEL drive or
Espressif ESP32-S3 USB device. If you have multiple boards attached, want to pin
a specific receiver MAC without an interactive prompt, or run non-interactively,
copy upload.example.json to upload.local.json (Git ignores it) or pass
SENDER_UPLOAD_PORT, RECEIVER_UPLOAD_PORT, and RECEIVER_MAC in the environment.

Pico:

  1. Hold BOOTSEL while connecting USB to expose the RP2350 drive.
  2. Run bash deploy.sh flash sender.

The uploader auto-detects the mounted RP2350 drive (or uses sender_port from
upload.local.json / SENDER_UPLOAD_PORT), verifies INFO_UF2.TXT for RP2350 family
identity, copies the UF2 and flushes the write. This is not a check of the exact
board model or an end-to-end firmware acceptance test.

Receiver:

  1. Connect the board's USB interface and ensure serial-device access.
  2. Run bash deploy.sh flash receiver.

The uploader auto-detects a single Espressif USB-Serial/JTAG port (or uses
receiver_port from upload.local.json / RECEIVER_UPLOAD_PORT) and reads the
board's MAC via esptool. If receiver_mac is configured, it refuses to flash any
other board; otherwise it prints the detected port/MAC and asks for confirmation
before backing up and flashing. Non-interactive flashing requires receiver_mac.

Before its first write, the uploader reads the complete 16 MiB receiver flash
into .backups/receiver-before.bin. It records identity and a SHA-256 checksum,
checks backup size and flash offsets, and invokes esptool to write the images.
An existing correctly sized backup is reused; this is not a new snapshot on every
flash. Preserve it if you need the original firmware. The backup directory is
ignored by Git and is not an automatic rollback mechanism.

Both boards must run the compatible protocol. Update them as a pair when changing
protocol encoding. A successful upload does not establish that wiring or pump
behavior is correct.


5. Pump behavior and HTTP interface
----------------------------------

All relays start OFF. The sender samples the float once per second and schedules
ON every 30 seconds while grounded and not paused. The receiver renews relay 1's
130-second deadline when it accepts a fresh ON. Duplicate retries are acknowledged
without renewing that deadline a second time.

Opening the float suppresses future ON messages and cancels a pending one. It
does not send an immediate OFF command; the existing receiver hold expires.
Likewise, startup with a grounded float waits for the scheduled ON rather than
closing the relay merely because the board booted.

The 30/130 timing leaves room for missed exchanges and long radio packets. It
is not a guarantee against an arbitrarily long outage. Detected receiver radio
faults open the relay; the watchdog resets a stalled board. The relay output is
commanded state, not feedback that the pump physically started or stopped.

HTTP API
~~~~~~~~

The Pico listens on port 80. Requests have no body. Opening http://wellhouse-modbus-bridge.local/
in a browser serves a small built-in page showing live pump/link status,
pause/resume buttons, and a raw Modbus command sender.

  GET  /                         Built-in status and Modbus command page
  POST /pump/pause               Pause for one hour
  POST /pump/resume              End the pause early
  GET  /pump/status              Read local state and last observed receiver state
  GET  /modbus/last-result       Read metadata for the last completed gateway operation
  POST /modbus/send?pdu=<hex>    Queue a raw Modbus PDU (unit + func + data) over LoRa

Examples, replacing PICO_IP with the DHCP address:

  curl --max-time 125 -X POST http://PICO_IP/pump/pause
  curl --max-time 125 -X POST http://PICO_IP/pump/resume
  curl http://PICO_IP/pump/status
  curl http://PICO_IP/modbus/last-result

Pause immediately suppresses ON at the sender and schedules a priority receiver
command. The receiver opens the relay when it receives that command. It cannot
interrupt an exchange already on the air, so this is not an emergency-stop channel.

Resume removes the pause. If the float is grounded, a confirmed resume schedules
a fresh ON. Neither resume nor pause expiry closes the relay by itself.

Control requests wait for acknowledgment, up to the control deadline of 120 seconds:

  200  {"confirmed":true}                    Receiver acknowledged the command
  503  {"error":"control pending"}           Another pause/resume is pending
  504  {"confirmed":false,"outcome":"unknown"} Acknowledgment was not obtained

A 504 does not prove the command failed to execute. Internal radio retries keep
the same command ID and do not restart an acknowledged pause's one-hour timer.
If the first transmission was lost, the retry carries only the remaining duration.
A new HTTP pause request creates a new one-hour pause.

Pause state is RAM-only. Rebooting a board clears that board's local pause. If the
Pico remains running, it reasserts its remaining pause after discovering a receiver
reboot and reconnecting. Rebooting both boards clears the pause.

Status fields
~~~~~~~~~~~~~

/pump/status returns JSON containing:

  float_grounded                  Current sampled float input
  receiver_commanded_on           Last receiver relay state reported in an ACK
  receiver_status_age_ms          Age of that observation
  receiver_pause_remaining_ms     Remaining remote pause inferred from the last ACK
  last_on_ack_ms                  Pico uptime when an ON was last acknowledged
  receiver_session               Current receiver boot ID; zero when disconnected
  wifi                           Current observed Wi-Fi link status
  pause_remaining_ms             Pico's remaining local pause
  clock_synced / utc_us           Pico clock validity and current UTC microseconds
  clock_sync_age_ms              Time since Pico NTP synchronization
  receiver_clock_synced           Whether a receiver clock correction was acknowledged
  receiver_clock_sync_age_ms      Age of that acknowledged correction
  queued                         Waiting Modbus jobs, excluding the active job

Receiver fields are observations, not live polling or physical feedback. In
particular, a reported ON can have expired since its acknowledgment. Initial zero
values can mean "not observed yet"; examine session, acknowledgment and age fields
together. GET requests read existing state and do not enqueue a radio query.

Up to four HTTP connections are available independently of four Modbus connections.
Responses close the HTTP connection. Invalid requests return 400; unknown routes
return 404. HTTP and Modbus have no authentication or TLS, and the radio protocol
has no cryptographic authentication. Access assumes a trusted installation/LAN.


6. Using Modbus TCP
------------------

Connect a standard Modbus TCP client to PICO_IP on port 502. The unit/device ID is
the address of the target RS-485 device. Addresses 1-254 are accepted; 0 (broadcast)
and 255 are rejected. Every operation expects a response.

The gateway removes the TCP framing, transports the unit ID and function/data,
adds the RTU CRC on the receiver, and transmits over RS-485. It validates the RTU
reply, removes its CRC, and restores a TCP response with the client's transaction
ID. Device exception replies pass through. Vendor-specific functions are forwarded,
although the client may need a custom decoder.

The gateway does not poll devices in the background, interpret register values,
or retain a database of readings. One client request produces one bus transaction,
subject to the duplicate suppression described below.

Python example
~~~~~~~~~~~~~~

PyModbus is installed in .venv by deploy.sh. Save this as a local script and run
it with .venv/bin/python. Replace the IP, unit and register with your device's
values; the register below is illustrative, not a universal status register.

  from pymodbus.client import ModbusTcpClient

  with ModbusTcpClient("PICO_IP", timeout=600, retries=0) as client:
      result = client.read_holding_registers(0x2100, count=1, device_id=1)
      if result.isError():
          print(result)
      else:
          print(result.registers)

This example matches the pinned PyModbus 3.16.2 API. Register addresses are
zero-based wire addresses, not 40001-style display notation. Consult the device
manual for supported functions, register ranges, maximum read sizes and scaling.

Use a generous timeout, such as 600 seconds, and disable automatic client retries.
LoRa transactions can take many seconds, especially with queued work or a lost
reply. A retry submitted by the client is a new operation; for writes that can
mean executing the command again. A gateway timeout is an uncertain outcome,
not proof that the device never received a write.

Limits and errors
~~~~~~~~~~~~~~~~~

  Request payload       Up to 223 bytes: unit ID plus Modbus PDU
  Radio packet          Up to 255 bytes, including the 32-byte transport header
  RTU response          Up to 256 bytes, including its two-byte CRC
  TCP clients           Four connections, each with up to two admitted requests
  Modbus queue          Eight waiting slots, plus one active transaction
  Queue expiry          Five minutes; checked when a job reaches the front
  Active deadline       Three minutes

Requests larger than one radio packet are rejected on the Pico. The receiver
never reconstructs a fragmented request. Large responses are fetched in pieces
and delivered as one normal Modbus TCP response; clients do not handle fragments.

Gateway-generated Modbus exceptions include:

  03  Invalid request bounds, unit or function; includes oversized radio requests
  06  Busy: queue full or a queued job expired
  0B  Gateway target failure: serial/link failure or an uncertain transaction

Malformed TCP framing closes the connection. Normal device exception replies
retain the device's own exception code. A Modbus TCP connection is a byte stream:
partial frames and multiple frames in one network receive are handled explicitly.


7. How the queue and radio protocol work
---------------------------------------

There are two scheduling levels. A Modbus transaction is the whole client operation;
a radio exchange is one sender packet and its receiver reply. A large transaction
can span a request exchange and several fetch exchanges. Pump commands can run
between those exchanges, but another Modbus transaction waits for the current
one to finish. This keeps the receiver cache available until all pieces are read.

Example:

  1. The Pico sends a Modbus REQUEST.
  2. The receiver executes it once on RS-485.
  3. A small reply returns RESULT with the response immediately.
     A larger reply returns READY with its length, timestamp and piece count.
  4. The Pico sends FETCH for piece 0; the receiver replies PIECE.
  5. If ON became due during that exchange, the Pico sends ON next.
  6. The Pico fetches the next piece, assembles the response and answers TCP.
  7. The next queued Modbus transaction can start.

With current limits, an inline result fits up to 207 response bytes because its
metadata takes 16 bytes. Larger results need at most two pieces of up to 223
bytes each. The protocol carries a piece count rather than relying on the client
to know response size in advance.

The sender scheduler first establishes a receiver session with HELLO. Once
connected, its selection order is pending pump control, pending ON, a saved
retry, clock correction/sync, then active or queued Modbus work. A control retry
uses the saved-retry path, allowing a pending ON to run first. New Modbus jobs
are FIFO. ON is a coalesced flag, not an ever-growing list of missed heartbeats.
Opening the float or pausing clears a waiting ON.

Radio timing
~~~~~~~~~~~~

At the configured modulation, a full 255-byte packet takes about 2.98 seconds
one way. Small packets take less time. The driver polls transmission completion;
it does not sleep for the full packet airtime.

A receiver must start its reply within two seconds of request reception; late
replies are dropped. The sender allows a 7-second reply window after its own
TX completes, then a 0.5-second quiet margin. TX has a 4-second radio timeout
and a 4.5-second software wait limit. These bounds keep a delayed receiver reply
from colliding with the sender's next exchange.

An exchange can be retried once using the same transaction identity. Pending pump
traffic can precede that retry. After repeated failure the sender re-handshakes.
Fault paths reserve quiet time for a potentially still-running exchange.

Identity and duplicate suppression
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Each board has a random boot identity. The sender also assigns monotonically
increasing transaction IDs during its run. Packets carry the sender session,
expected receiver boot ID, transaction ID and shared link ID. A versioned,
explicit-byte-order encoding avoids dependence on C struct layout.

The receiver has one fixed-size in-memory transaction cache. It records the request
identity and bytes, response or failure, and acquisition timestamp. A duplicate
request returns the stored result rather than repeating RS-485 execution. FETCH
retries return the same piece. ON, pause/resume and time sync do not evict it.
The next accepted Modbus transaction replaces it: cache size does not grow over
time. A new sender session or receiver reboot clears the relevant cached state.

The sender remembers the known receiver boot across reconnects. If the receiver
rebooted while a write's outcome was uncertain, the active request fails rather
than being silently re-executed against the new receiver session. This protects
internal retries; it cannot deduplicate a new request made by a client later.

Board loops and resources
~~~~~~~~~~~~~~~~~~~~~~~~~

The Pico main loop polls networking and the radio, samples the float, and advances
the scheduler. Network callbacks buffer traffic; the application loop admits jobs.
Client-generation tokens prevent a late response from being delivered to a new
connection that reused the same connection slot.

The ESP32 loop polls radio and serial completion while checking relay deadlines.
Serial replies are bounded by buffer size, response deadline and an idle gap.
The shared radio driver uses board-specific SPI hooks; the ESP32 hook chunks SPI
transfers while keeping chip select asserted for the complete radio command.

Core queues, packet buffers and the receiver cache are bounded. SDK/network drivers
have their own bounded pools and initialization allocations. Normal operation
writes no recurring application state to flash. Both boards have watchdogs and
bounded radio initialization retries. Radio configuration refreshes hourly outside
exchanges; the receiver also waits until its relay is OFF.


8. Clock synchronization and result timestamps
----------------------------------------------

The Pico requests NTP over Wi-Fi after boot and when the UTC date advances past
midnight. Failed network syncs back off from 10 seconds up to 10 minutes. If Wi-Fi
is unavailable at midnight, synchronization happens when connectivity and retries
allow it. Each successful NTP update immediately schedules a receiver sync job;
that job still respects an exchange in progress and pump priorities.

Receiver synchronization uses an NTP-style timestamp exchange over the custom
radio protocol, followed by a clock correction. It is not an NTP server on the
receiver. TX/RX endpoint times and predicted reply airtime separate clock offset
from queue/processing time. Implausible delay measurements are rejected and retried
with backoff. Hardware timing accuracy and daily clock drift remain to be measured.

Both boards maintain UTC as an offset from a monotonic clock. Relay hold, pause,
retry and queue timers use monotonic time, so a wall-clock correction does not
extend a relay hold or expire a queue unexpectedly.

The receiver timestamps acquisition of the serial response. That timestamp remains
attached to the cached result across retries and fetches. It tells you when the
gateway obtained the value, not when the VFD originally recorded an event.

GET /modbus/last-result exposes the most recently completed operation's metadata:

  transaction              Internal radio transaction ID
  tcp_transaction          Original client's Modbus TCP transaction ID
  client                   Internal connection-generation token
  unit / function          Addressed unit and requested function
  error                    Gateway error code, or zero for a transported response
  acquired_utc_us           Response acquisition time, UTC microseconds since epoch
  synchronized             Whether that acquisition had a valid receiver UTC time
  receiver_sync_age_ms      Receiver clock synchronization age reported with result

A device-generated Modbus exception is still a successfully transported response;
inspect the Modbus response itself for device exceptions. The metadata endpoint
is one global last-result slot shared by all clients, not a history or per-client
store. Another completed operation can replace it before an HTTP reader sees it.

Before receiver clock sync, acquisition time is zero and synchronized is false.
The unknown synchronization-age sentinel is 4294967295. Standard Modbus responses
are unchanged: timestamps are available through HTTP, not inserted into registers
or appended to the Modbus response.


9. Tests and first hardware checkout
------------------------------------

Run all host checks with:

  bash deploy.sh test

The test command requires no attached hardware and does not flash devices.

  control_test.cpp         Relay hold, duplicate ON, pause expiry, resume, reboot
  sender_runtime_test.cpp  Priority, fetch/retry, cached execution, reboot identity,
                           queue bounds, clock correction, malformed/stale replies
  gateway_test.cpp         Packet encoding, CRC, request bounds, pieces, TCP/HTTP
                           parsing and midnight-sync detection
  radio_test.cpp           Actual driver with fake SPI: maximum packets, airtime,
                           nonblocking polls, CRC rejection and BUSY timeout
  upload_test.py           Upload identity, backup and artifact safeguards
  interop_host.cpp         Production gateway engines with a simulated RS-485 bus
  interop_test.py          Real PyModbus over loopback TCP to that host simulation

Interoperability cases include unit routing, reads/writes, large responses,
unsupported/oversized requests, device exceptions, missing devices and a vendor
function. These tests exercise portable production logic, but do not prove the
Pico's Wi-Fi stack, electrical RS-485 interface or physical radio behavior.

After tests, run bash deploy.sh to compile both firmware targets. Run it again
without changing inputs to verify both print "unchanged; using verified build."

First hardware checkout
~~~~~~~~~~~~~~~~~~~~~~~

Keep the pump RUN circuit isolated while checking the new firmware:

  1. Verify board identities, antennas, wiring and serial settings. Configure
     Wi-Fi and upload ports. Build and flash compatible firmware on both boards.
  2. Watch boot/health logs. Verify the Pico obtains an IP and both radios initialize.
     Use a serial terminal at 115200 baud; stop programs holding an upload port.
  3. Read /pump/status. Ground/open the float and observe the relay/indicator.
     Confirm periodic renewal and expiry after ON stops. The green receiver LED
     follows commanded relay state; the Pico LED indicates local ON transmission,
     not a receiver acknowledgment or physical pump motion.
  4. Exercise pause and resume with the float grounded. Confirm pause opens the
     relay and resume requires a fresh ON. Check acknowledgment/error responses.
  5. Start with a documented read-only register on one RS-485 device. Confirm the
     unit, register, value and serial settings before trying any device writes.
  6. Verify timestamps become synchronized and remain attached to the right result.
     Measure actual sync error and drift if acquisition timing matters to your use.
  7. Exercise dropped radio replies, receiver reboot, bus disconnection and a large
     response. Confirm errors/recovery and that pump control runs between fetches.
  8. Measure timing under load and outages before restoring the operating RUN circuit.

Software build/test success is not a substitute for this checkout. No hardware
acceptance, long-duration soak or brownout qualification is claimed for version
2.0.0. The system cannot detect a welded relay, stuck float or every wiring failure;
independent well/cistern protection remains necessary. Deployment radio
channel/bandwidth/power also needs review for the installation.


10. Troubleshooting
-------------------

Build cannot create .venv or install tools:
  Check Python venv/pip support and network access. First setup needs both Python
  package downloads and PlatformIO platform/toolchain downloads, including the
  pinned Pico platform from GitHub. Use the first actual error in deploy.sh output.

An unchanged build recompiles:
  Source/configuration, tool metadata or output hashes changed, or an artifact or
  build receipt is missing. Local Wi-Fi settings count as build input. Interrupted
  builds intentionally rebuild. The host tests always run regardless of caching.

Pico will not flash:
  Confirm BOOTSEL mode and that sender_port in upload.local.json (or
  SENDER_UPLOAD_PORT) is the mounted RP2350 drive, not its normal USB serial
  device. The uploader refuses a different chip family.

Receiver will not flash:
  Check receiver_port and receiver_mac in upload.local.json (or
  RECEIVER_UPLOAD_PORT / RECEIVER_MAC), USB permissions, programs holding the
  port, available backup space and backup size. Read the identity/backup error
  before changing configuration.

No Wi-Fi or HTTP/Modbus connection:
  Check wifi.local.h, rebuild/reflash the sender, and inspect its USB log for an IP.
  Check DHCP, network isolation/firewall rules and access to ports 80 and 502.
  Empty template credentials intentionally leave Wi-Fi unconfigured. Repeated
  client connections can exhaust the bounded connection slots.

No relay response or gateway timeouts:
  Check that both radios report ready and both boards use the same firmware,
  GW_LINK_ID, frequency and modulation. Check power, antenna connections and range.
  Inspect receiver_session and last_on_ack_ms. Do not infer link health just from
  the Pico LED; it indicates a local transmission.

Relay remains ON after the float opens:
  Normal behavior is expiry of the last 130-second hold, not an immediate OFF.
  Use the pause API for an explicit scheduled stop. Check the physical contact and
  grounded/open interpretation if ON continues to renew.

Modbus returns 0B:
  Possible causes include no radio reply, receiver reboot, wrong bus unit, wrong
  serial settings, RS-485 wiring, device response timeout, CRC or response mismatch.
  Check the serial/radio logs and start with a small known-valid read. For a write,
  determine actual device state before submitting another operation.

Modbus returns 03 or 06:
  03 can be an invalid/oversized gateway request or a device exception. 06 can mean
  queue saturation or expiration. Reduce concurrent polling and inspect the device
  response. The gateway does not make an unsupported device register valid.

A pause returns 504:
  The receiver may still have paused. The Pico already suppresses ON locally.
  Inspect status and the physical receiver before interpreting an unknown outcome
  as failure. A newly submitted pause starts a new one-hour local pause.

Timestamps are zero or unsynchronized:
  Check Wi-Fi, DNS and reachability of NTP_SERVER on UDP port 123. Then allow the
  queued radio sync and correction to complete. Compare Pico and receiver clock
  flags. Operational timers continue working without valid UTC.

A status value seems stale:
  GET endpoints report cached observations. Check their age and transaction fields.
  /modbus/last-result is shared across clients and does not itself perform a read.


11. Making changes
------------------

For a new RS-485 device, first configure the device's address and serial settings;
no gateway code should be needed for ordinary supported Modbus traffic. Add a
client-side decoder if a vendor function needs one. Keep register interpretation
outside the transport layer.

For control/queue changes, edit gateway/sender.c or gateway/receiver.c and add a
regression case to the corresponding host test. Preserve these invariants:

  * Only the sender initiates radio traffic.
  * Only one exchange is in flight, including its reply/timeout window.
  * ON/control can run between result pieces, without replacing the cached result.
  * A retry cannot execute the same accepted Modbus operation twice.
  * A receiver reboot cannot silently turn an uncertain write into a new execution.
  * Relay and scheduling deadlines use monotonic time.
  * Buffers and queues remain bounded and oversized inputs are rejected.

For HTTP/TCP behavior, start with sender/network.c and gateway/frontend.h. Keep
TCP framing separate from gateway scheduling. Avoid adding blocking client work
inside the main loop or network callbacks.

For radio configuration changes, review the SX1262 setup, airtime calculation,
TX/reply deadlines, serial response budget and 30/130 relay timing together.
config.h deliberately rejects unreviewed modulation settings. Updating a define
without updating timing assumptions can create overlapping transmissions or
unexpected relay expiry.

For wire-format changes, update protocol.h and both endpoints, review the version
field, and extend encoding/interoperability tests. Flash both boards together.
For board changes, review the hardware hooks, GPIO mappings, platform environment
and upload identity checks.

Before handing off a change:

  bash deploy.sh test
  bash deploy.sh

Document what was verified in simulation/builds and what was verified on physical
hardware. Keep wifi.local.h, upload.local.json, generated images, caches and
receiver backups out of Git. Preserve the single deploy.sh entry point so normal
use does not depend on remembering toolchain paths or manual build/upload flags.
