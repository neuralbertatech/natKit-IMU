# natKit-IMU firmware, native ESP-IDF fork

A **fork** of the node firmware on native ESP-IDF (`idf.py`, no PlatformIO,
no Arduino), restructured around the primary/secondary architecture:

| Role | What it is | Radio / network |
|---|---|---|
| **leaf** (secondary) | sensor node | ESP-NOW only — no WiFi association, no MQTT, no NTP |
| **primary** | ESP-NOW hub, timing master, serial uplink | ESP-NOW + serial |
| **gateway** | serial in, MQTT out on the existing topic contract | WiFi or Ethernet |

One tree, three images: the role is a Kconfig choice
(`main/Kconfig.projbuild`) rather than a runtime switch, so a leaf image need
never contain WiFi or MQTT at all — that is the point of the split rather than
an optimisation.

> **This is additive.** The Arduino/PlatformIO firmware in [`../embeded`](../embeded)
> is untouched, still builds, and is what is on the bench.
> **[`../README.md`](../README.md) is the one place** that records which image is
> on which board and the single command that puts the current firmware back.

## Status: a leaf streams to a primary on a shared clock

| Slice | State |
|---|---|
| TEC-NATKIT-21 scaffold, roles, build-role.sh | done |
| TEC-NATKIT-22 BNO08x over `spi_master` + the CEVA `sh2` driver | done, ~357 Hz |
| TEC-NATKIT-23 on-air frame format | done: 524 B, one frame = one packet |
| TEC-NATKIT-24 leaf: sampling + `esp_now_send` | done |
| **#340 the 1-second ESP-NOW timing broadcast** | **done, see Timing below** |
| TEC-NATKIT-25 primary: node registry, reassembly, serial mux, backpressure | not started |
| TEC-NATKIT-26 gateway: WiFi/Ethernet, `esp-mqtt`, `esp_netif_sntp` | not started |
| TEC-NATKIT-27 bench against the current firmware, adopt or discard | not started |

`gateway.cpp` is still a stub that logs its role and idles. `primary.cpp` is a
working receiver and timing master but explicitly **not** TEC-NATKIT-25: no
persistence, no MAC-to-stream-id mapping, no serial mux, no backpressure.

## Timing (#340)

Leaves have no NTP and no wall clock by design, so their sample timestamps are
monotonic since their own boot and two nodes' timestamps are not comparable.
The primary is the clock master and closes that gap.

**Two packets per second, and the split is the mechanism.** The primary
broadcasts `TimeBeacon(seq, epoch)`, reads its own clock *inside the ESP-NOW
send callback* for that packet, and then broadcasts `TimeFollowUp(seq, tx_us)`
carrying it. Timing the beacon by reading the clock before `esp_now_send` would
measure the transmit queue instead: **that queue was measured at 1.4–13.8 ms and
varies packet to packet**, which is two to three orders of magnitude worse than
what is being estimated. The beacon also replaces the old `kPrimaryHere`
discovery packet, so there is one 1 Hz broadcast rather than two.

The leaf fits a **rolling least-squares line** over the last 32 pairs, giving
offset and skew, and reports it as a `SyncState`. It does **not** rewrite its
own timestamps: frames stay in raw device-monotonic time and the consumer
applies the shift, so the correction stays undoable and improvable, sample times
stay monotonic, and the sync quality travels alongside the stream (which is what
#318 needs).

**Measured, two PICO-V3-02, 5.5-minute soak** (`~/natkit-verification/…`):
327 beacons, 0 missed, 0 orphaned, 0 outliers; fit residual **25 µs rms**; locked
**10 s** after boot; heap flat. The primary scores the leaf's fit against probes
of its own, so the accuracy figure is not self-reported: **bias −168 µs, sd 32 µs,
5th–95th percentile spanning 98 µs**, with 3 excursions of ~2 ms in 300 probes.
Against "sync once at startup" the error grew to −745 µs over the same window.

Three findings worth not rediscovering:

- **`esp_wifi_get_tsf_time()` returns 0 here, confirmed on hardware.** The IDF
  documents TSF as reading 0 on a station that is not associated, and no node in
  this architecture ever associates. Any design that reaches for the TSF needs
  the primary to be a SoftAP first.
- **The MAC receive stamp is real but unusable.** `rx_ctrl->timestamp` exists and
  is a hardware stamp, but measured against `esp_timer` it scatters by **20–57 ms**,
  versus 25 µs for simply reading `esp_timer` in the receive callback. It is kept
  as a logged diagnostic, not as the estimator's input.
- **The primary's console blocks its own task for ~87 ms every second** (111 ms
  worst). It does *not* explain the probe excursions — only ~1% of probes excurse,
  so the higher-priority WiFi task is largely protecting the receive callback from
  it — but it is a real constraint on TEC-NATKIT-25's serial mux.

