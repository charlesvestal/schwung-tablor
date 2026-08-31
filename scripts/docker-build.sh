#!/usr/bin/env bash
# Runs INSIDE the ubuntu:22.04 build container on the VPS. Do not run on a host
# with newer glibc — the artifacts would not load on the Move.
set -euo pipefail
TARGET="${1:-all}"

python3 tools/gen_params.py
python3 tools/check_config.py
# Help lines that run off the 128px screen, or use a character the device font
# has no glyph for, fail silently on the device -- no wrap, no marker, no log.
python3 tools/check_help.py

# ---- Native DSP tests: compile and RUN in-container before cross-compiling.
# A red test here fails the whole build.
echo "=== native DSP tests ==="
mkdir -p build-native
g++ -O2 -std=c++17 -Wall \
    tests/test_dsp.cpp src/ported/wavetable.cpp \
    src/ported/audiofilter/ParametricCreator.cpp \
    -Isrc -Isrc/ported -o build-native/tablor_test -lm
./build-native/tablor_test

cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build --target "$TARGET" -j"$(nproc)"

# ---- Package for the Module Store ----
rm -rf dist/tablor
mkdir -p dist/tablor/wavetables dist/tablor/presets
cp build/dsp.so           dist/tablor/
cp src/module.json        dist/tablor/
cp src/movy_config.json   dist/tablor/
cp src/ui_pages.json      dist/tablor/
cp src/help.json          dist/tablor/
cp src/web_ui.html        dist/tablor/
cp src/presets/factory.tbl dist/tablor/presets/
cp -r src/wavetables/. dist/tablor/wavetables/ 2>/dev/null || true
(cd dist && tar -czf tablor-module.tar.gz tablor/)
echo "Tarball: dist/tablor-module.tar.gz"

echo; echo "=== Build output ==="
find build -maxdepth 1 -type f \( -name "*.so" -o -name "tablor_*" \) \
    -exec sh -c 'printf "%s\n  " "$1"; file -b "$1"' _ {} \;
