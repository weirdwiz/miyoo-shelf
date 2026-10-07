#!/bin/sh
# Rebuilds assets/fonts/Shelf-*.ttf: M PLUS Rounded 1c cut down to Latin (about 105 KB each
# instead of 3.5 MB), which keeps pushes over the Miyoo's slow Wi-Fi quick.
# Needs fonttools (pip install fonttools).
set -eu
cd "$(dirname "$0")/../assets/fonts"
for w in Bold ExtraBold; do
    curl -sfL --max-time 120 -o "/tmp/MPLUSRounded1c-$w.ttf" \
        "https://raw.githubusercontent.com/google/fonts/main/ofl/mplusrounded1c/MPLUSRounded1c-$w.ttf"
    pyftsubset "/tmp/MPLUSRounded1c-$w.ttf" \
        --unicodes="U+0020-007E,U+00A0-024F,U+2010-2027,U+2030-205E,U+20AC,U+2122,U+2190-2193,U+2605,U+2606,U+00B7" \
        --layout-features='kern,liga' --output-file="Shelf-$w.ttf"
done
