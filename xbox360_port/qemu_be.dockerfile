# Big-endian PowerPC test harness.
#
# The point: run endian-sensitive code on the DESKTOP instead of on a console.
# A hardware test costs a BadUpdate run (~30% success, up to 20 minutes,
# non-persistent), which is a terrible way to bisect a fixed-point maths bug.
# The software GTE, the asset loaders and the byte-swap passes are all pure C
# with no platform dependency, so they can run under qemu-ppc and be diffed
# against the known-good little-endian result.
#
# powerpc-linux-gnu is a different ABI from xenon bare-metal, but the same
# ENDIANNESS and the same 32-bit word size, which is what is under test.
#
#   docker build -t sh360-qemu -f xbox360_port/qemu_be.dockerfile .
FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-powerpc-linux-gnu g++-powerpc-linux-gnu \
        qemu-user \
        gcc g++ make file \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["/bin/bash"]
