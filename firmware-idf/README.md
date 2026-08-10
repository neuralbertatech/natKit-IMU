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

## Status: scaffold (TEC-NATKIT-21)

Every role is a stub that logs what it is and then reports uptime and heap on a
timer. What is **not** here yet, and where it lands:

| Missing | Slice |
|---|---|
| BNO08x over `spi_master` + the CEVA `sh2` driver | TEC-NATKIT-22 |
| the on-air frame format (fragment a ~5 KB frame, or shrink it) | TEC-NATKIT-23 |
| leaf: sampling + `esp_now_send` + following the timing broadcast | TEC-NATKIT-24 |
| primary: node registry, reassembly, serial mux, backpressure | TEC-NATKIT-25 |
| gateway: WiFi/Ethernet, `esp-mqtt`, `esp_netif_sntp` | TEC-NATKIT-26 |
| the 1-second ESP-NOW timing broadcast | #340 |
| bench against the current firmware, adopt or discard | TEC-NATKIT-27 |

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

Targets: **esp32** (the IMU board's ESP32-PICO-D4 / PICO-V3-02) and **esp32c3**
(natVR's EMG node). Both build all three roles.

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
- `../embeded` still builds unchanged at `pio run -e release`.

**Not yet verified: booting on hardware.** No board was attached when this slice
landed (`/dev/ttyUSB*` and `/dev/ttyACM*` both absent), so the boot banner and
role dispatch are unproven on silicon. Two ways to close it:

1. Attach the IMU board and `./build-role.sh leaf esp32 -p <PORT> flash monitor`
   — expect the banner (firmware/version, role, target/revision/cores/IDF,
   device id + MAC, reset reason, heap) then a status line every 10 s.
2. Boot it in QEMU with no hardware at all: `idf.py qemu monitor`. Espressif's
   QEMU **fails to install on this box** — the binary needs `libslirp.so.0`,
   which is not present (`sudo dnf install libslirp` should fix it; the 15 MB
   tarball is already cached in `~/.espressif/dist`, so a retry is quick).

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
