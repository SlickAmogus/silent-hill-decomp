/*
 * gte_diag.c - log the GTE projection state to diagnose the in-game projection.
 * Kept in its own TU so PsyCross's <psx/gtereg.h> (raw cop2 register access)
 * doesn't clash with the psyq <libgte.h> used by the rest of the port.
 */
#include <psx/gtereg.h>
#include "sh_log.h"

/* C2_OFX/OFY (16.16) + C2_H = projection setup; SZ3 = last vertex depth, IR1..3 =
 * last rotated vertex, SX2/SY2 = last projected screen coord. If near geometry
 * vanishes off-screen, SZ3 will be tiny and SX2/SY2 huge for those verts. */
void Gte_LogGeom(const char* tag)
{
    SH_DBG("[GTE] %s ofx=%d ofy=%d h=%d | sz3=%d ir=(%d,%d,%d) sxy2=(%d,%d)", tag,
           (int)(C2_OFX >> 16), (int)(C2_OFY >> 16), (int)C2_H,
           (int)C2_SZ3, (int)C2_IR1, (int)C2_IR2, (int)C2_IR3,
           (int)C2_SX2, (int)C2_SY2);
    /* ir = Rotation*vertex + Translation. Huge ir with sane depth means either the
     * view translation (tr) or the rotation matrix (4.12 fixed, unit ~4096) is off. */
    SH_DBG("[GTEM] tr=(%d,%d,%d) R=[%d %d %d / %d %d %d / %d %d %d]",
           (int)C2_TRX, (int)C2_TRY, (int)C2_TRZ,
           (int)C2_R11, (int)C2_R12, (int)C2_R13,
           (int)C2_R21, (int)C2_R22, (int)C2_R23,
           (int)C2_R31, (int)C2_R32, (int)C2_R33);
}
