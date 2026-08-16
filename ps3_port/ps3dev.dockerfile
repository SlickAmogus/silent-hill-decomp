# PS3 homebrew toolchain (ps3toolchain + PSL1GHT v2) — FALLBACK ONLY.
#
# The port builds with the pinned `scrapes/ps3toolchain-minimal` image; see
# ps3_port/README.md. This file exists so the toolchain can be rebuilt from
# source if that image ever disappears, because every OTHER published image is
# already a dead end: psl1ght/psl1ght is 11 years old and ships a v1 manifest
# that containerd >= 2.1 refuses outright; zeldin/ps3dev-docker has no
# linux/amd64 entry in its manifest list; wargio/ps3sdk and ps3dev/ps3dev do not
# exist as published repositories at all. ps3dev/ps3toolchain's own Dockerfile
# installs the build dependencies but leaves the toolchain.sh invocation
# commented out, so it produces a dev shell and not a toolchain. This finishes
# that job. Expect a long build; it compiles binutils, two gccs and newlib.
#
#   docker build -f ps3_port/ps3dev.dockerfile -t sh-ps3dev:latest ps3_port
#   docker run --rm -v C:\Claude\silenthill-ps3\silent-hill-decomp:/work \
#              -w /work sh-ps3dev:latest /bin/bash -lc 'bash ps3_port/ppu_gate.sh'
FROM debian:12-slim

ENV PS3DEV=/usr/local/ps3dev
ENV PSL1GHT=$PS3DEV
ENV PATH=$PATH:$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin

# Dependency set is ps3dev/ps3toolchain's Dockerfile plus the GMP/MPFR/MPC trio
# and python3: the stock list omits them, and gcc's configure then silently
# falls back to building its own in-tree copies, which roughly doubles the build.
RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf automake bison build-essential ca-certificates flex git \
        libelf-dev libgmp-dev libmpfr-dev libmpc-dev libncurses5-dev \
        libssl-dev libtool-bin make patch pkg-config python3 texinfo \
        wget xz-utils zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
RUN git clone --depth 1 https://github.com/ps3dev/ps3toolchain.git .
RUN ./toolchain.sh

WORKDIR /work
