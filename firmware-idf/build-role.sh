#!/usr/bin/env bash
# Build (or flash, or monitor) one role for one target.
#
# The role is a Kconfig choice, so without this wrapper "three images from one
# tree" means an interactive `idf.py menuconfig` between builds, and every image
# fighting over the same ./sdkconfig and ./build. Each (target, role) pair gets
# its own build directory and its own generated sdkconfig here, so a leaf image
# and a gateway image can both exist on disk and neither can silently inherit
# the other's config.
#
# Usage:
#   ./build-role.sh <leaf|primary|gateway> [esp32|esp32c3|esp32s3] [idf.py args...]
#
#   ./build-role.sh leaf                        # build, default target esp32
#   ./build-role.sh gateway esp32c3             # build for the C3
#   ./build-role.sh primary esp32s3             # build for the S3 (Ethernet board)
#   ./build-role.sh leaf esp32 -p /dev/ttyUSB0 flash monitor
#   ./build-role.sh primary esp32 menuconfig    # tweak this role's config only
#
# Plain `idf.py build` in this directory still works and gives the leaf image
# (the Kconfig default) in ./build -- that is the quick path; this script is the
# reproducible one.
set -euo pipefail

usage() {
  echo "usage: $0 <leaf|primary|gateway|espnow-probe|espnow-probe-receiver> [esp32|esp32c3|esp32s3] [idf.py args...]" >&2
}

# espnow-probe is not a node role -- it is the TEC-NATKIT-23 bench instrument,
# which builds instead of a role (see roles/espnow-probe.defaults). It lives here
# so it gets the same isolated build dir and sdkconfig as everything else.
#
# espnow-probe-receiver is the same instrument with the receiver switch set, and it
# is a separate target for a reason: the switch cannot be flipped by hand after a
# build, because ESP-IDF only reads the defaults when the generated sdkconfig does
# not exist yet. Editing it by hand yields a second SENDER, and two senders with no
# receiver look exactly like total packet loss.
role="${1:-}"
case "${role}" in
  leaf | primary | gateway | espnow-probe | espnow-probe-receiver) ;;
  *)
    usage
    exit 2
    ;;
esac
shift

target="esp32"
if [[ $# -gt 0 ]]; then
  case "${1}" in
    esp32 | esp32c3 | esp32s3)
      target="${1}"
      shift
      ;;
    -* | build | flash | monitor | menuconfig | fullclean | clean | size | size-components | app | erase-flash)
      # No target given -- keep the default and treat this as an idf.py arg.
      ;;
    *)
      echo "$0: unknown target '${1}' (expected esp32, esp32c3 or esp32s3)" >&2
      usage
      exit 2
      ;;
  esac
fi

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "$0: IDF_PATH is not set -- run 'source ~/esp/esp-idf/export.sh' first" >&2
  exit 1
fi

cd "$(dirname "$0")"

build_dir="build/${target}-${role}"

# An explicit SDKCONFIG_DEFAULTS replaces the automatic list, so
# sdkconfig.defaults.<target> has to be named here or the per-target settings
# (flash size, PSRAM) would be silently dropped.
defaults="sdkconfig.defaults;sdkconfig.defaults.${target};roles/${role}.defaults"

# ESP-IDF reads sdkconfig.defaults* ONLY when the generated sdkconfig does not
# exist yet. After that, editing a defaults file silently does nothing: the
# build succeeds and the setting is simply absent from the image. Measured the
# hard way -- a flash-driver option was "applied" for two builds before anyone
# looked at the generated sdkconfig.
sdkconfig_path="${build_dir}/sdkconfig"
if [[ -f "${sdkconfig_path}" ]]; then
  for f in "sdkconfig.defaults" "sdkconfig.defaults.${target}" "roles/${role}.defaults"; do
    if [[ -f "${f}" && "${f}" -nt "${sdkconfig_path}" ]]; then
      echo "$0: WARNING: ${f} is newer than ${sdkconfig_path} -- ESP-IDF will IGNORE it." >&2
      echo "$0:          run 'rm ${sdkconfig_path}' and rebuild to pick the change up." >&2
    fi
  done
fi

set -x
exec idf.py \
  -B "${build_dir}" \
  -D SDKCONFIG="${build_dir}/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="${defaults}" \
  -D IDF_TARGET="${target}" \
  "${@:-build}"
