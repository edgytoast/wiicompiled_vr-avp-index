#!/usr/bin/env bash
# Builds WiiCompiled Vision, the Apple Vision Pro app, from an existing translation.
#
#   visionos/Build-VisionOS.sh [--product base|retro_rewind] [--team TEAMID] [--simulator]
#                              [--build-dir DIR] [--dawn-package FILE] [--jobs N]
#                              [--install] [--open]
#
# Steps: Dawn for the visionOS SDK (Build-VisionOSDawn.sh, cached), the Xcode
# project (visionos/CMakeLists.txt, which pulls the runtime in), the build, and
# with --install the app onto the paired headset (xcrun devicectl). --open
# opens the generated project in Xcode instead of building, for signing setup or
# debugging.
#
# The translation must exist first, exactly as for a desktop build:
# docs/building-macos.md steps 1 to 5 leave generated/build_shards/shards.cmake
# behind. `generate-data-init --target-os macos` is the right flavour, but the
# runtime's CMake rewrites the Windows/Linux blob assembly for Mach-O too.
#
# Signing: a free Apple ID's personal team can sign for a headset paired with
# this Mac (Xcode > Settings > Accounts). Pass its id with --team, or leave it
# out and pick the team once in the generated project; CMake remembers the value.
set -euo pipefail

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
product="base"
team="${MKW_VISIONOS_TEAM:-}"
simulator=0
build_dir="${repo_root}/build-visionos"
dawn_package=""
jobs="$(sysctl -n hw.ncpu)"
install=0
open_project=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --product) product="${2:?}"; shift 2 ;;
        --team) team="${2:?}"; shift 2 ;;
        --simulator) simulator=1; shift ;;
        --build-dir) build_dir="${2:?}"; shift 2 ;;
        --dawn-package) dawn_package="${2:?}"; shift 2 ;;
        --jobs) jobs="${2:?}"; shift 2 ;;
        --install) install=1; shift ;;
        --open) open_project=1; shift ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
case "${product}" in base|retro_rewind) ;; *) echo "--product must be base or retro_rewind" >&2; exit 2 ;; esac

if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
sysroot="xros"
if [[ ${simulator} -eq 1 ]]; then sysroot="xrsimulator"; fi

shards="${repo_root}/generated/build_shards/shards.cmake"
if [[ ! -f "${shards}" ]]; then
    echo "ERROR: no translation at ${shards}; run the translator first (docs/building-macos.md, steps 1 to 5)" >&2
    exit 1
fi

if [[ -z "${dawn_package}" ]]; then
    dawn_args=()
    if [[ ${simulator} -eq 1 ]]; then dawn_args+=(--simulator); fi
    dawn_package="$("${repo_root}/visionos/Build-VisionOSDawn.sh" "${dawn_args[@]}" --jobs "${jobs}" | tail -n 1)"
fi
if [[ ! -f "${dawn_package}" ]]; then
    echo "ERROR: Dawn package not found: ${dawn_package}" >&2
    exit 1
fi
case "${dawn_package}" in /*) ;; *) dawn_package="${PWD}/${dawn_package}" ;; esac

cmake_args=(
    -S "${repo_root}/visionos" -B "${build_dir}" -G Xcode
    -DCMAKE_SYSTEM_NAME=visionOS
    "-DCMAKE_OSX_SYSROOT=${sysroot}"
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0
    "-DAURORA_DAWN_PACKAGE_URL=file://${dawn_package}"
    "-DMKW_VISIONOS_PRODUCT=${product}"
    -DAURORA_SDL3_PROVIDER=vendor
)
if [[ -n "${team}" ]]; then cmake_args+=("-DMKW_VISIONOS_TEAM=${team}"); fi
cmake "${cmake_args[@]}"

if [[ ${open_project} -eq 1 ]]; then
    open "${build_dir}/WiiCompiledVision.xcodeproj"
    exit 0
fi

# -allowProvisioningUpdates lets automatic signing register the headset and refresh the profile.
cmake --build "${build_dir}" --config Release --target WiiCompiledVision --parallel "${jobs}" -- \
    -allowProvisioningUpdates
app="${build_dir}/Release-${sysroot}/WiiCompiledVision.app"
if [[ ! -d "${app}" ]]; then
    app="$(find "${build_dir}" -maxdepth 2 -name WiiCompiledVision.app -type d | head -n 1)"
fi
echo "App: ${app}"

if [[ ${install} -eq 1 ]]; then
    if [[ ${simulator} -eq 1 ]]; then
        xcrun simctl install booted "${app}"
        echo "Installed on the booted visionOS simulator"
    else
        # The first paired Apple Vision Pro; pair in Xcode > Devices and Simulators.
        device="$(xcrun devicectl list devices --hide-headers 2>/dev/null | awk 'tolower($0) ~ /vision/ {print $3; exit}')"
        if [[ -z "${device}" ]]; then
            echo "ERROR: no paired Apple Vision Pro; pair it in Xcode first" >&2
            exit 1
        fi
        xcrun devicectl device install app --device "${device}" "${app}"
        echo "Installed on ${device}"
    fi
fi
