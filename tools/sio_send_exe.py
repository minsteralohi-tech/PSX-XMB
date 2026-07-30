#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Send a PS-EXE to PSX-iTests' Settings -> SIO Loader.

The default mode implements the UniROM/NoPS V2 protocol:

    PC -> PS1  SEXE
    PS1 -> PC  OKV2
    PC -> PS1  UPV2
    PS1 -> PC  OKAY

The PC then sends the 2048-byte PS-EXE header, four little-endian metadata
words (entry PC, load address, padded body size, body checksum), and the
body in 2048-byte chunks. After each chunk the console requests its checksum
with CHEK and replies MORE or ERR!.

Link settings are 115200 baud, 8 data bits, no parity, 2 stop bits:

    python tools/sio_send_exe.py COM3 myprogram.exe
    python tools/sio_send_exe.py /dev/ttyUSB0 myprogram.exe

Open Settings -> SIO Loader on the console first. Requires pyserial:

    python -m pip install pyserial

Use --raw only with an older build that expects an unframed PS-EXE stream.
"""

import argparse
from pathlib import Path
import struct
import sys
import time


HEADER_SIZE = 2048
PROTOCOL_CHUNK_SIZE = 2048


def write_bytes(ser, data, write_size, delay):
    """Write all bytes, optionally splitting USB writes for adapter quirks."""
    view = memoryview(data)
    offset = 0
    while offset < len(view):
        end = min(offset + write_size, len(view))
        written = ser.write(view[offset:end])
        if not written:
            raise TimeoutError("serial write timed out")
        offset += written
        if delay and offset < len(view):
            time.sleep(delay)
    ser.flush()


def wait_tag(ser, expected, timeout):
    """Find a four-byte response while ignoring any preceding TTY chatter."""
    deadline = time.monotonic() + timeout
    window = bytearray()

    while time.monotonic() < deadline:
        byte = ser.read(1)
        if not byte:
            continue
        window.extend(byte)
        if len(window) > 4:
            del window[:-4]
        value = bytes(window)
        if value in expected:
            return value

    names = ", ".join(tag.decode("ascii") for tag in expected)
    raise TimeoutError(f"timed out waiting for {names}")


def print_progress(sent, total, retries=0):
    percent = sent * 100 // total if total else 100
    retry_text = f", retries {retries}" if retries else ""
    print(
        f"\rsending: {percent:3d}%  ({sent}/{total}{retry_text})",
        end="",
        flush=True,
    )


def send_raw(ser, header, body, write_size, delay):
    """Compatibility mode for the project's previous unframed receiver."""
    payload = header + body
    sent = 0

    while sent < len(payload):
        end = min(sent + write_size, len(payload))
        write_bytes(ser, payload[sent:end], write_size, delay)
        sent = end
        print_progress(sent, len(payload))


def send_nops(ser, header, body, entry, load_address, write_size, delay,
              timeout):
    """Send one EXE using UniROM's NoPS framing and corrective protocol."""
    write_bytes(ser, b"SEXE", write_size, delay)
    response = wait_tag(ser, {b"OKV2", b"OKAY"}, timeout)

    protocol_v2 = response == b"OKV2"
    if protocol_v2:
        write_bytes(ser, b"UPV2", write_size, delay)
        wait_tag(ser, {b"OKAY"}, timeout)

    checksum = sum(body) & 0xFFFFFFFF
    metadata = struct.pack(
        "<IIII", entry, load_address, len(body), checksum
    )
    write_bytes(ser, header + metadata, write_size, delay)

    if not protocol_v2:
        write_bytes(ser, body, write_size, delay)
        print_progress(len(body), len(body))
        return

    sent = 0
    retries = 0
    while sent < len(body):
        chunk = body[sent:sent + PROTOCOL_CHUNK_SIZE]

        while True:
            write_bytes(ser, chunk, write_size, delay)
            wait_tag(ser, {b"CHEK"}, timeout)
            write_bytes(
                ser,
                struct.pack("<I", sum(chunk) & 0xFFFFFFFF),
                write_size,
                delay,
            )
            response = wait_tag(ser, {b"MORE", b"ERR!"}, timeout)
            if response == b"MORE":
                break
            retries += 1

        sent += len(chunk)
        print_progress(sent, len(body), retries)


def main():
    parser = argparse.ArgumentParser(
        description="Send a PS-EXE using the UniROM/NoPS SIO protocol."
    )
    parser.add_argument("port", help="serial port, e.g. COM3 or /dev/ttyUSB0")
    parser.add_argument("exe", help="PS-EXE file to send")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--write-size",
        "--chunk",
        dest="write_size",
        type=int,
        default=2048,
        help="maximum bytes per OS serial write (default: 2048)",
    )
    parser.add_argument(
        "--delay",
        type=float,
        default=0.0,
        help="seconds between split serial writes (normally unnecessary)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=8.0,
        help="seconds to wait for each console response (default: 8)",
    )
    parser.add_argument(
        "--raw",
        action="store_true",
        help="use the old unframed header/body stream",
    )
    args = parser.parse_args()

    if args.write_size < 1:
        parser.error("--write-size must be at least 1")
    if args.delay < 0:
        parser.error("--delay cannot be negative")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    try:
        import serial
    except ImportError:
        sys.exit("pyserial is required: python -m pip install pyserial")

    data = Path(args.exe).read_bytes()
    if len(data) < HEADER_SIZE or data[:8] != b"PS-X EXE":
        sys.exit("Not a PS-EXE (missing the 2048-byte PS-X EXE header)")

    header = data[:HEADER_SIZE]
    entry = struct.unpack_from("<I", header, 0x10)[0]
    load_address, text_size = struct.unpack_from("<II", header, 0x18)
    if len(data) < HEADER_SIZE + text_size:
        sys.exit(
            f"File too short: header says {text_size} payload bytes, "
            f"but only {len(data) - HEADER_SIZE} are present"
        )

    if args.raw:
        body = data[HEADER_SIZE:HEADER_SIZE + text_size]
    else:
        # NoPS pads the complete file to the protocol's 2048-byte boundary.
        padded = data + bytes((-len(data)) % PROTOCOL_CHUNK_SIZE)
        header = padded[:HEADER_SIZE]
        body = padded[HEADER_SIZE:]

    print(f"load address : 0x{load_address:08X}")
    print(f"entry PC     : 0x{entry:08X}")
    print(f"image size   : {text_size} bytes ({len(body)} bytes on wire)")
    print(f"protocol     : {'raw compatibility' if args.raw else 'UniROM/NoPS V2'}")

    try:
        with serial.Serial(
            args.port,
            args.baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_TWO,
            timeout=0.1,
            write_timeout=args.timeout,
        ) as ser:
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            time.sleep(0.2)

            if args.raw:
                send_raw(ser, header, body, args.write_size, args.delay)
            else:
                send_nops(
                    ser,
                    header,
                    body,
                    entry,
                    load_address,
                    args.write_size,
                    args.delay,
                    args.timeout,
                )
    except (OSError, TimeoutError) as exc:
        sys.exit(f"\nserial transfer failed: {exc}")
    except serial.SerialException as exc:
        sys.exit(f"\nserial transfer failed: {exc}")

    print("\ndone - the PS1 should now launch the program.")


if __name__ == "__main__":
    main()
