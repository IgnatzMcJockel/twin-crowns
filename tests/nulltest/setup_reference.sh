#!/bin/bash
# Builds the reference for the null test: MoT at our pin with JUCE, in ~/mot-ref, outside this repo.
# Needs cmake and JUCE's Linux dependencies, see .github/workflows/ci.yml in MoT.
#
# reference.patch changes that clone, never the submodule:
# - PedalRender writes 32-bit float instead of 24-bit. It also takes extra parameters, parameter
#   changes during the render and a block size from the environment, which is what lets it render
#   Red at all. Run it through nulltest.py, which sets all of that.
# - JUCE_DSP_ENABLE_SNAP_TO_ZERO=0, because JUCE only snaps on Intel: this is what MoT does on the Pi.
#
# Each repo comes from the copy next to this repo's origin if there is one (mirrors or forks
# under the same owner), and from upstream otherwise. Without credentials for the
# copy, git fails fast instead of prompting, and upstream is used.
set -e
pin=$(git rev-parse HEAD:third_party/MoT)
repo=$(pwd)
clone="$HOME/mot-ref"
base=$(git remote get-url origin | sed 's|/[^/]*$||')

source_of() {  # $1 = repo name, $2 = upstream URL
    if GIT_TERMINAL_PROMPT=0 git ls-remote -q "$base/$1" HEAD >/dev/null 2>&1; then
        echo "$base/$1"
    else
        echo "$2"
    fi
}

mot=$(source_of MoT https://github.com/tehguitarist/Monarch-of-Tone)
[ -d "$clone/.git" ] || git clone -q --filter=blob:none "$mot" "$clone"
cd "$clone"
git checkout -q tools/PedalRender.cpp CMakeLists.txt 2>/dev/null || true
git checkout -q "$pin"

# MoT's .gitmodules names the upstream URLs; point them at the copies where those exist.
rewrites=""
for name_url in JUCE=https://github.com/juce-framework/JUCE \
                chowdsp_wdf=https://github.com/Chowdhury-DSP/chowdsp_wdf \
                xsimd=https://github.com/xtensor-stack/xsimd; do
    name=${name_url%%=*}
    upstream=${name_url#*=}
    source=$(source_of "$name" "$upstream")
    [ "$source" = "$upstream" ] || rewrites="$rewrites -c url.$source.insteadOf=$upstream"
done
echo "sources: MoT $mot${rewrites:+, rewrites$rewrites}"
git $rewrites submodule update --init --depth 1 libs/JUCE libs/chowdsp_wdf libs/xsimd

git apply "$repo/tests/nulltest/reference.patch"

cmake -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build --target PedalRender -j8 | tail -1
echo "MoT $(git rev-parse HEAD), JUCE $(git -C libs/JUCE rev-parse HEAD)"
