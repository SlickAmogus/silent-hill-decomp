using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// ILM &lt;-&gt; OBJ model conversion — the in-process C# twin of pc_port/tools/ilm_obj.py.
    ///
    /// An SH1 .ILM holds several "models" that are really rigid BODY PARTS (CAT.ILM:
    /// 01BODY_T, 02FRLEG1 ... 18TAIL2). Animation transforms each part as a unit, so a
    /// part is exported as one OBJ object (`o NAME`) and import matches parts BY NAME.
    /// The first two ASCII characters of that name, parsed as decimal, ARE the bone
    /// index (bodyprog_bone_80044F14.c:185-197), so renaming, adding or deleting an
    /// object breaks animation silently — import refuses to do it.
    ///
    ///   Export : ILM                  -> OBJ + MTL + .ilmmeta.json
    ///   Import : edited OBJ + the ORIGINAL ILM -> patched ILM
    ///
    /// Import always needs the original ILM as a template: several fields have no known
    /// meaning (MeshHeader.unkPtr_14/unkCount_3, Material.field_14/16, the ModelHeader
    /// bitfield at 0xB) and are copied through verbatim rather than guessed at.
    ///
    /// Geometry facts this relies on, all verified against real files:
    ///   * Primitive vertex/normal indices are ABSOLUTE slots in a shared scratch pool,
    ///     NOT indices into the mesh's own arrays — see ResolvePool.
    ///   * A vertex is (verticesXy[i].vx, verticesXy[i].vy, verticesZ[i]) - XY and Z
    ///     live in two separate arrays.
    ///   * A TRIANGLE sets the 4th vertex index (byte at prim+0xF) to 0xFF and leaves
    ///     the 4th UV garbage. A QUAD is two triangles in PSX FT4 winding — see QuadLoop.
    ///   * UVs are u8 per axis packed as u16, U in the low byte, V in the high byte.
    ///   * Pointers in the file are file-relative offsets.
    ///
    /// ILM vertices are stored in the part's OWN BONE frame, never in model space: the
    /// draw path loads one matrix per bone (Vw_CoordToWorldAndViewMatrices ->
    /// SetRotMatrix/SetTransMatrix in func_8005A900) and feeds the raw ILM values to
    /// gte_rtpt with no pre-multiply. Exporting them verbatim therefore piles every part
    /// on the origin. The rest pose that places them lives in the .ANM, so export BAKES
    /// it in (world = R*v/4096 + T) and freezes the matrices into the meta.
    /// </summary>
    public static class IlmObjConverter
    {
        private const int TriSentinel = 0xFF;

        // Corner c's UV word sits at prim + UvOffsets[c] — the table is NOT uniform.
        private static readonly int[] UvOffsets = { 0, 4, 8, 0xA };

        // Python writes all three text files in text mode, so every '\n' becomes CRLF on
        // Windows; matching that keeps the C# output byte-diffable against the tool's.
        private const string Nl = "\r\n";

        private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

        private const int Q12 = 4096;
        private static readonly int[] IdentityR = { Q12, 0, 0, 0, Q12, 0, 0, 0, Q12 };

        // An edit smaller than this in world units is treated as "the artist did not touch
        // it": Blender's measured OBJ round-trip drift is ~3e-05 and the local quantisation
        // step is 0.5, so nothing real lives in the gap.
        private const double UnmovedEps = 1e-3;

        // Normals need their own, much looser gate. Blender stores custom split normals
        // compressed, so a normal survives its OBJ round-trip only to ~0.66 degrees
        // (measured worst case 1.15e-02 over 5684 normals in 22 CHARA models) — 25x the
        // vertex drift and 1.5x the s8 quantisation step itself. 0.05 is ~2.9 degrees.
        private const double NormalUnmovedEps = 0.05;

        // Replace-mode seam welding radius. Half a local quantisation step: coincident seam
        // vertices come out of the OBJ bit-identical (export bakes a foreign vertex with its
        // OWNER's matrix), so the default only has to absorb a modeller snapping by eye — it is
        // not a "merge nearby geometry" dial.
        //
        // Deliberately tight, because the two failure modes are not symmetric: welding too little
        // leaves a visible gap the modeller can see and fix by snapping the vertices; welding too
        // much silently binds geometry to the WRONG BONE, which only shows up once that limb
        // rotates. Measured over the shipped corpus, 0.5 welded 4 vertices on BFLU of which ALL
        // FOUR crossed a bone, while 0.01 keeps 121 of SRL's 122 real welds and crosses none.
        public const double WeldEpsDefault = 0.01;

        // How far past the weld radius to look purely to report a near miss. Wide enough to catch
        // an eyeballed seam, never used to accept one.
        private const double NearMissFactor = 100.0;

        // A PSX FT4 quad is a triangle STRIP: func_8005AC50 copies the four indices straight
        // into POLY_GT4 x0..x3, which the GPU renders as (v0,v1,v2)+(v1,v2,v3). An OBJ `f` is
        // a polygon LOOP, so emitting 0,1,2,3 makes every quad cross itself — the bowtie that
        // shatters the model in Blender while leaving every edge LENGTH unchanged, so no spike
        // metric can see it. OBJ corner i carries prim corner QuadLoop[i]. Swapping 2 and 3 is
        // an involution, so the same table maps OBJ corners back to prim corners on import.
        private static readonly int[] QuadLoop = { 0, 1, 3, 2 };
        private static readonly int[] TriLoop = { 0, 1, 2 };

        private static int[] CornerOrder(Prim p) { return p.Tri ? TriLoop : QuadLoop; }

        private class IlmException : Exception
        {
            /// <summary>The refusal is a topology mismatch — the OBJ no longer describes the same
            /// vertices and faces the template has — for which a full rebuild is the remedy. The
            /// launcher only offers replacement after one of these, so an unrelated failure (a
            /// renamed part, a missing meta) can never escalate into a silent rebuild.</summary>
            public readonly bool ReplaceMayHelp;
            public IlmException(string m) : base(m) { }
            public IlmException(string m, bool replaceMayHelp) : base(m) { ReplaceMayHelp = replaceMayHelp; }
        }

        // PSX Y grows downward; OBJ/Blender expect Y up, so Y is negated in both
        // directions. Values are small, so the s16 range is never at risk.
        private static double YOut(double v) { return -v; }
        private static double YIn(double v) { return -v; }

        // ILM normals point INWARD: dotted against (face centre - part centroid) in the part's
        // own local space, 93.5% of faces in the first 20 CHARA models come out negative. The
        // Y-negation above preserves that sign (M^T M == I), so emitting them the way positions
        // are emitted lights every model from the inside. Normals therefore get their own
        // direction convention — the extra flip is deliberate, not a copy of the position path.
        private static double[] NormalOut(double[] v) { return new[] { -v[0], v[1], -v[2] }; }
        private static double[] NormalIn(double[] v) { return new[] { -v[0], v[1], -v[2] }; }

        private static ushort U16(byte[] d, int o) { return (ushort)(d[o] | (d[o + 1] << 8)); }
        private static int U32(byte[] d, int o) { return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24); }
        private static short S16(byte[] d, int o) { return (short)(d[o] | (d[o + 1] << 8)); }
        private static sbyte S8(byte[] d, int o) { return (sbyte)d[o]; }

        private static void W16(byte[] d, int o, int v) { d[o] = (byte)v; d[o + 1] = (byte)(v >> 8); }
        private static void W32(byte[] d, int o, int v)
        { d[o] = (byte)v; d[o + 1] = (byte)(v >> 8); d[o + 2] = (byte)(v >> 16); d[o + 3] = (byte)(v >> 24); }
        private static void A16(List<byte> o, int v) { o.Add((byte)v); o.Add((byte)(v >> 8)); }
        private static void A32(List<byte> o, int v)
        { o.Add((byte)v); o.Add((byte)(v >> 8)); o.Add((byte)(v >> 16)); o.Add((byte)(v >> 24)); }

        // ---- results ------------------------------------------------------------

        public class ExportResult
        {
            public string ObjPath, MtlPath, MetaPath;
            public int Parts, Vertices, Prims, Materials;
            public int SeamDuplicates;
            public int Dangling;
            /// <summary>null when no usable .ANM was found — the parts are in local space and
            /// will pile on the origin. The launcher must show this before the user opens Blender.</summary>
            public string AnmName;
            public int Keyframe;
            public readonly List<string> Warnings = new List<string>();
            public string Error;
        }

        public class ImportResult
        {
            public string IlmPath;
            public int Parts, Vertices, Normals, Prims;
            public bool RestPoseIdentity;
            public readonly List<string> Warnings = new List<string>();
            public string Error;
            /// <summary>True when grow-mode actually rewrote the file (extra vertices or faces
            /// were found). A grow import of an unedited OBJ leaves this false and takes the
            /// byte-identical patch-in-place path.</summary>
            public bool Grew;
            /// <summary>Model indices that gained geometry, ascending.</summary>
            public readonly List<int> GrownParts = new List<int>();
            /// <summary>Per-part budget table (slots used vs the u8 ceilings), one line each.
            /// Populated on success AND on the ceiling refusals, so the launcher can show the
            /// user which part blew the budget and by how much.</summary>
            public readonly List<string> Report = new List<string>();

            /// <summary>True when replace-mode rebuilt every part's geometry from the OBJ.</summary>
            public bool Replaced;
            /// <summary>True when the OBJ was written back as a v7 high-poly model.</summary>
            public bool V7;
            /// <summary>Set on a refusal the caller can retry as a full rebuild — the OBJ's
            /// topology no longer matches the template. Never set for a structural failure
            /// (renamed part, missing meta), which a rebuild would refuse the same way.</summary>
            public bool ReplaceMayHelp;
            /// <summary>Set on the one replace refusal the caller can clear by itself: welding
            /// needs a rest pose, and without one the caller may retry with Weld = false.</summary>
            public bool WeldNeedsRestPose;
            /// <summary>Replace-mode: whether auto-weld ran, and at what radius.</summary>
            public bool WeldEnabled;
            public double WeldEps;
            /// <summary>Vertices collapsed onto an earlier-drawn part's pool slot, and the
            /// (reader, owner) pairs they fell into — ascending by reader then owner.</summary>
            public int WeldedVertices;
            public readonly List<WeldPairInfo> WeldPairs = new List<WeldPairInfo>();
            /// <summary>Welds that both MOVED a vertex and handed it to a DIFFERENT bone: the only
            /// kind that changes how the model animates, so it must reach the user.</summary>
            public readonly List<CrossedBoneInfo> CrossedBones = new List<CrossedBoneInfo>();
            /// <summary>Closest unwelded cross-part pair, when one fell outside the weld radius —
            /// a seam the modeller probably meant. -1 when nothing came close.</summary>
            public double NearMissDistance = -1.0;
        }

        /// <summary>One (reading part, owning part) weld bucket. The owner is always the
        /// earlier-drawn part: a pool slot only survives forwards.</summary>
        public class WeldPairInfo
        {
            public string Reader, Owner;
            public int Count;
        }

        public class CrossedBoneInfo
        {
            public string Reader, Owner;
            public int Count;
            public double MaxDistance;
        }

        /// <summary>How an import should treat geometry that no longer matches the template.
        /// Default is the historical behaviour: patch in place, refuse any topology change.</summary>
        public class ImportOptions
        {
            /// <summary>Extra vertices/faces inside a part become NEW geometry (full rewrite).</summary>
            public bool Grow;
            /// <summary>Rebuild EVERY part's geometry from the OBJ; the ILM is only the rig and
            /// the non-geometry template. Mutually exclusive with Grow.</summary>
            public bool Replace;
            /// <summary>Replace only: collapse coincident vertices of adjacent parts onto one
            /// pool slot, which is the format's only welding mechanism.</summary>
            public bool Weld = true;
            /// <summary>Replace only: welding radius in model units.</summary>
            public double WeldEps = WeldEpsDefault;
            /// <summary>Rebuild as a v7 HIGH-POLY model: a PC-only u32 geometry blob with no
            /// 256-slot pool packing, so a dense model keeps full detail (no decimation). The
            /// ILM is still the rig/material template. Mutually exclusive with Grow and Replace.</summary>
            public bool V7;
        }

        // ---- rest pose (.ANM) ---------------------------------------------------

        /// <summary>Reader for the skeleton half of a .ANM (include/bodyprog/formats/anm.h).
        ///
        /// Only the bind poses and one keyframe are needed. There is no true bind pose in the
        /// data: Anim_BoneInit only writes a rotation for bones whose rotationDataIdx is
        /// negative, and for HERO every non-root bone has one, so a keyframe must supply the
        /// rotations. Keyframe 0 is a reasonable default but is NOT reliably an idle pose.</summary>
        private class Anm
        {
            public readonly byte[] D;
            public readonly int DataOffset, RotationBoneCount, TranslationBoneCount;
            public readonly int KeyframeDataSize, BoneCount, KeyframeCount, ScaleLog2, RootYOffset;
            public readonly int[] Parent, RotIdx, TransIdx;
            public readonly int[][] T0;

            public Anm(byte[] data)
            {
                D = data;
                if (data.Length < 0x14) throw new IlmException("too short for an ANM header");
                DataOffset = U16(data, 0x00);
                RotationBoneCount = data[0x02];
                TranslationBoneCount = data[0x03];
                KeyframeDataSize = U16(data, 0x04);
                BoneCount = data[0x06];
                KeyframeCount = U16(data, 0x10);
                ScaleLog2 = data[0x12];
                // rootYOffset is a u8: reading it signed flips 140/154/155/220/255 negative.
                RootYOffset = data[0x13];

                Parent = new int[BoneCount];
                RotIdx = new int[BoneCount];
                TransIdx = new int[BoneCount];
                T0 = new int[BoneCount][];
                for (int i = 0; i < BoneCount; i++)
                {
                    int o = 0x14 + i * 6;
                    if (o + 6 > data.Length)
                        throw new IlmException("bind pose " + i.ToString(Inv) + " past end of file");
                    Parent[i] = S8(data, o);
                    RotIdx[i] = S8(data, o + 1);
                    TransIdx[i] = S8(data, o + 2);
                    T0[i] = new int[] { S8(data, o + 3), S8(data, o + 4), S8(data, o + 5) };
                }
            }

            public bool Valid()
            {
                if (BoneCount <= 0 || KeyframeCount <= 0) return false;
                if (RotationBoneCount * 9 + TranslationBoneCount * 3 != KeyframeDataSize) return false;
                if (DataOffset < 0x14 + BoneCount * 6) return false;
                // scaleLog2 is a u8 from an untrusted file and is only ever 0..4 in the corpus.
                // C# masks a shift count to 5 bits where Python widens without limit, so a wild
                // value would not merely produce nonsense, it would produce DIFFERENT nonsense —
                // reject it here instead and take the loud identity fallback.
                if (ScaleLog2 >= 16) return false;
                // fileSize is the PADDED size, so several real files run 1-3 bytes over their
                // keyframe block; the tail check must be <=, never ==.
                return (long)DataOffset + (long)KeyframeDataSize * KeyframeCount <= (long)D.Length + 4;
            }

            private int Sb(int o)
            {
                // valid()'s 4-byte tail slack lets local() read just past the buffer on a file
                // whose keyframe block is short. Python raises struct.error there; throw so the
                // launcher shows a message rather than an IndexOutOfRangeException stack.
                if (o < 0 || o >= D.Length)
                    throw new IlmException("ANM keyframe data runs past the end of the file");
                return (sbyte)D[o];
            }

            /// <summary>Per-bone local (R q12 row-major, T) — Anim_BoneInit + Anim_BoneUpdate.</summary>
            public void Local(int bone, int kf, out int[] R, out int[] T)
            {
                int baseOff = DataOffset + KeyframeDataSize * kf;
                int s = ScaleLog2;
                T = new int[3];
                if (TransIdx[bone] < 0)
                {
                    for (int i = 0; i < 3; i++) T[i] = T0[bone][i] << s;
                }
                else
                {
                    int o = baseOff + TransIdx[bone] * 3;
                    for (int i = 0; i < 3; i++) T[i] = Sb(o + i) << s;
                    // Keyed on the DATA INDEX, not the bone index.
                    if (TransIdx[bone] == 0) T[1] -= RootYOffset;
                }
                if (RotIdx[bone] < 0) R = (int[])IdentityR.Clone();
                else
                {
                    int o = baseOff + TranslationBoneCount * 3 + RotIdx[bone] * 9;
                    R = new int[9];
                    for (int i = 0; i < 9; i++) R[i] = Sb(o + i) << 5;
                }
            }

            /// <summary>Compose W_child = W_parent * L_child for every bone
            /// (Vw_CoordHierarchyMatrixCompute). Resolved recursively: the bind table is not
            /// guaranteed to list parents before children, and a bad parentBone (out of range,
            /// or self) must be treated as a root rather than crash — the game only survives
            /// those via the COORD_PTR_OK guard in vw_calc.c.</summary>
            public void World(int kf, out int[][] outR, out int[][] outT, List<string> warnings)
            {
                var wr = new int[BoneCount][];
                var wt = new int[BoneCount][];
                var state = new byte[BoneCount];
                for (int i = 0; i < BoneCount; i++) Resolve(i, kf, wr, wt, state, warnings);
                outR = wr; outT = wt;
            }

            private void Resolve(int i, int kf, int[][] wr, int[][] wt, byte[] state, List<string> warnings)
            {
                if (state[i] == 2) return;
                if (i == 0)
                {
                    // The root bone is NEVER animated. Anim_BoneInit calls GsInitCoordinate2
                    // (identity) and then loops from boneIdx = 1, as does Anim_BoneUpdate — see
                    // bodyprog_anim_800445A4.c:64 and :158. Bone 0's keyframe translation is root
                    // MOTION; what the game actually puts in boneCoords[0].coord.t is the
                    // character's map position (bloody_lisa.c:76, dahlia.c:100, kaufmann.c:82).
                    // Baking it would float every model ~280 units off the floor.
                    wr[0] = (int[])IdentityR.Clone();
                    wt[0] = new int[3];
                    state[0] = 2;
                    return;
                }
                int[] R, T;
                if (state[i] == 1)
                {
                    warnings.Add("bone " + i.ToString(Inv) + " is in a parent cycle; treated as a root");
                    Local(i, kf, out R, out T);
                    wr[i] = R; wt[i] = T; state[i] = 2;
                    return;
                }
                state[i] = 1;
                Local(i, kf, out R, out T);
                int p = Parent[i];
                if (p >= 0 && p < BoneCount && p != i)
                {
                    Resolve(p, kf, wr, wt, state, warnings);
                    int[] pr = wr[p], pt = wt[p];
                    var nr = new int[9];
                    for (int a = 0; a < 3; a++)
                        for (int b = 0; b < 3; b++)
                        {
                            // Arithmetic floor-shift, matching the GTE. Do NOT "clean this up" to
                            // /4096: C# integer division truncates toward zero and diverges from
                            // Python's >> on every negative product.
                            long acc = (long)pr[a * 3] * R[b] + (long)pr[a * 3 + 1] * R[3 + b] + (long)pr[a * 3 + 2] * R[6 + b];
                            long v = acc >> 12;
                            // A hostile bind table can make the composition diverge geometrically
                            // with chain depth; Python grows a bignum and prints garbage, C# would
                            // silently wrap. Real matrices sit at ~4096, so 2^20 is 256x margin.
                            if (v > (1 << 20) || v < -(1 << 20))
                                throw new IlmException("the ANM bone hierarchy composes to an unusable matrix");
                            nr[a * 3 + b] = (int)v;
                        }
                    var nt = new int[3];
                    for (int a = 0; a < 3; a++)
                    {
                        long acc = (long)pr[a * 3] * T[0] + (long)pr[a * 3 + 1] * T[1] + (long)pr[a * 3 + 2] * T[2];
                        nt[a] = (int)((acc >> 12) + pt[a]);
                    }
                    R = nr; T = nt;
                }
                wr[i] = R; wt[i] = T; state[i] = 2;
            }
        }

        /// <summary>func_800452EC: the first two ASCII digits of the part name ARE the bone index.
        /// Deliberately not char.IsDigit — that accepts Unicode Nd, which Python's str.isdigit()
        /// would too, but Name8 maps every byte &gt;= 0x80 to U+FFFD so only ASCII can reach here.</summary>
        private static int BoneOf(string name)
        {
            if (name.Length >= 2 && name[0] >= '0' && name[0] <= '9' && name[1] >= '0' && name[1] <= '9')
                return (name[0] - '0') * 10 + (name[1] - '0');
            return 0;
        }

        /// <summary>True 3x3 inverse of a q12 matrix, returned q12-scaled (inv(R/4096)*4096), or
        /// null when singular.
        ///
        /// The transpose is NOT usable here: every element is an s8&lt;&lt;5 and every composition
        /// step truncates &gt;&gt;12, so the composed matrices are only roughly orthonormal
        /// (|det| falls to ~0.72) and a transpose-unbake is wrong on 84% of the corpus.
        /// inv(R_q12) == inv(R_real)/4096, so the factor of 4096 folded in below is load-bearing —
        /// drop it and every vertex comes back scaled by 4096 while still looking plausible.</summary>
        private static double[] MatInverse(int[] R)
        {
            // 4096.0, never Q12: `R[i] / Q12` is integer division and yields a near-zero matrix.
            double a = R[0] / 4096.0, b = R[1] / 4096.0, c = R[2] / 4096.0;
            double d = R[3] / 4096.0, e = R[4] / 4096.0, f = R[5] / 4096.0;
            double g = R[6] / 4096.0, h = R[7] / 4096.0, i = R[8] / 4096.0;
            double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
            if (Math.Abs(det) < 1e-9) return null;
            var inv = new[] { (e * i - f * h), -(b * i - c * h), (b * f - c * e),
                              -(d * i - f * g), (a * i - c * g), -(a * f - c * d),
                              (d * h - e * g), -(a * h - b * g), (a * e - b * d) };
            for (int k = 0; k < 9; k++) inv[k] /= det;
            return inv;
        }

        private static double MatInfNorm(double[] m)
        {
            double best = 0.0;
            for (int r = 0; r < 3; r++)
            {
                double s = Math.Abs(m[r * 3]) + Math.Abs(m[r * 3 + 1]) + Math.Abs(m[r * 3 + 2]);
                if (s > best) best = s;
            }
            return best;
        }

        /// <summary>Local -> world. Float divide, not &gt;&gt;12: this is presentation, not the GTE.</summary>
        private static double[] Bake(int[] R, int[] T, int v0, int v1, int v2)
        {
            var o = new double[3];
            for (int a = 0; a < 3; a++)
                o[a] = ((double)R[a * 3] * v0 + (double)R[a * 3 + 1] * v1 + (double)R[a * 3 + 2] * v2) / 4096.0 + T[a];
            return o;
        }

        private static double[] BakeDir(int[] R, int v0, int v1, int v2)
        {
            var o = new double[3];
            for (int a = 0; a < 3; a++)
                o[a] = ((double)R[a * 3] * v0 + (double)R[a * 3 + 1] * v1 + (double)R[a * 3 + 2] * v2) / 4096.0;
            return o;
        }

        private static double[] Unbake(double[] rinv, int[] T, double[] w)
        {
            var d = new[] { w[0] - T[0], w[1] - T[1], w[2] - T[2] };
            var o = new double[3];
            for (int a = 0; a < 3; a++)
                o[a] = rinv[a * 3] * d[0] + rinv[a * 3 + 1] * d[1] + rinv[a * 3 + 2] * d[2];
            return o;
        }

        private static double[] UnbakeDir(double[] rinv, double[] w)
        {
            var o = new double[3];
            for (int a = 0; a < 3; a++)
                o[a] = rinv[a * 3] * w[0] + rinv[a * 3 + 1] * w[1] + rinv[a * 3 + 2] * w[2];
            return o;
        }

        // ILM stem -> ANM stem, transcribed from CHARA_FILE_INFOS
        // (src/bodyprog/sys/chara_data_info.c). First entry wins where one model has several
        // animation sets (SIBYL/DARIA/KAU/MAR).
        private static readonly Dictionary<string, string> IlmToAnm = BuildIlmToAnm();

        private static Dictionary<string, string> BuildIlmToAnm()
        {
            var m = new Dictionary<string, string>(StringComparer.Ordinal);
            string[] pairs = {
                "HERO","HB_BASE", "BIRD","BIRD", "BD2","BIRD", "DOG","DOG",
                "DG2","DOG", "CLD1","CLD1", "CLD2","CLD2", "CLD3","CLD2",
                "CLD4","CLD2", "SLT","SLT", "COC","COC", "JACK","JACK",
                "CKN","CKN", "FAT","FAT", "MTH","MTH", "PRS","PRS",
                "DUMMY","DUMMY", "PRSD","PRS", "WRM","WRM", "ROD","ROD",
                "BOS","BOS", "MAR","MAR", "MSB","MSB", "DEAD","DEAD",
                "SIBYL","SBL", "SRL","SRL", "CAT","CAT", "DARIA","DA",
                "LISA","LS", "BLISA","BLS", "AR","AR", "TAR","TAR",
                "BAR","BAR", "KAU","KAU", "BFLU","BFLU", "LITL","LITL",
                "DOC","DOC", "ICU","ICU",
            };
            for (int i = 0; i < pairs.Length; i += 2) m[pairs[i]] = pairs[i + 1];
            return m;
        }

        private static List<string> AnmDirs(string ilmPath)
        {
            // GetFullPath first: a relative ILM path otherwise yields an empty parent and the
            // sibling ANIM/ probe silently never matches.
            string d = Path.GetDirectoryName(Path.GetFullPath(ilmPath));
            var dirs = new List<string>();
            string parent = string.IsNullOrEmpty(d) ? null : Path.GetDirectoryName(d);
            if (!string.IsNullOrEmpty(parent)) dirs.Add(Path.Combine(parent, "ANIM"));
            if (!string.IsNullOrEmpty(d)) dirs.Add(d);
            return dirs;
        }

        /// <summary>Locate the .ANM holding this model's skeleton, or null.
        ///
        /// A candidate is accepted only if it parses, passes its arithmetic invariants, AND
        /// covers every bone index the part names ask for — that last check is what correctly
        /// rejects the tempting PRS2 -> PRS.ANM guess.</summary>
        private static Anm FindAnm(string ilmPath, Model[] models, string over, List<string> warnings, out string foundPath)
        {
            foundPath = null;
            int maxbone = 0;
            foreach (Model m in models) { int b = BoneOf(m.Name); if (b > maxbone) maxbone = b; }
            string stem = Path.GetFileNameWithoutExtension(ilmPath).ToUpperInvariant();

            var cands = new List<string>();
            if (!string.IsNullOrEmpty(over)) cands.Add(over);
            else
            {
                var names = new List<string>();
                string mapped;
                if (IlmToAnm.TryGetValue(stem, out mapped)) names.Add(mapped);
                names.Add(stem);
                List<string> dirs = AnmDirs(ilmPath);
                foreach (string name in names)
                    foreach (string d in dirs)
                        cands.Add(Path.Combine(d, name + ".ANM"));
            }

            foreach (string c in cands)
            {
                if (!File.Exists(c)) continue;
                Anm anm;
                try { anm = new Anm(File.ReadAllBytes(c)); }
                catch (Exception ex)
                {
                    warnings.Add(Path.GetFileName(c) + " is not a readable ANM (" + ex.Message + ")");
                    continue;
                }
                if (!anm.Valid())
                {
                    warnings.Add(Path.GetFileName(c) + " failed the ANM header sanity checks");
                    continue;
                }
                if (maxbone >= anm.BoneCount)
                {
                    warnings.Add(Path.GetFileName(c) + " has " + anm.BoneCount.ToString(Inv) +
                        " bones but this model needs bone " + maxbone.ToString(Inv));
                    continue;
                }
                foundPath = c;
                return anm;
            }
            return null;
        }

        private class RestPoses
        {
            public int[][] R, T;
            public string AnmName;   // null => identity
            public int Keyframe;
            public bool IsAnm;
        }

        private static RestPoses Identity(Model[] models, int keyframe)
        {
            var rp = new RestPoses { R = new int[models.Length][], T = new int[models.Length][], Keyframe = keyframe };
            for (int i = 0; i < models.Length; i++) { rp.R[i] = (int[])IdentityR.Clone(); rp.T[i] = new int[3]; }
            return rp;
        }

        /// <summary>Per-part (R q12, T) placing it in model space, plus provenance for the meta.</summary>
        private static RestPoses ComputeRestPoses(string ilmPath, Model[] models, string over, int keyframe, List<string> warnings)
        {
            string path;
            Anm anm = FindAnm(ilmPath, models, over, warnings, out path);
            if (anm == null)
            {
                string stem = Path.GetFileNameWithoutExtension(ilmPath).ToUpperInvariant();
                string tried;
                if (!string.IsNullOrEmpty(over))
                    tried = Path.GetFileName(over) + " was given with --anm but is not usable";
                else
                {
                    string mapped;
                    if (!IlmToAnm.TryGetValue(stem, out mapped)) mapped = stem;
                    var set = new List<string> { mapped };
                    if (!string.Equals(mapped, stem, StringComparison.Ordinal)) set.Add(stem);
                    set.Sort(StringComparer.Ordinal);
                    tried = "looked for " + string.Join(" and ", set.ToArray()) + " in ANIM/ and alongside the ILM";
                }
                warnings.Add("NO REST POSE for " + Path.GetFileName(ilmPath) + "." + Nl +
                    "No usable .ANM was found (" + tried + ")." + Nl +
                    "Every part is exported in its OWN local space, so they will sit piled on the" + Nl +
                    "origin and joint seams will not line up. The round-trip is still byte-exact -" + Nl +
                    "only the layout is wrong.");
                return Identity(models, keyframe);
            }

            int kf = keyframe;
            if (kf < 0 || kf >= anm.KeyframeCount)
            {
                warnings.Add("keyframe " + kf.ToString(Inv) + " is out of range (" + Path.GetFileName(path) +
                    " has " + anm.KeyframeCount.ToString(Inv) + "); using 0");
                kf = 0;
            }
            int[][] wr, wt;
            anm.World(kf, out wr, out wt, warnings);

            var rp = new RestPoses { R = new int[models.Length][], T = new int[models.Length][],
                                     AnmName = Path.GetFileName(path), Keyframe = kf, IsAnm = true };
            for (int i = 0; i < models.Length; i++)
            {
                int b = BoneOf(models[i].Name);
                rp.R[i] = wr[b];
                rp.T[i] = wt[b];
            }

            // Guard the unbake budget: half a quantisation step is 0.25 local units and a
            // 6-decimal OBJ carries ~5e-5 of world error. Measured worst case across the corpus
            // is 2.05, so this should never fire; it exists to turn "measured on the keyframes I
            // tried" into "enforced on whatever the user feeds it".
            for (int i = 0; i < models.Length; i++)
            {
                double[] inv = MatInverse(rp.R[i]);
                if (inv == null || MatInfNorm(inv) * 5e-5 >= 0.25)
                {
                    warnings.Add("part " + models[i].Name + " has a " + (inv == null ? "singular" : "badly conditioned") +
                        " rest-pose matrix; baking it would not survive the round-trip." + Nl +
                        "Falling back to LOCAL space for the WHOLE model (parts piled on the origin).");
                    return Identity(models, keyframe);
                }
            }
            return rp;
        }

        // ---- ILM model ----------------------------------------------------------

        private struct PoolRef { public bool Valid; public int Model, Mesh, Local; }

        private class Prim
        {
            public int Off;
            public byte[] Raw;
            public int[] U, V;
            public int Clut, Flags, MaterialIdx;
            public bool IsTransparent, Tri;
            public byte[] Vtx, Nrm;
            public PoolRef[] VRef, NRef;
        }

        private class Mesh
        {
            public int PrimCount, VertexCount, NormalCount, UnkCount3;
            public int PrimsP, XyP, ZP, NormP, UnkP;
            public short[] Vx, Vy, Vz;
            public sbyte[] Nx, Ny, Nz;
            public byte[] NCount;
            public Prim[] Prims;
        }

        private class Model
        {
            public int Idx, MeshCount, VertexOffset, NormalOffset;
            // Byte 0xB of the model header. field_B_4 = (Bits >> 4) & 3 marks a part the draw
            // path feeds through the third chain, which ignores the window offsets (V12).
            public int Bits;
            public int HdrOff;
            public string Name;
            public Mesh[] Meshes;
        }

        private class Ilm
        {
            public byte[] D;
            public int MatCount, MatsP, ModelCount, ModelHdrsP, ModelOrderP;
            public int[] BaseClutY;
            public Model[] Models;
        }

        /// <summary>Python's bytes.decode('ascii', 'replace'): every byte &gt;= 0x80 becomes U+FFFD.
        /// Encoding.ASCII.GetString would emit '?' instead, which then fails the import name
        /// check on a file that round-trips fine through the Python tool.</summary>
        private static string Name8(byte[] d, int o)
        {
            int n = 0;
            while (n < 8 && d[o + n] != 0) n++;
            var sb = new StringBuilder(n);
            for (int i = 0; i < n; i++) sb.Append(d[o + i] < 0x80 ? (char)d[o + i] : (char)0xFFFD);
            return sb.ToString();
        }

        private static Ilm ParseIlm(byte[] d)
        {
            if (d.Length < 0x14 || d[0] != (byte)'0' || d[1] != 6)
            {
                char magic = d.Length > 0 ? (char)d[0] : '\0';
                int ver = d.Length > 1 ? d[1] : 0;
                throw new IlmException("not an ILM (magic '" + magic + "' version " + ver.ToString(Inv) + ")");
            }

            var ilm = new Ilm();
            ilm.D = d;
            ilm.MatCount = d[3];
            ilm.MatsP = U32(d, 4);
            ilm.ModelCount = d[8];
            ilm.ModelHdrsP = U32(d, 0xC);
            ilm.ModelOrderP = U32(d, 0x10);

            ilm.BaseClutY = new int[ilm.MatCount];
            for (int i = 0; i < ilm.MatCount; i++)
                ilm.BaseClutY[i] = U16(d, ilm.MatsP + i * 24 + 0x10) >> 6;

            ilm.Models = new Model[ilm.ModelCount];
            for (int i = 0; i < ilm.ModelCount; i++)
            {
                int b = ilm.ModelHdrsP + i * 16;
                var m = new Model();
                m.Idx = i;
                m.Name = Name8(d, b);
                m.MeshCount = d[b + 8];
                m.VertexOffset = d[b + 9];
                m.NormalOffset = d[b + 0xA];
                m.Bits = d[b + 0xB];
                m.HdrOff = b;
                int meshHdrsP = U32(d, b + 0xC);
                m.Meshes = new Mesh[m.MeshCount];
                for (int k = 0; k < m.MeshCount; k++)
                    m.Meshes[k] = ParseMesh(d, meshHdrsP + k * 24);
                ilm.Models[i] = m;
            }
            return ilm;
        }

        private static Mesh ParseMesh(byte[] d, int mb)
        {
            var me = new Mesh();
            me.PrimCount = d[mb];
            me.VertexCount = d[mb + 1];
            me.NormalCount = d[mb + 2];
            me.UnkCount3 = d[mb + 3];
            me.PrimsP = U32(d, mb + 4);
            me.XyP = U32(d, mb + 8);
            me.ZP = U32(d, mb + 0xC);
            me.NormP = U32(d, mb + 0x10);
            me.UnkP = U32(d, mb + 0x14);

            me.Vx = new short[me.VertexCount];
            me.Vy = new short[me.VertexCount];
            me.Vz = new short[me.VertexCount];
            for (int v = 0; v < me.VertexCount; v++)
            {
                me.Vx[v] = S16(d, me.XyP + v * 4);
                me.Vy[v] = S16(d, me.XyP + v * 4 + 2);
                me.Vz[v] = S16(d, me.ZP + v * 2);
            }

            me.Nx = new sbyte[me.NormalCount];
            me.Ny = new sbyte[me.NormalCount];
            me.Nz = new sbyte[me.NormalCount];
            me.NCount = new byte[me.NormalCount];
            for (int n = 0; n < me.NormalCount; n++)
            {
                me.Nx[n] = S8(d, me.NormP + n * 4);
                me.Ny[n] = S8(d, me.NormP + n * 4 + 1);
                me.Nz[n] = S8(d, me.NormP + n * 4 + 2);
                me.NCount[n] = d[me.NormP + n * 4 + 3];
            }

            me.Prims = new Prim[me.PrimCount];
            for (int p = 0; p < me.PrimCount; p++)
                me.Prims[p] = ParsePrim(d, me.PrimsP + p * 20);
            return me;
        }

        private static Prim ParsePrim(byte[] d, int pb)
        {
            var pr = new Prim();
            pr.Off = pb;
            pr.Raw = new byte[20];
            Buffer.BlockCopy(d, pb, pr.Raw, 0, 20);
            pr.Flags = U16(d, pb + 6);
            pr.Clut = U16(d, pb + 2);
            // field_6's materialIdx is declared s8:7 in the decomp, so 127 means -1 "no
            // material" (DUMMY.ILM). Never normalise it to 0 — that binds a real material.
            pr.MaterialIdx = (pr.Flags >> 8) & 0x7F;
            pr.IsTransparent = ((pr.Flags >> 15) & 1) != 0;
            pr.Vtx = new byte[4];
            pr.Nrm = new byte[4];
            pr.U = new int[4];
            pr.V = new int[4];
            for (int i = 0; i < 4; i++)
            {
                pr.Vtx[i] = d[pb + 0xC + i];
                pr.Nrm[i] = d[pb + 0x10 + i];
                ushort w = U16(d, pb + UvOffsets[i]);
                pr.U[i] = w & 0xFF;
                pr.V[i] = (w >> 8) & 0xFF;
            }
            pr.Tri = pr.Vtx[3] == TriSentinel;
            pr.VRef = new PoolRef[4];
            pr.NRef = new PoolRef[4];
            return pr;
        }

        /// <summary>Map every primitive index to the (model, mesh, local index) it actually reads.
        ///
        /// Primitives do NOT index their own mesh: func_8005759C copies each part's vertices
        /// into a per-character scratch pool at ModelHeader.vertexOffset (screenXy_0[vertOffset])
        /// and the prims index that POOL. Parts are processed in LmHeader.modelOrder, and their
        /// pool ranges overlap on purpose — a part reads vertices an earlier part left in the
        /// pool wherever they meet, which is how joint seams stay welded (CAT 01BODY_T reads
        /// pool 16, written by 10HIP_TC). Resolving therefore means replaying the pool in draw
        /// order; subtracting vertexOffset instead yields negative indices on the seam prims.</summary>
        private static int[] ResolvePool(Ilm ilm)
        {
            var order = new int[ilm.ModelCount];
            // Python reads modelOrder with a slice, which truncates at EOF instead of raising;
            // the short list then fails the permutation test below and falls back to file
            // order. Indexing D directly would throw IndexOutOfRangeException on a corrupt
            // modelOrderP where the tool still exports.
            if (ilm.ModelOrderP >= 0 && ilm.ModelOrderP + ilm.ModelCount <= ilm.D.Length)
                for (int i = 0; i < ilm.ModelCount; i++) order[i] = ilm.D[ilm.ModelOrderP + i];

            var sorted = (int[])order.Clone();
            Array.Sort(sorted);
            for (int i = 0; i < sorted.Length; i++)
                if (sorted[i] != i)
                {
                    for (int j = 0; j < order.Length; j++) order[j] = j; // not a permutation: fall back to file order
                    break;
                }

            var vpool = new Dictionary<int, PoolRef>();
            var npool = new Dictionary<int, PoolRef>();
            foreach (int mi in order)
            {
                Model model = ilm.Models[mi];
                for (int k = 0; k < model.MeshCount; k++)
                {
                    Mesh mesh = model.Meshes[k];
                    for (int j = 0; j < mesh.VertexCount; j++)
                        vpool[model.VertexOffset + j] = new PoolRef { Valid = true, Model = mi, Mesh = k, Local = j };
                    for (int j = 0; j < mesh.NormalCount; j++)
                        npool[model.NormalOffset + j] = new PoolRef { Valid = true, Model = mi, Mesh = k, Local = j };
                    foreach (Prim p in mesh.Prims)
                    {
                        int n = p.Tri ? 3 : 4;
                        for (int i = 0; i < n; i++)
                        {
                            PoolRef r;
                            p.VRef[i] = vpool.TryGetValue(p.Vtx[i], out r) ? r : default(PoolRef);
                            p.NRef[i] = npool.TryGetValue(p.Nrm[i], out r) ? r : default(PoolRef);
                        }
                    }
                }
            }
            return order;
        }

        /// <summary>Palette row a prim samples: (clut &gt;&gt; 6) - (material.field_10 &gt;&gt; 6).</summary>
        private static int ClutRow(Prim p, int[] baseClutY)
        {
            int mi = p.MaterialIdx;
            int b = (mi >= 0 && mi < baseClutY.Length) ? baseClutY[mi] : 0;
            return (p.Clut >> 6) - b;
        }

        /// <summary>C's "%02d" pads to width 2 INCLUDING the sign, so -1 formats as "-1".
        /// ToString("D2") would give "-01" and break the CLUT-row name contract shared with
        /// ClutComposer / clut_tool.py.</summary>
        private static string Pad2(int v) { return v.ToString(Inv).PadLeft(2, '0'); }

        /// <summary>Material names encode the CLUT palette row and are how import pairs faces
        /// back to primitives, so export and import must build the string identically.</summary>
        private static string MtlName(Prim p, int[] baseClutY)
        {
            return "mat" + Pad2(p.MaterialIdx) + "_row" + Pad2(ClutRow(p, baseClutY)) + (p.IsTransparent ? "_alpha" : "");
        }

        private static string StripExt(string p)
        {
            string dir = Path.GetDirectoryName(p);
            string stem = Path.GetFileNameWithoutExtension(p);
            return string.IsNullOrEmpty(dir) ? stem : Path.Combine(dir, stem);
        }

        private static void WriteText(string path, string text)
        {
            File.WriteAllText(path, text, new UTF8Encoding(false)); // a BOM would make the OBJ's first byte non-ASCII
        }

        private static int RefKey(int model, int mesh, int local) { return (model << 16) | (mesh << 8) | local; }
        private static int RefKey(PoolRef r) { return (r.Model << 16) | (r.Mesh << 8) | r.Local; }

        // ---- part layout --------------------------------------------------------

        /// <summary>Ordered (own, foreign) vertex/normal reference lists for one `o` block.
        ///
        /// Every OBJ object must be self-contained — a face may only cite indices emitted
        /// inside its own block. Blender's OBJ exporter sorts objects alphabetically and
        /// re-partitions vertices by which faces touch them, so a globally-numbered layout (a
        /// face in part A citing a `v` line inside part B) cannot survive a Blender round-trip
        /// at all. Seam vertices are therefore DUPLICATED into every part that reads them,
        /// baked by their OWNER's matrix.</summary>
        private static void PartLayout(Model model, out List<PoolRef> ownV, out List<PoolRef> forV,
                                       out List<PoolRef> ownN, out List<PoolRef> forN)
        {
            int mi = model.Idx;
            ownV = new List<PoolRef>();
            ownN = new List<PoolRef>();
            var ownVs = new HashSet<int>();
            var ownNs = new HashSet<int>();
            for (int k = 0; k < model.MeshCount; k++)
            {
                Mesh me = model.Meshes[k];
                for (int j = 0; j < me.VertexCount; j++)
                {
                    ownV.Add(new PoolRef { Valid = true, Model = mi, Mesh = k, Local = j });
                    ownVs.Add(RefKey(mi, k, j));
                }
                for (int j = 0; j < me.NormalCount; j++)
                {
                    ownN.Add(new PoolRef { Valid = true, Model = mi, Mesh = k, Local = j });
                    ownNs.Add(RefKey(mi, k, j));
                }
            }
            forV = new List<PoolRef>();
            forN = new List<PoolRef>();
            var seenV = new HashSet<int>();
            var seenN = new HashSet<int>();
            for (int k = 0; k < model.MeshCount; k++)
                foreach (Prim p in model.Meshes[k].Prims)
                {
                    int n = p.Tri ? 3 : 4;
                    // Plain 0..n-1, NOT CornerOrder: this is discovery order over the prim's own
                    // corners and it fixes the OBJ's seam-duplicate numbering, so reordering it
                    // would renumber every `v` line for no reason.
                    for (int i = 0; i < n; i++)
                    {
                        PoolRef vr = p.VRef[i], nr = p.NRef[i];
                        if (vr.Valid && !ownVs.Contains(RefKey(vr)) && seenV.Add(RefKey(vr))) forV.Add(vr);
                        if (nr.Valid && !ownNs.Contains(RefKey(nr)) && seenN.Add(RefKey(nr))) forN.Add(nr);
                    }
                }
        }

        // ---- exact %.6f for a q12 rational --------------------------------------

        /// <summary>Format (neg ? -1 : 1) * n/4096 exactly as CPython's "%.6f" would.
        ///
        /// Every coordinate this tool emits is exactly n/4096 for an integer n, and 10^6/4096 is
        /// 15625/64, so the whole thing is integer arithmetic. No double formatter on .NET
        /// Framework rounds half-to-even the way "%.6f" does: "F6" pre-rounds to 15 significant
        /// digits and then rounds half-away-from-zero, which diverges on 5141 of the 91026
        /// coordinates in the CHARA corpus (every n congruent to 32 mod 64 is an exact midpoint).
        /// The sign is taken before rounding so a zero magnitude still prints "-0.000000",
        /// matching '%.6f' % -0.0 — 193 corpus coordinates need it.</summary>
        private static string FormatQ12(long n, bool neg)
        {
            bool isNeg = (n < 0) != neg;
            long a = n < 0 ? -n : n;
            long num = a * 15625L, q = num / 64L, r = num % 64L;
            if (r > 32L || (r == 32L && (q & 1L) != 0L)) q++;
            return (isNeg ? "-" : "") + (q / 1000000L).ToString(Inv) + "." +
                   (q % 1000000L).ToString(Inv).PadLeft(6, '0');
        }

        // Numerators over 4096 of Bake / BakeDir, so FormatQ12 never sees a double.
        private static long BakeNum(int[] R, int[] T, int a, int v0, int v1, int v2)
        {
            return (long)R[a * 3] * v0 + (long)R[a * 3 + 1] * v1 + (long)R[a * 3 + 2] * v2 + 4096L * T[a];
        }

        private static long BakeDirNum(int[] R, int a, int v0, int v1, int v2)
        {
            return (long)R[a * 3] * v0 + (long)R[a * 3 + 1] * v1 + (long)R[a * 3 + 2] * v2;
        }

        /// <summary>Each part's donor CLUT-row material name (mat00_rowNN), so the high-poly texture
        /// prep can re-label a new model's faces to names the v7 path accepts. A part uses its first
        /// prim's material; a part with no prims falls back to mat00_row00.</summary>
        public static Dictionary<string, string> PartMaterialRows(string ilmPath)
        {
            byte[] data = File.ReadAllBytes(ilmPath);
            Ilm ilm = ParseIlm(data);
            ResolvePool(ilm);
            var map = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (Model m in ilm.Models)
            {
                string sh = null;
                foreach (Mesh me in m.Meshes)
                    if (me.Prims != null && me.Prims.Length > 0) { sh = MtlName(me.Prims[0], ilm.BaseClutY); break; }
                map[m.Name] = sh != null ? sh : "mat00_row00";
            }
            return map;
        }

        /// <summary>Read the character TIM's native pixel size (the loose-override shader samples
        /// UVs as u/W, v/H, so a non-square TIM makes the atlas V need scaling). Looks for NAME.TIM
        /// beside the .ILM. Returns false when not found or unparseable.</summary>
        public static bool TryReadNativeTimDims(string ilmPath, out int w, out int h)
        {
            w = 0; h = 0;
            string dir = Path.GetDirectoryName(ilmPath);
            string stem = Path.GetFileNameWithoutExtension(ilmPath);
            string tim = Path.Combine(dir, stem + ".TIM");
            if (!File.Exists(tim)) tim = Path.Combine(dir, stem + ".tim");
            if (!File.Exists(tim)) return false;
            byte[] d = File.ReadAllBytes(tim);
            if (d.Length < 12 || d[0] != 0x10 || d[1] != 0 || d[2] != 0 || d[3] != 0) return false;
            uint flags = (uint)(d[4] | (d[5] << 8) | (d[6] << 16) | ((uint)d[7] << 24));
            int bpp = (int)(flags & 7);
            bool hasClut = ((flags >> 3) & 1) != 0;
            int p = 8;
            if (hasClut)
            {
                if (p + 4 > d.Length) return false;
                uint clen = (uint)(d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint)d[p + 3] << 24));
                p += (int)clen;
            }
            if (p + 12 > d.Length) return false;
            int cellW = d[p + 8] | (d[p + 9] << 8);
            int pixH = d[p + 10] | (d[p + 11] << 8);
            int mul = bpp == 0 ? 4 : bpp == 1 ? 2 : 1;
            w = cellW * mul; h = pixH;
            return w > 0 && h > 0;
        }

        // ---- export -------------------------------------------------------------

        public static ExportResult Export(string ilmPath, string outObjPath)
        {
            return Export(ilmPath, outObjPath, null, 0);
        }

        public static ExportResult Export(string ilmPath, string outObjPath, string anmOverride, int keyframe)
        {
            var res = new ExportResult();
            try { ExportCore(ilmPath, outObjPath, anmOverride, keyframe, res); }
            catch (Exception ex) { res.Error = ex.Message; }
            return res;
        }

        public static bool Export(string ilmPath, string outObjPath, out string error)
        {
            ExportResult res = Export(ilmPath, outObjPath);
            error = res.Error;
            return res.Error == null;
        }

        private static void ExportCore(string ilmPath, string outObjPath, string anmOverride, int keyframe, ExportResult res)
        {
            Ilm ilm = ParseIlm(File.ReadAllBytes(ilmPath));
            int[] order = ResolvePool(ilm);
            RestPoses poses = ComputeRestPoses(ilmPath, ilm.Models, anmOverride, keyframe, res.Warnings);

            if (string.IsNullOrEmpty(outObjPath)) outObjPath = StripExt(ilmPath) + ".obj";
            string mtlPath = StripExt(outObjPath) + ".mtl";
            string metaPath = StripExt(outObjPath) + ".ilmmeta.json";

            var obj = new List<string>();
            obj.Add("# Silent Hill ILM export: " + Path.GetFileName(ilmPath));
            obj.Add("# Each 'o' is a rigid animated body part - do NOT rename/add/remove them.");
            obj.Add("#");
            obj.Add("# Vertices are in WORLD space (the " + (poses.AnmName ?? "identity") + " rest pose is baked in).");
            obj.Add("# Seam vertices are DUPLICATED on purpose. A low-threshold 'Merge by");
            obj.Add("# Distance' is harmless, but an aggressive one welds them and import");
            obj.Add("# will refuse the file. Parts sharing a bone (hands, head/neck) overlap");
            obj.Add("# in the viewport on purpose; the game draws one at a time.");
            obj.Add("# Do NOT reorder vertices or faces (Mesh > Sort Elements) and do NOT move");
            obj.Add("# a face to a different material - import matches them by order.");
            obj.Add("# The model is ~390 units tall: press Home, or raise the viewport clip end.");
            obj.Add("mtllib " + Path.GetFileName(mtlPath));
            obj.Add("");

            int tbase = 1, vbase = 1, nbase = 1;
            var matNames = new List<string>();
            var matAlpha = new Dictionary<string, bool>();
            int dangling = 0;

            var js = new StringBuilder();
            js.Append('{').Append(Nl);
            js.Append(' ').Append(JStr("source")).Append(": ").Append(JStr(Path.GetFileName(ilmPath))).Append(',').Append(Nl);
            js.Append(' ').Append(JStr("drawOrder")).Append(": ");
            JIntArray(js, order, 1);
            js.Append(',').Append(Nl);
            js.Append(' ').Append(JStr("anm")).Append(": ").Append(poses.AnmName == null ? "null" : JStr(poses.AnmName)).Append(',').Append(Nl);
            js.Append(' ').Append(JStr("keyframe")).Append(": ").Append(poses.Keyframe.ToString(Inv)).Append(',').Append(Nl);
            js.Append(' ').Append(JStr("restPose")).Append(": ").Append(JStr(poses.IsAnm ? "anm" : "identity")).Append(',').Append(Nl);
            js.Append(' ').Append(JStr("models")).Append(": ");
            JOpenArray(js, ilm.Models.Length);

            for (int modelI = 0; modelI < ilm.Models.Length; modelI++)
            {
                Model model = ilm.Models[modelI];
                List<PoolRef> ownV, forV, ownN, forN;
                PartLayout(model, out ownV, out forV, out ownN, out forN);

                var vidx = new Dictionary<int, int>();
                for (int i = 0; i < ownV.Count; i++) vidx[RefKey(ownV[i])] = vbase + i;
                for (int i = 0; i < forV.Count; i++) vidx[RefKey(forV[i])] = vbase + ownV.Count + i;
                var nidx = new Dictionary<int, int>();
                for (int i = 0; i < ownN.Count; i++) nidx[RefKey(ownN[i])] = nbase + i;
                for (int i = 0; i < forN.Count; i++) nidx[RefKey(forN[i])] = nbase + ownN.Count + i;

                obj.Add("o " + model.Name);
                for (int pass = 0; pass < 2; pass++)
                {
                    List<PoolRef> list = pass == 0 ? ownV : forV;
                    foreach (PoolRef r in list)
                    {
                        // The OWNER's pose, not this part's: a seam duplicate is the neighbour's
                        // vertex and only lands on the joint when transformed by the neighbour.
                        int[] oR = poses.R[r.Model], oT = poses.T[r.Model];
                        Mesh me = ilm.Models[r.Model].Meshes[r.Mesh];
                        int x = me.Vx[r.Local], y = me.Vy[r.Local], z = me.Vz[r.Local];
                        obj.Add("v " + FormatQ12(BakeNum(oR, oT, 0, x, y, z), false) +
                                " " + FormatQ12(BakeNum(oR, oT, 1, x, y, z), true) +   // YOut
                                " " + FormatQ12(BakeNum(oR, oT, 2, x, y, z), false));
                    }
                }
                for (int pass = 0; pass < 2; pass++)
                {
                    List<PoolRef> list = pass == 0 ? ownN : forN;
                    foreach (PoolRef r in list)
                    {
                        int[] oR = poses.R[r.Model];
                        Mesh me = ilm.Models[r.Model].Meshes[r.Mesh];
                        int x = me.Nx[r.Local], y = me.Ny[r.Local], z = me.Nz[r.Local];
                        obj.Add("vn " + FormatQ12(BakeDirNum(oR, 0, x, y, z), true) +   // NormalOut
                                " " + FormatQ12(BakeDirNum(oR, 1, x, y, z), false) +
                                " " + FormatQ12(BakeDirNum(oR, 2, x, y, z), true));
                    }
                }
                vbase += ownV.Count + forV.Count;
                nbase += ownN.Count + forN.Count;

                Ind(js, 2).Append('{').Append(Nl);
                Ind(js, 3).Append(JStr("name")).Append(": ").Append(JStr(model.Name)).Append(',').Append(Nl);
                Ind(js, 3).Append(JStr("vertexOffset")).Append(": ").Append(model.VertexOffset.ToString(Inv)).Append(',').Append(Nl);
                Ind(js, 3).Append(JStr("normalOffset")).Append(": ").Append(model.NormalOffset.ToString(Inv)).Append(',').Append(Nl);
                Ind(js, 3).Append(JStr("restPose")).Append(": ").Append('{').Append(Nl);
                Ind(js, 4).Append(JStr("R")).Append(": ");
                JIntArray(js, poses.R[modelI], 4);
                js.Append(',').Append(Nl);
                Ind(js, 4).Append(JStr("T")).Append(": ");
                JIntArray(js, poses.T[modelI], 4);
                js.Append(Nl);
                Ind(js, 3).Append('}').Append(',').Append(Nl);
                Ind(js, 3).Append(JStr("ownVertexCount")).Append(": ").Append(ownV.Count.ToString(Inv)).Append(',').Append(Nl);
                Ind(js, 3).Append(JStr("ownNormalCount")).Append(": ").Append(ownN.Count.ToString(Inv)).Append(',').Append(Nl);
                Ind(js, 3).Append(JStr("foreignVerts")).Append(": ");
                JRefArray(js, forV);
                js.Append(',').Append(Nl);
                Ind(js, 3).Append(JStr("foreignNorms")).Append(": ");
                JRefArray(js, forN);
                js.Append(',').Append(Nl);
                Ind(js, 3).Append(JStr("meshes")).Append(": ");
                JOpenArray(js, model.MeshCount);

                for (int mi = 0; mi < model.MeshCount; mi++)
                {
                    Mesh mesh = model.Meshes[mi];

                    // UVs are per prim-corner, so emit one vt per corner in prim order. This is a
                    // separate pass before the face pass so the vt numbering stays contiguous.
                    foreach (Prim p in mesh.Prims)
                    {
                        int cn = p.Tri ? 3 : 4;
                        for (int c = 0; c < cn; c++)
                        {
                            // PSX texel centre -> OBJ [0,1], V flipped (OBJ origin is bottom-left).
                            // 256 is the PSX page width, not the texture width. Both values are
                            // exact multiples of 1/4096, so FormatQ12 applies unchanged.
                            obj.Add("vt " + FormatQ12(8L * (2 * p.U[c] + 1), false) +
                                    " " + FormatQ12(8L * (511 - 2 * p.V[c]), false));
                        }
                    }

                    int t = tbase;
                    Ind(js, 4).Append('{').Append(Nl);
                    Ind(js, 5).Append(JStr("vertexCount")).Append(": ").Append(mesh.VertexCount.ToString(Inv)).Append(',').Append(Nl);
                    Ind(js, 5).Append(JStr("normalCount")).Append(": ").Append(mesh.NormalCount.ToString(Inv)).Append(',').Append(Nl);
                    Ind(js, 5).Append(JStr("primCount")).Append(": ").Append(mesh.PrimCount.ToString(Inv)).Append(',').Append(Nl);
                    Ind(js, 5).Append(JStr("unkCount_3")).Append(": ").Append(mesh.UnkCount3.ToString(Inv)).Append(',').Append(Nl);
                    Ind(js, 5).Append(JStr("normalCounts")).Append(": ");
                    JByteArray(js, mesh.NCount, 5);
                    js.Append(',').Append(Nl);
                    Ind(js, 5).Append(JStr("prims")).Append(": ");
                    JOpenArray(js, mesh.PrimCount);

                    for (int pi = 0; pi < mesh.PrimCount; pi++)
                    {
                        Prim p = mesh.Prims[pi];
                        string mname = MtlName(p, ilm.BaseClutY);
                        if (!matAlpha.ContainsKey(mname)) { matAlpha[mname] = p.IsTransparent; matNames.Add(mname); }
                        obj.Add("usemtl " + mname);

                        int n = p.Tri ? 3 : 4;
                        int[] loop = CornerOrder(p);
                        var face = new StringBuilder("f");
                        for (int c = 0; c < n; c++)
                        {
                            int i = loop[c];
                            PoolRef vr = p.VRef[i], nr = p.NRef[i];
                            if (!vr.Valid || !nr.Valid)
                            {
                                dangling++;
                                // Substitute local 0 of THIS mesh (mi is the mesh index here).
                                if (!vr.Valid) vr = new PoolRef { Valid = true, Model = model.Idx, Mesh = mi, Local = 0 };
                                if (!nr.Valid) nr = new PoolRef { Valid = true, Model = model.Idx, Mesh = mi, Local = 0 };
                            }
                            // `t + i` indexes the vt emitted for PRIM corner i above, so the
                            // v/vt/vn triple stays together as the loop reorders corners.
                            face.Append(' ').Append(Lookup(vidx, vr, "vertex").ToString(Inv))
                                .Append('/').Append((t + i).ToString(Inv))
                                .Append('/').Append(Lookup(nidx, nr, "normal").ToString(Inv));
                        }
                        obj.Add(face.ToString());
                        t += n;

                        Ind(js, 6).Append('{').Append(Nl);
                        Ind(js, 7).Append(JStr("tri")).Append(": ").Append(p.Tri ? "true" : "false").Append(',').Append(Nl);
                        Ind(js, 7).Append(JStr("raw")).Append(": ").Append(JStr(Hex(p.Raw))).Append(Nl);
                        Ind(js, 6).Append('}').Append(pi + 1 < mesh.PrimCount ? "," : "").Append(Nl);
                    }

                    JCloseArray(js, mesh.PrimCount, 5);
                    js.Append(Nl);
                    Ind(js, 4).Append('}').Append(mi + 1 < model.MeshCount ? "," : "").Append(Nl);
                    tbase = t;
                }

                JCloseArray(js, model.MeshCount, 3);
                js.Append(Nl);
                Ind(js, 2).Append('}').Append(modelI + 1 < ilm.Models.Length ? "," : "").Append(Nl);
                obj.Add("");
            }

            JCloseArray(js, ilm.Models.Length, 1);
            js.Append(Nl).Append('}');

            var mtl = new StringBuilder();
            mtl.Append("# CLUT palette row is encoded in the material NAME (rowNN) - keep it.").Append(Nl);
            matNames.Sort(StringComparer.Ordinal); // Python sorted() is by code point; culture-aware sorts reorder '_' vs '-'
            foreach (string name in matNames)
            {
                mtl.Append(Nl).Append("newmtl ").Append(name).Append(Nl).Append("Kd 1.0 1.0 1.0").Append(Nl);
                if (matAlpha[name]) mtl.Append("d 0.5").Append(Nl);
            }

            WriteText(outObjPath, string.Join(Nl, obj.ToArray()));
            WriteText(mtlPath, mtl.ToString());
            WriteText(metaPath, js.ToString());

            int nv = 0, npr = 0;
            foreach (Model m in ilm.Models)
                foreach (Mesh me in m.Meshes) { nv += me.VertexCount; npr += me.PrimCount; }

            res.ObjPath = outObjPath;
            res.MtlPath = mtlPath;
            res.MetaPath = metaPath;
            res.Parts = ilm.Models.Length;
            res.Vertices = nv;
            res.SeamDuplicates = vbase - 1 - nv;
            res.Prims = npr;
            res.Materials = matNames.Count;
            res.Dangling = dangling;
            res.AnmName = poses.AnmName;
            res.Keyframe = poses.Keyframe;
        }

        private static int Lookup(Dictionary<int, int> map, PoolRef r, string what)
        {
            int v;
            if (!map.TryGetValue(RefKey(r), out v))
                throw new IlmException("primitive references a " + what + " slot no mesh owns (model " +
                    r.Model.ToString(Inv) + " mesh " + r.Mesh.ToString(Inv) + " local " + r.Local.ToString(Inv) + ")");
            return v;
        }

        private static string Hex(byte[] b)
        {
            var sb = new StringBuilder(b.Length * 2);
            foreach (byte x in b) sb.Append(x.ToString("x2", Inv));
            return sb.ToString();
        }

        // ---- .ilmmeta.json writer (json.dump indent=1, ensure_ascii) -------------

        private static StringBuilder Ind(StringBuilder sb, int n) { return sb.Append(' ', n); }

        private static void JOpenArray(StringBuilder sb, int count)
        {
            if (count == 0) sb.Append("[]");
            else sb.Append('[').Append(Nl);
        }

        private static void JCloseArray(StringBuilder sb, int count, int indent)
        {
            if (count != 0) Ind(sb, indent).Append(']');
        }

        private static void JIntArray(StringBuilder sb, int[] vals, int indent)
        {
            if (vals.Length == 0) { sb.Append("[]"); return; }
            sb.Append('[').Append(Nl);
            for (int i = 0; i < vals.Length; i++)
                Ind(sb, indent + 1).Append(vals[i].ToString(Inv)).Append(i + 1 < vals.Length ? "," : "").Append(Nl);
            Ind(sb, indent).Append(']');
        }

        private static void JByteArray(StringBuilder sb, byte[] vals, int indent)
        {
            if (vals.Length == 0) { sb.Append("[]"); return; }
            sb.Append('[').Append(Nl);
            for (int i = 0; i < vals.Length; i++)
                Ind(sb, indent + 1).Append(((int)vals[i]).ToString(Inv)).Append(i + 1 < vals.Length ? "," : "").Append(Nl);
            Ind(sb, indent).Append(']');
        }

        /// <summary>The foreignVerts / foreignNorms array of {ownerModel, mesh, local} objects,
        /// at the model-key indent. Empty renders inline as [] like every other array.</summary>
        private static void JRefArray(StringBuilder sb, List<PoolRef> refs)
        {
            if (refs.Count == 0) { sb.Append("[]"); return; }
            sb.Append('[').Append(Nl);
            for (int i = 0; i < refs.Count; i++)
            {
                Ind(sb, 4).Append('{').Append(Nl);
                Ind(sb, 5).Append(JStr("ownerModel")).Append(": ").Append(refs[i].Model.ToString(Inv)).Append(',').Append(Nl);
                Ind(sb, 5).Append(JStr("mesh")).Append(": ").Append(refs[i].Mesh.ToString(Inv)).Append(',').Append(Nl);
                Ind(sb, 5).Append(JStr("local")).Append(": ").Append(refs[i].Local.ToString(Inv)).Append(Nl);
                Ind(sb, 4).Append('}').Append(i + 1 < refs.Count ? "," : "").Append(Nl);
            }
            Ind(sb, 3).Append(']');
        }

        private static string JStr(string s)
        {
            var sb = new StringBuilder(s.Length + 2);
            sb.Append('"');
            foreach (char c in s)
            {
                if (c == '"') sb.Append("\\\"");
                else if (c == '\\') sb.Append("\\\\");
                else if (c == '\n') sb.Append("\\n");
                else if (c == '\r') sb.Append("\\r");
                else if (c == '\t') sb.Append("\\t");
                else if (c == '\b') sb.Append("\\b");
                else if (c == '\f') sb.Append("\\f");
                else if (c < 0x20 || c > 0x7E) sb.Append("\\u").Append(((int)c).ToString("x4", Inv));
                else sb.Append(c);
            }
            sb.Append('"');
            return sb.ToString();
        }

        // ---- .ilmmeta.json reader -----------------------------------------------

        /// <summary>The descent is recursive on fully untrusted input (a .ilmmeta.json arrives
        /// inside a downloaded mod archive), and a StackOverflowException cannot be caught on
        /// .NET 2.0+ — it kills the launcher with no message. The exporter's own meta nests 7
        /// deep, so any real file is far below this.</summary>
        private const int JsonMaxDepth = 200;

        private class JNode
        {
            public char Kind;                 // 'o' object, 'a' array, 's' string, 'n' number, 'b' bool, 'z' null
            public string Text;               // 's'/'n'/'b'
            public List<JNode> Items;         // 'a', and the values of 'o' in key order
            public List<string> Keys;         // 'o'
        }

        private static JNode ParseJson(string s, string what)
        {
            int i = 0;
            try
            {
                JSkipWs(s, ref i);
                JNode n = JValue(s, ref i, 0);
                JSkipWs(s, ref i);
                if (i != s.Length) throw new IlmException("trailing data");
                return n;
            }
            catch (Exception ex) { throw new IlmException(what + " is not valid JSON (" + ex.Message + ")"); }
        }

        private static void JSkipWs(string s, ref int i)
        {
            while (i < s.Length && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        }

        private static JNode JValue(string s, ref int i, int depth)
        {
            if (i >= s.Length) throw new IlmException("unexpected end of input");
            if (depth > JsonMaxDepth) throw new IlmException("nesting too deep");
            char c = s[i];
            if (c == '{') return JObject(s, ref i, depth + 1);
            if (c == '[') return JArray(s, ref i, depth + 1);
            if (c == '"') return new JNode { Kind = 's', Text = JString(s, ref i) };
            if (JLiteral(s, ref i, "true")) return new JNode { Kind = 'b', Text = "true" };
            if (JLiteral(s, ref i, "false")) return new JNode { Kind = 'b', Text = "false" };
            if (JLiteral(s, ref i, "null")) return new JNode { Kind = 'z' };
            return new JNode { Kind = 'n', Text = JNumber(s, ref i) };
        }

        private static bool JLiteral(string s, ref int i, string lit)
        {
            if (i + lit.Length > s.Length || string.CompareOrdinal(s, i, lit, 0, lit.Length) != 0) return false;
            i += lit.Length;
            return true;
        }

        private static JNode JObject(string s, ref int i, int depth)
        {
            var node = new JNode { Kind = 'o', Keys = new List<string>(), Items = new List<JNode>() };
            i++;
            JSkipWs(s, ref i);
            if (i < s.Length && s[i] == '}') { i++; return node; }
            for (; ; )
            {
                JSkipWs(s, ref i);
                if (i >= s.Length || s[i] != '"') throw new IlmException("expected a key at offset " + i.ToString(Inv));
                node.Keys.Add(JString(s, ref i));
                JSkipWs(s, ref i);
                if (i >= s.Length || s[i] != ':') throw new IlmException("expected ':' at offset " + i.ToString(Inv));
                i++;
                JSkipWs(s, ref i);
                node.Items.Add(JValue(s, ref i, depth));
                JSkipWs(s, ref i);
                if (i < s.Length && s[i] == ',') { i++; continue; }
                if (i < s.Length && s[i] == '}') { i++; return node; }
                throw new IlmException("expected ',' or '}' at offset " + i.ToString(Inv));
            }
        }

        private static JNode JArray(string s, ref int i, int depth)
        {
            var node = new JNode { Kind = 'a', Items = new List<JNode>() };
            i++;
            JSkipWs(s, ref i);
            if (i < s.Length && s[i] == ']') { i++; return node; }
            for (; ; )
            {
                JSkipWs(s, ref i);
                node.Items.Add(JValue(s, ref i, depth));
                JSkipWs(s, ref i);
                if (i < s.Length && s[i] == ',') { i++; continue; }
                if (i < s.Length && s[i] == ']') { i++; return node; }
                throw new IlmException("expected ',' or ']' at offset " + i.ToString(Inv));
            }
        }

        private static string JString(string s, ref int i)
        {
            var sb = new StringBuilder();
            i++;
            while (i < s.Length)
            {
                char c = s[i++];
                if (c == '"') return sb.ToString();
                if (c != '\\') { sb.Append(c); continue; }
                if (i >= s.Length) break;
                char e = s[i++];
                if (e == 'u')
                {
                    if (i + 4 > s.Length) throw new IlmException("truncated \\u escape");
                    int cp;
                    if (!int.TryParse(s.Substring(i, 4), NumberStyles.HexNumber, Inv, out cp))
                        throw new IlmException("bad \\u escape");
                    sb.Append((char)cp);
                    i += 4;
                }
                else if (e == '"') sb.Append('"');
                else if (e == '\\') sb.Append('\\');
                else if (e == '/') sb.Append('/');
                else if (e == 'b') sb.Append('\b');
                else if (e == 'f') sb.Append('\f');
                else if (e == 'n') sb.Append('\n');
                else if (e == 'r') sb.Append('\r');
                else if (e == 't') sb.Append('\t');
                else throw new IlmException("bad escape '\\" + e + "'");
            }
            throw new IlmException("unterminated string");
        }

        private static string JNumber(string s, ref int i)
        {
            int start = i;
            if (i < s.Length && (s[i] == '-' || s[i] == '+')) i++;
            while (i < s.Length && (char.IsDigit(s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    ((s[i] == '-' || s[i] == '+') && (s[i - 1] == 'e' || s[i - 1] == 'E')))) i++;
            if (i == start) throw new IlmException("unexpected character '" + s[start] + "' at offset " + start.ToString(Inv));
            return s.Substring(start, i - start);
        }

        private static JNode JGet(JNode o, string key)
        {
            if (o == null || o.Kind != 'o') return null;
            for (int i = 0; i < o.Keys.Count; i++) if (string.Equals(o.Keys[i], key, StringComparison.Ordinal)) return o.Items[i];
            return null;
        }

        /// <summary>An integer array out of the meta. The exporter only ever writes integers here;
        /// anything else means a hand-edited or foreign file and is worth naming.</summary>
        private static int[] JIntsOf(JNode n, int want, string what)
        {
            if (n == null || n.Kind != 'a' || n.Items.Count != want)
                throw new IlmException("the .ilmmeta.json " + what + " is not a " + want.ToString(Inv) + "-element array");
            var v = new int[want];
            for (int i = 0; i < want; i++)
            {
                long l;
                if (n.Items[i].Kind != 'n' || !long.TryParse(n.Items[i].Text, NumberStyles.Integer, Inv, out l) ||
                    l < int.MinValue || l > int.MaxValue)
                    throw new IlmException("the .ilmmeta.json " + what + " holds a non-integer value");
                v[i] = (int)l;
            }
            return v;
        }

        // ---- OBJ parsing --------------------------------------------------------

        private class ObjFace
        {
            public int[][] C;
            public string Mtl;
        }

        private class ObjObject
        {
            public string Name;
            public readonly List<ObjFace> Faces = new List<ObjFace>();
            // Global 1-based `v` / `vn` line numbers emitted inside this object, in file order.
            public readonly List<int> V = new List<int>();
            public readonly List<int> Vn = new List<int>();
        }

        private class ObjFile
        {
            public readonly List<double[]> Verts = new List<double[]>();
            public readonly List<double[]> Norms = new List<double[]>();
            public readonly List<double[]> Uvs = new List<double[]>();
            public readonly List<ObjObject> Objs = new List<ObjObject>();
        }

        /// <summary>Minimal OBJ reader: objects in file order, each with its v/vn/faces. v/vn/vt
        /// are GLOBAL file-order lists shared by every object, exactly as the Python has them.
        /// Splitting the keyword off at the first space only (never a tab) is deliberate — it
        /// mirrors the Python, which silently ignores a tab-separated OBJ.</summary>
        private static ObjFile ParseObj(string path)
        {
            var f = new ObjFile();
            ObjObject cur = null;
            string mtl = null;   // persists across `o` boundaries, as in the Python
            bool sawO = false;
            foreach (string raw in File.ReadLines(path))
            {
                string ln = raw.Trim();
                if (ln.Length == 0 || ln[0] == '#') continue;
                int sp = ln.IndexOf(' ');
                string k = sp < 0 ? ln : ln.Substring(0, sp);
                string rest = sp < 0 ? "" : ln.Substring(sp + 1);

                if (k == "usemtl") { mtl = rest.Trim(); continue; }
                // Milkshape 3D (and several older exporters) write groups as `g` and never
                // emit `o`; the part name is the bone binding either way. `o` wins when a
                // file carries both, because Blender writes `o NAME` plus a redundant
                // `g NAME` and treating those as two parts would double the count.
                if (k == "o" || (k == "g" && !sawO))
                {
                    if (k == "o" && !sawO)
                    {
                        sawO = true;
                        bool allEmpty = true;
                        foreach (ObjObject x in f.Objs) if (x.Faces.Count != 0) { allEmpty = false; break; }
                        if (f.Objs.Count != 0 && allEmpty) { f.Objs.Clear(); cur = null; }
                    }
                    string nm = rest.Trim();
                    if (k == "g" && (nm.Length == 0 || nm == "default" || nm == "off")) continue;
                    cur = new ObjObject { Name = nm };
                    f.Objs.Add(cur);
                }
                else if (k == "v")
                {
                    f.Verts.Add(Floats(rest, 3, "v"));
                    if (cur != null) cur.V.Add(f.Verts.Count);
                }
                else if (k == "vn")
                {
                    f.Norms.Add(Floats(rest, 3, "vn"));
                    if (cur != null) cur.Vn.Add(f.Norms.Count);
                }
                else if (k == "vt") f.Uvs.Add(Floats(rest, 2, "vt"));
                else if (k == "f")
                {
                    if (cur == null)
                    {
                        cur = new ObjObject { Name = "" };
                        f.Objs.Add(cur);
                    }
                    string[] toks = rest.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                    var corners = new int[toks.Length][];
                    for (int i = 0; i < toks.Length; i++)
                    {
                        string[] parts = toks[i].Split('/');
                        corners[i] = new[] { Component(parts, 0), Component(parts, 1), Component(parts, 2) };
                    }
                    cur.Faces.Add(new ObjFace { C = corners, Mtl = mtl });
                }
            }
            return f;
        }

        private static double[] Floats(string rest, int want, string kw)
        {
            string[] toks = rest.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
            if (toks.Length < want)
                throw new IlmException("'" + kw + "' line has " + toks.Length.ToString(Inv) + " values, expected at least " + want.ToString(Inv));
            var v = new double[want];
            for (int i = 0; i < want; i++)
            {
                // A comma decimal separator would silently corrupt every coordinate.
                if (!double.TryParse(toks[i], NumberStyles.Float, Inv, out v[i]))
                    throw new IlmException("'" + kw + "' line has a non-numeric value: " + toks[i]);
            }
            return v;
        }

        private static int Component(string[] parts, int i)
        {
            if (i >= parts.Length || parts[i].Length == 0) return 0;
            int v;
            if (!int.TryParse(parts[i], NumberStyles.Integer, Inv, out v))
                throw new IlmException("face index is not an integer: " + parts[i]);
            return v;
        }

        /// <summary>Line each ILM primitive up with the OBJ face that carries it.
        ///
        /// NOT positional: Blender's OBJ exporter groups a mesh's polygons by material so it can
        /// emit one `usemtl` per run, which reorders faces relative to the prim order we wrote.
        /// Relative order WITHIN a material is preserved by both writers, so consuming
        /// per-material queues in prim order re-pairs them exactly, and works unchanged on our
        /// own interleaved output.</summary>
        private static List<ObjFace> PairFaces(Model model, ObjObject obj, int[] baseClutY)
        {
            foreach (ObjFace fa in obj.Faces)
                if (fa.Mtl == null) return obj.Faces;   // no material info at all: fall back to file order

            var buckets = new Dictionary<string, List<ObjFace>>(StringComparer.Ordinal);
            var pos = new Dictionary<string, int>(StringComparer.Ordinal);
            foreach (ObjFace fa in obj.Faces)
            {
                List<ObjFace> q;
                if (!buckets.TryGetValue(fa.Mtl, out q)) { q = new List<ObjFace>(); buckets[fa.Mtl] = q; pos[fa.Mtl] = 0; }
                q.Add(fa);
            }

            var outp = new List<ObjFace>();
            foreach (Mesh mesh in model.Meshes)
                foreach (Prim p in mesh.Prims)
                {
                    string name = MtlName(p, baseClutY);
                    List<ObjFace> q;
                    if (!buckets.TryGetValue(name, out q) || pos[name] >= q.Count)
                        throw new IlmException("part '" + obj.Name + "': the OBJ has no unassigned face left with " +
                            "material '" + name + "'. Material names encode the CLUT palette row and are how faces " +
                            "are matched back to primitives - reassigning or renaming materials is not supported.", true);
                    outp.Add(q[pos[name]]);
                    pos[name] = pos[name] + 1;
                }
            return outp;
        }

        // ---- import -------------------------------------------------------------

        /// <summary>OBJ [0,1] -> PSX u8 texel. floor(), never round(): round() banker-rounds
        /// (u+0.5) up for odd u and drifts the texel by one every other coordinate, which
        /// accumulates into a visible texture shift across round-trips. Clamped to 255, the PSX
        /// page limit - NOT to the texture width, since a 128-wide TIM sits inside a 256-wide
        /// page and real UVs do reach 255.</summary>
        private static int UvByte(double f)
        {
            double s = f * 256.0;
            // Python's int(f * 256.0) raises on NaN/inf rather than writing a texel, and the
            // clamps below cannot stand in: every comparison against NaN is false, so NaN
            // would silently land on texel 0. Testing the product also catches a finite f
            // whose scaling overflows, which is exactly where Python raises OverflowError.
            if (double.IsNaN(s) || double.IsInfinity(s))
                throw new IlmException("UV coordinate is not a finite number: " + f.ToString("R", Inv));
            if (!(s > 0.0)) return 0;
            if (s >= 255.0) return 255;
            return (int)s;
        }

        /// <summary>math.floor(v + 0.5) — half-UP, matching the Python's _s16 and _write_normal.
        /// (int)(v + 0.5) is NOT a substitute: a C# cast truncates toward zero, so -2.3 would
        /// land on -1 where floor gives -2, and negative coordinates are the common case.</summary>
        private static double RoundHalfUp(double v) { return Math.Floor(v + 0.5); }

        private static short S16Checked(double v, string what)
        {
            double r = RoundHalfUp(v);
            if (double.IsNaN(r) || r < -32768.0 || r > 32767.0)
            {
                string shown = (r >= -9.0e18 && r <= 9.0e18) ? ((long)r).ToString(Inv) : r.ToString("R", Inv);
                throw new IlmException(what + " out of s16 range: " + shown + " (models use roughly -1365..573)");
            }
            return (short)(int)r;
        }

        private static int ClampS8(double v)
        {
            double r = RoundHalfUp(v);
            // Python's int(math.floor(v)) raises on NaN/inf. NaN fails both clamps below and
            // would reach (int)r, whose result is undefined in C# — on x64 it is int.MinValue,
            // which truncates to a zero-length normal that previews fine and breaks lighting.
            if (double.IsNaN(r) || double.IsInfinity(r))
                throw new IlmException("normal component is not a finite number: " + v.ToString("R", Inv));
            if (r < -128.0) return -128;
            if (r > 127.0) return 127;
            return (int)r;
        }

        private static double[] Unit(double[] v)
        {
            double l = Math.Sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l < 1e-12) return null;
            return new[] { v[0] / l, v[1] / l, v[2] / l };
        }

        /// <summary>Did the artist actually touch this? w0 is model-space, wObj is OBJ-space.</summary>
        private static bool Moved(double[] wObj, double[] w0)
        {
            return Math.Max(Math.Max(Math.Abs(wObj[0] - w0[0]), Math.Abs(wObj[1] - YOut(w0[1]))),
                            Math.Abs(wObj[2] - w0[2])) > UnmovedEps;
        }

        /// <summary>Unbake one owned vertex back into the ILM - but only if it moved.
        ///
        /// An untouched vertex is left byte-identical rather than round-tripped through the
        /// inverse, so an unedited export/import is byte-exact BY CONSTRUCTION: the inverse is
        /// not on the path at all, and a later change to the keyframe, the float precision or
        /// the matrix code cannot silently break that gate.</summary>
        private static void WriteVertex(byte[] data, Ilm ilm, PoolRef r, double[] wObj, int[] R, int[] T, double[] inv)
        {
            Mesh mesh = ilm.Models[r.Model].Meshes[r.Mesh];
            int j = r.Local;
            if (!Moved(wObj, Bake(R, T, mesh.Vx[j], mesh.Vy[j], mesh.Vz[j]))) return;
            if (inv == null)
                throw new IlmException("part rest-pose matrix is singular; cannot unbake edited geometry");
            double[] v = Unbake(inv, T, new[] { wObj[0], YIn(wObj[1]), wObj[2] });
            W16(data, mesh.XyP + j * 4, S16Checked(v[0], "vertex X"));
            W16(data, mesh.XyP + j * 4 + 2, S16Checked(v[1], "vertex Y"));
            W16(data, mesh.ZP + j * 2, S16Checked(v[2], "vertex Z"));
        }

        /// <summary>Unbake one owned normal - DIRECTION only, keeping the original length.
        ///
        /// A normal's magnitude is not artist data: the ILM stores s8 components at a fixed scale
        /// (|n| ~= 127) and Blender's OBJ round-trip renormalises every one of them to unit
        /// length. Comparing and rewriting raw components would therefore rewrite all 416 of
        /// HERO's normals as (0, +-1, 0) on an untouched round-trip, so both the unmoved test and
        /// the write work on the direction and re-apply the stored length.</summary>
        private static void WriteNormal(byte[] data, Ilm ilm, PoolRef r, double[] wObj, int[] R, double[] inv)
        {
            Mesh mesh = ilm.Models[r.Model].Meshes[r.Mesh];
            int j = r.Local;
            int ox = mesh.Nx[j], oy = mesh.Ny[j], oz = mesh.Nz[j];
            double l = Math.Sqrt((double)ox * ox + (double)oy * oy + (double)oz * oz);
            double[] dObj = Unit(NormalIn(wObj));
            double[] dOrig = Unit(BakeDir(R, ox, oy, oz));
            if (dObj == null || dOrig == null || l < 1e-9) return;
            if (Math.Max(Math.Max(Math.Abs(dObj[0] - dOrig[0]), Math.Abs(dObj[1] - dOrig[1])),
                         Math.Abs(dObj[2] - dOrig[2])) <= NormalUnmovedEps) return;
            if (inv == null) return;   // silent, unlike WriteVertex: a normal is recoverable
            double[] n = Unit(UnbakeDir(inv, dObj));
            if (n == null) return;
            // Byte +3 is s_Normal::count and is NEVER written: writing it previews fine and
            // breaks lighting.
            for (int off = 0; off < 3; off++)
                data[mesh.NormP + j * 4 + off] = (byte)(sbyte)ClampS8(n[off] * l);
        }

        public static ImportResult Import(string objPath, string ilmPath, string outIlmPath)
        {
            return Import(objPath, ilmPath, outIlmPath, false);
        }

        /// <summary>Import with optional grow-mode. With grow: false the file must keep its exact
        /// topology, which is the only mode ModManagerForm has ever used. With grow: true extra
        /// vertices and faces appended inside a part's `o` block become NEW geometry and the whole
        /// ILM is rewritten; a grow import that finds no growth falls through to the plain path,
        /// so the flag alone never changes a single byte.</summary>
        public static ImportResult Import(string objPath, string ilmPath, string outIlmPath, bool grow)
        {
            return Import(objPath, ilmPath, outIlmPath, new ImportOptions { Grow = grow });
        }

        /// <summary>Import with the full option set. Grow and Replace are alternatives: grow keeps
        /// the original geometry and adds to it, replace rebuilds all of it from the OBJ.</summary>
        public static ImportResult Import(string objPath, string ilmPath, string outIlmPath, ImportOptions opts)
        {
            if (opts == null) opts = new ImportOptions();
            var res = new ImportResult();
            try
            {
                if ((opts.Grow ? 1 : 0) + (opts.Replace ? 1 : 0) + (opts.V7 ? 1 : 0) > 1)
                    throw new IlmException("grow, replace and high-poly (v7) are alternatives: grow keeps the original " +
                        "geometry and adds to it, replace rebuilds it in the u8 pool, v7 writes a wide high-poly file.");
                ImportCore(objPath, ilmPath, outIlmPath, opts, res);
            }
            catch (IlmException ex)
            {
                res.Error = ex.Message;
                res.ReplaceMayHelp = ex.ReplaceMayHelp;
            }
            catch (Exception ex) { res.Error = ex.Message; }
            return res;
        }

        public static bool Import(string objPath, string ilmPath, string outIlmPath, out string error)
        {
            ImportResult res = Import(objPath, ilmPath, outIlmPath, false);
            error = res.Error;
            return res.Error == null;
        }

        private static void ImportCore(string objPath, string ilmPath, string outIlmPath, ImportOptions opts, ImportResult res)
        {
            bool grow = opts.Grow;
            string metaPath = StripExt(objPath) + ".ilmmeta.json";
            if (!File.Exists(metaPath))
                throw new IlmException("missing " + Path.GetFileName(metaPath) + " - it is written next to the OBJ by " +
                    "`export` and is required (it carries the bytes whose meaning is unknown)");
            JNode meta = ParseJson(File.ReadAllText(metaPath), Path.GetFileName(metaPath));

            // Patch in place. A tight rewrite would drop the slack the original layout leaves
            // after each array, and func_8005A900/func_8005AA08 read vertices and normals THREE
            // at a time without a bounds check - a repacked file would hand them an out-of-bounds
            // read the original never had.
            byte[] data = File.ReadAllBytes(ilmPath);
            Ilm ilm = ParseIlm(data);
            ResolvePool(ilm);
            ObjFile of = ParseObj(objPath);
            if (string.IsNullOrEmpty(outIlmPath)) outIlmPath = StripExt(objPath) + "_new.ILM";

            if (of.Objs.Count != ilm.Models.Length)
                throw new IlmException("OBJ has " + of.Objs.Count.ToString(Inv) + " objects but the ILM has " +
                    ilm.Models.Length.ToString(Inv) + " parts. Parts are bones - adding or removing them breaks the rig.");

            // Match BY NAME, never by position: Blender's OBJ exporter sorts objects
            // alphabetically, so positional matching dies on the first divergence.
            var byName = new Dictionary<string, ObjObject>(StringComparer.Ordinal);
            foreach (ObjObject oo in of.Objs)
            {
                if (byName.ContainsKey(oo.Name))
                    throw new IlmException("OBJ has two objects called '" + oo.Name + "' - part names are the bone " +
                        "binding and must be unique.");
                byName[oo.Name] = oo;
            }
            var modelNames = new HashSet<string>(StringComparer.Ordinal);
            foreach (Model m in ilm.Models) modelNames.Add(m.Name);
            var missing = new List<string>();
            foreach (Model m in ilm.Models) if (!byName.ContainsKey(m.Name)) missing.Add(m.Name);
            var extra = new List<string>();
            foreach (ObjObject oo in of.Objs) if (!modelNames.Contains(oo.Name)) extra.Add(oo.Name);
            if (missing.Count > 0 || extra.Count > 0)
            {
                // Blender appends .001 when a name is already taken, so re-importing the OBJ to
                // compare versions renames every object at once. That is one cause with one fix,
                // not the 46-name wall the generic message prints.
                if (extra.Count > 0 && BlenderSuffixed(extra, missing))
                    throw new IlmException("every object carries a Blender '.001'-style suffix (Blender adds one when " +
                        "the name is already taken in the scene). Remove the suffixes, or re-import into an empty " +
                        "scene - part names are the bone binding.");
                throw new IlmException("part name mismatch. Missing from the OBJ: " + JoinOrNone(missing) +
                    ". Not in the ILM: " + JoinOrNone(extra) + ". The first two characters of the name ARE the bone " +
                    "index, so renaming silently re-binds the part to bone 0.");
            }

            JNode metaModels = JGet(meta, "models");
            int metaCount = (metaModels != null && metaModels.Kind == 'a') ? metaModels.Items.Count : 0;
            if (metaCount != ilm.Models.Length)
                throw new IlmException("the .ilmmeta.json describes " + metaCount.ToString(Inv) + " parts but the ILM " +
                    "has " + ilm.Models.Length.ToString(Inv) + " - it was written for a different model.");

            var poseR = new int[metaCount][];
            var poseT = new int[metaCount][];
            var invs = new double[metaCount][];
            for (int i = 0; i < metaCount; i++)
            {
                JNode rp = JGet(metaModels.Items[i], "restPose");
                // Python's `mm.get("restPose") or {...}`: missing, null and {} all fall back.
                if (rp == null || rp.Kind != 'o' || rp.Keys.Count == 0)
                {
                    poseR[i] = (int[])IdentityR.Clone();
                    poseT[i] = new int[3];
                }
                else
                {
                    poseR[i] = JIntsOf(JGet(rp, "R"), 9, "models[" + i.ToString(Inv) + "].restPose.R");
                    poseT[i] = JIntsOf(JGet(rp, "T"), 3, "models[" + i.ToString(Inv) + "].restPose.T");
                }
                invs[i] = MatInverse(poseR[i]);
            }

            JNode restPoseKind = JGet(meta, "restPose");
            res.RestPoseIdentity = restPoseKind != null && restPoseKind.Kind == 's' &&
                                   string.Equals(restPoseKind.Text, "identity", StringComparison.Ordinal);

            if (opts.V7)
            {
                V7Import(objPath, ilmPath, outIlmPath, data, ilm, of, byName, poseR, poseT, invs, res);
                return;
            }

            if (opts.Replace)
            {
                ReplaceImport(objPath, ilmPath, outIlmPath, data, ilm, of, byName, poseR, poseT, invs,
                    opts.Weld, opts.WeldEps, res);
                return;
            }

            // Where each owned vertex lives in the OBJ, so a seam duplicate can be compared
            // against its owner's EDITED position rather than the owner's ORIGINAL. Any global
            // transform moves duplicate and owner identically, so comparing against the original
            // reported every seam as lost on something as routine as a uniform scale - training
            // the artist to ignore the warnings.
            var ownerV = new Dictionary<int, int>();
            foreach (Model m in ilm.Models)
            {
                List<PoolRef> ov, fv, on, fn;
                PartLayout(m, out ov, out fv, out on, out fn);
                ObjObject oo = byName[m.Name];
                for (int i = 0; i < ov.Count && i < oo.V.Count; i++) ownerV[RefKey(ov[i])] = oo.V[i];
            }

            if (grow)
            {
                bool need = false;
                foreach (Model m in ilm.Models)
                {
                    ObjObject oo = byName[m.Name];
                    List<PoolRef> ov, fv, on, fn;
                    PartLayout(m, out ov, out fv, out on, out fn);
                    int wantP = 0;
                    foreach (Mesh me in m.Meshes) wantP += me.PrimCount;
                    if (oo.V.Count > ov.Count + fv.Count || oo.Faces.Count > wantP) { need = true; break; }
                }
                if (need)
                {
                    GrowImport(objPath, ilmPath, outIlmPath, data, ilm, of, byName, poseR, poseT, invs, ownerV, res);
                    return;
                }
                // Zero actual growth falls through to the patch-in-place path so that grow-mode
                // on an unedited OBJ stays byte-identical to a plain import.
            }

            int nprim = 0, nvert = 0, nnorm = 0;
            var seamWarn = new List<string[]>();
            var seamNWarn = new List<string[]>();

            foreach (Model model in ilm.Models)
            {
                ObjObject obj = byName[model.Name];
                // Normal counts are deliberately NOT checked: Blender deduplicates `vn` lines, so
                // the count legitimately changes. Normals come back through the face corners.
                List<PoolRef> ownV, forV, ownN, forN;
                PartLayout(model, out ownV, out forV, out ownN, out forN);
                // Vertex count is checked BEFORE the face count: a 'Merge by Distance' drops both,
                // and the merge-specific message is the useful one.
                if (obj.V.Count != ownV.Count + forV.Count)
                    throw new IlmException("part '" + obj.Name + "' has " + obj.V.Count.ToString(Inv) + " vertices but " +
                        "the ILM expects " + (ownV.Count + forV.Count).ToString(Inv) + " (" + ownV.Count.ToString(Inv) +
                        " own + " + forV.Count.ToString(Inv) + " duplicated seam vertices). The usual cause is an " +
                        "aggressive 'Merge by Distance' - the seam duplicates are deliberate and must not be welded.", true);
                int want = 0;
                foreach (Mesh m in model.Meshes) want += m.PrimCount;
                if (obj.Faces.Count != want)
                    throw new IlmException("part '" + obj.Name + "' has " + obj.Faces.Count.ToString(Inv) + " faces but " +
                        "the ILM expects " + want.ToString(Inv) + " primitives. Adding or removing faces is not " +
                        "supported (counts are u8 and the bone/animation data lives in another file).", true);

                List<ObjFace> paired = PairFaces(model, obj, ilm.BaseClutY);
                PatchOriginalSubset(data, ilm, of, model, obj, ownV, forV, paired, poseR, poseT, invs,
                    ownerV, seamWarn, seamNWarn, ref nvert, ref nnorm, ref nprim);
            }

            data[2] = 0; // isLoaded: a 1 here makes the runtime skip pointer fix-up and crash
            File.WriteAllBytes(outIlmPath, data);

            res.IlmPath = outIlmPath;
            res.Parts = ilm.Models.Length;
            res.Vertices = nvert;
            res.Normals = nnorm;
            res.Prims = nprim;
            if (res.RestPoseIdentity)
                res.Warnings.Add("rest pose: IDENTITY (this OBJ was exported without an ANM)");
            SeamReport(seamWarn, "seam vertex/vertices moved", res.Warnings);
            SeamReport(seamNWarn, "seam normal(s) edited", res.Warnings);
        }

        /// <summary>Verify and patch the ORIGINAL geometry of one part in place: arity and corner
        /// checks, vertex/normal unbake, UV rewrite, seam warnings. Grow-mode runs the identical
        /// pass over its own pairing before appending anything, so this lives in one place — the
        /// plain path's byte-exactness depends on every line of it.</summary>
        private static void PatchOriginalSubset(byte[] data, Ilm ilm, ObjFile of, Model model, ObjObject obj,
            List<PoolRef> ownV, List<PoolRef> forV, List<ObjFace> paired,
            int[][] poseR, int[][] poseT, double[][] invs, Dictionary<int, int> ownerV,
            List<string[]> seamWarn, List<string[]> seamNWarn, ref int nvert, ref int nnorm, ref int nprim)
        {
            {
                var gidx = new Dictionary<int, int>();
                for (int i = 0; i < ownV.Count; i++) gidx[RefKey(ownV[i])] = obj.V[i];
                for (int i = 0; i < forV.Count; i++) gidx[RefKey(forV[i])] = obj.V[ownV.Count + i];

                // The OBJ's face->vertex indices are the AUTHORITATIVE mapping from a primitive
                // corner to a `v` line; inferring vertex identity positionally instead is what any
                // count-preserving reorder (Blender's Mesh > Sort Elements) silently scrambles.
                // Checking the two agree also catches a face reordered or reassigned within one
                // material, which PairFaces cannot see when the arity matches.
                int fi = 0;
                foreach (Mesh mesh in model.Meshes)
                    foreach (Prim p in mesh.Prims)
                    {
                        int[][] face = paired[fi].C; fi++;
                        int n = p.Tri ? 3 : 4;
                        if (face.Length != n)
                            throw new IlmException("part '" + obj.Name + "' face " + fi.ToString(Inv) + " has " +
                                face.Length.ToString(Inv) + " corners but the primitive is a " + (p.Tri ? "triangle" : "quad") +
                                ". Triangulating or merging faces is not supported.", true);
                        int[] loop = CornerOrder(p);
                        for (int c = 0; c < n; c++)
                        {
                            PoolRef vr = p.VRef[loop[c]];
                            int wantV;
                            if (!vr.Valid || !gidx.TryGetValue(RefKey(vr), out wantV)) continue;
                            if (face[c][0] != wantV)
                                throw new IlmException("part '" + obj.Name + "' face " + fi.ToString(Inv) + " corner " +
                                    c.ToString(Inv) + " references vertex " + face[c][0].ToString(Inv) + " but the " +
                                    "primitive needs " + wantV.ToString(Inv) + ". Vertices and faces must keep the " +
                                    "order they were exported in - do not use Mesh > Sort Elements, and do not move a " +
                                    "face to a different material.", true);
                        }
                    }

                // Normals are looked up through the FACE CORNERS, not by position: Blender's OBJ
                // exporter deduplicates `vn` lines, so the block's normal order is not preserved
                // the way the vertex order is. First occurrence wins, insertion order is kept.
                var nkeys = new List<PoolRef>();
                var nvals = new List<double[]>();
                var nseen = new HashSet<int>();
                fi = 0;
                foreach (Mesh mesh in model.Meshes)
                    foreach (Prim p in mesh.Prims)
                    {
                        int[][] face = paired[fi].C; fi++;
                        int[] loop = CornerOrder(p);
                        for (int c = 0; c < loop.Length; c++)
                        {
                            PoolRef nr = p.NRef[loop[c]];
                            int t = face[c][2];
                            if (!nr.Valid || t == 0) continue;
                            if (t < 0 || t > of.Norms.Count)
                                throw new IlmException("part '" + obj.Name + "' face " + fi.ToString(Inv) + " references " +
                                    "vn " + t.ToString(Inv) + " but the OBJ has only " + of.Norms.Count.ToString(Inv) +
                                    " 'vn' lines.");
                            if (nseen.Add(RefKey(nr))) { nkeys.Add(nr); nvals.Add(of.Norms[t - 1]); }
                        }
                    }

                for (int i = 0; i < ownV.Count; i++)
                {
                    PoolRef r = ownV[i];
                    WriteVertex(data, ilm, r, of.Verts[obj.V[i] - 1], poseR[r.Model], poseT[r.Model], invs[r.Model]);
                    nvert++;
                }

                // Seam duplicates are READ-ONLY: they have no storage of their own, the position
                // belongs to the part that owns the pool slot. Warn only when the duplicate itself
                // was edited AND now disagrees with its owner. Both conditions are needed: a global
                // transform moves the duplicate but keeps it equal to the owner (nothing is lost),
                // and editing the OWNER alone leaves the duplicate stale but the edit is still
                // applied, so neither deserves an alarm.
                for (int i = 0; i < forV.Count; i++)
                {
                    PoolRef r = forV[i];
                    double[] w = of.Verts[obj.V[ownV.Count + i] - 1];
                    int oi;
                    if (!ownerV.TryGetValue(RefKey(r), out oi)) continue;
                    double[] ow = of.Verts[oi - 1];
                    double diff = Math.Max(Math.Max(Math.Abs(w[0] - ow[0]), Math.Abs(w[1] - ow[1])), Math.Abs(w[2] - ow[2]));
                    if (diff <= UnmovedEps) continue;
                    Mesh om = ilm.Models[r.Model].Meshes[r.Mesh];
                    if (Moved(w, Bake(poseR[r.Model], poseT[r.Model], om.Vx[r.Local], om.Vy[r.Local], om.Vz[r.Local])))
                        seamWarn.Add(new[] { model.Name, ilm.Models[r.Model].Name });
                }

                for (int i = 0; i < nkeys.Count; i++)
                {
                    PoolRef r = nkeys[i];
                    double[] w = nvals[i];
                    if (r.Model != model.Idx)
                    {
                        // A seam normal belongs to its owner's block and has no storage here. Say so
                        // instead of dropping the edit in silence.
                        Mesh om = ilm.Models[r.Model].Meshes[r.Mesh];
                        double[] dObj = Unit(w);
                        double[] dExp = Unit(BakeDir(poseR[r.Model], om.Nx[r.Local], om.Ny[r.Local], om.Nz[r.Local]));
                        if (dObj != null && dExp != null)
                        {
                            double[] e = NormalOut(dExp);
                            if (Math.Max(Math.Max(Math.Abs(dObj[0] - e[0]), Math.Abs(dObj[1] - e[1])),
                                         Math.Abs(dObj[2] - e[2])) > NormalUnmovedEps)
                                seamNWarn.Add(new[] { model.Name, ilm.Models[r.Model].Name });
                        }
                        continue;
                    }
                    WriteNormal(data, ilm, r, w, poseR[r.Model], invs[r.Model]);
                    nnorm++;
                }

                fi = 0;
                foreach (Mesh mesh in model.Meshes)
                    foreach (Prim p in mesh.Prims)
                    {
                        int[][] face = paired[fi].C; fi++;
                        int[] loop = CornerOrder(p);
                        for (int c = 0; c < loop.Length; c++)
                        {
                            // Only the UV word is written. clut, flags, and the vertex/normal index
                            // bytes are POOL SLOTS in a different numbering space from the OBJ's
                            // v/vn numbers, so they survive from the template. A triangle's UV3 word
                            // at prim+0xA is never reached and keeps its garbage value, which is what
                            // makes an unedited round-trip byte-exact.
                            int t = face[c][1];
                            if (t == 0) continue;
                            if (t < 0)
                                throw new IlmException("relative/negative OBJ indices are not supported - re-export with absolute indices.");
                            if (t > of.Uvs.Count)
                                throw new IlmException("part '" + obj.Name + "' face " + fi.ToString(Inv) + " references vt " +
                                    t.ToString(Inv) + " but the OBJ has only " + of.Uvs.Count.ToString(Inv) + " 'vt' lines.");
                            double[] uv = of.Uvs[t - 1];
                            // UvOffsets is indexed by the PRIM corner, not the OBJ corner: the loop
                            // reorders 2 and 3, and writing through c would swap those two UVs on
                            // every quad, every round-trip.
                            W16(data, p.Off + UvOffsets[loop[c]], UvByte(uv[0]) | (UvByte(1.0 - uv[1]) << 8));
                        }
                        nprim++;
                    }
            }
        }

        /// <summary>One line per reading part, owners listed once: the Python collapses the raw
        /// (reader, owner) pairs through sorted(set(...)) before printing.</summary>
        private static void SeamReport(List<string[]> pairs, string what, List<string> into)
        {
            var readers = new List<string>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (string[] pr in pairs) if (seen.Add(pr[0])) readers.Add(pr[0]);
            readers.Sort(StringComparer.Ordinal);
            foreach (string reader in readers)
            {
                int count = 0;
                var owners = new List<string>();
                var oseen = new HashSet<string>(StringComparer.Ordinal);
                foreach (string[] pr in pairs)
                    if (string.Equals(pr[0], reader, StringComparison.Ordinal))
                    {
                        count++;
                        if (oseen.Add(pr[1])) owners.Add(pr[1]);
                    }
                owners.Sort(StringComparer.Ordinal);
                into.Add("part " + reader + ": " + count.ToString(Inv) + " " + what + " and were IGNORED (owned by " +
                    string.Join(", ", owners.ToArray()) + " - edit them there)");
            }
        }

        private static string JoinOrNone(List<string> v)
        {
            return v.Count == 0 ? "none" : string.Join(", ", v.ToArray());
        }

        /// <summary>Every extra name is an existing part name plus an all-digit '.NNN' suffix, and
        /// stripping those suffixes accounts for exactly the missing set.</summary>
        private static bool BlenderSuffixed(List<string> extra, List<string> missing)
        {
            var stems = new List<string>();
            foreach (string n in extra)
            {
                int dot = n.LastIndexOf('.');
                if (dot < 0) return false;
                string tail = n.Substring(dot + 1);
                if (tail.Length == 0) return false;
                foreach (char ch in tail) if (ch < '0' || ch > '9') return false;
                stems.Add(n.Substring(0, dot));
            }
            if (stems.Count != missing.Count) return false;
            stems.Sort(StringComparer.Ordinal);
            var want = new List<string>(missing);
            want.Sort(StringComparer.Ordinal);
            for (int i = 0; i < stems.Count; i++)
                if (!string.Equals(stems[i], want[i], StringComparison.Ordinal)) return false;
            return true;
        }

        // ---- validator twin (pc_port/src/lm_validate.c) --------------------------

        private const int LmPoolSlots = 256;
        private const int LmVertSlotCap = 255;
        private const int LmArraySlack = 8;
        private const int LmShadeSlack = 3;
        private const int LmMaxParts = 56;

        private static readonly string[] LmArrayNames = { "primitives", "verticesXy", "verticesZ", "normals", "shade" };

        /// <summary>The five per-mesh arrays as (offset, count, elementSize, requiredSlack).</summary>
        private static void MeshArrays(byte[] d, int mb, int[] offs, int[] cnts, int[] esz, int[] slack)
        {
            offs[0] = U32(d, mb + 4);  cnts[0] = d[mb];     esz[0] = 20; slack[0] = 0;
            offs[1] = U32(d, mb + 8);  cnts[1] = d[mb + 1]; esz[1] = 4;  slack[1] = LmArraySlack;
            offs[2] = U32(d, mb + 12); cnts[2] = d[mb + 1]; esz[2] = 2;  slack[2] = LmArraySlack;
            offs[3] = U32(d, mb + 16); cnts[3] = d[mb + 2]; esz[3] = 4;  slack[3] = LmArraySlack;
            offs[4] = U32(d, mb + 20); cnts[4] = d[mb + 3]; esz[4] = 1;  slack[4] = LmShadeSlack;
        }

        private static bool ExtentOk(int n, int off, int cnt, int es)
        {
            return off >= 0 && off <= n && (long)cnt * es <= (long)n - off;
        }

        /// <summary>C# twin of Lm_Validate (pc_port/src/lm_validate.c) and of the Python
        /// lm_validate. The Stage 1 rule table is normative for all three - a drift is a bug in
        /// whichever diverges. Grow-mode runs it at tailSlack 0: emitted files carry real in-file
        /// pad bytes, so they stay safe even for a loader with no malloc tail. Returns 0 on pass,
        /// else the rule id with its message; first failure wins in numeric rule order (every V5
        /// site is reported before any V6 site, so each rule is its own full pass).</summary>
        private static int LmValidate(byte[] d, int tailSlack, int anmBoneCount, bool skeletal, out string msg)
        {
            int n = d.Length;
            var offs = new int[5]; var cnts = new int[5]; var esz = new int[5]; var slack = new int[5];

            if (n < 20 || d[0] != 0x30 || d[1] != 6 || d[2] != 0)
            {
                msg = "V1: not a raw LM file (magic=0x" + (n > 0 ? d[0] : 0).ToString("X2", Inv) +
                      " ver=" + (n > 1 ? d[1] : 0).ToString(Inv) + " isLoaded=" + (n > 2 ? d[2] : 0).ToString(Inv) +
                      " size=" + n.ToString(Inv) + ")";
                return 1;
            }
            int matCount = d[3], matOff = U32(d, 4);
            int modelCount = d[8], hdrsOff = U32(d, 12), orderOff = U32(d, 16);

            if (!ExtentOk(n, matOff, matCount, 24))
            { msg = V2Msg("materials section out of file", matOff, matCount * 24, n); return 2; }
            if (!ExtentOk(n, hdrsOff, modelCount, 16))
            { msg = V2Msg("modelHdrs section out of file", hdrsOff, modelCount * 16, n); return 2; }
            if (!ExtentOk(n, orderOff, modelCount, 1))
            { msg = V2Msg("modelOrder section out of file", orderOff, modelCount, n); return 2; }

            for (int i = 0; i < modelCount; i++)
            {
                int mh = hdrsOff + i * 16;
                int meshCount = d[mh + 8], meshOff = U32(d, mh + 12);
                if (!ExtentOk(n, meshOff, meshCount, 24))
                { msg = V2Msg("part " + Name8(d, mh) + " meshHdrs section out of file", meshOff, meshCount * 24, n); return 2; }
                for (int j = 0; j < meshCount; j++)
                {
                    MeshArrays(d, meshOff + j * 24, offs, cnts, esz, slack);
                    for (int a = 0; a < 5; a++)
                    {
                        if (cnts[a] == 0) continue;
                        if (!ExtentOk(n, offs[a], cnts[a], esz[a]))
                        {
                            msg = V2Msg("part " + Name8(d, mh) + " mesh " + j.ToString(Inv) + " " + LmArrayNames[a] +
                                        " section out of file", offs[a], cnts[a] * esz[a], n);
                            return 2;
                        }
                        if ((offs[a] & 3) != 0)
                        {
                            msg = "V2: part " + Name8(d, mh) + " mesh " + j.ToString(Inv) + " " + LmArrayNames[a] +
                                  " section misaligned (off=0x" + offs[a].ToString("X", Inv) + ")";
                            return 2;
                        }
                    }
                }
            }

            if (skeletal && modelCount > LmMaxParts)
            { msg = "V3: " + modelCount.ToString(Inv) + " parts exceeds skeleton cap " + LmMaxParts.ToString(Inv); return 3; }

            for (int i = 0; i < modelCount; i++)
                if (d[orderOff + i] >= modelCount)
                {
                    msg = "V4: modelOrder[" + i.ToString(Inv) + "]=" + ((int)d[orderOff + i]).ToString(Inv) +
                          " out of range (" + modelCount.ToString(Inv) + " parts)";
                    return 4;
                }

            int vertCap = skeletal ? LmVertSlotCap : LmPoolSlots;
            for (int i = 0; i < modelCount; i++)
            {
                int mh = hdrsOff + i * 16;
                for (int j = 0; j < d[mh + 8]; j++)
                {
                    int vc = d[U32(d, mh + 12) + j * 24 + 1];
                    if (d[mh + 9] + vc > vertCap)
                    {
                        msg = "V5: part " + Name8(d, mh) + " mesh " + j.ToString(Inv) + " vertex window " +
                              ((int)d[mh + 9]).ToString(Inv) + "+" + vc.ToString(Inv) + " exceeds " + vertCap.ToString(Inv);
                        return 5;
                    }
                }
            }
            for (int i = 0; i < modelCount; i++)
            {
                int mh = hdrsOff + i * 16;
                for (int j = 0; j < d[mh + 8]; j++)
                {
                    int nc = d[U32(d, mh + 12) + j * 24 + 2];
                    if (d[mh + 10] + nc > LmPoolSlots)
                    {
                        msg = "V6: part " + Name8(d, mh) + " mesh " + j.ToString(Inv) + " normal window " +
                              ((int)d[mh + 10]).ToString(Inv) + "+" + nc.ToString(Inv) + " exceeds " + LmPoolSlots.ToString(Inv);
                        return 6;
                    }
                }
            }
            for (int i = 0; i < modelCount; i++)
            {
                int mh = hdrsOff + i * 16;
                for (int j = 0; j < d[mh + 8]; j++)
                {
                    int sc = d[U32(d, mh + 12) + j * 24 + 3];
                    if (d[mh + 10] + sc > LmPoolSlots)
                    {
                        msg = "V7: part " + Name8(d, mh) + " mesh " + j.ToString(Inv) + " shade window " +
                              ((int)d[mh + 10]).ToString(Inv) + "+" + sc.ToString(Inv) + " exceeds " + LmPoolSlots.ToString(Inv);
                        return 7;
                    }
                }
            }
            for (int i = 0; i < modelCount; i++)
            {
                int mh = hdrsOff + i * 16;
                for (int j = 0; j < d[mh + 8]; j++)
                {
                    int mb = U32(d, mh + 12) + j * 24;
                    // env mode 1 advances a slot even for a count-byte 0 (see the C twin in
                    // lm_validate.c): per-normal advance is max(count, 1).
                    int walk = 0;
                    for (int k = 0; k < d[mb + 2]; k++)
                    {
                        int c = d[U32(d, mb + 16) + k * 4 + 3];
                        walk += c != 0 ? c : 1;
                    }
                    if (d[mh + 10] + walk > LmPoolSlots)
                    {
                        msg = "V8: part " + Name8(d, mh) + " mesh " + j.ToString(Inv) + " shade walk " +
                              ((int)d[mh + 10]).ToString(Inv) + "+" + walk.ToString(Inv) + " exceeds " + LmPoolSlots.ToString(Inv);
                        return 8;
                    }
                }
            }

            for (int i = 0; i < modelCount; i++)
            {
                int mh = hdrsOff + i * 16;
                for (int j = 0; j < d[mh + 8]; j++)
                {
                    MeshArrays(d, U32(d, mh + 12) + j * 24, offs, cnts, esz, slack);
                    for (int a = 0; a < 5; a++)
                    {
                        if (cnts[a] == 0 || slack[a] == 0) continue;
                        long have = (long)n + tailSlack - ((long)offs[a] + (long)cnts[a] * esz[a]);
                        if (have < slack[a])
                        {
                            msg = "V9: part " + Name8(d, mh) + " mesh " + j.ToString(Inv) + " " + LmArrayNames[a] +
                                  " array needs " + slack[a].ToString(Inv) + " slack bytes, has " + have.ToString(Inv);
                            return 9;
                        }
                    }
                }
            }

            // V10 is structural (see lm_validate.h): V5-strict + V11 make a slot-255 quad corner
            // impossible, and no file scan can detect one.

            if (skeletal)
            {
                var vstamp = new bool[LmPoolSlots];
                var nstamp = new bool[LmPoolSlots];
                for (int i = 0; i < modelCount; i++)
                {
                    int mh = hdrsOff + d[orderOff + i] * 16;
                    int vertOff = d[mh + 9], normOff = d[mh + 10], meshOff = U32(d, mh + 12);
                    for (int j = 0; j < d[mh + 8]; j++)
                    {
                        int mb = meshOff + j * 24;
                        for (int s = vertOff; s < vertOff + d[mb + 1] && s < LmPoolSlots; s++) vstamp[s] = true;
                        for (int s = normOff; s < normOff + d[mb + 2] && s < LmPoolSlots; s++) nstamp[s] = true;
                    }
                    int primIdx = 0;
                    for (int j = 0; j < d[mh + 8]; j++)
                    {
                        int mb = meshOff + j * 24;
                        int primsOff = U32(d, mb + 4);
                        for (int k = 0; k < d[mb]; k++)
                        {
                            int pb = primsOff + k * 20;
                            int corners = d[pb + 0xF] == 0xFF ? 3 : 4;
                            for (int c = 0; c < corners; c++)
                            {
                                if (!vstamp[d[pb + 0xC + c]])
                                {
                                    msg = "V11: part " + Name8(d, mh) + " prim " + primIdx.ToString(Inv) + " corner " +
                                          c.ToString(Inv) + " reads unwritten vertex slot " + ((int)d[pb + 0xC + c]).ToString(Inv);
                                    return 11;
                                }
                                if (!nstamp[d[pb + 0x10 + c]])
                                {
                                    msg = "V11: part " + Name8(d, mh) + " prim " + primIdx.ToString(Inv) + " corner " +
                                          c.ToString(Inv) + " reads unwritten normal slot " + ((int)d[pb + 0x10 + c]).ToString(Inv);
                                    return 11;
                                }
                            }
                            primIdx++;
                        }
                    }
                }
            }

            for (int i = 0; i < modelCount; i++)
            {
                int mh = hdrsOff + i * 16;
                int b4 = (d[mh + 11] >> 4) & 3;
                if (b4 != 0 && (d[mh + 9] != 0 || d[mh + 10] != 0))
                {
                    msg = "V12: part " + Name8(d, mh) + " field_B_4=" + b4.ToString(Inv) +
                          " requires vertexOffset==0 && normalOffset==0 (got " + ((int)d[mh + 9]).ToString(Inv) +
                          "/" + ((int)d[mh + 10]).ToString(Inv) + ")";
                    return 12;
                }
            }

            if (anmBoneCount > 0)
                for (int i = 0; i < modelCount; i++)
                {
                    int mh = hdrsOff + i * 16;
                    if (d[mh] >= 0x30 && d[mh] <= 0x39 && d[mh + 1] >= 0x30 && d[mh + 1] <= 0x39)
                    {
                        int bone = (d[mh] - 0x30) * 10 + (d[mh + 1] - 0x30);
                        if (bone >= anmBoneCount)
                        {
                            msg = "V13: part " + Name8(d, mh) + " binds bone " + bone.ToString(Inv) + " but the ANM has " +
                                  anmBoneCount.ToString(Inv) + " bones";
                            return 13;
                        }
                    }
                }

            msg = null;
            return 0;
        }

        private static string V2Msg(string what, int off, int len, int size)
        {
            return "V2: " + what + " (off=0x" + off.ToString("X", Inv) + " len=0x" + len.ToString("X", Inv) +
                   " size=0x" + size.ToString("X", Inv) + ")";
        }

        // ---- grow-mode ----------------------------------------------------------

        private static int Align4(int nbytes) { return (4 - nbytes % 4) % 4; }

        /// <summary>&gt;= 3 zero bytes after the shade stream, landing the next section 4-aligned
        /// (F1: func_800574D4's copy strides 4 bytes past the stream). A count of 0 yields 4, not
        /// 0 — that is the Python's behaviour and the emitted layout depends on it.</summary>
        private static int ShadePad(int cnt)
        {
            int pad = Align4(cnt);
            return pad < 3 ? pad + 4 : pad;
        }

        private struct ShadeEdge { public int Mesh, ByteIdx, Slot; public PoolRef Ref; }

        /// <summary>Shade-stream bytes are vertex POOL SLOT references: replay the pool and resolve
        /// each byte to its writer, exactly like prim corners. Needed even though growing a shaded
        /// part is refused — an unshaded grown part can relocate a writer whose slot a shaded
        /// ungrown part's stream reads. Indexed by model index; each list is in mesh/byte order and
        /// the models are visited in DRAW order, so callers must walk `order`, never a Dictionary.
        ///
        /// Note the stamping here is NOT interleaved with the reads the way ResolvePool's is: the
        /// whole part's vertex windows are stamped first, then its shade bytes resolved.</summary>
        private static List<ShadeEdge>[] ShadeEdges(Ilm ilm, int[] order)
        {
            var outp = new List<ShadeEdge>[ilm.ModelCount];
            var vpool = new Dictionary<int, PoolRef>();
            foreach (int mi in order)
            {
                Model m = ilm.Models[mi];
                for (int k = 0; k < m.MeshCount; k++)
                {
                    Mesh me = m.Meshes[k];
                    for (int j = 0; j < me.VertexCount; j++)
                        vpool[m.VertexOffset + j] = new PoolRef { Valid = true, Model = mi, Mesh = k, Local = j };
                }
                var lst = new List<ShadeEdge>();
                for (int k = 0; k < m.MeshCount; k++)
                {
                    Mesh me = m.Meshes[k];
                    for (int b = 0; b < me.UnkCount3; b++)
                    {
                        if (me.UnkP < 0 || me.UnkP + b >= ilm.D.Length)
                            throw new IlmException("part '" + m.Name + "' shade stream runs past the end of the file");
                        int slot = ilm.D[me.UnkP + b];
                        PoolRef r;
                        vpool.TryGetValue(slot, out r);
                        lst.Add(new ShadeEdge { Mesh = k, ByteIdx = b, Slot = slot, Ref = r });
                    }
                }
                if (lst.Count > 0) outp[mi] = lst;
            }
            return outp;
        }

        /// <summary>PairFaces, but the per-material queues may hold MORE faces than the part has
        /// primitives: the unconsumed remainder (in file order) is the new geometry. Relative order
        /// within a material is preserved by both writers, so the first count(prims-with-M) faces
        /// of queue M are the originals.
        ///
        /// The Python marks consumed faces by id() and filters; C# has no safe equivalent (ObjFace
        /// is reference-typed and List.Contains would be an O(n^2) reference scan), so the leftover
        /// set is reconstructed from the per-material consumed counts. Bucket order IS file order,
        /// so this yields exactly the same faces in exactly the same order.</summary>
        private static List<ObjFace> PairFacesGrow(Model model, ObjObject obj, int[] baseClutY, out List<ObjFace> leftover)
        {
            int primCount = 0;
            foreach (Mesh me in model.Meshes) primCount += me.PrimCount;

            foreach (ObjFace fa in obj.Faces)
                if (fa.Mtl == null)
                {
                    if (obj.Faces.Count > primCount)
                        throw new IlmException("part '" + obj.Name + "' has faces without a usemtl - grow-mode needs " +
                            "material info to tell new faces from original ones.", true);
                    leftover = new List<ObjFace>();
                    return obj.Faces;
                }

            var buckets = new Dictionary<string, List<ObjFace>>(StringComparer.Ordinal);
            var pos = new Dictionary<string, int>(StringComparer.Ordinal);
            foreach (ObjFace fa in obj.Faces)
            {
                List<ObjFace> q;
                if (!buckets.TryGetValue(fa.Mtl, out q)) { q = new List<ObjFace>(); buckets[fa.Mtl] = q; pos[fa.Mtl] = 0; }
                q.Add(fa);
            }

            var outp = new List<ObjFace>();
            foreach (Mesh mesh in model.Meshes)
                foreach (Prim p in mesh.Prims)
                {
                    string name = MtlName(p, baseClutY);
                    List<ObjFace> q;
                    if (!buckets.TryGetValue(name, out q) || pos[name] >= q.Count)
                        throw new IlmException("part '" + obj.Name + "': the OBJ has no unassigned face left with " +
                            "material '" + name + "'. Material names encode the CLUT palette row and are how faces " +
                            "are matched back to primitives - reassigning or renaming materials is not supported.", true);
                    outp.Add(q[pos[name]]);
                    pos[name] = pos[name] + 1;
                }

            leftover = new List<ObjFace>();
            var seen = new Dictionary<string, int>(StringComparer.Ordinal);
            foreach (ObjFace fa in obj.Faces)
            {
                int s;
                seen.TryGetValue(fa.Mtl, out s);
                seen[fa.Mtl] = s + 1;
                if (s >= pos[fa.Mtl]) leftover.Add(fa);
            }
            return outp;
        }

        /// <summary>OBJ-space direction -> part-local s8 triple at the stock |n|=127 scale.</summary>
        private static int[] QuantNewNormal(double[] inv, double[] w)
        {
            double[] d = Unit(NormalIn(w));
            if (d == null) return null;
            double[] nl = Unit(UnbakeDir(inv, d));
            if (nl == null) return null;
            return new[] { ClampS8(nl[0] * 127.0), ClampS8(nl[1] * 127.0), ClampS8(nl[2] * 127.0) };
        }

        private class NewPrim
        {
            public bool Tri;
            public Prim Tmpl;
            public int[] VKind;    // 0 own, 1 foreign, 2 new
            public int[] VA, VB;   // own: local | foreign: (ownerModel, ownerLocal) | new: newIdx
            public int[] NIdx;     // index into this part's new-normal list
            public int[] Uu, Vv;
        }

        private static int VcNew(Ilm ilm, List<int[]>[] newVerts, int mi)
        {
            return ilm.Models[mi].Meshes[0].VertexCount + (newVerts[mi] == null ? 0 : newVerts[mi].Count);
        }

        private static int NcNew(Ilm ilm, List<int[]>[] newNormals, int mi)
        {
            return ilm.Models[mi].Meshes[0].NormalCount + (newNormals[mi] == null ? 0 : newNormals[mi].Count);
        }

        private static int CountOf(List<int[]>[] a, int mi) { return a[mi] == null ? 0 : a[mi].Count; }

        private static string BudgetCol(int total, int delta)
        {
            return delta != 0
                ? total.ToString(Inv) + " (" + (total - delta).ToString(Inv) + "+" + delta.ToString(Inv) + ")"
                : total.ToString(Inv);
        }

        /// <summary>The per-part slot budget: how many vertex/normal/prim slots each part now uses,
        /// where its pool window lands, and how much headroom is left against the u8 ceilings. This
        /// is the sheet a user needs to know how much more they can add, so it is emitted on the
        /// ceiling refusals too, not only on success.</summary>
        private static void GrowBudget(Ilm ilm, HashSet<int> grown, int[] vO2, int[] nO2, int[][] finals,
                                       int oldSize, int newSize, List<string> into)
        {
            into.Add("   part              bone  verts        norms        prims        shade  window");
            int extV = 0, extN = 0;
            foreach (Model m in ilm.Models)
            {
                int mi = m.Idx;
                int[] f = finals[mi];
                if (vO2[mi] + f[0] > extV) extV = vO2[mi] + f[0];
                if (nO2[mi] + f[1] > extN) extN = nO2[mi] + f[1];
                into.Add("   " + m.Name.PadRight(8) + (grown.Contains(mi) ? " (grown)" : "").PadRight(9) +
                         " " + BoneOf(m.Name).ToString(Inv).PadLeft(3) + "  " +
                         BudgetCol(f[0], f[4]).PadRight(12) + " " +
                         BudgetCol(f[1], f[5]).PadRight(12) + " " +
                         BudgetCol(f[2], f[6]).PadRight(12) + " " +
                         f[3].ToString(Inv).PadLeft(5) + "  " +
                         "v[" + vO2[mi].ToString(Inv) + "," + (vO2[mi] + f[0]).ToString(Inv) + ") n[" +
                         nO2[mi].ToString(Inv) + "," + (nO2[mi] + f[1]).ToString(Inv) + ")");
            }
            into.Add("   vertex slots: extent " + extV.ToString(Inv) + " / 255 (" + (255 - extV).ToString(Inv) +
                     " free)   normal slots: extent " + extN.ToString(Inv) + " / 256");
            into.Add("   parts: " + ilm.ModelCount.ToString(Inv) + " / 56    file: 0x" + newSize.ToString("X", Inv) +
                     " (was 0x" + oldSize.ToString("X", Inv) + ")");
        }

        private static byte SlotByte(int v)
        {
            if (v < 0 || v > 255)
                throw new IlmException("grow internal error: pool slot " + v.ToString(Inv) + " out of u8 range");
            return (byte)v;
        }

        private static void PutU32(List<byte> o, int at, int v)
        {
            o[at] = (byte)v; o[at + 1] = (byte)(v >> 8); o[at + 2] = (byte)(v >> 16); o[at + 3] = (byte)(v >> 24);
        }

        private static void AddRange(List<byte> o, byte[] src, int off, int len, string what)
        {
            // Python's data[a:b] clamps at EOF; a C# copy throws an opaque exception instead, and a
            // hand-edited source ILM really can point a section past the end.
            if (off < 0 || len < 0 || (long)off + len > src.Length)
                throw new IlmException("grow: the " + what + " section runs past the end of the source ILM");
            for (int i = 0; i < len; i++) o.Add(src[off + i]);
        }

        private static void AddZeros(List<byte> o, int n) { for (int i = 0; i < n; i++) o.Add(0); }

        private static double[] VertAt(ObjFile of, int vl)
        {
            if (vl < 1 || vl > of.Verts.Count)
                throw new IlmException("a new face cites vertex " + vl.ToString(Inv) + " which the OBJ does not define");
            return of.Verts[vl - 1];
        }

        /// <summary>Full-rewrite import: original geometry keeps its bytes (modulo edits and
        /// renumbered pool slots), new geometry is appended, windows are re-laid-out.
        ///
        /// Preservation argument for the re-layout: an original sharing edge (writer W, local j,
        /// reader R) needs slot vO'_W+j untouched by every part drawn strictly between W and R.
        /// (a) W grown -> its slots are fresh and exclusive, nobody else ever writes them.
        /// (b) W ungrown -> the slot is the original vO_W+j; the original file guarantees no
        /// in-between part covered it, and relocated grown parts only move AWAY (a removed
        /// intermediate writer cannot break a last-writer relationship). (c) A part's own reads are
        /// always safe (it writes immediately before it reads). Pinned offset-0 parts (field_B_4,
        /// V12) EXPAND instead of moving, which case (b) does not cover — hence the explicit
        /// expansion-zone conflict check below.</summary>
        private static void GrowImport(string objPath, string ilmPath, string outIlmPath, byte[] data, Ilm ilm,
            ObjFile of, Dictionary<string, ObjObject> byName, int[][] poseR, int[][] poseT, double[][] invs,
            Dictionary<int, int> ownerV, ImportResult res)
        {
            int mc = ilm.ModelCount;
            if (ilm.ModelOrderP < 0 || (long)ilm.ModelOrderP + mc > ilm.D.Length)
                throw new IlmException("--grow: the modelOrder table runs past the end of the file.");
            var order = new int[mc];
            for (int i = 0; i < mc; i++) order[i] = ilm.D[ilm.ModelOrderP + i];
            var sortedOrder = (int[])order.Clone();
            Array.Sort(sortedOrder);
            for (int i = 0; i < mc; i++)
                if (sortedOrder[i] != i)
                    throw new IlmException("--grow: modelOrder is not a permutation, so the sharing-graph replay " +
                        "cannot be trusted (no stock file does this).");
            var dpos = new int[mc];
            for (int i = 0; i < mc; i++) dpos[order[i]] = i;
            List<ShadeEdge>[] shadeMap = ShadeEdges(ilm, order);

            // A dangling ref means either the traversal is wrong or this is a prop file whose
            // corners resolve against the wielder's pool - either way the sharing graph cannot be
            // rebuilt, so growth must refuse (never add a fallback).
            foreach (Model m in ilm.Models)
                foreach (Mesh me in m.Meshes)
                    foreach (Prim p in me.Prims)
                    {
                        int n = p.Tri ? 3 : 4;
                        for (int i = 0; i < n; i++)
                            if (!p.VRef[i].Valid || !p.NRef[i].Valid)
                                throw new IlmException("part '" + m.Name + "' reads a pool slot no part has written " +
                                    "(a held-item/prop file resolves those against the wielder's pool at draw time) - " +
                                    "--grow requires a fully self-contained skeletal file.");
                    }
            foreach (int mi in order)
            {
                List<ShadeEdge> lst = shadeMap[mi];
                if (lst == null) continue;
                foreach (ShadeEdge e in lst)
                    if (!e.Ref.Valid)
                        throw new IlmException("part '" + ilm.Models[mi].Name + "' shade stream reads an unwritten " +
                            "pool slot - --grow requires a fully self-contained skeletal file.");
            }

            var newVerts = new List<int[]>[mc];
            var newNormals = new List<int[]>[mc];
            var newPrims = new List<NewPrim>[mc];
            var grown = new HashSet<int>();
            var seamWarn = new List<string[]>();
            var seamNWarn = new List<string[]>();
            int nvert = 0, nnorm = 0, nprim = 0, unusedPrim = 0;

            foreach (Model model in ilm.Models)
            {
                ObjObject obj = byName[model.Name];
                int mi = model.Idx;
                List<PoolRef> ownV, forV, ownN, forN;
                PartLayout(model, out ownV, out forV, out ownN, out forN);
                int expV = ownV.Count + forV.Count;
                int want = 0;
                foreach (Mesh me in model.Meshes) want += me.PrimCount;

                if (obj.V.Count < expV)
                    throw new IlmException("part '" + obj.Name + "' has " + obj.V.Count.ToString(Inv) + " vertices but " +
                        "the ILM expects " + expV.ToString(Inv) + " (" + ownV.Count.ToString(Inv) + " own + " +
                        forV.Count.ToString(Inv) + " duplicated seam vertices). The usual cause is an aggressive " +
                        "'Merge by Distance' - the seam duplicates are deliberate and must not be welded.", true);
                if (obj.Faces.Count < want)
                    throw new IlmException("part '" + obj.Name + "' has " + obj.Faces.Count.ToString(Inv) + " faces but " +
                        "the ILM has " + want.ToString(Inv) + " primitives - --grow only ADDS geometry, it cannot " +
                        "remove faces.", true);

                List<ObjFace> leftover;
                List<ObjFace> paired = PairFacesGrow(model, obj, ilm.BaseClutY, out leftover);
                int extraV = obj.V.Count - expV;
                if (extraV != 0 || leftover.Count != 0)
                {
                    grown.Add(mi);
                    if (model.MeshCount > 1)
                        throw new IlmException("part '" + obj.Name + "' has " + model.MeshCount.ToString(Inv) +
                            " meshes; growth supports single-mesh parts only", true);
                    int shadeBytes = 0;
                    foreach (Mesh me in model.Meshes) shadeBytes += me.UnkCount3;
                    if (shadeBytes != 0)
                        throw new IlmException("part '" + obj.Name + "' carries a shade stream (" +
                            shadeBytes.ToString(Inv) + " bytes); growing shaded parts is not supported yet", true);
                }

                // Original subset: the same verification and the same in-place edits as the plain
                // import path (the rewrite below then copies the patched bytes).
                PatchOriginalSubset(data, ilm, of, model, obj, ownV, forV, paired, poseR, poseT, invs,
                    ownerV, seamWarn, seamNWarn, ref nvert, ref nnorm, ref unusedPrim);

                if (!grown.Contains(mi)) continue;

                double[] inv = invs[mi];
                int[] T = poseT[mi];
                if (inv == null)
                    throw new IlmException("part '" + obj.Name + "' rest-pose matrix is singular; cannot unbake new geometry");

                var nv = new List<int[]>();
                for (int pos = expV; pos < obj.V.Count; pos++)
                {
                    double[] w = of.Verts[obj.V[pos] - 1];
                    double[] v = Unbake(inv, T, new[] { w[0], YIn(w[1]), w[2] });
                    nv.Add(new int[] { S16Checked(v[0], "new vertex X"), S16Checked(v[1], "new vertex Y"),
                                       S16Checked(v[2], "new vertex Z") });
                }
                newVerts[mi] = nv;

                var vlinePos = new Dictionary<int, int>();
                for (int i = 0; i < obj.V.Count; i++) vlinePos[obj.V[i]] = i;

                var primsFlat = new List<Prim>();
                var legalSet = new HashSet<string>(StringComparer.Ordinal);
                foreach (Mesh me in model.Meshes)
                    foreach (Prim p in me.Prims) { primsFlat.Add(p); legalSet.Add(MtlName(p, ilm.BaseClutY)); }
                var legal = new List<string>(legalSet);
                legal.Sort(StringComparer.Ordinal);   // Python sorted() is by code point

                var ndKeys = new List<int[]>();
                var ndMap = new Dictionary<int, int>();
                string partName = obj.Name;
                Func<double[], int> newNormal = w =>
                {
                    int[] key = QuantNewNormal(inv, w);
                    if (key == null)
                        throw new IlmException("part '" + partName + "': a new face has a degenerate normal");
                    int pk = ((key[0] + 128) << 16) | ((key[1] + 128) << 8) | (key[2] + 128);
                    int idx;
                    if (!ndMap.TryGetValue(pk, out idx)) { idx = ndKeys.Count; ndMap[pk] = idx; ndKeys.Add(key); }
                    return idx;
                };

                var npList = new List<NewPrim>();
                foreach (ObjFace f in leftover)
                {
                    int[][] face = f.C;
                    int arity = face.Length;
                    if (arity != 3 && arity != 4)
                        throw new IlmException("part '" + obj.Name + "': a new face has " + arity.ToString(Inv) +
                            " corners; the format only has triangles and quads", true);
                    if (!legalSet.Contains(f.Mtl))
                        throw new IlmException("part '" + obj.Name + "': new face uses material '" + f.Mtl +
                            "'; new faces must reuse one of the part's existing materials (" +
                            string.Join(", ", legal.ToArray()) + ")", true);
                    Prim tmpl = null;
                    foreach (Prim p in primsFlat)
                        if (string.Equals(MtlName(p, ilm.BaseClutY), f.Mtl, StringComparison.Ordinal)) { tmpl = p; break; }

                    bool tri = arity == 3;
                    int[] loop = tri ? TriLoop : QuadLoop;
                    var np = new NewPrim
                    {
                        Tri = tri, Tmpl = tmpl,
                        VKind = new int[arity], VA = new int[arity], VB = new int[arity],
                        NIdx = new int[arity], Uu = new int[arity], Vv = new int[arity]
                    };
                    int flatSlot = -1;
                    for (int i = 0; i < arity; i++)
                    {
                        int[] cor = face[loop[i]];
                        int vl = cor[0], tl = cor[1], nl = cor[2];
                        int pos;
                        if (!vlinePos.TryGetValue(vl, out pos))
                            throw new IlmException("part '" + obj.Name + "': a new face cites vertex " + vl.ToString(Inv) +
                                " outside its own 'o' block - new cross-part seams are not supported (weld to an " +
                                "existing seam duplicate instead)", true);
                        if (pos < ownV.Count) { np.VKind[i] = 0; np.VA[i] = ownV[pos].Local; }
                        else if (pos < expV)
                        {
                            PoolRef r = forV[pos - ownV.Count];
                            np.VKind[i] = 1; np.VA[i] = r.Model; np.VB[i] = r.Local;
                        }
                        else { np.VKind[i] = 2; np.VA[i] = pos - expV; }

                        if (tl == 0)
                            throw new IlmException("new face in part '" + obj.Name + "' has no UVs - unwrap the new geometry");
                        if (tl < 0 || tl > of.Uvs.Count)
                            throw new IlmException("part '" + obj.Name + "': a new face references vt " + tl.ToString(Inv) +
                                " but the OBJ has only " + of.Uvs.Count.ToString(Inv) + " 'vt' lines.");
                        double[] uv = of.Uvs[tl - 1];
                        np.Uu[i] = UvByte(uv[0]);
                        np.Vv[i] = UvByte(1.0 - uv[1]);

                        if (nl != 0)
                        {
                            if (nl < 0 || nl > of.Norms.Count)
                                throw new IlmException("part '" + obj.Name + "': a new face references vn " + nl.ToString(Inv) +
                                    " but the OBJ has only " + of.Norms.Count.ToString(Inv) + " 'vn' lines.");
                            np.NIdx[i] = newNormal(of.Norms[nl - 1]);
                        }
                        else
                        {
                            if (flatSlot < 0)
                            {
                                // One flat normal per face, from OBJ corners 0,1,2 in OBJ order.
                                double[] p0 = VertAt(of, face[0][0]), p1 = VertAt(of, face[1][0]), p2 = VertAt(of, face[2][0]);
                                double e10 = p1[0] - p0[0], e11 = p1[1] - p0[1], e12 = p1[2] - p0[2];
                                double e20 = p2[0] - p0[0], e21 = p2[1] - p0[1], e22 = p2[2] - p0[2];
                                flatSlot = newNormal(new[] { e11 * e22 - e12 * e21,
                                                             e12 * e20 - e10 * e22,
                                                             e10 * e21 - e11 * e20 });
                            }
                            np.NIdx[i] = flatSlot;
                        }
                    }
                    npList.Add(np);
                }
                newPrims[mi] = npList;
                newNormals[mi] = ndKeys;
                nprim += npList.Count;
            }

            // Re-layout: ungrown parts keep their windows; grown parts relocate whole to fresh
            // exclusive ranges above the original extents; pinned (field_B_4) parts expand in place
            // at offset 0 (V12: the third chain ignores offsets).
            int extV0 = 0, extN0 = 0;
            foreach (Model m in ilm.Models)
                foreach (Mesh me in m.Meshes)
                {
                    if (m.VertexOffset + me.VertexCount > extV0) extV0 = m.VertexOffset + me.VertexCount;
                    if (m.NormalOffset + me.NormalCount > extN0) extN0 = m.NormalOffset + me.NormalCount;
                }

            var vO2 = new int[mc];
            var nO2 = new int[mc];
            var pinned = new List<int>();
            var isPinned = new bool[mc];
            foreach (Model m in ilm.Models)
            {
                int mi = m.Idx;
                if (!grown.Contains(mi)) { vO2[mi] = m.VertexOffset; nO2[mi] = m.NormalOffset; }
                else if (((m.Bits >> 4) & 3) != 0)
                {
                    if (m.VertexOffset != 0 || m.NormalOffset != 0)
                        throw new IlmException("part '" + m.Name + "': field_B_4=" + (((m.Bits >> 4) & 3)).ToString(Inv) +
                            " with window offsets " + m.VertexOffset.ToString(Inv) + "/" + m.NormalOffset.ToString(Inv) +
                            " violates V12 in the ORIGINAL file - fix the source ILM first.");
                    vO2[mi] = 0; nO2[mi] = 0;
                    pinned.Add(mi); isPinned[mi] = true;
                }
            }
            int vbase = extV0, nbase = extN0;
            foreach (int p2 in pinned)
            {
                int a = VcNew(ilm, newVerts, p2); if (a > vbase) vbase = a;
                int b = NcNew(ilm, newNormals, p2); if (b > nbase) nbase = b;
            }
            foreach (Model m in ilm.Models)
            {
                int mi = m.Idx;
                if (grown.Contains(mi) && !isPinned[mi])
                {
                    vO2[mi] = vbase; vbase += VcNew(ilm, newVerts, mi);
                    nO2[mi] = nbase; nbase += NcNew(ilm, newNormals, mi);
                }
            }

            var finals = new int[mc][];
            foreach (Model m in ilm.Models)
            {
                int mi = m.Idx;
                int vc = 0, nc = 0, pc = 0, sw = 0;
                foreach (Mesh me in m.Meshes)
                {
                    vc += me.VertexCount; nc += me.NormalCount; pc += me.PrimCount;
                    foreach (byte b in me.NCount) sw += b;
                }
                int dv = CountOf(newVerts, mi), dn = CountOf(newNormals, mi);
                int dp = newPrims[mi] == null ? 0 : newPrims[mi].Count;
                finals[mi] = new[] { vc + dv, nc + dn, pc + dp, sw + dn, dv, dn, dp };
            }

            foreach (Model m in ilm.Models)
            {
                int[] f = finals[m.Idx];
                if (f[2] > 255 || f[0] > 255 || f[1] > 255)
                {
                    GrowBudget(ilm, grown, vO2, nO2, finals, data.Length, 0, res.Report);
                    throw new IlmException("part '" + m.Name + "': grown counts " + f[0].ToString(Inv) + " verts / " +
                        f[1].ToString(Inv) + " normals / " + f[2].ToString(Inv) + " prims exceed the u8 ceiling 255");
                }
            }
            if (vbase > 255)
            {
                GrowBudget(ilm, grown, vO2, nO2, finals, data.Length, 0, res.Report);
                throw new IlmException("REFUSED V5: vertex slot extent " + vbase.ToString(Inv) + " exceeds 255 - vertex " +
                    "slot 255 is reserved (0xFF in a quad's 4th corner byte is the triangle sentinel)");
            }
            if (nbase > 256)
            {
                GrowBudget(ilm, grown, vO2, nO2, finals, data.Length, 0, res.Report);
                throw new IlmException("REFUSED V6: normal slot extent " + nbase.ToString(Inv) + " exceeds 256");
            }

            // Pinned expansion conflict check (see the preservation argument above).
            if (pinned.Count > 0)
            {
                var vedges = new List<int[]>();
                var nedges = new List<int[]>();
                foreach (Model m in ilm.Models)
                {
                    int rmi = m.Idx;
                    foreach (Mesh me in m.Meshes)
                        foreach (Prim p in me.Prims)
                        {
                            int n = p.Tri ? 3 : 4;
                            for (int i = 0; i < n; i++)
                            {
                                vedges.Add(new[] { (int)p.Vtx[i], p.VRef[i].Model, rmi });
                                nedges.Add(new[] { (int)p.Nrm[i], p.NRef[i].Model, rmi });
                            }
                        }
                    if (shadeMap[rmi] != null)
                        foreach (ShadeEdge e in shadeMap[rmi]) vedges.Add(new[] { e.Slot, e.Ref.Model, rmi });
                }
                foreach (int P in pinned)
                {
                    Model m = ilm.Models[P];
                    for (int zone = 0; zone < 2; zone++)
                    {
                        int lo = zone == 0 ? m.Meshes[0].VertexCount : m.Meshes[0].NormalCount;
                        int hi = zone == 0 ? VcNew(ilm, newVerts, P) : NcNew(ilm, newNormals, P);
                        List<int[]> edges = zone == 0 ? vedges : nedges;
                        string kind = zone == 0 ? "vertex" : "normal";
                        foreach (int[] e in edges)
                            if (e[0] >= lo && e[0] < hi && dpos[e[1]] < dpos[P] && dpos[P] < dpos[e[2]])
                                throw new IlmException("part '" + m.Name + "' cannot grow at offset 0: expanding its " +
                                    kind + " window to " + hi.ToString(Inv) + " would overwrite pool slot " +
                                    e[0].ToString(Inv) + " between writer " + ilm.Models[e[1]].Name + " and reader " +
                                    ilm.Models[e[2]].Name);
                    }
                }
            }

            // Emit. Layout mirrors stock: header | materials | modelHdrs | reserved zero block |
            // modelOrder | pad4 | per part meshHdrs + arrays, each array followed by real in-file
            // pad bytes so the file passes V9 at tailSlack 0.
            var outb = new List<byte>(data.Length * 2);
            AddRange(outb, data, 0, 20, "header");
            int matsP = outb.Count;
            AddRange(outb, data, ilm.MatsP, 24 * ilm.MatCount, "materials");
            int hdrsP = outb.Count;
            AddZeros(outb, 16 * mc);
            int reservedFrom = ilm.ModelHdrsP + 16 * mc;
            if (ilm.ModelOrderP < reservedFrom)
                throw new IlmException("grow: the source ILM puts modelOrder inside its model header table; the " +
                    "sections must not overlap.");
            AddRange(outb, data, reservedFrom, ilm.ModelOrderP - reservedFrom, "block between the model headers and modelOrder");
            int orderP = outb.Count;
            AddRange(outb, data, ilm.ModelOrderP, mc, "modelOrder");
            AddZeros(outb, Align4(outb.Count));
            PutU32(outb, 4, matsP);
            PutU32(outb, 0xC, hdrsP);
            PutU32(outb, 0x10, orderP);
            outb[2] = 0; // isLoaded: a 1 makes the runtime skip pointer fix-up and crash

            foreach (Model m in ilm.Models)
            {
                int mi = m.Idx;
                int meshHdrsP2 = outb.Count;
                AddZeros(outb, 24 * m.MeshCount);
                var meshInfo = new int[m.MeshCount][];
                for (int k = 0; k < m.MeshCount; k++)
                {
                    Mesh me = m.Meshes[k];
                    // Growth is single-mesh, so every addition lands in mesh 0.
                    List<int[]> addV = k == 0 && newVerts[mi] != null ? newVerts[mi] : null;
                    List<int[]> addN = k == 0 && newNormals[mi] != null ? newNormals[mi] : null;
                    List<NewPrim> addP = k == 0 && newPrims[mi] != null ? newPrims[mi] : null;
                    int nAddV = addV == null ? 0 : addV.Count;
                    int nAddN = addN == null ? 0 : addN.Count;
                    int nAddP = addP == null ? 0 : addP.Count;

                    int primsP = outb.Count;
                    foreach (Prim p in me.Prims)
                    {
                        var raw = new byte[20];
                        if (p.Off < 0 || p.Off + 20 > data.Length)
                            throw new IlmException("grow: part '" + m.Name + "' has a primitive outside the source ILM");
                        Buffer.BlockCopy(data, p.Off, raw, 0, 20);
                        int n = p.Tri ? 3 : 4;
                        for (int i = 0; i < n; i++)
                        {
                            raw[0xC + i] = SlotByte(vO2[p.VRef[i].Model] + p.VRef[i].Local);
                            raw[0x10 + i] = SlotByte(nO2[p.NRef[i].Model] + p.NRef[i].Local);
                        }
                        outb.AddRange(raw);
                    }
                    if (addP != null)
                        foreach (NewPrim np in addP)
                        {
                            var raw = new byte[20];
                            Buffer.BlockCopy(data, np.Tmpl.Off, raw, 0, 20);
                            int arity = np.Tri ? 3 : 4;
                            for (int i = 0; i < arity; i++)
                            {
                                int slot;
                                if (np.VKind[i] == 0) slot = vO2[mi] + np.VA[i];
                                else if (np.VKind[i] == 1) slot = vO2[np.VA[i]] + np.VB[i];
                                else slot = vO2[mi] + me.VertexCount + np.VA[i];
                                raw[0xC + i] = SlotByte(slot);
                                raw[0x10 + i] = SlotByte(nO2[mi] + me.NormalCount + np.NIdx[i]);
                                W16(raw, UvOffsets[i], np.Uu[i] | (np.Vv[i] << 8));
                            }
                            if (np.Tri)
                            {
                                raw[0xC + 3] = 0xFF;
                                raw[0x10 + 3] = 0;   // stock convention: tris carry 0
                            }
                            outb.AddRange(raw);
                        }

                    int xyP = outb.Count;
                    AddRange(outb, data, me.XyP, 4 * me.VertexCount, "part '" + m.Name + "' verticesXy");
                    if (addV != null)
                        foreach (int[] v in addV)
                        {
                            outb.Add((byte)v[0]); outb.Add((byte)(v[0] >> 8));
                            outb.Add((byte)v[1]); outb.Add((byte)(v[1] >> 8));
                        }
                    AddZeros(outb, LmArraySlack);

                    int zP = outb.Count;
                    AddRange(outb, data, me.ZP, 2 * me.VertexCount, "part '" + m.Name + "' verticesZ");
                    if (addV != null)
                        foreach (int[] v in addV) { outb.Add((byte)v[2]); outb.Add((byte)(v[2] >> 8)); }
                    AddZeros(outb, Align4(outb.Count) + LmArraySlack);

                    int normP = outb.Count;
                    AddRange(outb, data, me.NormP, 4 * me.NormalCount, "part '" + m.Name + "' normals");
                    if (addN != null)
                        foreach (int[] s in addN)
                        {
                            outb.Add((byte)(sbyte)s[0]); outb.Add((byte)(sbyte)s[1]); outb.Add((byte)(sbyte)s[2]);
                            // Count byte 1: the stock 1:1 pattern (sum(count) == vertexCount); it only
                            // feeds the env-mode-1/2 walk, which V8 bounds.
                            outb.Add(1);
                        }
                    AddZeros(outb, LmArraySlack);

                    int cnt = me.UnkCount3;
                    int nc2 = me.NormalCount + nAddN;
                    int unkP;
                    if (cnt != 0)
                    {
                        unkP = outb.Count;
                        foreach (ShadeEdge e in shadeMap[mi])
                            if (e.Mesh == k) outb.Add(SlotByte(vO2[e.Ref.Model] + e.Ref.Local));
                    }
                    else
                    {
                        // Stock relation unkP = normP + 4*normalCount; with count 0 the pointer is
                        // never dereferenced beyond address formation.
                        unkP = normP + 4 * nc2;
                    }
                    AddZeros(outb, ShadePad(cnt));

                    meshInfo[k] = new[] { me.PrimCount + nAddP, me.VertexCount + nAddV, nc2, cnt,
                                          primsP, xyP, zP, normP, unkP };
                }
                for (int k = 0; k < m.MeshCount; k++)
                {
                    int hb = meshHdrsP2 + 24 * k;
                    int[] info = meshInfo[k];
                    outb[hb] = SlotByte(info[0]);
                    outb[hb + 1] = SlotByte(info[1]);
                    outb[hb + 2] = SlotByte(info[2]);
                    outb[hb + 3] = SlotByte(info[3]);
                    for (int a = 0; a < 5; a++) PutU32(outb, hb + 4 + a * 4, info[4 + a]);
                }
                int mhb = hdrsP + 16 * mi;
                if (m.HdrOff < 0 || m.HdrOff + 8 > data.Length)
                    throw new IlmException("grow: part '" + m.Name + "' header runs past the end of the source ILM");
                for (int i = 0; i < 8; i++) outb[mhb + i] = data[m.HdrOff + i];
                outb[mhb + 8] = (byte)m.MeshCount;
                outb[mhb + 9] = SlotByte(vO2[mi]);
                outb[mhb + 10] = SlotByte(nO2[mi]);
                outb[mhb + 11] = (byte)m.Bits;
                PutU32(outb, mhb + 0xC, meshHdrsP2);
            }

            byte[] blob = outb.ToArray();

            // The validator twin at strict authoring settings; V13 through the ANM.
            string anmPath;
            var anmWarn = new List<string>();
            Anm anm = FindAnm(ilmPath, ilm.Models, null, anmWarn, out anmPath);
            string vmsg;
            if (LmValidate(blob, 0, anm != null ? anm.BoneCount : -1, true, out vmsg) != 0)
            {
                GrowBudget(ilm, grown, vO2, nO2, finals, data.Length, blob.Length, res.Report);
                throw new IlmException("REFUSED " + vmsg);
            }

            // Sharing-graph verification on the EMITTED bytes: every original corner and shade byte
            // must resolve to the same (writer part, local index), every new corner to its own part.
            // This is the converter's own acceptance test.
            Ilm ilm2 = ParseIlm(blob);
            int[] order2 = ResolvePool(ilm2);
            for (int i = 0; i < mc; i++)
                if (order2[i] != order[i]) throw new IlmException("grow internal error: modelOrder changed");
            int foreign = 0, kept = 0;
            foreach (Model m in ilm.Models)
            {
                int mi = m.Idx;
                Model m2 = ilm2.Models[mi];
                for (int k = 0; k < m.MeshCount; k++)
                {
                    Mesh me = m.Meshes[k], me2 = m2.Meshes[k];
                    for (int pi = 0; pi < me.PrimCount; pi++)
                    {
                        Prim p = me.Prims[pi], p2 = me2.Prims[pi];
                        int n = p.Tri ? 3 : 4;
                        for (int i = 0; i < n; i++)
                            for (int which = 0; which < 2; which++)
                            {
                                PoolRef oldR = which == 0 ? p.VRef[i] : p.NRef[i];
                                PoolRef newR = which == 0 ? p2.VRef[i] : p2.NRef[i];
                                string expectName = ilm.Models[oldR.Model].Name;
                                bool ok = newR.Valid && ilm2.Models[newR.Model].Name == expectName && newR.Local == oldR.Local;
                                if (oldR.Model != mi) { foreign++; if (ok) kept++; }
                                if (!ok)
                                    throw new IlmException("grow internal error: part " + m.Name + " prim " +
                                        pi.ToString(Inv) + " corner " + i.ToString(Inv) + " edge mismatch");
                            }
                    }
                }
                if (newPrims[mi] == null) continue;
                int vcold = m.Meshes[0].VertexCount, ncold = m.Meshes[0].NormalCount;
                for (int j = 0; j < newPrims[mi].Count; j++)
                {
                    NewPrim np = newPrims[mi][j];
                    Prim p2 = m2.Meshes[0].Prims[m.Meshes[0].PrimCount + j];
                    int arity = np.Tri ? 3 : 4;
                    for (int i = 0; i < arity; i++)
                    {
                        string wantName; int wantLocal;
                        if (np.VKind[i] == 0) { wantName = m.Name; wantLocal = np.VA[i]; }
                        else if (np.VKind[i] == 1) { wantName = ilm.Models[np.VA[i]].Name; wantLocal = np.VB[i]; }
                        else { wantName = m.Name; wantLocal = vcold + np.VA[i]; }
                        PoolRef r = p2.VRef[i];
                        if (!r.Valid || ilm2.Models[r.Model].Name != wantName || r.Local != wantLocal)
                            throw new IlmException("grow internal error: new prim " + j.ToString(Inv) + " corner " +
                                i.ToString(Inv) + " vertex edge mismatch");
                        r = p2.NRef[i];
                        if (!r.Valid || ilm2.Models[r.Model].Name != m.Name || r.Local != ncold + np.NIdx[i])
                            throw new IlmException("grow internal error: new prim " + j.ToString(Inv) + " corner " +
                                i.ToString(Inv) + " normal edge mismatch");
                    }
                }
            }
            List<ShadeEdge>[] shade2 = ShadeEdges(ilm2, order2);
            foreach (int mi in order)
            {
                List<ShadeEdge> lst = shadeMap[mi];
                if (lst == null) continue;
                List<ShadeEdge> lst2 = shade2[mi];
                if (lst2 == null || lst.Count != lst2.Count)
                    throw new IlmException("grow internal error: shade stream length changed");
                for (int i = 0; i < lst.Count; i++)
                {
                    ShadeEdge a = lst[i], b = lst2[i];
                    if (a.Mesh != b.Mesh || a.ByteIdx != b.ByteIdx || !b.Ref.Valid ||
                        ilm2.Models[b.Ref.Model].Name != ilm.Models[a.Ref.Model].Name || b.Ref.Local != a.Ref.Local)
                        throw new IlmException("grow internal error: part " + ilm.Models[mi].Name + " shade byte " +
                            i.ToString(Inv) + " edge mismatch");
                }
            }

            File.WriteAllBytes(outIlmPath, blob);

            var grownSorted = new List<int>(grown);
            grownSorted.Sort();
            res.IlmPath = outIlmPath;
            res.Parts = mc;
            res.Vertices = nvert;
            res.Normals = nnorm;
            res.Prims = nprim;
            res.Grew = true;
            res.GrownParts.AddRange(grownSorted);

            foreach (int mi in grownSorted)
                res.Report.Add("   part " + ilm.Models[mi].Name + ": +" + CountOf(newVerts, mi).ToString(Inv) +
                    " verts, +" + CountOf(newNormals, mi).ToString(Inv) + " normals, +" +
                    (newPrims[mi] == null ? 0 : newPrims[mi].Count).ToString(Inv) + " prims" +
                    (isPinned[mi] ? " (pinned at offset 0: third-chain part)" : ""));
            res.Report.Add("   " + nvert.ToString(Inv) + " original vertices, " + nnorm.ToString(Inv) +
                " normals patched; sharing graph preserved: " + kept.ToString(Inv) + "/" + foreign.ToString(Inv) +
                " foreign edges");
            if (res.RestPoseIdentity)
                res.Warnings.Add("rest pose: IDENTITY (this OBJ was exported without an ANM)");
            SeamReport(seamWarn, "seam vertex/vertices moved", res.Warnings);
            SeamReport(seamNWarn, "seam normal(s) edited", res.Warnings);
            GrowBudget(ilm, grown, vO2, nO2, finals, data.Length, blob.Length, res.Report);
        }

        // ---- C-style numeric formatting -----------------------------------------

        // ---- exact decimal expansion of a double --------------------------------
        //
        // Every finite double is m * 2^e for integers m, e, so it has an EXACT finite decimal
        // expansion — up to ~767 digits for a subnormal. Producing it needs big integers (a long
        // overflows at mantissa * 10^4), so these three helpers carry a decimal bignum in base
        // 10^9 limbs, least significant first. They exist for one reason: printf's rounding.

        private const int BigBase = 1000000000;

        private static List<int> BigFrom(ulong m)
        {
            var a = new List<int>();
            while (m != 0) { a.Add((int)(m % (ulong)BigBase)); m /= (ulong)BigBase; }
            if (a.Count == 0) a.Add(0);
            return a;
        }

        /// <summary>Multiply in place by a factor no larger than 10^9, so limb * factor + carry
        /// stays inside a long.</summary>
        private static void BigMul(List<int> a, int factor)
        {
            long carry = 0;
            for (int i = 0; i < a.Count; i++)
            {
                long cur = (long)a[i] * factor + carry;
                a[i] = (int)(cur % BigBase);
                carry = cur / BigBase;
            }
            while (carry != 0) { a.Add((int)(carry % BigBase)); carry /= BigBase; }
        }

        /// <summary>Multiply in place by baseValue^power, in chunks small enough for BigMul.</summary>
        private static void BigMulPow(List<int> a, int baseValue, int power)
        {
            int chunk = 1, per = 0;
            while ((long)chunk * baseValue <= BigBase) { chunk *= baseValue; per++; }
            while (power >= per) { BigMul(a, chunk); power -= per; }
            for (int i = 0; i < power; i++) BigMul(a, baseValue);
        }

        private static string BigToString(List<int> a)
        {
            int top = a.Count - 1;
            while (top > 0 && a[top] == 0) top--;
            var sb = new StringBuilder();
            sb.Append(a[top].ToString(Inv));
            for (int j = top - 1; j >= 0; j--) sb.Append(a[j].ToString(Inv).PadLeft(9, '0'));
            return sb.ToString();
        }

        /// <summary>Add one to a non-negative decimal digit string.</summary>
        private static string DecInc(string s)
        {
            var c = s.ToCharArray();
            for (int i = c.Length - 1; i >= 0; i--)
            {
                if (c[i] != '9') { c[i]++; return new string(c); }
                c[i] = '0';
            }
            return "1" + new string(c);
        }

        /// <summary>C's (and Python's) "%.*f": round-half-to-EVEN on the double's EXACT binary
        /// value. .NET's "F0"/"F3" do neither — they pre-round to 15 significant digits and then
        /// round half AWAY from zero, so a distance of exactly 12.5 prints "13" where the Python
        /// prints "12". Half-way cases are reachable at 0 decimals (every k+0.5 is a double) and
        /// the 15-digit pre-round bites at any magnitude, which is why this is exact arithmetic
        /// rather than a formatting flag.</summary>
        /// <summary>Digits of round(|v| * 10^decimals), half-to-even on the exact binary value.
        /// `decimals` may be negative, which rounds to a multiple of a power of ten — %g needs
        /// that to extract significant digits from a large value.</summary>
        private static string ScaledDigits(double v, int decimals, out bool neg)
        {
            long bits = BitConverter.DoubleToInt64Bits(v);
            neg = bits < 0;
            int be = (int)((bits >> 52) & 0x7FF);
            ulong m = (ulong)(bits & 0xFFFFFFFFFFFFFL);
            int e;
            if (be == 0) e = -1074;
            else { m |= 1UL << 52; e = be - 1075; }

            // value == n / 10^frac exactly: m/2^k == (m*5^k)/10^k, and an integer needs no scaling.
            List<int> n = BigFrom(m);
            int frac;
            if (e >= 0) { BigMulPow(n, 2, e); frac = 0; }
            else { BigMulPow(n, 5, -e); frac = -e; }

            int drop = frac - decimals;
            if (drop <= 0)
            {
                BigMulPow(n, 10, -drop);
                return BigToString(n);
            }
            string s = BigToString(n);
            if (s.Length <= drop) s = s.PadLeft(drop + 1, '0');
            string keep = s.Substring(0, s.Length - drop);
            string cut = s.Substring(s.Length - drop);
            bool up;
            if (cut[0] > '5') up = true;
            else if (cut[0] < '5') up = false;
            else
            {
                bool restZero = true;
                for (int i = 1; i < cut.Length && restZero; i++) if (cut[i] != '0') restZero = false;
                // Exactly half rounds to EVEN; anything above it rounds up.
                up = !restZero || ((keep[keep.Length - 1] - '0') & 1) != 0;
            }
            return up ? DecInc(keep) : keep;
        }

        private static string FormatFixed(double v, int decimals)
        {
            if (double.IsNaN(v)) return "nan";
            if (double.IsInfinity(v)) return v > 0 ? "inf" : "-inf";
            bool neg;
            string q = ScaledDigits(v, decimals, out neg).PadLeft(decimals + 1, '0');
            string body = decimals == 0
                ? q
                : q.Substring(0, q.Length - decimals) + "." + q.Substring(q.Length - decimals);
            // -0.0 and a negative that rounds to zero both keep the sign, as C does.
            return (neg ? "-" : "") + body;
        }

        /// <summary>C's "%g" at the default precision 6, which is how the Python prints the weld
        /// radius. .NET's "G6" is not a substitute: it writes "1E-06" where C writes "1e-06", and
        /// it switches to exponent form on different thresholds. The exponent is taken from the
        /// value rounded to 6 significant digits, not from the raw value — 9.999995e-08 is a
        /// %g of 9.99999e-08, and reading the exponent off .NET's own "E5" (which rounds half
        /// away from zero) turns it into 1e-07.</summary>
        private static string PyG(double v)
        {
            if (double.IsNaN(v)) return "nan";
            if (double.IsInfinity(v)) return v > 0 ? "inf" : "-inf";
            if (v == 0.0) return (1.0 / v) < 0 ? "-0" : "0";

            bool neg;
            // Math.Log10 can land one out at a power of ten; the digit count corrects it, and a
            // rounding carry (999999.6 -> 1000000) needs the same correction.
            int exp = (int)Math.Floor(Math.Log10(Math.Abs(v)));
            string digits = null;
            for (int guard = 0; guard < 4; guard++)
            {
                digits = ScaledDigits(v, 5 - exp, out neg);
                if (digits.Length == 6) break;
                exp += digits.Length - 6;
            }

            if (exp < -4 || exp >= 6)
            {
                string mant = TrimTrailingZeros(digits.Substring(0, 1) + "." + digits.Substring(1));
                return (v < 0 ? "-" : "") + mant + "e" + (exp < 0 ? "-" : "+") +
                       Math.Abs(exp).ToString(Inv).PadLeft(2, '0');
            }
            return TrimTrailingZeros(FormatFixed(v, 5 - exp));
        }

        private static string TrimTrailingZeros(string s)
        {
            if (s.IndexOf('.') < 0) return s;
            s = s.TrimEnd('0');
            return s.EndsWith(".", StringComparison.Ordinal) ? s.Substring(0, s.Length - 1) : s;
        }

        // ---- full replacement (import --replace) --------------------------------

        private struct Cell : IEquatable<Cell>
        {
            public long X, Y, Z;
            public bool Equals(Cell o) { return X == o.X && Y == o.Y && Z == o.Z; }
            public override bool Equals(object o) { return o is Cell && Equals((Cell)o); }
            public override int GetHashCode()
            {
                long h = (X * 73856093L) ^ (Y * 19349663L) ^ (Z * 83492791L);
                return (int)h ^ (int)(h >> 32);
            }
        }

        /// <summary>floor() into a cell coordinate. The Python's int(math.floor(x)) is arbitrary
        /// precision; saturating here could only merge cells more than 9e18 apart, and every weld
        /// is still gated on the real distance — the s16 vertex check downstream refuses anything
        /// that large long before it could matter.</summary>
        private static long FloorLong(double v)
        {
            double f = Math.Floor(v);
            if (double.IsNaN(f)) return 0;
            if (f <= -9.2e18) return long.MinValue;
            if (f >= 9.2e18) return long.MaxValue;
            return (long)f;
        }

        /// <summary>Uniform-cell spatial hash. One cell size for both add and query, or the keys
        /// do not match and nothing is ever found; Near walks the 27 cells around a point in a
        /// fixed dx/dy/dz order, and each cell keeps insertion order, because both callers take
        /// the FIRST acceptable candidate.</summary>
        private class Grid
        {
            private readonly Dictionary<Cell, List<int>> _cells = new Dictionary<Cell, List<int>>();
            private readonly double _size;

            public Grid(double size) { _size = size; }

            private Cell CellOf(double[] p)
            {
                return new Cell { X = FloorLong(p[0] / _size), Y = FloorLong(p[1] / _size), Z = FloorLong(p[2] / _size) };
            }

            public void Add(double[] p, int val)
            {
                Cell c = CellOf(p);
                List<int> lst;
                if (!_cells.TryGetValue(c, out lst)) { lst = new List<int>(); _cells[c] = lst; }
                lst.Add(val);
            }

            public List<int> Near(double[] p)
            {
                Cell c = CellOf(p);
                var outp = new List<int>();
                for (int dx = -1; dx <= 1; dx++)
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dz = -1; dz <= 1; dz++)
                        {
                            List<int> lst;
                            if (_cells.TryGetValue(new Cell { X = c.X + dx, Y = c.Y + dy, Z = c.Z + dz }, out lst))
                                outp.AddRange(lst);
                        }
                return outp;
            }
        }

        private static double Dist2(double[] a, double[] b)
        {
            double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

        /// <summary>Which part owns a vertex line, and where in that part's rebuilt array.</summary>
        private struct VLoc { public int Mi, J; }

        /// <summary>A rebuilt normal: either one of the template's (Fresh false, A = its index) or
        /// a newly quantised s8 triple.</summary>
        private struct NormKey : IEquatable<NormKey>
        {
            public bool Fresh;
            public int A, B, C;
            public bool Equals(NormKey o) { return Fresh == o.Fresh && A == o.A && B == o.B && C == o.C; }
            public override bool Equals(object o) { return o is NormKey && Equals((NormKey)o); }
            public override int GetHashCode() { return (Fresh ? 1 : 0) ^ (A << 1) ^ (B << 9) ^ (C << 17); }
        }

        private struct PlanCorner
        {
            public int Own, Loc;
            public NormKey NK;
            public int U, V;
        }

        private class PlanPrim
        {
            public bool Tri;
            public string Mtl;
            public PlanCorner[] Corners;
        }

        /// <summary>One mesh's fully resolved bytes, ready for EmitIlm.</summary>
        private class PartBlock
        {
            public List<byte[]> Prims = new List<byte[]>();
            public List<int[]> Verts = new List<int[]>();
            public List<int[]> Norms = new List<int[]>();
            public List<int> Shade = new List<int>();
        }

        /// <summary>Assign every part a contiguous pool window no in-between part clobbers.
        ///
        /// Lifetimes are PER SLOT, not per part: a window is contiguous but its slots die at
        /// different times, which is how stock files fit 466 vertices (KAU) into a 90-slot pool.
        /// Slot off+j is busy from the writer's draw position until the draw position of the last
        /// part that reads it, and two occupancies of one slot clash when those closed intervals
        /// touch — a part that writes a slot at the very position it is last read still clobbers
        /// it, because vertices are copied into the pool before the part's own primitives draw.
        /// The coarser per-PART model was tried first and rejected by the corpus: it calls 7488
        /// windows in the 257 shipped files illegal, the per-slot model calls zero.
        ///
        /// First fit over that occupancy, walking parts in draw order and trying the template's
        /// own offset before anything else, so an unedited replace reproduces the original
        /// windows exactly. `bound` raises the cap requirement without reserving slots (the shade
        /// stream indexes a different array off the same normalOffset, so V7/V8 bound it but it
        /// never occupies a normal slot). Returns the offsets, or null with the blocked part and
        /// the parts holding the pool across its span.</summary>
        private static int[] PoolAlloc(int cap, int[] placeOrder, int[] dpos, int[] counts, int[][] life,
                                       int[] bound, int[] keep, out int failMi, out List<int> failLive)
        {
            failMi = -1;
            failLive = null;
            int mc = counts.Length;
            var busy = new Dictionary<int, List<int[]>>();
            var off = new int[mc];
            for (int i = 0; i < mc; i++) off[i] = -1;

            foreach (int mi in placeOrder)
            {
                int need = Math.Max(counts[mi], bound == null ? 0 : bound[mi]);
                int chosen = -1;
                int s = dpos[mi];
                // The template's own offset first, then plain first fit; a duplicate candidate is
                // harmless because the first acceptable one wins.
                for (int attempt = (keep != null && keep[mi] >= 0 && keep[mi] + need <= cap) ? -1 : 0;
                     attempt <= cap - need; attempt++)
                {
                    int o = attempt < 0 ? keep[mi] : attempt;
                    bool clear = true;
                    for (int j = 0; j < counts[mi] && clear; j++)
                    {
                        List<int[]> lst;
                        if (!busy.TryGetValue(o + j, out lst)) continue;
                        int e = life[mi][j];
                        foreach (int[] iv in lst)
                            if (s <= iv[1] && iv[0] <= e) { clear = false; break; }
                    }
                    if (clear) { chosen = o; break; }
                }

                if (chosen < 0)
                {
                    // Blockers are the parts whose slots stay live anywhere across this part's OWN
                    // live span, not just at its draw position — a window has to be free for its
                    // whole span, which is what made the search fail.
                    int spanEnd = dpos[mi];
                    for (int j = 0; j < counts[mi]; j++) if (life[mi][j] > spanEnd) spanEnd = life[mi][j];
                    var live = new List<int>();
                    var seen = new HashSet<int>();
                    foreach (KeyValuePair<int, List<int[]>> kv in busy)
                        foreach (int[] iv in kv.Value)
                            if (dpos[mi] <= iv[1] && iv[0] <= spanEnd && seen.Add(iv[2])) live.Add(iv[2]);
                    live.Sort();
                    failMi = mi;
                    failLive = live;
                    return null;
                }

                off[mi] = chosen;
                for (int j = 0; j < counts[mi]; j++)
                {
                    List<int[]> lst;
                    if (!busy.TryGetValue(chosen + j, out lst)) { lst = new List<int[]>(); busy[chosen + j] = lst; }
                    lst.Add(new[] { dpos[mi], life[mi][j], mi });
                }
            }
            return off;
        }

        /// <summary>Flag geometry that sits nowhere near the part it was put in.
        ///
        /// The commonest replace mistake is geometry landing in the wrong object: the object name
        /// IS the bone, so a hand modelled inside `01CHEST_` silently animates with the chest. It
        /// cannot be caught by asking which bone is nearest, because a part's mesh legitimately
        /// runs from its own bone toward the child one — measured over the corpus, only 43% of
        /// correctly assigned stock parts have their own bone closest to their centroid, so that
        /// test would cry wolf on the majority of valid work.
        ///
        /// Comparing against the TEMPLATE instead is sound: whatever the modeller built, a
        /// replacement part belongs roughly where that part already was.</summary>
        private static void WarnDisplacedParts(Ilm ilm, Dictionary<string, ObjObject> byName, ObjFile of,
                                               int[][] poseR, int[][] poseT, List<string> into)
        {
            var names = new List<string>();
            var refC = new List<double[]>();
            var newC = new List<double[]>();
            double[] lo = null, hi = null;

            foreach (Model model in ilm.Models)
            {
                Mesh[] meshes = model.Meshes;
                int nv = 0;
                foreach (Mesh me in meshes) nv += me.VertexCount;
                ObjObject o;
                if (nv == 0 || !byName.TryGetValue(model.Name, out o) || o.V.Count == 0) continue;

                int[] R = poseR[model.Idx], T = poseT[model.Idx];
                var acc = new double[3];
                foreach (Mesh me in meshes)
                    for (int j = 0; j < me.VertexCount; j++)
                    {
                        double[] w = Bake(R, T, me.Vx[j], me.Vy[j], me.Vz[j]);
                        // The span is the model's real extent, so it stays in ILM orientation;
                        // only the centroid is flipped to match the Y-up OBJ it is compared with.
                        if (lo == null)
                        {
                            lo = new[] { w[0], w[1], w[2] };
                            hi = new[] { w[0], w[1], w[2] };
                        }
                        else
                            for (int a = 0; a < 3; a++)
                            {
                                if (w[a] < lo[a]) lo[a] = w[a];
                                if (w[a] > hi[a]) hi[a] = w[a];
                            }
                        acc[0] += w[0]; acc[1] += YOut(w[1]); acc[2] += w[2];
                    }
                names.Add(model.Name);
                refC.Add(new[] { acc[0] / nv, acc[1] / nv, acc[2] / nv });

                var nacc = new double[3];
                foreach (int vl in o.V)
                {
                    double[] w = of.Verts[vl - 1];
                    nacc[0] += w[0]; nacc[1] += w[1]; nacc[2] += w[2];
                }
                newC.Add(new[] { nacc[0] / o.V.Count, nacc[1] / o.V.Count, nacc[2] / o.V.Count });
            }

            if (names.Count < 2) return;
            double diag = Math.Sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) +
                                    (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                    (hi[2] - lo[2]) * (hi[2] - lo[2]));
            double limit = diag * 0.25;

            var bad = new List<int>();
            var dist = new double[names.Count];
            for (int i = 0; i < names.Count; i++)
            {
                dist[i] = Math.Sqrt(Dist2(newC[i], refC[i]));
                if (dist[i] > limit) bad.Add(i);
            }
            // Python sorts the (distance, name) tuples in REVERSE, so a tie falls back to the
            // name descending.
            bad.Sort((x, y) =>
            {
                int c = dist[y].CompareTo(dist[x]);
                return c != 0 ? c : string.CompareOrdinal(names[y], names[x]);
            });
            foreach (int i in bad)
                into.Add("part " + names[i] + " sits " + FormatFixed(dist[i], 0) + " units from where " + names[i] +
                    " is on the original (over " + FormatFixed(limit, 0) + ", a quarter of the model). If that " +
                    "geometry belongs to a different body part, move it into that OBJECT - the object name is the " +
                    "bone, so it will animate with " + names[i] + " as it stands.");
        }

        /// <summary>Serialize a whole ILM from per-part geometry, template for everything else.
        ///
        /// Layout mirrors stock: header | materials | modelHdrs | reserved zero block | modelOrder
        /// | pad4 | per part meshHdrs + arrays, each array followed by real in-file pad bytes so
        /// the file passes V9 at tailSlack 0. GrowImport emits the same layout inline because it
        /// streams the template's own array bytes through untouched; this path has every byte
        /// already materialised, so it can be written straight out.</summary>
        private static byte[] EmitIlm(byte[] data, Ilm ilm, List<PartBlock>[] parts, int[] vO2, int[] nO2)
        {
            int mc = ilm.ModelCount;
            var outb = new List<byte>(data.Length * 2);
            AddRange(outb, data, 0, 20, "header");
            int matsP = outb.Count;
            AddRange(outb, data, ilm.MatsP, 24 * ilm.MatCount, "materials");
            int hdrsP = outb.Count;
            AddZeros(outb, 16 * mc);
            int reservedFrom = ilm.ModelHdrsP + 16 * mc;
            if (ilm.ModelOrderP < reservedFrom)
                throw new IlmException("replace: the source ILM puts modelOrder inside its model header table; the " +
                    "sections must not overlap.");
            AddRange(outb, data, reservedFrom, ilm.ModelOrderP - reservedFrom, "block between the model headers and modelOrder");
            int orderP = outb.Count;
            AddRange(outb, data, ilm.ModelOrderP, mc, "modelOrder");
            AddZeros(outb, Align4(outb.Count));
            PutU32(outb, 4, matsP);
            PutU32(outb, 0xC, hdrsP);
            PutU32(outb, 0x10, orderP);
            outb[2] = 0; // isLoaded: a 1 makes the runtime skip pointer fix-up and crash

            foreach (Model m in ilm.Models)
            {
                int mi = m.Idx;
                int meshHdrsP2 = outb.Count;
                AddZeros(outb, 24 * m.MeshCount);
                var meshInfo = new int[m.MeshCount][];
                for (int k = 0; k < m.MeshCount; k++)
                {
                    PartBlock blk = parts[mi][k];
                    int primsP = outb.Count;
                    foreach (byte[] raw in blk.Prims) outb.AddRange(raw);

                    int xyP = outb.Count;
                    foreach (int[] v in blk.Verts)
                    {
                        outb.Add((byte)v[0]); outb.Add((byte)(v[0] >> 8));
                        outb.Add((byte)v[1]); outb.Add((byte)(v[1] >> 8));
                    }
                    AddZeros(outb, LmArraySlack);

                    int zP = outb.Count;
                    foreach (int[] v in blk.Verts) { outb.Add((byte)v[2]); outb.Add((byte)(v[2] >> 8)); }
                    AddZeros(outb, Align4(outb.Count) + LmArraySlack);

                    int normP = outb.Count;
                    foreach (int[] n in blk.Norms)
                    {
                        outb.Add((byte)(sbyte)n[0]); outb.Add((byte)(sbyte)n[1]);
                        outb.Add((byte)(sbyte)n[2]); outb.Add((byte)n[3]);
                    }
                    AddZeros(outb, LmArraySlack);

                    int cnt = blk.Shade.Count;
                    int unkP;
                    if (cnt != 0)
                    {
                        unkP = outb.Count;
                        foreach (int slot in blk.Shade) outb.Add(SlotByte(slot));
                    }
                    else
                    {
                        // Stock relation unkP = normP + 4*normalCount; with count 0 the pointer is
                        // never dereferenced beyond address formation.
                        unkP = normP + 4 * blk.Norms.Count;
                    }
                    AddZeros(outb, ShadePad(cnt));

                    meshInfo[k] = new[] { blk.Prims.Count, blk.Verts.Count, blk.Norms.Count, cnt,
                                          primsP, xyP, zP, normP, unkP };
                }
                for (int k = 0; k < m.MeshCount; k++)
                {
                    int hb = meshHdrsP2 + 24 * k;
                    int[] info = meshInfo[k];
                    outb[hb] = SlotByte(info[0]);
                    outb[hb + 1] = SlotByte(info[1]);
                    outb[hb + 2] = SlotByte(info[2]);
                    outb[hb + 3] = SlotByte(info[3]);
                    for (int a = 0; a < 5; a++) PutU32(outb, hb + 4 + a * 4, info[4 + a]);
                }
                int mhb = hdrsP + 16 * mi;
                if (m.HdrOff < 0 || m.HdrOff + 8 > data.Length)
                    throw new IlmException("replace: part '" + m.Name + "' header runs past the end of the source ILM");
                for (int i = 0; i < 8; i++) outb[mhb + i] = data[m.HdrOff + i];
                outb[mhb + 8] = (byte)m.MeshCount;
                outb[mhb + 9] = SlotByte(vO2[mi]);
                outb[mhb + 10] = SlotByte(nO2[mi]);
                outb[mhb + 11] = (byte)m.Bits;
                PutU32(outb, mhb + 0xC, meshHdrsP2);
            }
            return outb.ToArray();
        }

        /// <summary>Rebuild every part's geometry from the OBJ, keeping the template's rig.
        ///
        /// Unlike grow-mode this does not need the original vertices or faces to survive: counts,
        /// topology and material assignment all come from the OBJ. What still comes from the
        /// template ILM is everything whose meaning is unknown or runtime-owned — the 24-byte
        /// material blocks, modelOrder, ModelHeader byte 0xB, the align4(modelCount) reserved
        /// block, and each primitive's non-index bytes (clut, flags, the runtime's tpage low byte,
        /// materialIdx 127) taken from a primitive of the SAME material name.
        ///
        /// Seams are re-derived rather than declared: coincident vertices in different parts
        /// collapse onto the earliest-drawn part's pool slot, which is the only welding mechanism
        /// the format has (no skinning, no weights — a corner that reads a foreign slot simply
        /// follows that part's bone).</summary>
        // ---- v7 high-poly import (mirrors ilm_obj.py _wide_import / _emit_wide_ilm) ---------
        //
        // A v7 file is a narrow v6-shaped spine the game binds against unchanged (meshCount=0 on
        // every part, so the stock draw walkers are no-ops) plus a PC-only u32 geometry blob a
        // dedicated wide drawer consumes. No 256-slot pool, no decimation, no seam weld — each
        // part carries its own vertices, normals and prims at full density.

        private const uint TRI_SENTINEL32 = 0xFFFFFFFFu;

        private class WidePrim { public int[] Vtx, Nrm, Uv; public int Clut, Flags; }

        private class WidePart
        {
            public int Ordinal, Bone;
            public List<int[]> Verts, Norms;
            public List<WidePrim> Prims;
        }

        /// <summary>Serialize a v7 high-poly ILM. Spine (materials, modelOrder, part names and the
        /// 0xB draw-chain bits) is copied verbatim from the donor so the rig/ANM/material paths run
        /// on a real s_LmHeader; the wide blob follows in ascending model index.</summary>
        private static byte[] EmitWideIlm(byte[] data, Ilm ilm, List<WidePart> wideParts)
        {
            var o = new List<byte>(data.Length);
            for (int i = 0; i < 20; i++) o.Add(data[i]);
            o[1] = 7;                        // version 7 is the only discriminator (v6 pinned by the validator)
            o[2] = 0;                        // isLoaded: a 1 makes the runtime skip pointer fix-up and crash
            o[9] = 0; o[10] = 0; o[11] = 0;
            for (int i = 0; i < 12; i++) o.Add(0);   // v7 header extension, filled in once offsets are known

            int matsP = o.Count;
            for (int i = 0; i < 24 * ilm.MatCount; i++) o.Add(data[ilm.MatsP + i]);
            int orderP = o.Count;
            for (int i = 0; i < ilm.ModelCount; i++) o.Add(data[ilm.ModelOrderP + i]);
            for (int i = Align4(o.Count); i > 0; i--) o.Add(0);
            int hdrsP = o.Count;
            for (int i = 0; i < 16 * ilm.ModelCount; i++) o.Add(0);
            int zeroMeshP = o.Count;
            for (int i = 0; i < 24; i++) o.Add(0);
            for (int i = Align4(o.Count); i > 0; i--) o.Add(0);

            foreach (Model m in ilm.Models)
            {
                int hb = hdrsP + 16 * m.Idx;
                for (int i = 0; i < 8; i++) o[hb + i] = data[m.HdrOff + i];  // name = rig binding
                o[hb + 8] = 0;                       // meshCount 0 -> stock walkers are no-ops
                o[hb + 9] = 0;                       // vertexOffset (spine never drawn natively)
                o[hb + 10] = 0;                      // normalOffset
                o[hb + 11] = (byte)m.Bits;           // field_B draw-chain selector, for parity
                o[hb + 0xC] = (byte)zeroMeshP; o[hb + 0xD] = (byte)(zeroMeshP >> 8);
                o[hb + 0xE] = (byte)(zeroMeshP >> 16); o[hb + 0xF] = (byte)(zeroMeshP >> 24);
            }

            int wideP = o.Count;
            foreach (WidePart wp in wideParts)
            {
                A32(o, wp.Ordinal);
                o.Add((byte)wp.Bone); o.Add(0); o.Add(0); o.Add(0);   // bone + 3 pad
                A32(o, 1);                                            // one mesh per part
                A32(o, wp.Verts.Count); A32(o, wp.Norms.Count); A32(o, wp.Prims.Count);
                foreach (int[] v in wp.Verts) { A16(o, v[0]); A16(o, v[1]); A16(o, v[2]); A16(o, 0); }
                foreach (int[] n in wp.Norms) { A16(o, n[0]); A16(o, n[1]); A16(o, n[2]); A16(o, 0); }
                foreach (WidePrim pr in wp.Prims)
                {
                    for (int i = 0; i < 4; i++) A32(o, pr.Vtx[i]);
                    for (int i = 0; i < 4; i++) A32(o, pr.Nrm[i]);
                    for (int i = 0; i < 4; i++) A16(o, pr.Uv[i]);
                    A16(o, pr.Clut); A16(o, pr.Flags);
                    A32(o, 0);                                        // pad
                }
            }
            int wideSize = o.Count - wideP;

            byte[] b = o.ToArray();
            W32(b, 4, matsP); W32(b, 0xC, hdrsP); W32(b, 0x10, orderP);
            W32(b, 0x14, wideP); W32(b, 0x18, wideSize); W32(b, 0x1C, ilm.ModelCount);
            return b;
        }

        private static void V7Import(string objPath, string ilmPath, string outIlmPath, byte[] data, Ilm ilm,
            ObjFile of, Dictionary<string, ObjObject> byName, int[][] poseR, int[][] poseT, double[][] invs,
            ImportResult res)
        {
            foreach (Model m in ilm.Models)
            {
                if (m.MeshCount != 1)
                    throw new IlmException("part '" + m.Name + "' has " + m.MeshCount.ToString(Inv) + " meshes; high-poly " +
                        "rebuild uses one mesh per part and cannot guess how to split the OBJ across them.");
                if (m.Meshes[0].UnkCount3 != 0)
                    throw new IlmException("part '" + m.Name + "' carries a shade stream; the high-poly format has no " +
                        "shade stream (per-normal advance is fixed at 1).");
            }

            // Non-fatal rig checks surfaced to the user (MISSING parts are already refused above by
            // the name match): a part with no faces draws nothing, and a part sitting far from where
            // the donor expects it is usually mis-assigned geometry or a left/right swap.
            WarnDisplacedParts(ilm, byName, of, poseR, poseT, res.Warnings);
            foreach (Model mm in ilm.Models)
            {
                ObjObject oz = byName[mm.Name];
                if (oz.Faces.Count == 0)
                    res.Warnings.Add("part '" + mm.Name + "' has no faces, so it will be invisible in game. Assign its " +
                        "geometry in Blender (parts sharing a bone can hold it) or it stays empty.");
            }

            var wideParts = new List<WidePart>();
            foreach (Model m in ilm.Models)   // ascending model index -> part ordinal
            {
                int mi = m.Idx;
                string name = m.Name;
                ObjObject oo = byName[name];
                int[] T = poseT[mi];
                double[] inv = invs[mi];
                if (inv == null)
                    throw new IlmException("part '" + name + "' rest-pose matrix is singular; cannot unbake geometry.");

                // Self-contained: a face may only cite vertices its own 'o' block declares, and the
                // wide vertex index is that line's position in the block.
                var vpos = new Dictionary<int, int>();
                for (int i = 0; i < oo.V.Count; i++) vpos[oo.V[i]] = i;

                var wideVerts = new List<int[]>();
                foreach (int vl in oo.V)
                {
                    double[] w = of.Verts[vl - 1];
                    double[] v = Unbake(inv, T, new double[] { w[0], YIn(w[1]), w[2] });
                    wideVerts.Add(new int[] {
                        S16Checked(v[0], "vertex X"), S16Checked(v[1], "vertex Y"), S16Checked(v[2], "vertex Z") });
                }

                Mesh me = m.Meshes[0];
                var buckets = new Dictionary<string, List<Prim>>(StringComparer.Ordinal);
                foreach (Prim p in me.Prims)
                {
                    string nm = MtlName(p, ilm.BaseClutY);
                    List<Prim> q;
                    if (!buckets.TryGetValue(nm, out q)) { q = new List<Prim>(); buckets[nm] = q; }
                    q.Add(p);
                }
                var legal = new List<string>(buckets.Keys);
                legal.Sort(StringComparer.Ordinal);
                var matpos = new Dictionary<string, int>(StringComparer.Ordinal);
                foreach (string k in buckets.Keys) matpos[k] = 0;

                var nlist = new List<int[]>();
                var ndedup = new Dictionary<string, int>(StringComparer.Ordinal);

                var widePrims = new List<WidePrim>();
                for (int fidx = 0; fidx < oo.Faces.Count; fidx++)
                {
                    ObjFace f = oo.Faces[fidx];
                    int[][] face = f.C;
                    int arity = face.Length;
                    if (arity != 3 && arity != 4)
                        throw new IlmException("part '" + name + "' face " + (fidx + 1).ToString(Inv) + " has " +
                            arity.ToString(Inv) + " corners; the format only has triangles and quads - triangulate " +
                            "or quadrangulate the mesh first.");
                    bool tri = arity == 3;
                    int[] loop = tri ? TriLoop : QuadLoop;

                    string mname = f.Mtl;
                    if (mname == null)
                    {
                        if (legal.Count != 1)
                            throw new IlmException("part '" + name + "' face " + (fidx + 1).ToString(Inv) + " has no " +
                                "usemtl and the part uses " + legal.Count.ToString(Inv) + " materials (" +
                                string.Join(", ", legal) + "). Material names encode the CLUT palette row, so a face " +
                                "without one cannot be placed.");
                        mname = legal[0];
                    }
                    List<Prim> bk;
                    if (!buckets.TryGetValue(mname, out bk))
                        throw new IlmException("part '" + name + "' face " + (fidx + 1).ToString(Inv) + " uses material '" +
                            mname + "'; a replacement may only use materials the part already had (" +
                            (legal.Count > 0 ? string.Join(", ", legal) : "none") + ") - the name encodes the CLUT " +
                            "palette row and there is nowhere to invent one.");
                    Prim tp = bk[Math.Min(matpos[mname], bk.Count - 1)];
                    matpos[mname] = matpos[mname] + 1;

                    int sent = unchecked((int)TRI_SENTINEL32);
                    var vtx = new int[] { sent, sent, sent, sent };
                    var nrm = new int[] { sent, sent, sent, sent };
                    var uv = new int[4];
                    double[] flat = null;
                    for (int i = 0; i < arity; i++)
                    {
                        int[] corner = face[loop[i]];
                        int vlc = corner[0], tlc = corner[1], nlc = corner[2];
                        int local;
                        if (!vpos.TryGetValue(vlc, out local))
                            throw new IlmException("part '" + name + "' face " + (fidx + 1).ToString(Inv) + " cites vertex " +
                                vlc.ToString(Inv) + " outside its own 'o' block - high-poly parts are self-contained " +
                                "(no cross-part welding), so each carries its own seam vertices.");
                        vtx[i] = local;
                        if (tlc == 0)
                            throw new IlmException("part '" + name + "' face " + (fidx + 1).ToString(Inv) + " has no UVs - " +
                                "unwrap the geometry (every primitive corner carries a texel).");
                        double[] uvc = of.Uvs[tlc - 1];
                        uv[i] = UvByte(uvc[0]) | (UvByte(1.0 - uvc[1]) << 8);
                        double[] nvec;
                        if (nlc != 0) nvec = of.Norms[nlc - 1];
                        else
                        {
                            if (flat == null)
                            {
                                double[] p0 = of.Verts[face[0][0] - 1];
                                double[] p1 = of.Verts[face[1][0] - 1];
                                double[] p2 = of.Verts[face[2][0] - 1];
                                double[] e1 = new double[] { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
                                double[] e2 = new double[] { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
                                flat = new double[] { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                    e1[0] * e2[1] - e1[1] * e2[0] };
                            }
                            nvec = flat;
                        }
                        int[] key = QuantNewNormal(inv, nvec);
                        if (key == null)
                            throw new IlmException("part '" + name + "': a face has a degenerate normal.");
                        string nk = key[0].ToString(Inv) + "," + key[1].ToString(Inv) + "," + key[2].ToString(Inv);
                        int nidx;
                        if (!ndedup.TryGetValue(nk, out nidx)) { nidx = nlist.Count; ndedup[nk] = nidx; nlist.Add(key); }
                        nrm[i] = nidx;
                    }
                    widePrims.Add(new WidePrim { Vtx = vtx, Nrm = nrm, Uv = uv, Clut = tp.Clut, Flags = tp.Flags });
                }
                wideParts.Add(new WidePart { Ordinal = mi, Bone = BoneOf(name), Verts = wideVerts, Norms = nlist, Prims = widePrims });
            }

            // The converter-side v7 validator (lm_validate_v7) is not ported; the game's runtime
            // parser (Pc_WideLm_Parse) re-validates and falls back to the invisible spine on any
            // malformed blob, so a bad file is non-corrupting rather than refused here.
            byte[] blob = EmitWideIlm(data, ilm, wideParts);
            File.WriteAllBytes(outIlmPath, blob);

            res.V7 = true;
            res.IlmPath = outIlmPath;
            res.Parts = wideParts.Count;
            int tv = 0, tn = 0, tpr = 0;
            foreach (WidePart wp in wideParts) { tv += wp.Verts.Count; tn += wp.Norms.Count; tpr += wp.Prims.Count; }
            res.Vertices = tv; res.Normals = tn; res.Prims = tpr;
            foreach (WidePart wp in wideParts)
                res.Report.Add(ilm.Models[wp.Ordinal].Name + ": " + wp.Verts.Count.ToString(Inv) + " verts, " +
                    wp.Prims.Count.ToString(Inv) + " faces");
        }

        private static void ReplaceImport(string objPath, string ilmPath, string outIlmPath, byte[] data, Ilm ilm,
            ObjFile of, Dictionary<string, ObjObject> byName, int[][] poseR, int[][] poseT, double[][] invs,
            bool weld, double weldEps, ImportResult res)
        {
            int mc = ilm.ModelCount;
            WarnDisplacedParts(ilm, byName, of, poseR, poseT, res.Warnings);

            if (ilm.ModelOrderP < 0 || (long)ilm.ModelOrderP + mc > ilm.D.Length)
                throw new IlmException("replace: the modelOrder table runs past the end of the file.");
            var order = new int[mc];
            for (int i = 0; i < mc; i++) order[i] = ilm.D[ilm.ModelOrderP + i];
            var sortedOrder = (int[])order.Clone();
            Array.Sort(sortedOrder);
            for (int i = 0; i < mc; i++)
                if (sortedOrder[i] != i)
                    throw new IlmException("replace: modelOrder is not a permutation, so the draw order that decides " +
                        "seam ownership cannot be trusted (no stock file does this).");

            if (weld && res.RestPoseIdentity)
            {
                // Without a rest pose every part sits on the origin, so "coincident" stops meaning
                // "meets at a joint" and welds the model to itself.
                res.WeldNeedsRestPose = true;
                throw new IlmException("this OBJ was exported without a rest pose, so every part is piled on the " +
                    "origin and coincidence no longer means adjacency - auto-weld would fuse unrelated parts. " +
                    "Re-export with the model's animation file, or rebuild without welding.");
            }

            var dpos = new int[mc];
            for (int i = 0; i < mc; i++) dpos[order[i]] = i;
            var names = new string[mc];
            foreach (Model m in ilm.Models) names[m.Idx] = m.Name;

            foreach (Model m in ilm.Models)
            {
                if (m.MeshCount != 1)
                    throw new IlmException("part '" + m.Name + "' has " + m.MeshCount.ToString(Inv) + " meshes; " +
                        "replacement rebuilds one mesh per part and cannot guess how to split the OBJ across them.");
                foreach (Mesh me in m.Meshes)
                    foreach (Prim p in me.Prims)
                    {
                        int n = p.Tri ? 3 : 4;
                        for (int i = 0; i < n; i++)
                            if (!p.VRef[i].Valid || !p.NRef[i].Valid)
                                throw new IlmException("part '" + m.Name + "' reads a pool slot no part has written " +
                                    "(a held-item/prop file resolves those against the wielder's pool at draw time) - " +
                                    "replacement requires a fully self-contained skeletal file.");
                    }
            }

            List<ShadeEdge>[] shadeMap = ShadeEdges(ilm, order);
            foreach (int mi in order)
            {
                List<ShadeEdge> lst = shadeMap[mi];
                if (lst == null) continue;
                foreach (ShadeEdge e in lst)
                    if (!e.Ref.Valid)
                        throw new IlmException("part '" + names[mi] + "' shade stream reads an unwritten pool slot - " +
                            "replacement requires a fully self-contained skeletal file.");
            }

            // Ownership of a vertex line is fixed by the 'o' block that declares it; the weld below
            // only decides whose POOL SLOT it ends up in.
            var linePart = new Dictionary<int, int>();
            foreach (Model m in ilm.Models)
                foreach (int vl in byName[m.Name].V) linePart[vl] = m.Idx;

            // Weld, walking parts in draw order against a grid of the vertices earlier parts have
            // already claimed. Owners are therefore always drawn earlier by construction — the
            // direction the mechanic forces (verified over 20 stock files: all 2969 cross-part
            // corner references point backwards in modelOrder, zero exceptions). Each part is added
            // to the grid only after its own lines are resolved, so nothing ever welds to itself.
            var redirect = new Dictionary<int, int>();
            var owned = new List<int>[mc];
            var weldOrder = new List<string>();
            var weldPairs = new Dictionary<string, WeldPairInfo>(StringComparer.Ordinal);
            var crossOrder = new List<string>();
            var crossers = new Dictionary<string, CrossedBoneInfo>(StringComparer.Ordinal);
            bool hasNearMiss = false;
            double nearMiss = 0.0;
            // One cell size for both add and query, sized for the near-miss radius — the wider of
            // the two, since a near miss is searched for and reported but never accepted.
            var grid = new Grid(weldEps * NearMissFactor);

            foreach (int mi in order)
            {
                var mine = new List<int>();
                foreach (int vl in byName[names[mi]].V)
                {
                    double[] w = of.Verts[vl - 1];
                    int best = -1;
                    double bestd = 0.0;
                    if (weld)
                    {
                        // Search a wider radius than we accept, purely so a modeller who placed a
                        // seam by eye gets told to widen the radius instead of silently shipping a
                        // gap that only opens when the limb rotates.
                        foreach (int cand in grid.Near(w))
                        {
                            double d = Dist2(w, of.Verts[cand - 1]);
                            if (d <= weldEps * weldEps)
                            {
                                if (best < 0 || d < bestd) { best = cand; bestd = d; }
                            }
                            else if (!hasNearMiss || d < nearMiss) { hasNearMiss = true; nearMiss = d; }
                        }
                    }
                    if (best < 0) { mine.Add(vl); continue; }

                    redirect[vl] = best;
                    string ownerName = names[linePart[best]];
                    string key = names[mi] + "\0" + ownerName;
                    WeldPairInfo wp;
                    if (!weldPairs.TryGetValue(key, out wp))
                    {
                        wp = new WeldPairInfo { Reader = names[mi], Owner = ownerName };
                        weldPairs[key] = wp;
                        weldOrder.Add(key);
                    }
                    wp.Count++;
                    // A weld that both MOVES a vertex and hands it to a different BONE is the only
                    // kind that can change how the model animates; between parts sharing a bone the
                    // matrix is the same one, and a zero-distance weld does not move anything.
                    if (bestd > 0.0 && BoneOf(names[mi]) != BoneOf(ownerName))
                    {
                        CrossedBoneInfo cb;
                        if (!crossers.TryGetValue(key, out cb))
                        {
                            cb = new CrossedBoneInfo { Reader = names[mi], Owner = ownerName };
                            crossers[key] = cb;
                            crossOrder.Add(key);
                        }
                        cb.Count++;
                        double dd = Math.Sqrt(bestd);
                        if (dd > cb.MaxDistance) cb.MaxDistance = dd;
                    }
                }
                owned[mi] = mine;
                foreach (int vl in mine) grid.Add(of.Verts[vl - 1], vl);
            }

            var vlocal = new Dictionary<int, VLoc>();
            foreach (int mi in order)
                for (int j = 0; j < owned[mi].Count; j++)
                    vlocal[owned[mi][j]] = new VLoc { Mi = mi, J = j };

            // Vertex bytes. An unmoved vertex keeps the template's exact s16 triple rather than
            // being round-tripped through the inverse matrix, the same contract the patch-in-place
            // path gives — so an unedited replacement carries the original geometry bytes, not a
            // re-quantisation of them.
            //
            // The original-to-new map follows the weld, and has to: a shade stream names the
            // template's vertices, and a vertex of its own part can perfectly well end up owned by
            // a neighbour (CKN.ILM does exactly that).
            var newVerts = new List<int[]>[mc];
            var orig2new = new Dictionary<int, VLoc>[mc];
            foreach (int mi in order)
            {
                newVerts[mi] = new List<int[]>();
                orig2new[mi] = new Dictionary<int, VLoc>();
                int[] R = poseR[mi], T = poseT[mi];
                double[] inv = invs[mi];
                Mesh me = ilm.Models[mi].Meshes[0];
                var og = new Grid(UnmovedEps);
                for (int j = 0; j < me.VertexCount; j++)
                {
                    double[] b = Bake(R, T, me.Vx[j], me.Vy[j], me.Vz[j]);
                    og.Add(new[] { b[0], YOut(b[1]), b[2] }, j);
                }
                foreach (int vl in byName[names[mi]].V)
                {
                    double[] w = of.Verts[vl - 1];
                    int hit = -1;
                    foreach (int cand in og.Near(w))
                        if (!Moved(w, Bake(R, T, me.Vx[cand], me.Vy[cand], me.Vz[cand]))) { hit = cand; break; }

                    if (vlocal.ContainsKey(vl))
                    {
                        if (hit >= 0) newVerts[mi].Add(new int[] { me.Vx[hit], me.Vy[hit], me.Vz[hit] });
                        else
                        {
                            if (inv == null)
                                throw new IlmException("part '" + names[mi] + "' rest-pose matrix is singular; cannot " +
                                    "unbake replacement geometry");
                            double[] v = Unbake(inv, T, new[] { w[0], YIn(w[1]), w[2] });
                            newVerts[mi].Add(new int[] { S16Checked(v[0], "vertex X"), S16Checked(v[1], "vertex Y"),
                                                         S16Checked(v[2], "vertex Z") });
                        }
                    }
                    if (hit >= 0 && !orig2new[mi].ContainsKey(hit))
                    {
                        int tgt;
                        if (!redirect.TryGetValue(vl, out tgt)) tgt = vl;
                        orig2new[mi][hit] = vlocal[tgt];
                    }
                }
            }

            // Normal directions of the template, in OBJ space, for the unmoved test.
            var origDirs = new double[mc][][];
            foreach (int mi in order)
            {
                Mesh me = ilm.Models[mi].Meshes[0];
                int[] R = poseR[mi];
                origDirs[mi] = new double[me.NormalCount][];
                for (int j = 0; j < me.NormalCount; j++)
                    origDirs[mi][j] = Unit(NormalOut(BakeDir(R, me.Nx[j], me.Ny[j], me.Nz[j])));
            }

            var usedOrig = new HashSet<int>[mc];      // template normal indices that survived
            var freshOrder = new List<int[]>[mc];     // newly quantised triples, first-use order
            var freshMap = new Dictionary<int, int>[mc];
            for (int i = 0; i < mc; i++)
            {
                usedOrig[i] = new HashSet<int>();
                freshOrder[i] = new List<int[]>();
                freshMap[i] = new Dictionary<int, int>();
            }

            // Normal ownership follows VERTEX ownership — verified exactly true of the corpus: all
            // 260215 corners in the 257 shipped files have the same owner for their vertex slot and
            // their normal slot.
            Func<int, double[], NormKey> normalKey = (owner, wObj) =>
            {
                double[] d = Unit(wObj);
                if (d == null)
                    throw new IlmException("part '" + names[owner] + "': a face has a degenerate normal");
                int hit = -1;
                double hitd = 0.0;
                double[][] od = origDirs[owner];
                for (int j = 0; j < od.Length; j++)
                {
                    if (od[j] == null) continue;
                    double e = Math.Max(Math.Max(Math.Abs(d[0] - od[j][0]), Math.Abs(d[1] - od[j][1])),
                                        Math.Abs(d[2] - od[j][2]));
                    if (e <= NormalUnmovedEps && (hit < 0 || e < hitd)) { hit = j; hitd = e; }
                }
                if (hit >= 0)
                {
                    usedOrig[owner].Add(hit);
                    return new NormKey { Fresh = false, A = hit };
                }
                int[] q = QuantNewNormal(invs[owner], wObj);
                if (q == null)
                    throw new IlmException("part '" + names[owner] + "': a face has a degenerate normal");
                int pk = ((q[0] + 128) << 16) | ((q[1] + 128) << 8) | (q[2] + 128);
                if (!freshMap[owner].ContainsKey(pk))
                {
                    freshMap[owner][pk] = freshOrder[owner].Count;
                    freshOrder[owner].Add(q);
                }
                return new NormKey { Fresh = true, A = q[0], B = q[1], C = q[2] };
            };

            // Faces -> primitive plans. Corner order is the FT4 strip/loop involution, exactly as
            // the other two import paths use it.
            var plans = new List<PlanPrim>[mc];
            foreach (int mi in order)
            {
                ObjObject o = byName[names[mi]];
                var plan = new List<PlanPrim>();
                for (int fidx = 0; fidx < o.Faces.Count; fidx++)
                {
                    int[][] face = o.Faces[fidx].C;
                    int arity = face.Length;
                    if (arity != 3 && arity != 4)
                        throw new IlmException("part '" + names[mi] + "' face " + (fidx + 1).ToString(Inv) + " has " +
                            arity.ToString(Inv) + " corners; the format only has triangles and quads - triangulate or " +
                            "quadrangulate the mesh first.");
                    int[] loop = arity == 3 ? TriLoop : QuadLoop;
                    var corners = new PlanCorner[arity];
                    double[] flat = null;
                    for (int i = 0; i < arity; i++)
                    {
                        int vl = face[loop[i]][0], tl = face[loop[i]][1], nl = face[loop[i]][2];
                        VLoc loc;
                        int tgt;
                        if (!redirect.TryGetValue(vl, out tgt)) tgt = vl;
                        if (!vlocal.TryGetValue(tgt, out loc))
                            throw new IlmException("part '" + names[mi] + "' cites vertex " + vl.ToString(Inv) +
                                ", which no 'o' block declares.");
                        if (dpos[loc.Mi] > dpos[mi])
                            throw new IlmException("part '" + names[mi] + "' face " + (fidx + 1).ToString(Inv) +
                                " welds to part '" + names[loc.Mi] + "', which draws LATER (position " +
                                dpos[loc.Mi].ToString(Inv) + " vs " + dpos[mi].ToString(Inv) + "). A pool slot only " +
                                "survives forwards, so the owner must be the earlier-drawn part - move the geometry, " +
                                "or rebuild without welding.");
                        if (tl == 0)
                            throw new IlmException("part '" + names[mi] + "' face " + (fidx + 1).ToString(Inv) +
                                " has no UVs - unwrap the geometry (every primitive corner carries a texel).");
                        if (tl < 0 || tl > of.Uvs.Count)
                            throw new IlmException("part '" + names[mi] + "' face " + (fidx + 1).ToString(Inv) +
                                " references vt " + tl.ToString(Inv) + " but the OBJ has only " +
                                of.Uvs.Count.ToString(Inv) + " 'vt' lines.");

                        double[] nvec;
                        if (nl != 0)
                        {
                            if (nl < 0 || nl > of.Norms.Count)
                                throw new IlmException("part '" + names[mi] + "' face " + (fidx + 1).ToString(Inv) +
                                    " references vn " + nl.ToString(Inv) + " but the OBJ has only " +
                                    of.Norms.Count.ToString(Inv) + " 'vn' lines.");
                            nvec = of.Norms[nl - 1];
                        }
                        else
                        {
                            if (flat == null)
                            {
                                // One flat normal per face, from OBJ corners 0,1,2 in OBJ order.
                                double[] p0 = VertAt(of, face[0][0]), p1 = VertAt(of, face[1][0]), p2 = VertAt(of, face[2][0]);
                                double e10 = p1[0] - p0[0], e11 = p1[1] - p0[1], e12 = p1[2] - p0[2];
                                double e20 = p2[0] - p0[0], e21 = p2[1] - p0[1], e22 = p2[2] - p0[2];
                                flat = new[] { e11 * e22 - e12 * e21, e12 * e20 - e10 * e22, e10 * e21 - e11 * e20 };
                            }
                            nvec = flat;
                        }
                        double[] uv = of.Uvs[tl - 1];
                        corners[i] = new PlanCorner
                        {
                            Own = loc.Mi, Loc = loc.J, NK = normalKey(loc.Mi, nvec),
                            U = UvByte(uv[0]), V = UvByte(1.0 - uv[1])
                        };
                    }
                    plan.Add(new PlanPrim { Tri = arity == 3, Mtl = o.Faces[fidx].Mtl, Corners = corners });
                }
                plans[mi] = plan;
            }

            var nlocal = new Dictionary<NormKey, int>[mc];
            var newNorms = new List<int[]>[mc];
            foreach (int mi in order)
            {
                Mesh me = ilm.Models[mi].Meshes[0];
                // Matched originals first, in the template's own order, so an unedited replacement
                // reproduces the normal array instead of permuting it.
                var idx = new Dictionary<NormKey, int>();
                var lst = new List<int[]>();
                var keys = new List<int>(usedOrig[mi]);
                keys.Sort();
                foreach (int j in keys)
                {
                    idx[new NormKey { Fresh = false, A = j }] = lst.Count;
                    lst.Add(new int[] { me.Nx[j], me.Ny[j], me.Nz[j], me.NCount[j] });
                }
                foreach (int[] q in freshOrder[mi])
                {
                    idx[new NormKey { Fresh = true, A = q[0], B = q[1], C = q[2] }] = lst.Count;
                    lst.Add(new int[] { q[0], q[1], q[2], 1 });
                }
                nlocal[mi] = idx;
                newNorms[mi] = lst;
            }

            // Slot lifetimes: a slot lives from its writer's draw position to the last position
            // that reads it, counting the shade stream as a reader.
            var vlife = new int[mc][];
            var nlife = new int[mc][];
            foreach (int mi in order)
            {
                vlife[mi] = new int[newVerts[mi].Count];
                for (int j = 0; j < vlife[mi].Length; j++) vlife[mi][j] = dpos[mi];
                nlife[mi] = new int[newNorms[mi].Count];
                for (int j = 0; j < nlife[mi].Length; j++) nlife[mi][j] = dpos[mi];
            }
            foreach (int mi in order)
                foreach (PlanPrim pl in plans[mi])
                    foreach (PlanCorner c in pl.Corners)
                    {
                        if (vlife[c.Own][c.Loc] < dpos[mi]) vlife[c.Own][c.Loc] = dpos[mi];
                        int nl = nlocal[c.Own][c.NK];
                        if (nlife[c.Own][nl] < dpos[mi]) nlife[c.Own][nl] = dpos[mi];
                    }

            var newShade = new List<VLoc>[mc];
            foreach (int mi in order)
            {
                var refs = new List<VLoc>();
                List<ShadeEdge> lst = shadeMap[mi];
                if (lst != null)
                    foreach (ShadeEdge e in lst)
                    {
                        VLoc hit;
                        if (!orig2new[e.Ref.Model].TryGetValue(e.Ref.Local, out hit))
                            throw new IlmException("part '" + names[mi] + "' has a shade stream whose byte " +
                                e.ByteIdx.ToString(Inv) + " points at a vertex of part '" + names[e.Ref.Model] +
                                "' that the replacement removed. Shade streams are per-vertex and cannot be " +
                                "re-derived - keep that vertex, or replace a model without one.");
                        refs.Add(hit);
                        if (vlife[hit.Mi][hit.J] < dpos[mi]) vlife[hit.Mi][hit.J] = dpos[mi];
                    }
                newShade[mi] = refs;
            }

            foreach (int mi in order)
                for (int what = 0; what < 3; what++)
                {
                    int n = what == 0 ? newVerts[mi].Count : what == 1 ? newNorms[mi].Count : plans[mi].Count;
                    if (n <= 255) continue;
                    string kind = what == 0 ? "vertices" : what == 1 ? "normals" : "primitives";
                    throw new IlmException("part '" + names[mi] + "' has " + n.ToString(Inv) + " " + kind + "; the " +
                        "count is a u8 and the ceiling is 255. Split the detail across parts, or decimate.");
                }

            var vcnt = new int[mc];
            var ncnt = new int[mc];
            var nbound = new int[mc];
            foreach (int mi in order)
            {
                vcnt[mi] = newVerts[mi].Count;
                ncnt[mi] = newNorms[mi].Count;
                int walk = 0;
                foreach (int[] n in newNorms[mi]) walk += n[3] != 0 ? n[3] : 1;
                nbound[mi] = Math.Max(ncnt[mi], Math.Max(newShade[mi].Count, walk));
            }

            var keepV = new int[mc];
            var keepN = new int[mc];
            int pinnedCount = 0;
            foreach (Model m in ilm.Models)
            {
                // V12: the third draw chain ignores window offsets, so those parts are pinned to 0
                // and may not be relocated.
                if (((m.Bits >> 4) & 3) != 0)
                {
                    if (m.VertexOffset != 0 || m.NormalOffset != 0)
                        throw new IlmException("part '" + m.Name + "': field_B_4=" + (((m.Bits >> 4) & 3)).ToString(Inv) +
                            " with window offsets " + m.VertexOffset.ToString(Inv) + "/" + m.NormalOffset.ToString(Inv) +
                            " violates V12 in the ORIGINAL file - fix the source ILM first.");
                    keepV[m.Idx] = 0; keepN[m.Idx] = 0;
                    pinnedCount++;
                }
                else { keepV[m.Idx] = m.VertexOffset; keepN[m.Idx] = m.NormalOffset; }
            }

            string vtag, ntag;
            int[] vO2 = Allocate(LmVertSlotCap, vcnt, vlife, null, keepV, "vertex", order, dpos, names, pinnedCount, out vtag);
            int[] nO2 = Allocate(LmPoolSlots, ncnt, nlife, nbound, keepN, "normal", order, dpos, names, pinnedCount, out ntag);

            // Primitive bytes. Every prim inherits a template of the SAME material name, consumed
            // in order so an unedited replacement reuses each original's own bytes; the surplus of
            // a grown material falls back to that material's first.
            var parts = new List<PartBlock>[mc];
            var intentV = new Dictionary<int, int>();   // (mi,fidx,corner) -> owner<<16 | local
            var intentN = new Dictionary<int, int>();
            foreach (int mi in order)
            {
                Mesh me = ilm.Models[mi].Meshes[0];
                var buckets = new Dictionary<string, List<Prim>>(StringComparer.Ordinal);
                var bucketOrder = new List<string>();
                foreach (Prim p in me.Prims)
                {
                    string nm = MtlName(p, ilm.BaseClutY);
                    List<Prim> q;
                    if (!buckets.TryGetValue(nm, out q)) { q = new List<Prim>(); buckets[nm] = q; bucketOrder.Add(nm); }
                    q.Add(p);
                }
                var legal = new List<string>(bucketOrder);
                legal.Sort(StringComparer.Ordinal);
                var pos = new Dictionary<string, int>(StringComparer.Ordinal);
                foreach (string nm in bucketOrder) pos[nm] = 0;

                var blk = new PartBlock();
                for (int fidx = 0; fidx < plans[mi].Count; fidx++)
                {
                    PlanPrim pl = plans[mi][fidx];
                    string name = pl.Mtl;
                    if (name == null)
                    {
                        if (legal.Count != 1)
                            throw new IlmException("part '" + names[mi] + "' face " + (fidx + 1).ToString(Inv) +
                                " has no usemtl and the part uses " + legal.Count.ToString(Inv) + " materials (" +
                                string.Join(", ", legal.ToArray()) + "). Material names encode the CLUT palette row, " +
                                "so a face without one cannot be placed.");
                        name = legal[0];
                    }
                    List<Prim> bucket;
                    if (!buckets.TryGetValue(name, out bucket))
                        throw new IlmException("part '" + names[mi] + "' face " + (fidx + 1).ToString(Inv) +
                            " uses material '" + name + "'; a replacement may only use materials the part already " +
                            "had (" + (legal.Count == 0 ? "none" : string.Join(", ", legal.ToArray())) + ") - the " +
                            "name encodes the CLUT palette row and there is nowhere to invent one.", true);
                    Prim tp = bucket[Math.Min(pos[name], bucket.Count - 1)];
                    pos[name] = pos[name] + 1;

                    var raw = new byte[20];
                    Buffer.BlockCopy(data, tp.Off, raw, 0, 20);
                    for (int i = 0; i < pl.Corners.Length; i++)
                    {
                        PlanCorner c = pl.Corners[i];
                        int nslot = nO2[c.Own] + nlocal[c.Own][c.NK];
                        raw[0xC + i] = SlotByte(vO2[c.Own] + c.Loc);
                        raw[0x10 + i] = SlotByte(nslot);
                        W16(raw, UvOffsets[i], c.U | (c.V << 8));
                        int ik = (mi << 16) | (fidx << 3) | i;
                        intentV[ik] = (c.Own << 16) | c.Loc;
                        intentN[ik] = (c.Own << 16) | nlocal[c.Own][c.NK];
                    }
                    if (pl.Tri)
                    {
                        raw[0xC + 3] = TriSentinel;
                        raw[0x10 + 3] = 0;   // stock convention: 6109/6109 tris carry 0
                    }
                    blk.Prims.Add(raw);
                }
                blk.Verts = newVerts[mi];
                blk.Norms = newNorms[mi];
                foreach (VLoc s in newShade[mi]) blk.Shade.Add(vO2[s.Mi] + s.J);
                parts[mi] = new List<PartBlock> { blk };
            }

            byte[] blob = EmitIlm(data, ilm, parts, vO2, nO2);

            string anmPath;
            var anmWarn = new List<string>();
            Anm anm = FindAnm(ilmPath, ilm.Models, null, anmWarn, out anmPath);
            string vmsg;
            if (LmValidate(blob, 0, anm != null ? anm.BoneCount : -1, true, out vmsg) != 0)
                throw new IlmException("REFUSED " + vmsg);

            // Acceptance test on the EMITTED bytes: ResolvePool replays the pool in modelOrder, so
            // a prim's resolved reference is literally what the slot HOLDS at the moment that
            // primitive draws. Any weld broken by an intervening writer shows up here — and nowhere
            // else, since a static render cannot see it.
            Ilm ilm2 = ParseIlm(blob);
            ResolvePool(ilm2);
            foreach (int mi in order)
            {
                Prim[] prims2 = ilm2.Models[mi].Meshes[0].Prims;
                for (int fidx = 0; fidx < prims2.Length; fidx++)
                {
                    Prim p2 = prims2[fidx];
                    int n = p2.Tri ? 3 : 4;
                    for (int i = 0; i < n; i++)
                    {
                        int ik = (mi << 16) | (fidx << 3) | i;
                        int wantV = intentV[ik], wantN = intentN[ik];
                        PoolRef vr = p2.VRef[i], nr = p2.NRef[i];
                        bool ok = vr.Valid && nr.Valid && vr.Mesh == 0 && nr.Mesh == 0 &&
                                  ((vr.Model << 16) | vr.Local) == wantV && ((nr.Model << 16) | nr.Local) == wantN;
                        if (!ok)
                            throw new IlmException("replace internal error: part " + names[mi] + " prim " +
                                fidx.ToString(Inv) + " corner " + i.ToString(Inv) + " reads the wrong pool slot - " +
                                "the window layout clobbers a weld.");
                    }
                }
            }

            File.WriteAllBytes(outIlmPath, blob);

            int totV = 0, totN = 0, totP = 0;
            foreach (int mi in order) { totV += vcnt[mi]; totN += ncnt[mi]; totP += plans[mi].Count; }

            res.IlmPath = outIlmPath;
            res.Parts = mc;
            res.Vertices = totV;
            res.Normals = totN;
            res.Prims = totP;
            res.Replaced = true;
            res.WeldEnabled = weld;
            res.WeldEps = weldEps;

            res.Report.Add("   " + mc.ToString(Inv) + " parts, " + totV.ToString(Inv) + " vertices, " +
                totN.ToString(Inv) + " normals, " + totP.ToString(Inv) + " prims rebuilt");
            if (weld)
            {
                weldOrder.Sort(StringComparer.Ordinal);   // '\0' sorts below every name character
                int tot = 0;
                foreach (string k in weldOrder) tot += weldPairs[k].Count;
                res.WeldedVertices = tot;
                res.Report.Add("   welded " + tot.ToString(Inv) + " vertices across " +
                    weldOrder.Count.ToString(Inv) + " part pairs (eps " + PyG(weldEps) + "):");
                foreach (string k in weldOrder)
                {
                    WeldPairInfo wp = weldPairs[k];
                    res.WeldPairs.Add(wp);
                    res.Report.Add("      " + wp.Reader.PadRight(10) + " reads " + wp.Owner.PadRight(10) + "  " +
                        wp.Count.ToString(Inv));
                }
                if (weldOrder.Count == 0)
                    res.Report.Add("      (none - no coincident vertices in different parts)");
                if (hasNearMiss)
                {
                    double d = Math.Sqrt(nearMiss);
                    res.NearMissDistance = d;
                    res.Report.Add("   HINT: the closest UNWELDED pair of vertices in different parts is " +
                        FormatFixed(d, 3) + " units apart, just outside --weld-eps " + PyG(weldEps) + ". If those " +
                        "were meant to be one seam, snap them together in your modeller (exact is best) or re-run " +
                        "with --weld-eps " + FormatFixed(d * 1.01, 3) + ".");
                }
                crossOrder.Sort(StringComparer.Ordinal);
                foreach (string k in crossOrder)
                {
                    CrossedBoneInfo cb = crossers[k];
                    res.CrossedBones.Add(cb);
                    res.Report.Add("   NOTE: " + cb.Count.ToString(Inv) + " vertex/vertices of " + cb.Reader +
                        " moved up to " + FormatFixed(cb.MaxDistance, 3) + " units onto " + cb.Owner +
                        ", which is a DIFFERENT bone - they will now animate with " + cb.Owner +
                        ". Lower --weld-eps if that is not what you meant.");
                }
            }
            else
                res.Report.Add("   welding DISABLED: every part carries its own seam vertices, so the joints only " +
                    "stay closed while the parts overlap");

            int vext = 0, next = 0;
            foreach (int mi in order)
            {
                if (vO2[mi] + vcnt[mi] > vext) vext = vO2[mi] + vcnt[mi];
                if (nO2[mi] + ncnt[mi] > next) next = nO2[mi] + ncnt[mi];
            }
            res.Report.Add("   windows: vertex extent " + vext.ToString(Inv) + " / " + LmVertSlotCap.ToString(Inv) +
                " (" + vtag + "), normal extent " + next.ToString(Inv) + " / " + LmPoolSlots.ToString(Inv) +
                " (" + ntag + ")");
            res.Report.Add("   file: 0x" + blob.Length.ToString("X", Inv) + " (was 0x" + data.Length.ToString("X", Inv) + ")");
            if (res.RestPoseIdentity)
                res.Report.Add("   rest pose: IDENTITY (this OBJ was exported without an ANM)");
        }

        /// <summary>Try the three window-layout strategies in order. The template's own offsets
        /// first, so an unedited replacement reproduces the original file's windows; then plain
        /// first fit; then largest-part-first, which is skipped outright when any part is pinned to
        /// offset 0 because relocating a pinned part is not an option to try.</summary>
        private static int[] Allocate(int cap, int[] counts, int[][] life, int[] bound, int[] keep, string kind,
                                      int[] order, int[] dpos, string[] names, int pinnedCount, out string tag)
        {
            int mc = counts.Length;
            var big = new int[mc];
            Array.Copy(order, big, mc);
            var rank = new int[mc];
            for (int i = 0; i < mc; i++) rank[order[i]] = i;
            // Python's sorted() is stable, so equal counts keep draw order; List.Sort is not, hence
            // the explicit tiebreak.
            Array.Sort(big, (a, b) =>
            {
                int c = counts[b].CompareTo(counts[a]);
                return c != 0 ? c : rank[a].CompareTo(rank[b]);
            });

            int failMi = -1;
            List<int> failLive = null;
            for (int strategy = 0; strategy < 3; strategy++)
            {
                if (pinnedCount > 0 && strategy == 2) continue;
                int[] po = strategy == 2 ? big : order;
                int[] kp = strategy == 0 ? keep : null;
                int[] off = PoolAlloc(cap, po, dpos, counts, life, bound, kp, out failMi, out failLive);
                if (off != null)
                {
                    tag = strategy == 0 ? "template windows" : strategy == 1 ? "first fit" : "largest first";
                    return off;
                }
            }

            var hogs = new List<int>(failLive);
            // failLive is already ascending, and Python's stable sort keeps that order on a tie.
            hogs.Sort((a, b) =>
            {
                int c = counts[b].CompareTo(counts[a]);
                return c != 0 ? c : a.CompareTo(b);
            });
            var shown = new List<string>();
            for (int i = 0; i < hogs.Count && i < 6; i++)
                shown.Add(names[hogs[i]] + " (" + counts[hogs[i]].ToString(Inv) + ")");
            int need = Math.Max(counts[failMi], bound == null ? 0 : bound[failMi]);
            throw new IlmException("REFUSED: no " + kind + " window fits part '" + names[failMi] + "' (" +
                need.ToString(Inv) + " slots, cap " + cap.ToString(Inv) + "). The pool is held across its draw " +
                "position by " + (shown.Count == 0 ? "nothing" : string.Join(", ", shown.ToArray())) +
                (hogs.Count > 6 ? " and " + (hogs.Count - 6).ToString(Inv) + " more" : "") +
                ". Either shed geometry in those parts, or shorten the welds that keep their slots alive - a slot " +
                "cannot be reused until the last part that reads it has drawn.", true);
        }
    }
}
