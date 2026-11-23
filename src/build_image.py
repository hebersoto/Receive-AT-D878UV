#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 KP3M
"""Build the modified AnyTone AT-D878UV firmware set (analog APRS RX pop-up).

Two vector hooks (SysTick + TIM6), Radio Info line redirected, lengths
fixed. PLUS: patch the firmware's shared screen-0x68 popup body (two centered std-font
draws at 0x08041f10) to `bl our_draw68` + NOP padding, so our appended draw function
renders the popup (small-font, multi-line, no diamond for our RX popup; original look
preserved for the TX popup / screen 0x38 via a group-pointer check inside our_draw68).

Usage: build_image.py <stock_dir> <hook.elf> <hook.bin> <out_dir>
"""
import struct, sys, os, hashlib, subprocess

BASE        = 0x08004000
HOOK_ADDR   = 0x080E7600
VEC_SYSTICK = 0x3C
VEC_TIM6    = 70 * 4            # 0x118
VER_PTR_OFF = 0x08037844 - BASE
RAM_BUF     = 0x20015800
STOCK_SYSTK = 0x08007CA9
STOCK_TIM6  = 0x08004A13
STOCK_VERP  = 0x080AAE70
NAME        = "D878UV_V4.00_20250725"

# screen-0x68 popup body patch (shared with TX popup + screen 0x38)
BODY_VA     = 0x08041f10        # first instr of the two-draw block (0x68 entry nop is at f0e)
BODY_OFF    = BODY_VA - BASE
BODY_LEN    = 0x38             # 56 bytes = the two std-font draw blocks
BODY_ORIG   = bytes.fromhex(
    "944800680068102141220b46cde900210290812241210d20eef75bfb"
    "8d4800684068102154220b46cde900210290812254210d20eef74dfb")
NOP         = b"\x00\xbf"

def sym(elf, name):
    out = subprocess.check_output(["arm-none-eabi-nm", elf]).decode()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit("symbol not found: " + name)

def thumb_bl(pc, target):
    """Encode a Thumb BL (4 bytes, little-endian) from pc to target."""
    imm = target - (pc + 4)
    if imm & 1:
        raise SystemExit("BL target not halfword aligned")
    imm >>= 1
    if not (-(1 << 23) <= imm < (1 << 23)):
        raise SystemExit("BL target out of range")
    S  = (imm >> 23) & 1
    I1 = (imm >> 22) & 1
    I2 = (imm >> 21) & 1
    imm10 = (imm >> 11) & 0x3FF
    imm11 = imm & 0x7FF
    J1 = (I1 ^ 1) ^ S
    J2 = (I2 ^ 1) ^ S
    hw1 = 0xF000 | (S << 10) | imm10
    hw2 = 0xD000 | (J1 << 13) | (J2 << 11) | imm11
    return struct.pack("<HH", hw1, hw2)

def main():
    stock, elf, hookp, out = sys.argv[1:5]
    os.makedirs(out, exist_ok=True)
    st = sym(elf, "st_hook"); t6 = sym(elf, "t6_hook"); dr = sym(elf, "our_draw68")
    cdd = bytearray(open(os.path.join(stock, NAME+".CDD"), "rb").read())
    cdi = bytearray(open(os.path.join(stock, NAME+".CDI"), "rb").read())
    spi = bytearray(open(os.path.join(stock, NAME+".spi"), "rb").read())
    hook = open(hookp, "rb").read()

    assert len(cdd) == 931328 == HOOK_ADDR - BASE
    assert struct.unpack_from("<I", cdd, VEC_SYSTICK)[0] == STOCK_SYSTK
    assert struct.unpack_from("<I", cdd, VEC_TIM6)[0]    == STOCK_TIM6
    assert struct.unpack_from("<I", cdd, VER_PTR_OFF)[0] == STOCK_VERP
    assert st == HOOK_ADDR, "st_hook must be at image base (0x%08x), got 0x%08x" % (HOOK_ADDR, st)
    assert struct.unpack_from("<I", spi, 6)[0]   == 931328, ".spi length offset mismatch"
    assert struct.unpack_from("<I", cdi, 260)[0] == 931328, ".CDI length offset mismatch"
    # the popup body must be exactly the stock two-draw block before we overwrite it
    assert bytes(cdd[BODY_OFF:BODY_OFF+BODY_LEN]) == BODY_ORIG, "screen-0x68 body mismatch"

    # append our code, patch vectors + Radio Info ptr
    cdd += hook + b"\x00" * ((-len(hook)) % 4)
    new_len = len(cdd)
    struct.pack_into("<I", cdd, VEC_SYSTICK, st | 1)
    struct.pack_into("<I", cdd, VEC_TIM6,    t6 | 1)
    struct.pack_into("<I", cdd, VER_PTR_OFF, RAM_BUF)
    struct.pack_into("<I", spi, 6, new_len)
    struct.pack_into("<I", cdi, 260, new_len)

    # patch the popup body: bl our_draw68 + NOP fill (keeps the timer tail at 0x08041f4c)
    patch = thumb_bl(BODY_VA, dr) + NOP * ((BODY_LEN - 4) // 2)
    assert len(patch) == BODY_LEN
    cdd[BODY_OFF:BODY_OFF+BODY_LEN] = patch

    if BASE + new_len > 0x08100000:
        raise SystemExit("ERROR: image overflows 1 MB flash (0x%08x)" % (BASE+new_len))
    if not (new_len == struct.unpack_from('<I',spi,6)[0] == struct.unpack_from('<I',cdi,260)[0]):
        raise SystemExit("ERROR: declared-length mismatch across files")

    open(os.path.join(out, NAME+".CDD"), "wb").write(cdd)
    open(os.path.join(out, NAME+".CDI"), "wb").write(cdi)
    open(os.path.join(out, NAME+".spi"), "wb").write(spi)

    print("Modified firmware image built in", out)
    print("  st_hook   @", hex(st), "-> vector[15]")
    print("  t6_hook   @", hex(t6), "-> vector[70]")
    print("  our_draw68@", hex(dr), "-> bl at 0x%08x" % BODY_VA)
    print("  new image size :", new_len, "(stock 931328, +%d)" % (new_len-931328))
    print("  SysTick vec    : %08x -> %08x" % (STOCK_SYSTK, struct.unpack_from('<I',cdd,VEC_SYSTICK)[0]))
    print("  TIM6 vec       : %08x -> %08x" % (STOCK_TIM6,  struct.unpack_from('<I',cdd,VEC_TIM6)[0]))
    print("  Radio Info ptr : %08x -> %08x" % (STOCK_VERP,  struct.unpack_from('<I',cdd,VER_PTR_OFF)[0]))
    print("  0x68 body patch: %s" % patch[:4].hex())
    print("  consistency    :", "OK" if new_len==struct.unpack_from('<I',spi,6)[0]==struct.unpack_from('<I',cdi,260)[0] else "MISMATCH")
    print("  within 1 MB    :", "OK" if BASE+new_len <= 0x08100000 else "OVERFLOW")
    print("  CDD sha256     :", hashlib.sha256(cdd).hexdigest())

if __name__ == "__main__":
    main()
