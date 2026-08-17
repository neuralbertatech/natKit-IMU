"""Read the primary's framed serial uplink (TEC-NATKIT-25).

This is the gateway's job done on a laptop: find frame boundaries in a byte
stream you may have joined half way through, validate them, and say what
arrived. It exists as a committed tool rather than a scratch script because it
is the only independent check that the framing works -- the firmware's own
counters say what it *sent*.

RESYNCHRONISATION IS THE POINT, not a nicety. The reader scans for the magic,
trusts nothing until the CRC checks out, and on a bad frame advances by ONE byte
rather than by the claimed length -- because a corrupt length field is exactly
the case where trusting it walks you past the next good frame. Every discarded
byte is counted, so "it resynced" is a number rather than an impression.

That also makes the bring-up mode work: with CONFIG_NATKIT_UPLINK_UART_NUM=0 the
frames are interleaved with the console's log text on one wire, and the log lines
are simply bytes that never validate. Anything the reader skips is reported as
garbage, which in that mode is mostly readable ASCII.

usage:
  read_uplink.py <port> [seconds] [--baud N] [--print-logs]
"""

import argparse
import binascii
import struct
import sys
import time
from collections import defaultdict

MAGIC = b"NK"
VERSION = 1
HEADER = 18
CRC = 4

TYPE_DATA = 1
TYPE_NODE_STATUS = 2
TYPE_PRIMARY_STATUS = 3
TYPE_NAMES = {TYPE_DATA: "data", TYPE_NODE_STATUS: "node", TYPE_PRIMARY_STATUS: "primary"}


def parse_frame(buf, i):
    """Try to read a frame at buf[i:]. Returns (frame, consumed) or (None, 0).

    (None, 0) means "not a frame here, or not enough bytes yet" -- the caller
    decides whether to wait for more or to step forward one byte.
    """
    if len(buf) - i < HEADER + CRC:
        return None, 0
    if buf[i : i + 2] != MAGIC:
        return None, 0
    version = buf[i + 2]
    ftype = buf[i + 3]
    stream_id, seq, length = struct.unpack_from("<QIH", buf, i + 4)
    total = HEADER + length + CRC
    if length > 2048:
        return None, 0  # implausible: a corrupt length, not a long frame
    if len(buf) - i < total:
        return None, 0  # incomplete; wait for more bytes rather than rejecting
    payload = bytes(buf[i + HEADER : i + HEADER + length])
    (want,) = struct.unpack_from("<I", buf, i + HEADER + length)
    got = binascii.crc32(bytes(buf[i : i + HEADER + length])) & 0xFFFFFFFF
    if want != got:
        return None, 0
    if version != VERSION:
        return None, 0
    return {
        "type": ftype,
        "stream_id": stream_id,
        "seq": seq,
        "payload": payload,
    }, total


def _sample_size(frame_version):
    """Bytes per sample, which depends on the frame version.

    ⚠️ Version 2 added the magnetometer: 13 floats instead of 10, so 62 bytes
    instead of 50. This used to be a bare 50 here, which would have reported a
    length mismatch on every v2 frame and pointed the blame at the transport.
    """
    floats = 13 if frame_version >= 2 else 10
    return 8 + floats * 4 + 2


def decode_data(payload):
    """The canonical NatImuBulkDataSchema header, as imu_frame.hpp lays it out."""
    if len(payload) < 24:
        return None
    schema, samples, rate = struct.unpack_from("<HHI", payload, 0)
    seq_no, device_ts = struct.unpack_from("<QQ", payload, 8)
    return {
        "schema": schema,
        "samples": samples,
        "rate": rate,
        "seq_no": seq_no,
        "device_ts_us": device_ts,
        "expected_len": 24 + _sample_size(schema) * samples,
    }


