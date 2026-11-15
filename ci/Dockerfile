# Build environment for CI: the same Debian release, compiler and glibc as the pi-Stomp, so CI
# builds what the Pi would build itself, even when the CI host moves to a newer release.
FROM debian:trixie

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential lv2-dev \
    && rm -rf /var/lib/apt/lists/*
