#!/usr/bin/env bash
# Stage the locally built Jazz homebrew into the Retro-Go SD firmware tree,
# rebuild the emulated SD card, and launch gwemu.
#
# Usage:
#   ./run_gwemu.sh [--qmp PORT] [other run_gwemu.sh flags]
#
# Environment:
#   FW_ROOT   Retro-Go SD checkout (default: ../game-and-watch-retro-go-sd)
set -euo pipefail

if command -v docker >/dev/null 2>&1; then
    CR=docker
elif command -v podman >/dev/null 2>&1; then
    CR=podman
else
    echo "Neither docker nor podman found." >&2
    exit 1
fi

JAZZ_ROOT="$(cd "$(dirname "$0")" && pwd)"
FW_ROOT="${FW_ROOT:-$JAZZ_ROOT/../game-and-watch-retro-go-sd}"
FW_ROOT="$(cd "$FW_ROOT" && pwd)"

if [ ! -f "$JAZZ_ROOT/Jazz.bin" ]; then
    echo "Jazz.bin not found in $JAZZ_ROOT" >&2
    if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
        echo "Building Jazz.bin..." >&2
        make -C "$JAZZ_ROOT"
    else
        echo "No arm toolchain; building Jazz.bin in container..." >&2
        "$CR" run --rm --user "$(id -u):$(id -g)" \
            -v "$JAZZ_ROOT:/opt/workdir" -w /opt/workdir \
            sylverb/retro-go-sd-builder:v1.5 make
    fi
fi
if [ ! -f "$JAZZ_ROOT/Jazz.jj" ]; then
    echo "Jazz.jj not found in $JAZZ_ROOT (run tools/jj_pack.py first)." >&2
    exit 1
fi

echo "[stage] Jazz.bin + Jazz.jj -> $FW_ROOT/sd_content/homebrews/"
mkdir -p "$FW_ROOT/sd_content/homebrews"
cp -f "$JAZZ_ROOT/Jazz.bin" "$JAZZ_ROOT/Jazz.jj" "$FW_ROOT/sd_content/homebrews/"

echo "[config] writing sd_content/CONFIG (auto-start /homebrews/Jazz.bin)"
python3 - "$FW_ROOT/sd_content/CONFIG" <<'PY'
import struct, zlib, sys

CONFIG_MAGIC = 0xCAFEF00D
CONFIG_VERSION = 9
STARTUP_FILE_LEN = 256
BROWSE_SUBPATH_LEN = 96
RESERVED_APP_LEN = 32
CONFIG_SIZE = 416

startup_file = "/homebrews/Jazz.bin"
menu_timeout_s = 0

reserved = bytearray(RESERVED_APP_LEN)
blob = struct.pack(
    "<I"
    "12B"
    f"{STARTUP_FILE_LEN}s"
    "3H"
    f"{BROWSE_SUBPATH_LEN}s"
    "B"
    "x"
    "I"
    f"{RESERVED_APP_LEN}s"
    "I",
    CONFIG_MAGIC,
    CONFIG_VERSION,
    6,
    0,
    4,
    8,
    2,
    0,
    0,
    0,
    0,
    0,
    0,
    startup_file.encode(),
    menu_timeout_s,
    0,
    0,
    b"",
    0,
    1,
    bytes(reserved),
    0,
)
assert len(blob) == CONFIG_SIZE, (len(blob), CONFIG_SIZE)

crc = zlib.crc32(blob[:-4]) & 0xFFFFFFFF
blob = blob[:-4] + struct.pack("<I", crc)

with open(sys.argv[1], "wb") as fh:
    fh.write(blob)
print(f"  wrote {sys.argv[1]} ({len(blob)} bytes), startup_file={startup_file}")
PY

echo "[build] rebuilding emulated SD card"
# Prefer the patched Zelda bank-1 image so gwemu chainloads Retro-Go (bank 2).
if [ ! -f "$FW_ROOT/backup/internal_flash_backup_zelda.bin" ] || [ "$(stat -f%z "$FW_ROOT/backup/internal_flash_backup_zelda.bin" 2>/dev/null || echo 0)" -lt 262144 ]; then
    for cand in "$JAZZ_ROOT/../gwemu/backup/qemu-images" "/Users/gbr/code/gwemu/backup/qemu-images"; do
        if [ -f "$cand/zelda-bank1-patched.bin" ]; then
            mkdir -p "$FW_ROOT/backup"
            cp "$cand/zelda-bank1-patched.bin" "$FW_ROOT/backup/internal_flash_backup_zelda.bin"
            break
        fi
    done
