# Writes build_info.h (build time and git commit) into the build directory before compiling, so
# /api/system can say which firmware is running. Only files that include it rebuild when it changes.

Import("env")  # noqa: F821

import subprocess
from datetime import datetime
from pathlib import Path

project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
try:
    commit = subprocess.check_output(["git", "describe", "--always", "--dirty"], cwd=project_dir,
                                     text=True, stderr=subprocess.DEVNULL).strip()
except (OSError, subprocess.CalledProcessError):
    commit = "unknown"

out_dir = Path(env.subst("$BUILD_DIR")) / "generated"  # noqa: F821
out_dir.mkdir(parents=True, exist_ok=True)
header = out_dir / "build_info.h"
contents = (f'#pragma once\n#define FIRMWARE_BUILD "{datetime.now().strftime("%Y-%m-%d %H:%M")} {commit}"\n')
if not header.exists() or header.read_text() != contents:
    header.write_text(contents)
env.Append(CPPPATH=[str(out_dir)])  # noqa: F821
