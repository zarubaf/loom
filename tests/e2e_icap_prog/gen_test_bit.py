#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Generate a minimal test partial bitstream for ICAP e2e simulation.
#
# The file contains a short dummy header followed by the ICAP sync word and
# 24 NOOP (Type-1 NOP) words.  The behavioral ICAPE3 stub in
# xilinx_primitives.sv fires PRDONE 16 cycles after the last write regardless
# of content, so this is sufficient to exercise the full host→RTL path.
#
# Byte layout follows UG570 Table 2-7 (big-endian storage in the file):
#   sync word bytes: 0xAA 0x99 0x55 0x66
#   NOP  word bytes: 0x20 0x00 0x00 0x00

import sys

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test.bit"

    SYNC = bytes([0xAA, 0x99, 0x55, 0x66])   # ICAP sync word (host searches for this)
    NOOP = bytes([0x20, 0x00, 0x00, 0x00])   # Type-1 NOP packet

    # A few dummy header bytes — real .bit files have a ~100-byte ASCII header;
    # the host skips everything before the sync word.
    header = bytes([0xFF, 0xFF, 0xFF, 0xFF,
                    0x00, 0x00, 0x00, 0xBB,
                    0x11, 0x22, 0x33, 0x44])

    # 24 NOOPs after the sync word — well above the 16-cycle PRDONE threshold
    payload = SYNC + NOOP * 24

    with open(out, "wb") as f:
        f.write(header)
        f.write(payload)

    print(f"Generated {out}: {len(header)}-byte header + {len(payload)}-byte payload "
          f"({len(payload)//4} words after sync)")

if __name__ == "__main__":
    main()