## Prerequisites

```sh
source ~/esp/esp-idf/export.sh   # ESP-IDF v5.5.3 on this box
```

Nothing else. This tree shares no toolchain with `../embeded`, which is the
main reason the fork is native IDF — see the trap at the bottom.

## Build

Quick path — builds the **leaf** image (the Kconfig default) for **esp32** into
`./build`:

```sh
idf.py set-target esp32
idf.py build
```

Reproducible path — one command per (role, target), each with its own build
directory and its own generated `sdkconfig`, so images cannot inherit each
other's config:

```sh
./build-role.sh leaf                  # esp32 (default target) -> build/esp32-leaf
./build-role.sh primary esp32c3       #        -> build/esp32c3-primary
./build-role.sh gateway esp32         #        -> build/esp32-gateway

./build-role.sh leaf esp32 -p /dev/ttyUSB0 flash monitor
./build-role.sh primary esp32 menuconfig    # edits only that role's sdkconfig
```

Targets: **esp32** (the IMU board's ESP32-PICO-V3-02) and **esp32c3** (natVR's
EMG node). Both build all three roles.

## The ESP-NOW bench probe

```sh
./build-role.sh espnow-probe esp32 -p /dev/ttyACM0 flash
```

Not a fourth role -- the instrument used to decide part of the architecture
(TEC-NATKIT-23), selected by `CONFIG_NATKIT_ESPNOW_PROBE` instead of a node role.
It reports the chip's ESP-NOW version, sweeps payload sizes to find the real
ceiling, and measures send rate with the actual 524-byte IMU frame.

Measured on the PICO-D4 (2026-08-11): **ESP-NOW v2, ceiling exactly 1470 bytes**
(1470 accepted, 1471 rejected with `ESP_ERR_ESPNOW_ARG`), so the 524-byte frame
fits in **one packet** and there is no fragmentation to design. 50/50 frames
confirmed at the IMU's real rate; 177 frames/s (91 KB/s) flat out, with the
excess refused at the API rather than lost silently.

For loss measurement, flash a second board with
`CONFIG_NATKIT_ESPNOW_PROBE_RECEIVER=y` on the same channel: each packet carries
a 4-byte sequence number, so the receiver reports **sequence gaps** rather than
just a lower count.

## Boot it with no board attached

```sh
./build-role.sh gateway esp32 qemu      # builds, then boots in QEMU on stdout
./build-role.sh leaf esp32c3 qemu       # the C3 image, via qemu-riscv32
```

Espressif's QEMU emulates both targets, which is how a role that is not on any
bench board still gets a boot check. Three things to know:

- **Use the plain `qemu` action, not `qemu monitor`.** The monitor refuses to run
  without a TTY ("Monitor requires standard input to be attached to TTY"),
  whereas `qemu` alone runs in the foreground with `-serial mon:stdio`, so
  `timeout 40 ./build-role.sh … qemu </dev/null` captures the console cleanly.
- **QEMU's efuse is blank**, so the MAC reads `00:00:00:00:00:00` and the device
  id reads **0**. That is an emulator artifact, not a bug — do not "fix" it. The
  device-id invariant can only be checked on real silicon.
- It emulates neither the BNO08x, nor ESP-NOW peers, nor a real UART peer, so the
  sensor and radio slices still need the bench. What it does prove is boot, role
  dispatch, the banner and heap behaviour.

If `qemu-system-xtensa` is missing, `idf_tools.py install qemu-xtensa
qemu-riscv32` installs both; it needs the system `libslirp` present.

## Configuration

`sdkconfig` is **generated and gitignored** — it bakes in the target, so
committing it is how a checkout quietly builds for the wrong chip or ignores a
change someone made to the defaults. The committed source of truth is:

- `sdkconfig.defaults` — shared, every role and target
- `sdkconfig.defaults.esp32` / `.esp32c3` — per target (flash size, PSRAM)
- `roles/{leaf,primary,gateway}.defaults` — the role choice

Only deliberate deviations from the IDF defaults live in those files, each with
the reason it is there.

> **ESP-IDF reads `sdkconfig.defaults*` only when the generated `sdkconfig` does
> not exist yet.** After the first build, editing a defaults file silently does
> nothing — the build succeeds and the setting is just absent from the image.
> `build-role.sh` warns when a defaults file is newer than the generated
> `sdkconfig`; the fix is `rm build/<target>-<role>/sdkconfig` and rebuild.

## Layout

