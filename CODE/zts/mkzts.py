#!/usr/bin/env python3
"""mkzts.py — 把 ELF64 ET_REL 打包成 ZTS 模块（760B 头 + 16B 对齐 + 正文）

用法:
  python mkzts.py --in module.rel --out 0x00000001.zts \
      --name example --desc "示例算法" --author Deshab --license MIT \
      --category 9 --major 1 --minor 0 --patch 0 \
      --uuid 00112233445566778899aabbccddeeff --deps 0
"""
import argparse
import struct
import sys

ZTS_MAGIC = 0x53545A00
ZTS_FORMAT_VERSION = 2
HEADER_SIZE = 760
BODY_OFFSET = 768

HEADER_FMT = "<IHH64s256s64s32s16sIIIIIQQQQQI256s"


def fnv1a32(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def cstr(s: str, n: int) -> bytes:
    b = s.encode("utf-8")[: n - 1]
    return b + b"\x00" * (n - len(b))


def parse_uuid(s: str) -> bytes:
    s = s.replace("-", "").replace(" ", "")
    if len(s) != 32:
        raise SystemExit("uuid must be 32 hex chars")
    return bytes.fromhex(s)


def elf_symbol_offsets(body: bytes):
    if body[:4] != b"\x7fELF":
        raise SystemExit("not an ELF file")
    if body[4] != 2 or body[5] != 1:
        raise SystemExit("need ELF64 little-endian")
    e_type, e_machine = struct.unpack_from("<HH", body, 16)
    if e_type != 1:
        raise SystemExit("need ET_REL (link with ld.lld -r)")
    if e_machine != 0x3E:
        raise SystemExit("need x86_64")
    e_shoff = struct.unpack_from("<Q", body, 40)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", body, 58)
    if e_shentsize != 64 or e_shnum == 0:
        raise SystemExit("bad section table")

    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link,
         sh_info, sh_addralign, sh_entsize) = struct.unpack_from("<IIQQQQIIQQ", body, off)
        secs.append(dict(offset=sh_offset, size=sh_size, type=sh_type,
                         link=sh_link, entsize=sh_entsize))

    syms = None
    for s in secs:
        if s["type"] == 2:  # SHT_SYMTAB
            syms = s
            break
    if syms is None:
        raise SystemExit("no .symtab; do not strip the relocatable object")

    strtab = secs[syms["link"]]
    str_off = strtab["offset"]
    want = {"zts_module_init": 0, "zts_module_exec": 0, "zts_module_cleanup": 0}
    entsize = syms["entsize"] or 24
    n = syms["size"] // entsize
    for i in range(n):
        off = syms["offset"] + i * entsize
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
            "<IBBHQQ", body, off)
        if st_shndx == 0 or st_shndx >= 0xFF00 or st_shndx >= len(secs):
            continue
        name_off = str_off + st_name
        end = body.find(b"\x00", name_off)
        name = body[name_off:end].decode("ascii", "replace") if end >= 0 else ""
        if name in want:
            want[name] = secs[st_shndx]["offset"] + st_value
    return want


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", dest="out", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--desc", default="")
    ap.add_argument("--author", default="Deshab")
    ap.add_argument("--license", default="MIT")
    ap.add_argument("--category", type=int, default=9)
    ap.add_argument("--major", type=int, default=1)
    ap.add_argument("--minor", type=int, default=0)
    ap.add_argument("--patch", type=int, default=0)
    ap.add_argument("--uuid", required=True)
    ap.add_argument("--deps", type=int, default=0)
    args = ap.parse_args()

    body = open(args.inp, "rb").read()
    offs = elf_symbol_offsets(body)
    uuid = parse_uuid(args.uuid)
    checksum = fnv1a32(body)

    hdr = struct.pack(
        HEADER_FMT,
        ZTS_MAGIC, ZTS_FORMAT_VERSION, 2,
        cstr(args.name, 64), cstr(args.desc, 256),
        cstr(args.author, 64), cstr(args.license, 32), uuid,
        args.category, args.major, args.minor, args.patch, args.deps,
        offs["zts_module_init"], offs["zts_module_cleanup"], offs["zts_module_exec"],
        0, 0,
        checksum, b"\x00" * 256,
    )
    assert len(hdr) == HEADER_SIZE, len(hdr)

    out = bytearray()
    out += hdr
    out += b"\x00" * (BODY_OFFSET - HEADER_SIZE)
    out += body
    open(args.out, "wb").write(out)
    print(f"[mkzts] {args.out}: name={args.name} v{args.major}.{args.minor}.{args.patch} "
          f"body={len(body)} total={len(out)} crc=0x{checksum:08x}")


if __name__ == "__main__":
    main()
