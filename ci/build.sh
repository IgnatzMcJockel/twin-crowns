#!/bin/sh
# Builds the bundle and runs the tests in the container from ci/Dockerfile. Run from the repo
# root on an arm64 host with Docker; the Makefile then adds -mcpu=cortex-a76 as on the pi-Stomp.
# The pin check runs here, on the host: inside the container git wouldn't trust the mounted
# checkout and the check would skip itself.
set -e

sh tests/check_pins.sh

# Docker directly if the runner's user may, otherwise through sudo, which must not ask for a
# password (-n): a prompt would hang the job.
if docker info >/dev/null 2>&1; then
    docker=docker
else
    docker="sudo -n docker"
fi

$docker build -q -t twin-crowns-build ci >/dev/null

# The container runs as root; hand build/ back to the runner's user either way.
$docker run --rm -v "$PWD:/src" -w /src twin-crowns-build sh -c "
    status=0
    make -j4 && make test || status=\$?
    chown -R $(id -u):$(id -g) build 2>/dev/null || true
    exit \$status
"
