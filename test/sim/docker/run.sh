#!/bin/sh
# Runs a command inside the obuspa-sim-dev image against the current working tree.
# The tree (tracked + untracked, minus ignored files) is synced via tar into the
# persistent volume 'obuspa-sim-src' mounted at /src, so builds in /src/build-linux
# are incremental across runs. This works even when the repo is outside the
# directories Colima/Docker Desktop share with the VM.
#   test/sim/docker/run.sh 'cmake -S . -B build-linux && cmake --build build-linux -j8'
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
export COPYFILE_DISABLE=1   # no macOS ._ metadata files in the tar stream
git ls-files -z --cached --others --exclude-standard \
  | xargs -0 tar -cf - 2>/dev/null \
  | docker run --rm -i --init -v obuspa-sim-src:/src -w /src obuspa-sim-dev \
      bash -c 'tar -xf - --overwrite && exec bash -c "$0"' "$*"