def decode_primary_status(p):
    if len(p) < 84:
        return None
    (device_id, uptime_us) = struct.unpack_from("<QQ", p, 0)
    (epoch, free_heap, min_heap, nodes, rejected, unknown) = struct.unpack_from("<6I", p, 16)
    (queued, sent, dropped, timeouts) = struct.unpack_from("<4I", p, 40)
    (bytes_sent,) = struct.unpack_from("<Q", p, 56)
    (typ, bound, worst, samples) = struct.unpack_from("<4I", p, 64)
    (quality, measured, sealed) = struct.unpack_from("<3B", p, 80)
    return dict(
        device_id=device_id, uptime_s=uptime_us / 1e6, epoch=epoch,
        free_heap=free_heap, min_heap=min_heap, nodes=nodes, rejected=rejected,
        unknown=unknown, queued=queued, sent=sent, dropped=dropped,
        timeouts=timeouts, bytes_sent=bytes_sent, coh_typical=typ,
        coh_bound=bound, coh_worst=worst, coh_samples=samples,
        coh_quality=quality, coh_measured=measured, sealed=sealed,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("seconds", nargs="?", type=float, default=60.0)
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--print-logs", action="store_true",
                    help="print skipped bytes as text (useful in console-shared mode)")
    args = ap.parse_args()

    import serial

    port = serial.Serial(args.port, args.baud, timeout=0.2)
    buf = bytearray()
    started = time.time()
    deadline = started + args.seconds

    frames = defaultdict(int)
    bytes_skipped = 0
    skipped_run = bytearray()
    last_uplink_seq = None
    uplink_gaps = 0
    per_stream_last = {}
    per_stream_gaps = defaultdict(int)
    per_stream_frames = defaultdict(int)
    last_primary = None
    first_primary = None
    frames_at_first_status = 0

    def flush_skipped():
        nonlocal skipped_run
        if args.print_logs and skipped_run:
            sys.stdout.write(skipped_run.decode("utf-8", "replace"))
            sys.stdout.flush()
        skipped_run = bytearray()

    while time.time() < deadline:
        chunk = port.read(4096)
        if chunk:
            buf.extend(chunk)

        i = 0
        while i < len(buf):
            frame, consumed = parse_frame(buf, i)
            if frame is None:
                # Not a frame here. If we might simply be short of bytes, stop and
                # wait; otherwise step forward exactly ONE byte. Stepping by a
                # claimed length would trust a field we have not validated.
                if buf[i : i + 2] == MAGIC and len(buf) - i < 2048 + HEADER + CRC:
                    break
                skipped_run.append(buf[i])
                bytes_skipped += 1
                i += 1
                continue

            flush_skipped()
            frames[frame["type"]] += 1

            if last_uplink_seq is not None and frame["seq"] != last_uplink_seq + 1:
                if frame["seq"] > last_uplink_seq:
                    uplink_gaps += frame["seq"] - last_uplink_seq - 1
            last_uplink_seq = frame["seq"]

            if frame["type"] == TYPE_DATA:
                d = decode_data(frame["payload"])
                if d:
                    sid = frame["stream_id"]
                    per_stream_frames[sid] += 1
                    prev = per_stream_last.get(sid)
                    if prev is not None and d["seq_no"] > prev + 1:
                        per_stream_gaps[sid] += d["seq_no"] - prev - 1
                    per_stream_last[sid] = d["seq_no"]
            elif frame["type"] == TYPE_PRIMARY_STATUS:
                last_primary = decode_primary_status(frame["payload"])
                if first_primary is None:
                    first_primary = last_primary
                    # Frames parsed before the first status arrived cannot be
                    # attributed to a window the primary agrees about.
                    frames_at_first_status = sum(frames.values())

            i += consumed

        del buf[:i]

    flush_skipped()
    elapsed = time.time() - started

    print()
    print(f"=== {elapsed:.1f}s on {args.port} at {args.baud} baud ===")
    total = sum(frames.values())
    print(f"frames: {total} total " +
          ", ".join(f"{TYPE_NAMES.get(t, t)}={n}" for t, n in sorted(frames.items())))
    print(f"uplink sequence gaps: {uplink_gaps}   (primary dropped it, or the wire did)")
    print(f"bytes skipped resyncing: {bytes_skipped}")
    for sid in sorted(per_stream_frames):
        print(f"  stream {sid}: {per_stream_frames[sid]} data frames, "
              f"radio seq gaps {per_stream_gaps[sid]}")
    if last_primary:
        p = last_primary
        print(f"primary {p['device_id']}: up {p['uptime_s']:.0f}s, heap {p['free_heap']} "
              f"(min {p['min_heap']}), registry {'SEALED' if p['sealed'] else 'open'} "
              f"with {p['nodes']} node(s), {p['rejected']} rejected")
        print(f"  uplink self-report: queued {p['queued']}, sent {p['sent']}, "
              f"DROPPED {p['dropped']}, write timeouts {p['timeouts']}, "
              f"{p['bytes_sent']} B")
        print(f"  coherence: typical {p['coh_typical']} us, bound {p['coh_bound']} us, "
              f"worst {p['coh_worst']} us over {p['coh_samples']} samples "
              f"({'measured' if p['coh_measured'] else 'derived'})")
        # The check that matters: the primary says what it sent, we say what we
        # got. If those disagree beyond what it admits dropping, the wire is
        # losing frames and nobody is counting them.
        #
        # Reconciled over a WINDOW both ends agree on, not against the primary's
        # cumulative counters. Those count from ITS boot, which includes every
        # frame sent before this reader attached -- comparing them to what we
        # parsed makes a healthy link look like it is losing three quarters of
        # its traffic. (It did, in the first run of this tool.)
        if first_primary is not None and first_primary is not last_primary:
            sent_delta = p["sent"] - first_primary["sent"]
            parsed_delta = total - frames_at_first_status
            print(f"  RECONCILE over the window between the first and last status "
                  f"frame: primary sent {sent_delta}, we parsed {parsed_delta}, "
                  f"missing {sent_delta - parsed_delta} "
                  f"(it also reports dropping "
                  f"{p['dropped'] - first_primary['dropped']} in that window)")
        else:
            print("  RECONCILE: needs at least two primary status frames")


if __name__ == "__main__":
    main()
