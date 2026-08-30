#!/usr/bin/env bash
# Run a command inside the reproducible build container.
#
#   ./tools/docker/run.sh                 build the KNOMI2 firmware
#   ./tools/docker/run.sh firmware        same, explicitly
#   ./tools/docker/run.sh assets          regenerate the GIFs and their .c files
#   ./tools/docker/run.sh shell           interactive shell
#   ./tools/docker/run.sh <cmd> [args..]  anything else
#
# The repo is bind-mounted and the container runs as you, so generated files
# land in the working tree owned by you, not root.
set -euo pipefail

REPO="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
IMAGE="${KNOMI_BUILD_IMAGE:-knomi-build}"

if [ -z "$(docker images -q "$IMAGE" 2>/dev/null)" ]; then
    echo "building $IMAGE (first run pulls the ESP32 toolchain, ~1GB)..." >&2
    docker build -t "$IMAGE" "$REPO/tools/docker"
fi

case "${1:-firmware}" in
    firmware) CMD=(pio run -e knomiv2) ;;
    assets)   CMD=(bash -c 'cd tools/stealthburner && python3 build_gifs.py && python3 gen_c.py') ;;
    shell)    CMD=(bash) ;;
    *)        CMD=("$@") ;;
esac

# only allocate a TTY when we actually have one, so this works in CI/pipes too
TTY=()
[ -t 0 ] && [ -t 1 ] && TTY=(-i -t)

exec docker run --rm "${TTY[@]}" \
    -u "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -v "$REPO":/repo \
    -w /repo \
    "$IMAGE" "${CMD[@]}"
