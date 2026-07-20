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

        // A PSX FT4 quad is a triangle STRIP: func_8005AC50 copies the four indices straight
        // into POLY_GT4 x0..x3, which the GPU renders as (v0,v1,v2)+(v1,v2,v3). An OBJ `f` is
        // a polygon LOOP, so emitting 0,1,2,3 makes every quad cross itself — the bowtie that
        // shatters the model in Blender while leaving every edge LENGTH unchanged, so no spike
        // metric can see it. OBJ corner i carries prim corner QuadLoop[i]. Swapping 2 and 3 is
        // an involution, so the same table maps OBJ corners back to prim corners on import.
        private static readonly int[] QuadLoop = { 0, 1, 3, 2 };
        private static readonly int[] TriLoop = { 0, 1, 2 };

        private static int[] CornerOrder(Prim p) { return p.Tri ? TriLoop : QuadLoop; }

        private class IlmException : Exception { public IlmException(string m) : base(m) { } }

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
            public int PrimsP, XyP, ZP, NormP;
            public short[] Vx, Vy, Vz;
            public sbyte[] Nx, Ny, Nz;
            public byte[] NCount;
            public Prim[] Prims;
        }

        private class Model
        {
            public int Idx, MeshCount, VertexOffset, NormalOffset;
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
            foreach (string raw in File.ReadLines(path))
            {
                string ln = raw.Trim();
                if (ln.Length == 0 || ln[0] == '#') continue;
                int sp = ln.IndexOf(' ');
                string k = sp < 0 ? ln : ln.Substring(0, sp);
                string rest = sp < 0 ? "" : ln.Substring(sp + 1);

                if (k == "usemtl") { mtl = rest.Trim(); continue; }
                if (k == "o")
                {
                    cur = new ObjObject { Name = rest.Trim() };
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
                            "are matched back to primitives - reassigning or renaming materials is not supported.");
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
            var res = new ImportResult();
            try { ImportCore(objPath, ilmPath, outIlmPath, res); }
            catch (Exception ex) { res.Error = ex.Message; }
            return res;
        }

        public static bool Import(string objPath, string ilmPath, string outIlmPath, out string error)
        {
            ImportResult res = Import(objPath, ilmPath, outIlmPath);
            error = res.Error;
            return res.Error == null;
        }

        private static void ImportCore(string objPath, string ilmPath, string outIlmPath, ImportResult res)
        {
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
                        "aggressive 'Merge by Distance' - the seam duplicates are deliberate and must not be welded.");
                int want = 0;
                foreach (Mesh m in model.Meshes) want += m.PrimCount;
                if (obj.Faces.Count != want)
                    throw new IlmException("part '" + obj.Name + "' has " + obj.Faces.Count.ToString(Inv) + " faces but " +
                        "the ILM expects " + want.ToString(Inv) + " primitives. Adding or removing faces is not " +
                        "supported (counts are u8 and the bone/animation data lives in another file).");

                var gidx = new Dictionary<int, int>();
                for (int i = 0; i < ownV.Count; i++) gidx[RefKey(ownV[i])] = obj.V[i];
                for (int i = 0; i < forV.Count; i++) gidx[RefKey(forV[i])] = obj.V[ownV.Count + i];

                List<ObjFace> paired = PairFaces(model, obj, ilm.BaseClutY);

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
                                ". Triangulating or merging faces is not supported.");
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
                                    "face to a different material.");
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
    }
}
