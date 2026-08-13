#!/usr/bin/env bash
# Build and install jack-play from the jack-tools source on Debian trixie /
# Raspberry Pi OS, where the jack-tools package is no longer available.
#
# Builds only jack-play (not the whole jack-tools suite), so it only needs
# the JACK, libsndfile and libsamplerate dev packages.
set -euo pipefail

ORIG_URL="https://deb.debian.org/debian/pool/main/j/jack-tools/jack-tools_20131226.orig.tar.bz2"

echo "==> Installing build dependencies"
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    curl \
    libjack-jackd2-dev \
    libsndfile1-dev \
    libsamplerate0-dev

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> Downloading jack-tools source"
curl -sSL -o "$TMP/jack-tools.tar.bz2" "$ORIG_URL"
tar -xjf "$TMP/jack-tools.tar.bz2" -C "$TMP"
cd "$TMP/rju"

echo "==> Building jack-play"
# jack-play only needs a subset of the c-common helper library.
# Its include graph (file, memory, jack-client, jack-ringbuffer, jack-port,
# jack-transport, observe-signal, sound-file) requires only jack/jack.h,
# sndfile.h and samplerate.h.
(cd c-common && \
 gcc -Wall -O2 -c file.c memory.c jack-client.c jack-ringbuffer.c \
     jack-port.c jack-transport.c observe-signal.c sound-file.c && \
 ar -rcs lib-c-common.a file.o memory.o jack-client.o jack-ringbuffer.o \
     jack-port.o jack-transport.o observe-signal.o sound-file.o)
make jack-play

echo "==> Installing jack-play to /usr/local/bin"
sudo install -m 0755 jack-play /usr/local/bin/

echo "==> Done:"
jack-play -h
