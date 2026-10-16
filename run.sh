#!/usr/bin/env bash
# Run the development build (release/orange, from ./build.sh) through the same
# launcher as an installed orange: sudo, and a per-run log in
# ~/orange_data/logs. Arguments are passed to orange (see orange --help).
set -euo pipefail
REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ORANGE_APPDIR="$REPO/release" exec "$REPO/packaging/orange-launcher.sh" "$@"
