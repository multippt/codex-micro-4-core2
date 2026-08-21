#!/usr/bin/env bash

set -euo pipefail

usage() {
  echo "Usage: $0 {core2|tab5|all} [PlatformIO arguments...]" >&2
}

target="${1:-core2}"
if [[ $# -gt 0 ]]; then
  shift
fi

case "$target" in
  core2)
    builds=("core2:m5stack-core2")
    ;;
  tab5)
    builds=("tab5:m5stack-tab5")
    ;;
  all)
    builds=("core2:m5stack-core2" "tab5:m5stack-tab5")
    ;;
  *)
    usage
    exit 2
    ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
local_pio="$project_root/.pio-core/penv/bin/pio"

if command -v pio >/dev/null 2>&1; then
  pio_executable="$(command -v pio)"
elif [[ -x "$local_pio" ]]; then
  pio_executable="$local_pio"
else
  echo "PlatformIO Core was not found. Install 'pio' or make the project-local .pio-core runtime available." >&2
  exit 127
fi

build_temp_dir="$project_root/.pio-tmp"
mkdir -p "$build_temp_dir"

for build in "${builds[@]}"; do
  board="${build%%:*}"
  environment="${build#*:}"
  packages_dir="$project_root/.pio-packages/$board"
  build_dir="$project_root/.pio/build-$board"

  echo "Building $environment with isolated packages and build state"
  echo "  Packages: $packages_dir"
  echo "  Build:    $build_dir"

  PLATFORMIO_CORE_DIR="$project_root/.pio-core" \
  PLATFORMIO_PACKAGES_DIR="$packages_dir" \
  PLATFORMIO_BUILD_DIR="$build_dir" \
  TMPDIR="$build_temp_dir/" \
    "$pio_executable" run --project-dir "$project_root" -e "$environment" "$@"
done