```
firmware-idf/
├── CMakeLists.txt              project (natkit_imu_idf)
├── build-role.sh               one command per (role, target)
├── sdkconfig.defaults[.target] committed build config
├── roles/*.defaults            the role choice as a config fragment
└── main/
    ├── Kconfig.projbuild       role choice + status-log interval
    ├── main.cpp                boot banner, NVS, role dispatch
    ├── node_role.hpp/.cpp      role enum, names, the idle status loop
    ├── device_id.hpp/.cpp      the topic-name device id (see below)
    ├── espnow_link.hpp/.cpp    the whole radio surface + the wire packet types
    ├── imu_frame.hpp/.cpp      the canonical 524-byte frame, built on the node
    ├── time_sync.hpp/.cpp      the rolling clock fit against the primary (#340)
    ├── bno08x.hpp/.cpp         the sensor over spi_master + CEVA sh2
    ├── leaf.cpp                \
    ├── primary.cpp              > one per role; each carries the notes for
    └── gateway.cpp             /  the slice that fills it in
```

All three role sources are compiled into **every** image even though only one
entry point is called. Conditional registration would mean two of the three
roles are never compiled in a given build, and a break in one would only
surface when someone happened to select that role.

## Compatibility invariants

These exist so a recording from the fork is comparable with one from the
current firmware — which is what the adopt-or-discard decision rests on.

- **The device id must stay bit-identical**: the six bytes of the default efuse
  MAC packed big-endian into a `uint64`, rendered in decimal (e.g.
  `13793649670644`). That number is inside every topic name the bridge and
  backend already use. See `main/device_id.cpp`.
- **The gateway keeps the MQTT contract**:
  `natKit/receiving/<Topic>-<id>-Json-<Schema>`. Nothing server-side should
  change for this epic.
- **The frame contents should match** what `../embeded` sends, so the two can be
  benched against each other rather than merely compared in spirit.

## Verification

What has been checked, as of the scaffold slice:

- All six images build clean: 3 roles × {esp32, esp32c3}. App sizes
  `0x2c060`–`0x2c0b0` bytes (esp32) and `0x2db00`–`0x2db40` (esp32c3), against
  the default 1 MB app partition — 83% free, so no partition work is needed yet
  even though `../embeded` outgrew its default.
- Each build's generated `sdkconfig` carries the expected
  `CONFIG_NATKIT_ROLE_*` and a 4 MB flash size — a role fragment that failed to
  apply would still have built, so this is checked rather than assumed.
- **Boots on the real node** (ESP32-PICO-V3-02 rev v3.0, MAC
  `0c:8b:95:96:b9:f4`): the leaf image logs `natKit-IMU-idf v0.1.0`,
  `role: leaf`, `target: esp32 rev 3.0, 2 core(s), ESP-IDF v5.5.3`,
  `device id: 13793649670644 (mac 0c:8b:95:96:b9:f4)`, `last reset: power-on`,
  then the idle loop with **heap flat at 297112 B** across 40 s. The board's
  real MAC also confirms the `static_assert`'s device id independently.
- **The rollback command works end to end.** After a full clean rebuild
  (`pio run -e release -t fullclean` then build, 25m07s),
  `pio run -e release -t upload` restored the Arduino firmware, which came back
  up as `natKit-IMU v0.5.0` / `Unique ID: 13793649670644`, synced NTP and
  resumed publishing to
  `natKit/sending/Data-13793649670644-Binary-NatImuBulkDataSchema`.

- **Every one of the six images has now been booted, not just built.**
  leaf/esp32 on the real node; primary/esp32, gateway/esp32 and leaf/esp32c3 in
  QEMU (the C3 correctly reports `rev 0.3, 1 core(s)`, so the packed-revision
  handling holds on both targets). Each dispatched to its own role and settled
  into a flat-heap idle loop.

Console captures are in `~/natkit-verification/598a800/` with a `MANIFEST.md`.

One note for the next person on a console: **read the serial port from exactly
one process.** Two readers split the byte stream and produce plausible-looking
interleaved garbage — half of one line spliced into another — which reads like a
firmware bug and is not one. Reset and read in a single process.

## Traps

- **PlatformIO and this tree must not share a toolchain.** `espressif32` and
  `pioarduino` both use `~/.platformio/packages/framework-arduinoespressif32`
  and cannot coexist: after installing the other platform, a PlatformIO build of
  `../embeded` needs that directory removed first. A native `idf.py` fork
  sidesteps this, which is part of why the fork is native — but `../embeded`
  still lives under PlatformIO, so the trap survives *there*.
- **`main` does not get every component's headers for free.** Including
  `esp_timer.h` failed with a bare "No such file or directory" until `esp_timer`
  was added to `REQUIRES` in `main/CMakeLists.txt`. If a new include cannot be
  found, that is the first thing to check.
- **`esp_chip_info_t::revision` is packed `MXX`** (wafer major × 100 + minor), so
  a PICO-V3-02 reads `301`, not `3`. There is no `full_revision` field in
  v5.5.3.
