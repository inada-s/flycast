#!/usr/bin/env python

"""
Convert asm into c++ (flycast codes).
"""

import re
import time

import gen_abi

r_ope = re.compile(r"^\s*?([0-9a-f]+):\s*([0-9a-f]+) ([0-9a-f]+)\s*(.*)$")
r_symbol = re.compile(r"^([0-9a-f]+) <(\w+)>:")
r_section = re.compile(r"^Disassembly of section ([0-9a-zA-Z._]+):")

# Check the boundary with the game before touching the output.
symbols = {}
for line in open('bin/symbols.txt'):
    parts = line.split()
    if len(parts) == 3:
        symbols[parts[2]] = int(parts[0], 16)
gen_abi.check(symbols)

section = None
start = False
out = []
for line in open('bin/gdxsv_patch.asm'):
    line = line.rstrip()
    if 'Disassembly' in line:
        start = True
    if not start:
        continue

    g = r_section.match(line)
    if g:
        section = g.group(1).strip()
        print("section", section)
        out.append(f"//\n")
        out.append(f"// section {section}\n")
        out.append(f"//\n")

    g = r_ope.match(line)
    if g:
        addr = int(g.group(1), 16)
        data = int(g.group(2), 16) | int(g.group(3), 16) << 8
        if section == "gdx.func":
            addr += 0x80000000
        out.append(f"gdxsv_WriteMem16(0x{addr:08x}u, 0x{data:04x}u); // {g.group(4)}\n")

    g = r_symbol.match(line)
    if g:
        addr = int(g.group(1), 16)
        name = g.group(2)
        if section in ("gdx.data", "gdx.func"):
            out.append(f'symbols_["{name}"] = 0x{addr:08x};\n')

with open('../core/gdxsv/gdxsv_patch.inc', 'w') as f:
    f.writelines(out)
    # Slot 99 bypasses dialing. Publish the payload's hooks with it, after its
    # code/data are present and before SH4 resumes. Offline savestates of
    # released versions keep their installed pointers; after loading, they
    # must support reconnecting to the lobby and playing a complete battle.
    gen_abi.emit_payload_hooks(f)
    f.write(f'symbols_[":patch_id"] = {str(int(time.time()) % 100000000)};\n')
    f.write(f'gdxsv_WriteMem32(symbols_["patch_id"], symbols_[":patch_id"]);\n')
gen_abi.write_host_header(gen_abi.HOST_HEADER)
