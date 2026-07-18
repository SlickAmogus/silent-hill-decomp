#ifndef PC_DECALS_H
#define PC_DECALS_H

/* Bullet-hole decals (PC-only, config key `bullet_decals`).
 *
 * When a PLAYER firearm trace (Ray_CharaTraceQuery in func_8008A3E0) ends on
 * world geometry — trace hasHit with trace->character == NULL — the combat
 * hook passes the clipped Q19.12 hit position (trace->target) plus the bullet
 * direction here. Ray_TraceRun already clips the segment against IPD
 * collision, so no re-cast is needed: the impact point IS the wall/floor hit.
 *
 * Decals persist until map change (cleared alongside
 * HiresOverride_PoolSlotsReset in Ipd_TexturesInit), capped at 64 FIFO.
 * The texture is gamedata/decal.png (32x32 RGBA; any size works — it is
 * mapped 0..1 over the quad), served through hires-override virtual pool
 * slot 255, which Ipd_TexturesInit keeps out of the chunk-texture pool.
 * A missing PNG logs once and disables the feature for the session.
 *
 * Requires the engine types (VECTOR3/GsOT): include after "game.h".
 */

/* Record a bullet impact on world geometry. `pos` = clipped world-space hit
 * point (Q19.12); `dir` = bullet direction vector (Q12, unnormalized);
 * `origin` = the trace's start point (the hand), used to sample the wall
 * plane with two offset rays so wall decals lie flat on the surface. */
void Pc_DecalAddBulletImpact(const VECTOR3* pos, const VECTOR3* dir, const VECTOR3* origin);

/* Project + emit all live decals into the world ordering table. Call once
 * per frame during world rendering (after Ipd_ChunkCheckDraw, same OT). */
void Pc_DecalsDraw(GsOT* ot);

/* Clear the decal FIFO and mark the pool-slot texture for re-registration.
 * Call where HiresOverride_PoolSlotsReset runs (map init frees the slot). */
void Pc_DecalsReset(void);

#endif /* PC_DECALS_H */