fi
# Ensure bank-1 bootloader backup is available for INTFLASH_BANK=2 builds.
if [ ! -f "$FW_ROOT/backup/internal_flash_backup_mario.bin" ] && [ ! -f "$FW_ROOT/backup/internal_flash_backup_zelda.bin" ]; then
    for cand in "$JAZZ_ROOT/../gwemu/backup" "/Users/gbr/code/gwemu/backup"; do
        if [ -f "$cand/internal_flash_backup_zelda.bin" ]; then
            mkdir -p "$FW_ROOT/backup"
            cp "$cand/internal_flash_backup_zelda.bin" "$FW_ROOT/backup/"
            break
        fi
        if [ -f "$cand/internal_flash_backup_mario.bin" ]; then
            mkdir -p "$FW_ROOT/backup"
            cp "$cand/internal_flash_backup_mario.bin" "$FW_ROOT/backup/"
            break
        fi
    done
fi

cd "$FW_ROOT"
rm -f build/sdcard.img build/.sd_content.stamp
# Build firmware + sd_content in the Retro-Go SD builder container.
"$CR" run --rm --user "$(id -u):$(id -g)" \
    -v "$FW_ROOT:/opt/workdir" -w /opt/workdir \
    sylverb/retro-go-sd-builder:v1.5 \
    make -j"$(sysctl -n hw.ncpu)" INTFLASH_BANK=1 release CHECK_DIRTY_SUBMODULE=0
# Create the gwemu SD card image locally (needs mtools, not the ARM toolchain).
# GWEMU_PREREQS= prevents a local rebuild of the release target.
make gwemu_release GWEMU_PREREQS= INTFLASH_BANK=1 CHECK_DIRTY_SUBMODULE=0

# Force a 3x window on startup (320x240 * 3 = 960x720 + 32px fixed menubar = 960x752).
# gwemu requires startup_size = 'last_used' to honor last_width / last_height.
GWEMU_CFG="$HOME/Library/Application Support/gwemu/gwemu/gwemu.toml"
mkdir -p "$(dirname "$GWEMU_CFG")"
python3 - "$GWEMU_CFG" <<'PYCFG'
import re, sys
path = sys.argv[1]
try:
    text = open(path).read()
except FileNotFoundError:
    text = ""

target_keys = {
    'startup_size': "'last_used'",
    'last_width': '960',
    'last_height': '752',
}

pattern = re.compile(r'(^|\n)\[display\.window\]\n(.*?)(?=(\n\[|\Z))', re.DOTALL)
m = pattern.search(text)
if m:
    sec = m.group(2)
    lines = sec.splitlines()
    keys_found = set()
    new_lines = []
    for line in lines:
        k = line.split('=')[0].strip() if '=' in line else None
        if k in target_keys:
            new_lines.append(f'{k} = {target_keys[k]}')
            keys_found.add(k)
        else:
            new_lines.append(line)
    for k, v in target_keys.items():
        if k not in keys_found:
            new_lines.append(f'{k} = {v}')
    new_sec = '\n'.join(new_lines).strip() + '\n'
    prefix = m.group(1)
    text = text[:m.start()] + prefix + '[display.window]\n' + new_sec + text[m.end():]
else:
    sec_text = '\n[display.window]\n' + '\n'.join(f'{k} = {v}' for k, v in target_keys.items()) + '\n'
    text = text.rstrip() + ('\n' if text.strip() else '') + sec_text

with open(path, "w") as fh:
    fh.write(text)
PYCFG

echo "[run] launching gwemu"
QMP_PORT="${QMP_PORT:-4477}"
exec "$FW_ROOT/gwemu_bin" -M gnw-h7b0 \
  -global gnw-h7b0-soc.bank1-image="$FW_ROOT/build/qemu_bank1.bin" \
  -global gnw-h7b0-soc.bank2-image="$FW_ROOT/build/qemu_bank2.bin" \
  -global gnw-h7b0-soc.extflash-image="$FW_ROOT/build/extflash.bin" \
  -drive if=sd,file="$FW_ROOT/build/sdcard.img",format=raw \
  -audiodev sdl3,id=snd0 -global gnw-h7b0-sai1.audiodev=snd0 \
  -display gwemu \
  -qmp tcp:localhost:${QMP_PORT},server,nowait \
  "$@"
