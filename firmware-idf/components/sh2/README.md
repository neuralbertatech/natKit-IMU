# sh2 — Hillcrest/CEVA SH-2 sensor hub driver (vendored)

Third-party. `sh2.c`, `sh2_SensorValue.c`, `sh2_util.c`, `shtp.c` and their
headers are Hillcrest Laboratories' (now CEVA) reference driver for the BNO08x
sensor hub, **Apache License 2.0** — the licence header at the top of each file
is the authoritative statement and none of them has been edited.

## Where these came from, and why that matters

Copied verbatim out of `Adafruit BNO08x` 1.2.5's `src/`, which is where the
Arduino firmware in [`../../../embeded`](../../../embeded) gets them (pinned in
its `platformio.ini`). They are plain C with no Arduino dependency, so the whole
Arduino coupling lived in the *wrapper*, not the driver.

Using the identical driver is deliberate: TEC-NATKIT-22 requires that a
recording from this fork be comparable with one from the current firmware, and
the driver is what decodes raw hub reports into `sh2_SensorValue_t` — the fixed
point scaling, the quaternion layout, the status byte. Reimplementing or
"modernising" any of that would silently change values that are supposed to
match, and the comparison in TEC-NATKIT-27 would then be measuring the port
rather than the architecture.

**Do not edit these files.** What belongs to us is the HAL and the wrapper
(`main/bno08x.{hpp,cpp}`): SPI transport, reset, the setup ordering and the
accuracy handling. The vendor's own compiler warnings are silenced in
`CMakeLists.txt` rather than patched away, so this stays a clean copy that can be
diffed against a newer upstream release.

## What the driver expects from us

`include/sh2_hal.h` defines the contract: `open`, `close`, `read`, `write`,
`getTimeUs`. Buffer limits worth knowing when sizing SPI transfers —
`SH2_HAL_MAX_TRANSFER_IN` is 384 and `SH2_HAL_DMA_SIZE` is 512, so the SPI bus is
configured with a 512-byte maximum transfer.
