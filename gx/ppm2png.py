#!/usr/bin/env python3
"""Converts a binary PPM (what gx writes with GX_FRAME) to a PNG."""
import struct
import sys
import zlib


def chunk(kind, data):
    body = kind + data
    return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))


def main(src, dst):
    data = open(src, 'rb').read()
    fields, pos = [], 0
    while len(fields) < 4:  # magic, width, height, maximum
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end + 1
    if fields[0] != b'P6' or fields[3] != b'255':
        sys.exit('%s is not an 8-bit binary PPM' % src)
    width, height = int(fields[1]), int(fields[2])
    rows = b''.join(b'\0' + data[pos + y * width * 3:pos + (y + 1) * width * 3]
                    for y in range(height))
    with open(dst, 'wb') as out:
        out.write(b'\x89PNG\r\n\x1a\n')
        out.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)))
        out.write(chunk(b'IDAT', zlib.compress(rows, 6)))
        out.write(chunk(b'IEND', b''))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
