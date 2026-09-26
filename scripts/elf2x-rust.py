#!/usr/bin/env python3
"""Hide GNU_STACK from PSn00bSDK's elf2x without changing the linked ELF."""

from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


def main():
    arguments = sys.argv[1:]
    inputs = [argument for argument in arguments if argument != "-q"]
    if len(inputs) != 2:
        raise SystemExit("usage: elf2x-rust.py [-q] input.elf output.exe")
    source, output = inputs
    data = bytearray(Path(source).read_bytes())
    if data[:6] != b"\x7fELF\x01\x01":
        raise SystemExit("expected a little-endian ELF32 file")
    offset = struct.unpack_from("<I", data, 28)[0]
    entry_size, count = struct.unpack_from("<HH", data, 42)
    headers = [data[offset + index * entry_size:offset + (index + 1) * entry_size]
               for index in range(count)]
    # elf2x assumes the last program header carries the load size. Rust emits
    # GNU_STACK after LOAD, so omit that metadata header in the temporary copy.
    load_headers = [header for header in headers
                    if struct.unpack_from("<I", header)[0] != 0x6474E551]
    struct.pack_into("<H", data, 44, len(load_headers))
    for index, header in enumerate(load_headers):
        data[offset + index * entry_size:offset + (index + 1) * entry_size] = header
    with tempfile.TemporaryDirectory() as directory:
        prepared = Path(directory) / "prepared.elf"
        prepared.write_bytes(data)
        tool = shutil.which("elf2x")
        if tool is None:
            raise SystemExit("elf2x is unavailable")
        return subprocess.call([tool, *arguments[:-2], str(prepared), output])


if __name__ == "__main__":
    sys.exit(main())
