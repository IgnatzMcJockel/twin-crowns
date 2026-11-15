#!/bin/sh
# Checks that third_party/chowdsp_wdf is checked out at the commit MoT pins for its own
# libs/chowdsp_wdf, so we build MoT's DSP against exactly the library MoT uses.
# Skips outside a git checkout, e.g. in the sources `make deploy` copies to the Pi.
set -e

if ! git -C third_party/MoT rev-parse HEAD >/dev/null 2>&1 \
    || ! git -C third_party/chowdsp_wdf rev-parse HEAD >/dev/null 2>&1; then
    echo "pins: no git checkout, skipped"
    exit 0
fi

mot_pin=$(git -C third_party/MoT ls-tree HEAD libs/chowdsp_wdf | awk '{print $3}')
ours=$(git -C third_party/chowdsp_wdf rev-parse HEAD)

if [ "$mot_pin" != "$ours" ]; then
    echo "pins: third_party/chowdsp_wdf is at $ours, but MoT pins $mot_pin" >&2
    exit 1
fi

echo "pins: chowdsp_wdf matches MoT's pin ($ours)"
