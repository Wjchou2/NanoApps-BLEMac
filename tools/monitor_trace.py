#!/usr/bin/env python3
"""Save NanoApps RAM trace entries to the Mac while an ejected Nano runs."""

import argparse
from datetime import datetime
from functools import partial
import math
import os
from pathlib import Path
import platform
import struct
import subprocess
import time

from macos_transport import peek_ejected


ROOT = Path(__file__).resolve().parents[1]
REPORT = 0x086C9B84
REPORT_MAGIC = 0x32505452
TRACE_MAGIC = 0x48425452
TRACE_RANGE = (0x08D00000, 0x0A000000)
TRACE_HEADER = struct.Struct('<4I')
TRACE_ENTRY = struct.Struct('<8sII')
TRACE_CAPACITY = 256
TRACE_BYTES = TRACE_HEADER.size + TRACE_CAPACITY * TRACE_ENTRY.size


def read_header(peek, address):
    magic, write_index, count, boot_seq = TRACE_HEADER.unpack(
        peek(address, TRACE_HEADER.size))
    if magic != TRACE_MAGIC or write_index != count % TRACE_CAPACITY:
        raise RuntimeError('RAM trace changed during read')
    return count, boot_seq


def trace_batch(peek, cursor):
    """Return a new (address, boot_seq, count) cursor and one batch of lines."""
    report = peek(REPORT + 0x2C, 0x128)
    if struct.unpack_from('<I', report)[0] != REPORT_MAGIC:
        raise RuntimeError('NanoApps boot report is unavailable')
    address, = struct.unpack_from('<I', report, 0x150 - 0x2C)
    if not TRACE_RANGE[0] <= address <= TRACE_RANGE[1] - TRACE_BYTES or address % 16:
        raise RuntimeError('NanoApps has not published an active trace')

    end, boot_seq = read_header(peek, address)
    same_ring = cursor is not None and cursor[:2] == (address, boot_seq) and cursor[2] <= end
    start = cursor[2] if same_ring else 0
    lines = [] if same_ring else [f'boot sequence {boot_seq}, ring {address:#010x}']
    if end == start:
        return (address, boot_seq, end), lines

    # Allow one slot for a write that has started but is not committed.
    first = max(start, end - TRACE_CAPACITY + 1)
    slot = first % TRACE_CAPACITY
    length = min(end - first, TRACE_CAPACITY - slot)
    entries = peek(address + TRACE_HEADER.size + slot * TRACE_ENTRY.size,
                   length * TRACE_ENTRY.size)
    if first + length < end:
        entries += peek(address + TRACE_HEADER.size,
                        (end - first - length) * TRACE_ENTRY.size)

    confirmed, confirmed_boot_seq = read_header(peek, address)
    if confirmed_boot_seq != boot_seq or confirmed < end:
        raise RuntimeError('RAM trace restarted during read')

    # The producer can advance while individual RAM pages are being read.
    available = min(end, max(first, confirmed - TRACE_CAPACITY + 1))
    if available > start:
        lines.append(f'{available - start} entries overwritten before capture')
    for number in range(available, end):
        offset = (number - first) * TRACE_ENTRY.size
        tag, first_value, second_value = TRACE_ENTRY.unpack_from(entries, offset)
        tag = tag.rstrip(b' \0').decode('ascii', 'replace')
        lines.append(f'{number + 1:7d} {tag:<8} {first_value:#010x} {second_value:#010x}')
    return (address, boot_seq, end), lines


def monitor(peek, output, interval):
    cursor = None
    interrupted = False
    while True:
        try:
            cursor, lines = trace_batch(peek, cursor)
        except (RuntimeError, subprocess.SubprocessError) as error:
            stderr = getattr(error, 'stderr', b'') or b''
            details = stderr.decode(errors='replace').strip().splitlines()
            message = details[-1] if details else str(error)
            lines = [] if interrupted else [f'capture paused: {message}']
            interrupted = True
        else:
            if interrupted:
                lines.insert(0, 'capture resumed')
                interrupted = False
        if lines:
            timestamp = datetime.now().astimezone().isoformat(timespec='milliseconds')
            output.writelines(f'{timestamp} {line}\n' for line in lines)
            output.flush()
            os.fsync(output.fileno())
        time.sleep(interval)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='trace file on the Mac, appended if it exists')
    parser.add_argument('--interval', type=float, default=0.5, help='seconds between reads (default: 0.5)')
    args = parser.parse_args()
    if not math.isfinite(args.interval) or args.interval <= 0:
        parser.error('--interval must be a positive number')
    if platform.system() != 'Darwin':
        parser.error('this monitor uses the macOS USB transport')
    if not (ROOT / 'tools/macos/ipod_sg_raw').is_file():
        parser.error('build tools/macos/ipod_sg_raw with ./start before ejecting the Nano')
    print(f'Writing to {args.output}. Leave the Nano connected and ejected. Press Ctrl-C to stop.',
          flush=True)
    reader = partial(peek_ejected, ROOT, capture_output=True, timeout=2)
    try:
        with args.output.open('a', encoding='utf-8') as output:
            monitor(reader, output, args.interval)
    except KeyboardInterrupt:
        print('Trace capture stopped.')


if __name__ == '__main__':
    main()
