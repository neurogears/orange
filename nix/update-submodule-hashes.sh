#!/usr/bin/env bash
# Prints the fetchFromGitHub hashes for the pinned submodule revs used by
# flake.nix. Run after bumping a submodule pin, then paste the printed
# hash values into flake.nix. The revs below must match flake.nix exactly.
#
# Usage: bash nix/update-submodule-hashes.sh
set -euo pipefail

if ! command -v nix >/dev/null 2>&1; then
    echo "error: nix is required" >&2
    exit 1
fi

fetch_hash() {
    local name=$1 owner=$2 repo=$3 rev=$4
    local hash
    hash=$(nix flake prefetch "github:$owner/$repo/$rev" --json 2>/dev/null | sed 's/.*"hash":"\([^"]*\)".*/\1/')
    printf '%s:\n  hash  = "%s";\n\n' "$name" "$hash"
}

fetch_hash imgui           ocornut   imgui              3fec562da1d5af5f5c35e236e3e843ef0eadbcf3
fetch_hash implot          epezent   implot             f156599faefe316f7dd20fe6c783bf87c8bb6fd9
fetch_hash imguiFileDialog aiekick   ImGuiFileDialog    517a4a8b9c4a4f662f811e1f9f0e976fac546def
fetch_hash iconFontHeaders juliettef IconFontCppHeaders 7d6ff1f4ba51e7a2b142be39457768abece1549c
