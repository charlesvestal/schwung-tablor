#!/usr/bin/env bash
#
# Install the Tablor module on the Move SAFELY.
#
# Critical: never scp directly over a live dsp.so. The shim dlopen()s it into
# MoveOriginal, so overwriting the file mutates the mmap'd code pages of a
# running process — which segfaults the whole firmware. Upload to a temp name,
# then mv: rename(2) is atomic and leaves the old inode intact for the running
# process. New code is picked up when the slot next loads the module.
#
#   ./scripts/deploy.sh [host]      (default: move.local)
set -euo pipefail

HOST="${1:-move.local}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
DEST="/data/UserData/schwung/modules/sound_generators/tablor"

[ -f "$SRC/build/dsp.so" ] || { echo "no build/dsp.so — run ./scripts/build.sh first" >&2; exit 1; }

echo "==> $HOST:$DEST"
ssh "$HOST" "mkdir -p $DEST/wavetables"

ssh "$HOST" "mkdir -p $DEST/presets"
scp -q "$SRC/build/dsp.so"          "$HOST:$DEST/dsp.so.new"
scp -q "$SRC/src/module.json"       "$HOST:$DEST/module.json.new"
scp -q "$SRC/src/ui_pages.json"     "$HOST:$DEST/ui_pages.json.new"
scp -q "$SRC/src/help.json"         "$HOST:$DEST/help.json"
scp -q "$SRC/src/web_ui.html"       "$HOST:$DEST/web_ui.html"
scp -q "$SRC/src/presets/factory.tbl" "$HOST:$DEST/presets/factory.tbl"

# Atomic swap. Do NOT replace this with a direct scp.
# (also removes the retired custom editor — Schwung 0.12+'s stock
# hierarchy UI is the interface now)
ssh "$HOST" "cd $DEST && \
    mv -f dsp.so.new dsp.so && \
    mv -f module.json.new module.json && \
    mv -f ui_pages.json.new ui_pages.json && \
    rm -f ui_chain.js && \
    chmod 755 dsp.so && ls -l dsp.so module.json"

# Loader test binary (run it on the device: cd $DEST && ./tablor_loadtest ./dsp.so)
if [ -f "$SRC/build/tablor_loadtest" ]; then
    scp -q "$SRC/build/tablor_loadtest" "$HOST:$DEST/tablor_loadtest.new"
    ssh "$HOST" "cd $DEST && mv -f tablor_loadtest.new tablor_loadtest && chmod 755 tablor_loadtest"
fi

# Factory wavetables, if present locally
if [ -d "$SRC/src/wavetables" ] && [ -n "$(ls -A "$SRC/src/wavetables" 2>/dev/null)" ]; then
    rsync -az -e ssh "$SRC/src/wavetables/" "$HOST:$DEST/wavetables/"
fi

echo "==> done. Reload the slot (or restart the Shadow UI) to pick up new code."
