#!/usr/bin/env python3
"""Build levels.bin — the raw image for the 'levels' data partition.

Layout: a tiny read-only file table followed by the file blobs, all
4-byte aligned so esp_partition_mmap'd pointers are usable directly.

  offset 0:  uint32 magic 'OLVL' (0x4C564C4F little-endian)
  offset 4:  uint32 count
  offset 8:  count * entry { char name[16]; uint32 offset; uint32 size; }
  then:      file data (each 4-byte aligned)

Usage:  python make_levels.py <out.bin> <file1> [file2 ...]
"""
import os
import struct
import sys

MAGIC = 0x4C564C4F  # 'OLVL'


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    out_path = sys.argv[1]
    files = sys.argv[2:]

    header_size = 8 + len(files) * 24
    data_off = (header_size + 3) & ~3

    entries = []
    blobs = []
    for f in files:
        blob = open(f, 'rb').read()
        name = os.path.basename(f).encode()
        if len(name) > 15:
            sys.exit(f'name too long: {name}')
        entries.append((name, data_off, len(blob)))
        blobs.append(blob)
        data_off = (data_off + len(blob) + 3) & ~3

    with open(out_path, 'wb') as out:
        out.write(struct.pack('<II', MAGIC, len(files)))
        for name, off, size in entries:
            out.write(struct.pack('<16sII', name, off, size))
        pos = 8 + len(files) * 24
        for (name, off, size), blob in zip(entries, blobs):
            out.write(b'\0' * (off - pos))
            out.write(blob)
            pos = off + size

    total = os.path.getsize(out_path)
    print(f'{out_path}: {len(files)} files, {total} bytes ({total/1024/1024:.2f} MB)')
    for name, off, size in entries:
        print(f'  {name.decode():14s} @ 0x{off:07X}  {size:8d} B')


if __name__ == '__main__':
    main()
