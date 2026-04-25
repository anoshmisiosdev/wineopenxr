#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

# Stand-in for `winebuild --builtin <dll>`. Wine's loader checks for the
# 32-byte string "Wine builtin DLL" (NUL-padded) at file offset 64, between
# the DOS header and the PE header. Without it, WINEDLLOVERRIDES="wineopenxr=b"
# refuses to load the DLL as a builtin

import struct
import sys

SIGNATURE = b"Wine builtin DLL" + b"\0" * 16

if len(sys.argv) != 2:
    sys.exit("usage: sign_builtin.py <file.dll>")

with open(sys.argv[1], "r+b") as f:
    header = f.read(64)
    if len(header) < 64 or header[:2] != b"MZ":
        sys.exit(f"{sys.argv[1]}: not a PE file")
    e_lfanew = struct.unpack_from("<I", header, 0x3c)[0]
    if e_lfanew < 64 + len(SIGNATURE):
        sys.exit(f"{sys.argv[1]}: no room for signature (e_lfanew={e_lfanew})")
    f.seek(64)
    f.write(SIGNATURE)
