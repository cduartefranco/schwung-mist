#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-mist-builder"

# When run on the host (no cross toolchain), build inside Docker then exit.
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Building MIST module (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm -v "$REPO_ROOT:/build" -u "$(id -u):$(id -g)" -w /build \
        "$IMAGE_NAME" ./scripts/build.sh
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"
mkdir -p build dist/mist

${CROSS_PREFIX}gcc -O3 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG -Idsp \
    dsp/mist.c \
    -o build/mist.so -lm

cp module.json  dist/mist/module.json
cp help.json    dist/mist/help.json
cp build/mist.so dist/mist/mist.so
chmod +x dist/mist/mist.so
[ -f LICENSE ]   && cp LICENSE   dist/mist/LICENSE   || true
[ -f README.md ] && cp README.md dist/mist/README.md || true
[ -d ui ]        && cp -R ui     dist/mist/ui        || true

cd dist
tar -czvf mist-module.tar.gz mist/
echo "OK: dist/mist-module.tar.gz"
