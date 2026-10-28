/* Forwarder: the XDK build must not put xbox_port/include on its path (its
 * nxdk-era xtl.h and memory.h shims would shadow the real XDK headers), so
 * the shim headers it does need are reached one at a time through here. */
#include "../../../xbox_port/include/gpu_nv2a.h"
