#!/bin/sh
# Builds sbs-overlay-probe, on the Steam Frame or the frame-probe GitHub workflow's
# arm64 runner. Needs g++ and the pinned openvr.h, which is reused from a
# frame-sbs-toggle checkout when present and downloaded (checksum-verified)
# otherwise. Set OPENVR_INCLUDE_DIR to use another copy.
set -eu
cd "$(dirname "$0")"

revision=0924064316de3effbcd1acf1e309182a2deb1c05
expected=1e6ed57199896cc1f7c5484e50fa18955e97be15be690beb28d998c877ead7fd

if ! command -v g++ >/dev/null 2>&1; then
    echo "g++ not found" >&2
    exit 1
fi

include=${OPENVR_INCLUDE_DIR:-"$PWD/include"}
if [ -z "${OPENVR_INCLUDE_DIR:-}" ]; then
    mkdir -p include
    header=include/openvr.h
    toggle="$HOME/.local/share/frame-sbs-toggle/build/include/openvr.h"
    if [ ! -f "$header" ] && [ -f "$toggle" ]; then
        cp "$toggle" "$header"
    fi
    if [ ! -f "$header" ] || [ "$(sha256sum "$header" | cut -d' ' -f1)" != "$expected" ]; then
        url="https://raw.githubusercontent.com/ValveSoftware/openvr/$revision/headers/openvr.h"
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL -o "$header.tmp" "$url"
        else
            wget -q -O "$header.tmp" "$url"
        fi
        if [ "$(sha256sum "$header.tmp" | cut -d' ' -f1)" != "$expected" ]; then
            rm -f "$header.tmp"
            echo "openvr.h checksum mismatch" >&2
            exit 1
        fi
        mv "$header.tmp" "$header"
    fi
fi

# Static C++ runtime so a CI-built binary doesn't depend on the Frame's libstdc++.
g++ -std=c++17 -O2 -Wall -Wextra -I"$include" sbs_overlay_probe.cpp -static-libstdc++ -static-libgcc -pthread -ldl \
    -o sbs-overlay-probe
echo "Built $PWD/sbs-overlay-probe"
