import glob
import os
import shutil
import subprocess
import sys

# Assemble every include/*.pio into include/*.pio.h before the C++ build.
candidates = [shutil.which("pioasm"), "/usr/local/bin/pioasm"]
PIOASM = next((p for p in candidates if p and os.path.exists(p)), None)
if PIOASM is None:
    sys.stderr.write("pre_build.py: pioasm not found in PATH or /usr/local/bin\n")
    sys.exit(1)

for src in sorted(glob.glob("include/*.pio")):
    out = src + ".h"
    result = subprocess.run([PIOASM, src, out])
    if result.returncode != 0:
        sys.stderr.write(f"pre_build.py: pioasm failed on {src}\n")
        sys.exit(1)
    print(f"pioasm: {src} -> {out}")
