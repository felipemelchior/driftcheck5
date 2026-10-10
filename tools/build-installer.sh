#!/usr/bin/env bash
# DriftCheck - builds the installer payload from the finished app folder.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   bash tools/build-installer.sh
#
# Embeds every file of dist/<TITLE_ID>/ in an ELF payload (installer/installer.c)
# that writes them to /data/homebrew/<TITLE_ID>/ on the console. Run `make` first.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}
[[ -x $sdk/bin/prospero-clang ]] || {
    echo "no payload SDK at $sdk: run make first, or set PS5_PAYLOAD_SDK" >&2
    exit 1
}

read -r title version < <(python3 -I - "$root/sce_sys/param.json" <<'PY'
import json, sys
param = json.load(open(sys.argv[1]))
print(param["titleId"], param["contentVersion"])
PY
)
app="$root/dist/$title"
[[ -f $app/eboot.bin ]] || {
    echo "no app folder at $app: run make first" >&2
    exit 1
}

work="$root/build/installer"
rm -rf "$work"
mkdir -p "$work"

# One .incbin per file, and the table the payload walks.
python3 -I - "$app" "$work" "$title" "$version" <<'PY'
import os, sys
app, work, title, version = sys.argv[1:5]
files = []
for folder, _, names in os.walk(app):
    for name in sorted(names):
        full = os.path.join(folder, name)
        files.append((os.path.relpath(full, app), full))
files.sort()
with open(os.path.join(work, "installer_data.S"), "w") as asm:
    asm.write('    .section .rodata\n')
    for index, (_, full) in enumerate(files):
        asm.write(f'    .balign 16\n    .global installer_file_{index}_start\n'
                  f'installer_file_{index}_start:\n    .incbin "{full}"\n'
                  f'    .global installer_file_{index}_end\ninstaller_file_{index}_end:\n')
with open(os.path.join(work, "installer_files.h"), "w") as header:
    header.write('#include <stddef.h>\n\n')
    header.write(f'#define INSTALLER_DESTINATION "/data/homebrew/{title}"\n')
    header.write(f'#define INSTALLER_VERSION "{version}"\n\n')
    header.write('typedef struct {\n\tconst char *name;\n\tconst unsigned char *start;\n'
                 '\tconst unsigned char *end;\n} installer_file;\n\n')
    for index in range(len(files)):
        header.write(f'extern const unsigned char installer_file_{index}_start[];\n'
                     f'extern const unsigned char installer_file_{index}_end[];\n')
    header.write('\n/* eboot.bin and param.json last: ShadowMountPlus registers a title once\n'
                 ' * param.json exists, and the app is only complete once eboot.bin is. */\n')
    def order(entry):
        name = entry[1][0]
        return (name == "sce_sys/param.json", name == "eboot.bin", name)
    ordered = sorted(enumerate(files), key=order)
    header.write('static const installer_file installer_files[] = {\n')
    for index, (name, _) in ordered:
        header.write(f'\t{{"{name}", installer_file_{index}_start, installer_file_{index}_end}},\n')
    header.write('};\nstatic const size_t installer_file_count = '
                 'sizeof(installer_files) / sizeof(installer_files[0]);\n')
print(f"{len(files)} files embedded")
PY

out="$root/dist/DriftCheck.elf"
"$sdk/bin/prospero-clang" -O2 -Wall -Wextra -I"$work" \
    -o "$out" "$root/installer/installer.c" "$work/installer_data.S"
# The loader reads the payload up to its section table; trailing bytes break the protocol.
python3 "$root/tools/validate-loader-elf.py" "$out"
echo "installer: $out ($(stat -c %s "$out") bytes, DriftCheck $version for $title)"
