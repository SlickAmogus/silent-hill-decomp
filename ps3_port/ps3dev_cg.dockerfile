# ps3toolchain + PSL1GHT + the Cg toolkit cgcomp actually needs.
#
#   docker build -f ps3_port/ps3dev_cg.dockerfile -t sh-ps3dev:cg ps3_port
#
# The base image ships `cgcomp`, which is only a FRONT END: it dlopen's NVIDIA's
# libCg.so at run time and, without it, every invocation dies with "Unable to
# load Cg, aborting" before reading a single line of shader. So the base image
# alone cannot build a shader, and the RSX backend cannot draw anything without
# one.
#
# Cg 3.1 (April 2012) is the last release and is still served by NVIDIA's
# download host, unauthenticated. It unpacks to /usr/lib64, which Debian's
# loader does not search -- hence the explicit LD_LIBRARY_PATH rather than a
# bare ldconfig, which silently does not help.
FROM scrapes/ps3toolchain-minimal:latest

RUN cd /tmp \
 && wget -q --timeout=120 -O cg.tgz \
      https://developer.download.nvidia.com/cg/Cg_3.1/Cg-3.1_April2012_x86_64.tgz \
 && tar xzf cg.tgz -C / \
 && rm -f cg.tgz \
 && test -f /usr/lib64/libCg.so

ENV LD_LIBRARY_PATH=/usr/lib64

WORKDIR /work
