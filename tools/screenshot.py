#!/usr/bin/env python3
"""Takes a screenshot of the device over USB serial (FAP_SCREENSHOT_V1).

    python tools/screenshot.py /dev/cu.usbmodem1101 shot.png

The firmware answers with a header line and the full 240x320 frame as
little-endian RGB565 (see main/fap_screenshot.c); the octopus scene is
included as it appears on the panel. Run it with the ESP-IDF Python (it has
pyserial), and close idf.py monitor first: the port can only be open once.
"""

import struct
import sys
import time
import zlib

import serial

CMD = b"FAP_SCREENSHOT_V1"


def write_png(path, width, height, rgb565_le):
    rows = []
    for y in range(height):
        line = bytearray([0])
        base = y * width * 2
        for x in range(width):
            (v,) = struct.unpack_from("<H", rgb565_le, base + x * 2)
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            line += bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31))
        rows.append(bytes(line))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip())
        return 2
    port, out = sys.argv[1], sys.argv[2]

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 5
    # Leave DTR/RTS alone: on the USB Serial/JTAG they reset the chip.
    ser.dtr = False
    ser.rts = False
    ser.open()
    try:
        ser.reset_input_buffer()
        ser.write(CMD + b"\n")
        deadline = time.time() + 10
        header = None
        while time.time() < deadline:
            line = ser.readline()
            if line.startswith(CMD + b" "):
                header = line.decode("ascii").split()
                break
        if not header:
            print("no screenshot header from the device", file=sys.stderr)
            return 1
        _, width, height, fmt, size = header
        width, height, size = int(width), int(height), int(size)
        if fmt != "RGB565LE" or size != width * height * 2:
            print(f"unexpected header: {' '.join(header)}", file=sys.stderr)
            return 1
        data = bytearray()
        while len(data) < size and time.time() < deadline + 20:
            data += ser.read(size - len(data))
        if len(data) != size:
            print(f"short frame: {len(data)}/{size} bytes", file=sys.stderr)
            return 1
    finally:
        ser.close()

    write_png(out, width, height, bytes(data))
    print(f"saved {out} ({width}x{height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
