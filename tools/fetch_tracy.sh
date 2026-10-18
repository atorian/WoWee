#!/usr/bin/env bash
# Fetch the Tracy profiler's client into extern/tracy, where a build with
# -DWOWEE_ENABLE_TRACY=ON looks for it. Tracy is not vendored: a build without
# the option needs none of it, and pays nothing for the zones it marks.
#
#   tools/fetch_tracy.sh [tag]        (default v0.11.1)
#
# The profiler GUI (or tracy-capture) that connects to the client has to be
# the same release: Tracy's protocol changes between versions. Prebuilt
# Windows ones are on https://github.com/wolfpld/tracy/releases.
#
# Then a build of its own, so the ordinary build-dev keeps its cache:
#
#   cmake --preset dev -B build-tracy -DWOWEE_ENABLE_TRACY=ON
#   ninja -C build-tracy wowee
set -euo pipefail

tag="${1:-v0.11.1}"
root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$root/extern/tracy"

if [[ -d "$dest/.git" ]]; then
    git -C "$dest" fetch --depth 1 origin "refs/tags/$tag:refs/tags/$tag"
    git -C "$dest" -c advice.detachedHead=false checkout "$tag"
else
    git -c advice.detachedHead=false clone --depth 1 --branch "$tag" \
        https://github.com/wolfpld/tracy "$dest"
fi

if [[ ! -f "$dest/public/TracyClient.cpp" ]]; then
    echo "fetch_tracy: $dest/public/TracyClient.cpp is missing after the checkout" >&2
    exit 1
fi
echo "Tracy $tag in $dest"
