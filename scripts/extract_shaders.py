#!/usr/bin/env python3
"""Extract the Xenos shader containers embedded in RR6's default.xex.

RR6 (XDK 2.0.2135) ships every shader inside the encrypted, basic-compressed
XEX rather than as loose .vso/.pso files, and its containers use the early
0x102A0E0x revision. This decrypts the image, scans it for containers and
writes one file per unique container, which XenosRecomp then consumes:

    <out_dir>/vs_<VA>.vso    vertex shaders   (flags & 1 == 1)
    <out_dir>/ps_<VA>.pso    pixel shaders    (flags & 1 == 0)

<VA> is the guest address of the first copy of that container, so a name can
be looked up directly in a disassembler. Duplicate containers (same bytes at
several addresses) are written once and their extra addresses listed in
shaders.csv alongside the first.

The build runs this into out/build/<preset>/generated/shaders (see
cmake/shader_cache.cmake). Run by hand, it defaults to out/shaders.

Usage: extract_shaders.py [xex] [out_dir]
Requires: cryptography (pip install cryptography)
"""
import csv
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

XEX_RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
XEX_DEVKIT_KEY = bytes(16)
OPT_FILE_FORMAT_INFO = 0x000003FF
OPT_IMAGE_BASE_ADDRESS = 0x00010201

CONTAINER_REV_0E = 0x102A0E00
CTAB_HEADER_SIZE = 0x1C
CTAB_VERSION_VS = 0xFFFE0300
CTAB_VERSION_PS = 0xFFFF0300
MAX_CONTAINER_SIZE = 0x20000


def load_image(xex: bytes) -> tuple[bytes, int]:
    magic, _flags, pe_offset, _res, sec_offset, header_count = struct.unpack_from(
        ">4sIIIII", xex, 0)
    if magic != b"XEX2":
        raise SystemExit("not an XEX2 file")

    headers = dict(struct.unpack_from(">II", xex, 24 + i * 8) for i in range(header_count))
    image_base = headers.get(OPT_IMAGE_BASE_ADDRESS)
    if image_base is None:
        # Without the optional header the base lives in the security info.
        image_base = struct.unpack_from(">I", xex, sec_offset + 0x110)[0]

    ffi = headers[OPT_FILE_FORMAT_INFO]
    info_size, encryption, compression = struct.unpack_from(">IHH", xex, ffi)
    if compression != 1:
        raise SystemExit(f"unsupported XEX compression type {compression} (only basic)")
    blocks = [struct.unpack_from(">II", xex, ffi + 8 + i * 8)
              for i in range((info_size - 8) // 8)]

    payload = xex[pe_offset:]
    if encryption:
        payload = payload[: len(payload) // 16 * 16]
        file_key = xex[sec_offset + 0x150: sec_offset + 0x160]
        for key in (XEX_RETAIL_KEY, XEX_DEVKIT_KEY):
            session = Cipher(algorithms.AES(key), modes.ECB()).decryptor().update(file_key)
            plain = Cipher(algorithms.AES(session), modes.CBC(bytes(16))).decryptor().update(payload)
            if plain[:2] == b"MZ":
                payload = plain
                break
        else:
            raise SystemExit("XEX decryption failed with both retail and devkit keys")

    image = bytearray()
    pos = 0
    for data_size, zero_size in blocks:
        image += payload[pos: pos + data_size]
        image += bytes(zero_size)
        pos += data_size
    if image[:2] != b"MZ":
        raise SystemExit("decompressed image has no PE header")
    return bytes(image), image_base


def scan_containers(image: bytes):
    """Yields (offset, size, is_vertex) for every rev 0x0E container."""
    for i in range(0, len(image) - 0x18, 4):
        flags, virtual_size, physical_size, _c, ctab_offset, _d = struct.unpack_from(
            ">6I", image, i)
        if flags & 0xFFFFFFFE != CONTAINER_REV_0E:
            continue
        size = virtual_size + physical_size
        if not 0 < size <= MAX_CONTAINER_SIZE or i + size > len(image):
            continue
        if ctab_offset + 0x14 > virtual_size:
            continue
        _ctab_size, ctab_header, _creator, version = struct.unpack_from(
            ">4I", image, i + ctab_offset)
        is_vertex = bool(flags & 1)
        if ctab_header != CTAB_HEADER_SIZE:
            continue
        if version != (CTAB_VERSION_VS if is_vertex else CTAB_VERSION_PS):
            continue
        yield i, size, is_vertex


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    xex_path = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "assets" / "default.xex"
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "out" / "shaders"

    image, image_base = load_image(xex_path.read_bytes())
    out_dir.mkdir(parents=True, exist_ok=True)

    unique: dict[bytes, dict] = {}
    for offset, size, is_vertex in scan_containers(image):
        blob = image[offset: offset + size]
        va = image_base + offset
        entry = unique.get(blob)
        if entry is None:
            unique[blob] = {"va": va, "vertex": is_vertex, "size": size, "aliases": []}
        else:
            entry["aliases"].append(va)

    for path in list(out_dir.glob("vs_*.vso")) + list(out_dir.glob("ps_*.pso")):
        path.unlink()

    rows = []
    for blob, e in sorted(unique.items(), key=lambda kv: kv[1]["va"]):
        name = f"{'vs' if e['vertex'] else 'ps'}_{e['va']:08X}.{'vso' if e['vertex'] else 'pso'}"
        (out_dir / name).write_bytes(blob)
        rows.append([name, f"0x{e['va']:08X}", e["size"],
                     " ".join(f"0x{a:08X}" for a in e["aliases"])])

    with open(out_dir / "shaders.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["file", "va", "size", "alias_vas"])
        w.writerows(rows)

    vs = sum(1 for e in unique.values() if e["vertex"])
    refs = sum(1 + len(e["aliases"]) for e in unique.values())
    print(f"{len(unique)} unique shaders ({vs} vs, {len(unique) - vs} ps) "
          f"from {refs} containers -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
