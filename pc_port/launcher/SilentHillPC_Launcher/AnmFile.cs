using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>Thrown for pose-query misuse (bad bone index, undersized output arrays) and
    /// for a bone hierarchy that composes to an unusable matrix. Parsing and JSON import
    /// report through (null, out error) instead, so the launcher can show a message without
    /// a try/catch at every load site.</summary>
    public class AnmException : Exception
    {
        public AnmException(string message) : base(message) { }
    }

    /// <summary>
    /// Complete Silent Hill 1 .ANM animation file: parser, engine-exact pose math, and a
    /// lossless editable JSON round trip. The public twin of the private skeleton reader
    /// inside IlmObjConverter, which only needs one keyframe and stays private.
    ///
    /// Layout (little-endian, verified against the decomp and all 55 headered retail ANMs):
    ///
    ///   0x00 u16 dataOffset   404 in every retail file (0x14 + 64*6): the bind region is
    ///                         always sized for 64 slots and zero-padded past boneCount
    ///   0x02 u8  rotationBoneCount
    ///   0x03 u8  translationBoneCount
    ///   0x04 u16 keyframeDataSize   == rot*9 + trans*3, exact in every file
    ///   0x06 u8  boneCount
    ///   0x07 s8  pad7         0 in retail; preserved verbatim
    ///   0x08 u32 activeBones  bit i gates the NPC bone update; 0xFFFFFFFF in retail
    ///   0x0C u32 fileSize     == align4(dataOffset + keyframeDataSize*keyframeCount)
    ///   0x10 u16 keyframeCount
    ///   0x12 u8  scaleLog2    0..4 in retail
    ///   0x13 u8  rootYOffset  a u8, NOT signed: 255 on human rigs
    ///
    /// Bind poses, boneCount x 6 bytes at 0x14: s8 parentBone (root = -1), s8 rotation
    /// channel (-1 = static identity), s8 translation channel (-1 = static
    /// translationInitial &lt;&lt; scaleLog2), s8[3] translationInitial.
    ///
    /// Keyframes at dataOffset, keyframeDataSize bytes each: FIRST translationBoneCount x 3
    /// s8 (x,y,z per translation channel), THEN rotationBoneCount x 9 s8 — a 3x3 rotation
    /// matrix in ROW-MAJOR order whose value v is the q3.12 entry v&lt;&lt;5. These are raw
    /// matrix coefficients, NOT angles: 127 becomes 4064, never 4096, and the interpolated
    /// matrices are not orthonormal, so no slerp or renormalisation anywhere.
    ///
    /// BYTE IDENTITY CONTRACT: for an untouched Load -> ToJson -> FromJson -> ToBytes round
    /// trip, the first FileSize bytes equal the source file's first FileSize bytes exactly.
    /// Bytes beyond FileSize are disc-extraction sector slack and excluded. This is why
    /// pad7, activeBones and the 0-3 fileSize alignment pad bytes are all carried verbatim:
    /// 20 retail files put 0x00 0x77 / 0x00 0x77 0x88 in that pad, not zeros.
    /// </summary>
    public sealed class AnmFile
    {
        private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
        private const string Nl = "\r\n";

        private const int Q12 = 4096;
        private static readonly int[] IdentityR = { Q12, 0, 0, 0, Q12, 0, 0, 0, Q12 };

        /// <summary>0x14 + 64*6. Every retail file uses this layout and the emitter only
        /// writes it, so Parse refuses anything else: silently re-emitting a nonstandard
        /// layout as 404 would break the byte-identity contract without a word.</summary>
        public const int CanonicalDataOffset = 0x14 + 64 * 6;

        private const int HeaderSize = 0x14;

        /// <summary>The largest retail file is 113,684 bytes; this cap only exists so a
        /// hostile JSON (keyframeCount 65535 x a full channel set) cannot make the launcher
        /// allocate hundreds of megabytes before validation can object.</summary>
        private const int MaxFileSize = 16 * 1024 * 1024;

        // ---- header, verbatim ---------------------------------------------------

        public int DataOffset { get; private set; }             // always CanonicalDataOffset
        public int RotationBoneCount { get; private set; }
        public int TranslationBoneCount { get; private set; }
        public int KeyframeDataSize { get; private set; }
        public int BoneCount { get; private set; }
        public int Pad7 { get; private set; }                   // s8, preserved verbatim
        public uint ActiveBones { get; private set; }
        public int FileSize { get; private set; }
        public int KeyframeCount { get; private set; }
        public int ScaleLog2 { get; private set; }
        public int RootYOffset { get; private set; }            // u8 — see header comment

        /// <summary>The exact buffer the file was parsed from, unmodified, including any
        /// disc-extraction slack past FileSize. For a FromJson instance this is the
        /// re-emitted canonical bytes (== ToBytes()), since no source buffer exists.</summary>
        public byte[] RawData { get; private set; }

        // ---- bind poses, one entry per bone -------------------------------------

        public int[] Parent { get; private set; }
        public int[] RotIdx { get; private set; }
        public int[] TransIdx { get; private set; }
        public int[][] T0 { get; private set; }

        /// <summary>Raw s8 keyframe payload, KeyframeDataSize * KeyframeCount values. Kept
        /// as sbyte so every read site gets sign extension for free — reading these through
        /// a byte[] is exactly the bug that flips rootYOffset-sized values negative.</summary>
        private sbyte[] _kf;

        /// <summary>The 0-3 bytes between the last keyframe and fileSize, verbatim.</summary>
        public byte[] TailPad { get; private set; }

        private AnmFile() { }

        // ---- little-endian primitives -------------------------------------------

        private static int U16(byte[] d, int o) { return d[o] | (d[o + 1] << 8); }
        private static uint U32(byte[] d, int o)
        { return (uint)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)); }
        private static void W16(byte[] d, int o, int v) { d[o] = (byte)v; d[o + 1] = (byte)(v >> 8); }
        private static void W32(byte[] d, int o, uint v)
        { d[o] = (byte)v; d[o + 1] = (byte)(v >> 8); d[o + 2] = (byte)(v >> 16); d[o + 3] = (byte)(v >> 24); }

        private static long Align4(long x) { return (x + 3) & ~3L; }

        // ---- load / parse -------------------------------------------------------

        /// <summary>null + error on failure — an unreadable file, or bytes Parse refuses.</summary>
        public static AnmFile Load(string path, out string error)
        {
            byte[] data;
            try { data = File.ReadAllBytes(path); }
            catch (Exception ex)
            {
                error = path + ": " + ex.Message;
                return null;
            }
            AnmFile anm = Parse(data, out error);
            if (anm == null) error = Path.GetFileName(path) + ": " + error;
            return anm;
        }

        /// <summary>null + error on anything that is not a headered SH1 .ANM. Strict on
        /// purpose: the headerless HB_M*/HB_WEP* continuation blobs on the disc start
        /// straight at keyframe data and would otherwise parse as garbage headers.</summary>
        public static AnmFile Parse(byte[] data, out string error)
        {
            error = null;
            if (data == null || data.Length < HeaderSize)
            {
                error = "too short for an ANM header (20 bytes)";
                return null;
            }

            var a = new AnmFile();
            a.DataOffset = U16(data, 0x00);
            a.RotationBoneCount = data[0x02];
            a.TranslationBoneCount = data[0x03];
            a.KeyframeDataSize = U16(data, 0x04);
            a.BoneCount = data[0x06];
            a.Pad7 = (sbyte)data[0x07];
            a.ActiveBones = U32(data, 0x08);
            long fileSize = U32(data, 0x0C);
            a.KeyframeCount = U16(data, 0x10);
            a.ScaleLog2 = data[0x12];
            // rootYOffset is a u8: reading it signed flips 140/154/155/220/255 negative.
            a.RootYOffset = data[0x13];

            if (a.DataOffset != CanonicalDataOffset)
            {
                error = "dataOffset is " + a.DataOffset.ToString(Inv) + ", not 404 — not a headered ANM " +
                        "(the HB_* continuation blobs have no header)";
                return null;
            }
            if (a.KeyframeDataSize != a.RotationBoneCount * 9 + a.TranslationBoneCount * 3)
            {
                error = "keyframeDataSize " + a.KeyframeDataSize.ToString(Inv) + " does not match the channel counts (" +
                        a.RotationBoneCount.ToString(Inv) + " rot, " + a.TranslationBoneCount.ToString(Inv) + " trans)";
                return null;
            }
            if (a.KeyframeDataSize == 0)
            {
                error = "the file declares no animation channels";
                return null;
            }
            // 64 is a hard format bound, not a style choice: dataOffset 404 leaves room for
            // exactly 64 bind slots, so a larger boneCount reads keyframe bytes as binds.
            if (a.BoneCount < 1 || a.BoneCount > 64)
            {
                error = "boneCount " + a.BoneCount.ToString(Inv) + " is outside 1..64";
                return null;
            }
            if (a.KeyframeCount < 1)
            {
                error = "keyframeCount is 0";
                return null;
            }
            // scaleLog2 is only ever 0..4 in the corpus. C# masks a shift count to 5 bits
            // where the format's intent is unbounded, so a wild value would not merely
            // produce nonsense, it would produce DIFFERENT nonsense than the PSX — reject.
            if (a.ScaleLog2 >= 16)
            {
                error = "scaleLog2 " + a.ScaleLog2.ToString(Inv) + " is not a plausible scale (retail uses 0..4)";
                return null;
            }

            long rawEnd = a.DataOffset + (long)a.KeyframeDataSize * a.KeyframeCount;
            if (fileSize != Align4(rawEnd))
            {
                error = "fileSize " + fileSize.ToString(Inv) + " does not equal align4(dataOffset + keyframeDataSize*keyframeCount) = " +
                        Align4(rawEnd).ToString(Inv);
                return null;
            }
            if (fileSize > data.Length)
            {
                error = "fileSize " + fileSize.ToString(Inv) + " runs past the end of the buffer (" +
                        data.Length.ToString(Inv) + " bytes)";
                return null;
            }
            a.FileSize = (int)fileSize;

            a.Parent = new int[a.BoneCount];
            a.RotIdx = new int[a.BoneCount];
            a.TransIdx = new int[a.BoneCount];
            a.T0 = new int[a.BoneCount][];
            for (int i = 0; i < a.BoneCount; i++)
            {
                int o = HeaderSize + i * 6;
                a.Parent[i] = (sbyte)data[o];
                a.RotIdx[i] = (sbyte)data[o + 1];
                a.TransIdx[i] = (sbyte)data[o + 2];
                a.T0[i] = new int[] { (sbyte)data[o + 3], (sbyte)data[o + 4], (sbyte)data[o + 5] };
                // A non-negative channel index past its count would read another channel's
                // bytes (or the next keyframe) every frame; refuse rather than animate junk.
                if (a.RotIdx[i] >= a.RotationBoneCount)
                {
                    error = "bone " + i.ToString(Inv) + " rotation channel " + a.RotIdx[i].ToString(Inv) +
                            " is past rotationBoneCount " + a.RotationBoneCount.ToString(Inv);
                    return null;
                }
                if (a.TransIdx[i] >= a.TranslationBoneCount)
                {
                    error = "bone " + i.ToString(Inv) + " translation channel " + a.TransIdx[i].ToString(Inv) +
                            " is past translationBoneCount " + a.TranslationBoneCount.ToString(Inv);
                    return null;
                }
            }

            // ToBytes always zero-pads the bind region to 404, so non-zero bytes here could
            // never round-trip; every retail file is zero there. Refuse loudly instead of
            // silently re-emitting different bytes.
            for (int o = HeaderSize + a.BoneCount * 6; o < CanonicalDataOffset; o++)
                if (data[o] != 0)
                {
                    error = "bind region padding at 0x" + o.ToString("X", Inv) + " is not zero — the file cannot round-trip byte-identically";
                    return null;
                }

            int kfBytes = a.KeyframeDataSize * a.KeyframeCount;
            a._kf = new sbyte[kfBytes];
            for (int i = 0; i < kfBytes; i++) a._kf[i] = (sbyte)data[a.DataOffset + i];

            a.TailPad = new byte[a.FileSize - (int)rawEnd];
            for (int i = 0; i < a.TailPad.Length; i++) a.TailPad[i] = data[rawEnd + i];

            a.RawData = data;
            return a;
        }

        // ---- engine-exact pose math ---------------------------------------------

        /// <summary>Raw keyframe value at (keyframe, byte offset inside the keyframe) — for
        /// the two interpolation reads below. Offsets are validated by construction
        /// (channel indices are range-checked at parse), so no bounds check is needed.</summary>
        private int Kf(int kf, int off) { return _kf[kf * KeyframeDataSize + off]; }

        /// <summary>Local (R q12 row-major into outR9, T into outT3) for one bone between
        /// two keyframes — Anim_BoneUpdate, bit-exact. All shifts are ARITHMETIC floor
        /// shifts, which C# '>>' on int already is; never "clean up" to division, which
        /// truncates toward zero and diverges on every negative value.
        ///
        /// kf1 may equal kf0 (a held pose). Keyframe indices are clamped to
        /// [0, KeyframeCount-1] and alpha to [0, 4096] internally — only because reading
        /// past the payload would throw; callers are expected to pass validated ranges.
        ///
        /// Bone 0's channel data is returned as stored (it is root MOTION, useful to a
        /// viewer); WorldPose is where the engine's root override lives.</summary>
        public void LocalPose(int bone, int kf0, int kf1, int alphaQ12, int[] outR9, int[] outT3)
        {
            if (bone < 0 || bone >= BoneCount)
                throw new AnmException("bone " + bone.ToString(Inv) + " is outside 0.." + (BoneCount - 1).ToString(Inv));
            if (outR9 == null || outR9.Length < 9)
                throw new AnmException("outR9 must hold at least 9 ints");
            if (outT3 == null || outT3.Length < 3)
                throw new AnmException("outT3 must hold at least 3 ints");

            if (kf0 < 0) kf0 = 0; else if (kf0 >= KeyframeCount) kf0 = KeyframeCount - 1;
            if (kf1 < 0) kf1 = 0; else if (kf1 >= KeyframeCount) kf1 = KeyframeCount - 1;
            int alpha = alphaQ12;
            if (alpha < 0) alpha = 0; else if (alpha > Q12) alpha = Q12;

            int s = ScaleLog2;
            int ti = TransIdx[bone];
            if (ti < 0)
            {
                // Static path: NO rootYOffset here — the engine only subtracts it on the
                // animated slot-0 read.
                for (int i = 0; i < 3; i++) outT3[i] = T0[bone][i] << s;
            }
            else
            {
                int o = ti * 3;
                for (int i = 0; i < 3; i++)
                {
                    int f0 = Kf(kf0, o + i), f1 = Kf(kf1, o + i);
                    outT3[i] = (f0 << s) + (((f1 - f0) * alpha) >> (12 - s));
                }
                // Keyed on the DATA INDEX, not the bone index: humanoids share channel 0
                // across bones 0, 1 and 11, and every one of them gets the offset.
                if (ti == 0) outT3[1] -= RootYOffset;
            }

            int ri = RotIdx[bone];
            if (ri < 0)
            {
                for (int i = 0; i < 9; i++) outR9[i] = IdentityR[i];
            }
            else
            {
                int o = TranslationBoneCount * 3 + ri * 9;
                for (int i = 0; i < 9; i++)
                {
                    int f0 = Kf(kf0, o + i), f1 = Kf(kf1, o + i);
                    // Component-wise linear on raw matrix entries, f<<5 into q12. The
                    // interpolated matrix is NOT orthonormal and must not be normalised —
                    // the PSX renders exactly this drift.
                    outR9[i] = (f0 << 5) + (((f1 - f0) * alpha) >> 7);
                }
            }
        }

        /// <summary>World pose of every bone (Vw_CoordHierarchyMatrixCompute). outR / outT
        /// must be caller arrays of length &gt;= BoneCount; each slot receives a freshly
        /// allocated int[9] / int[3], so callers can hold poses from several calls at once.
        ///
        /// Bone 0 (root) is NEVER animated: Anim_BoneInit writes identity and both loops
        /// start at bone 1. Its keyframe translation is root MOTION — the game puts the
        /// character's map position there — so the world root here is identity R, zero T.
        ///
        /// A parent that is out of range or the bone itself is treated as a root, and
        /// parent cycles are broken with the bone treated as a root (the game only
        /// survives such tables via the COORD_PTR_OK guard in vw_calc.c). warnings, when
        /// non-null, receives one line per broken cycle.</summary>
        public void WorldPose(int kf0, int kf1, int alphaQ12, int[][] outR, int[][] outT)
        {
            WorldPose(kf0, kf1, alphaQ12, outR, outT, null);
        }

        public void WorldPose(int kf0, int kf1, int alphaQ12, int[][] outR, int[][] outT, List<string> warnings)
        {
            if (outR == null || outR.Length < BoneCount || outT == null || outT.Length < BoneCount)
                throw new AnmException("outR/outT must hold at least BoneCount (" + BoneCount.ToString(Inv) + ") slots");
            var state = new byte[BoneCount];
            for (int i = 0; i < BoneCount; i++) Resolve(i, kf0, kf1, alphaQ12, outR, outT, state, warnings);
        }

        /// <summary>Resolved recursively: the bind table is not guaranteed to list parents
        /// before children. state: 0 untouched, 1 resolving (a revisit means a cycle), 2 done.</summary>
        private void Resolve(int i, int kf0, int kf1, int alpha, int[][] wr, int[][] wt, byte[] state, List<string> warnings)
        {
            if (state[i] == 2) return;
            if (i == 0)
            {
                wr[0] = (int[])IdentityR.Clone();
                wt[0] = new int[3];
                state[0] = 2;
                return;
            }
            var R = new int[9];
            var T = new int[3];
            if (state[i] == 1)
            {
                if (warnings != null)
                    warnings.Add("bone " + i.ToString(Inv) + " is in a parent cycle; treated as a root");
                LocalPose(i, kf0, kf1, alpha, R, T);
                wr[i] = R; wt[i] = T; state[i] = 2;
                return;
            }
            state[i] = 1;
            LocalPose(i, kf0, kf1, alpha, R, T);
            int p = Parent[i];
            if (p >= 0 && p < BoneCount && p != i)
            {
                Resolve(p, kf0, kf1, alpha, wr, wt, state, warnings);
                int[] pr = wr[p], pt = wt[p];
                var nr = new int[9];
                for (int a = 0; a < 3; a++)
                    for (int b = 0; b < 3; b++)
                    {
                        // Arithmetic floor shift, matching the GTE — see LocalPose.
                        long acc = (long)pr[a * 3] * R[b] + (long)pr[a * 3 + 1] * R[3 + b] + (long)pr[a * 3 + 2] * R[6 + b];
                        long v = acc >> 12;
                        // A hostile bind table can make the composition diverge geometrically
                        // with chain depth and int would silently wrap. Real matrices sit at
                        // ~4096, so 2^20 is 256x margin.
                        if (v > (1 << 20) || v < -(1 << 20))
                            throw new AnmException("the ANM bone hierarchy composes to an unusable matrix");
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

        // ---- JSON export --------------------------------------------------------

        private const string Readme =
            "Silent Hill 1 .ANM as raw payload. Every value below is a SIGNED BYTE: edits must stay in [-128,127] " +
            "or the file cannot be rebuilt. keyframes[k].r holds one 3x3 rotation matrix per rotation channel, " +
            "ROW-MAJOR, as raw matrix coefficients (NOT angles): the engine uses value*32 as a q3.12 matrix entry, " +
            "so 127 -> 4064, never 4096. keyframes[k].t holds one [x,y,z] per translation channel; the engine " +
            "scales it by 1<<scaleLog2 at runtime and subtracts rootYOffset from Y on channel 0. " +
            "bones[i].rotChannel/transChannel index those arrays; -1 = no channel (identity rotation / " +
            "translationInitial<<scaleLog2). Playback interpolates every raw value component-wise linearly " +
            "between keyframes. activeBones and tailPad are hex; both, plus pad7, are preserved verbatim so an " +
            "untouched round trip stays byte-identical.";

        private static StringBuilder Ind(StringBuilder sb, int n) { return sb.Append(' ', n); }

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

        private static void JField(StringBuilder sb, string key, string rendered)
        {
            Ind(sb, 1).Append(JStr(key)).Append(": ").Append(rendered).Append(',').Append(Nl);
        }

        private static string HexBytes(byte[] b)
        {
            var sb = new StringBuilder(b.Length * 2);
            for (int i = 0; i < b.Length; i++) sb.Append(b[i].ToString("X2", Inv));
            return sb.ToString();
        }

        /// <summary>The canonical editable form: raw s8 values, header fields verbatim. One
        /// keyframe per line — LS.ANM has 619 keyframes of 183 values, so pretty-printing
        /// every number would make a 10 MB file no editor enjoys.</summary>
        public string ToJson()
        {
            var sb = new StringBuilder(1024 + FileSize * 5);
            sb.Append('{').Append(Nl);
            JField(sb, "_readme", JStr(Readme));
            JField(sb, "dataOffset", DataOffset.ToString(Inv));
            JField(sb, "rotationBoneCount", RotationBoneCount.ToString(Inv));
            JField(sb, "translationBoneCount", TranslationBoneCount.ToString(Inv));
            JField(sb, "keyframeDataSize", KeyframeDataSize.ToString(Inv));
            JField(sb, "boneCount", BoneCount.ToString(Inv));
            JField(sb, "pad7", Pad7.ToString(Inv));
            JField(sb, "activeBones", JStr(ActiveBones.ToString("X8", Inv)));
            JField(sb, "fileSize", FileSize.ToString(Inv));
            JField(sb, "keyframeCount", KeyframeCount.ToString(Inv));
            JField(sb, "scaleLog2", ScaleLog2.ToString(Inv));
            JField(sb, "rootYOffset", RootYOffset.ToString(Inv));
            JField(sb, "tailPad", JStr(HexBytes(TailPad)));

            Ind(sb, 1).Append("\"bones\": [").Append(Nl);
            for (int i = 0; i < BoneCount; i++)
            {
                Ind(sb, 2).Append("{\"parent\": ").Append(Parent[i].ToString(Inv))
                          .Append(", \"rotChannel\": ").Append(RotIdx[i].ToString(Inv))
                          .Append(", \"transChannel\": ").Append(TransIdx[i].ToString(Inv))
                          .Append(", \"translationInitial\": [").Append(T0[i][0].ToString(Inv)).Append(',')
                          .Append(T0[i][1].ToString(Inv)).Append(',').Append(T0[i][2].ToString(Inv)).Append("]}")
                          .Append(i + 1 < BoneCount ? "," : "").Append(Nl);
            }
            Ind(sb, 1).Append("],").Append(Nl);

            Ind(sb, 1).Append("\"keyframes\": [").Append(Nl);
            for (int k = 0; k < KeyframeCount; k++)
            {
                int baseOff = k * KeyframeDataSize;
                Ind(sb, 2).Append("{\"t\": [");
                for (int c = 0; c < TranslationBoneCount; c++)
                {
                    int o = baseOff + c * 3;
                    if (c > 0) sb.Append(',');
                    sb.Append('[').Append(((int)_kf[o]).ToString(Inv)).Append(',')
                      .Append(((int)_kf[o + 1]).ToString(Inv)).Append(',')
                      .Append(((int)_kf[o + 2]).ToString(Inv)).Append(']');
                }
                sb.Append("], \"r\": [");
                for (int c = 0; c < RotationBoneCount; c++)
                {
                    int o = baseOff + TranslationBoneCount * 3 + c * 9;
                    if (c > 0) sb.Append(',');
                    sb.Append('[');
                    for (int i = 0; i < 9; i++)
                    {
                        if (i > 0) sb.Append(',');
                        sb.Append(((int)_kf[o + i]).ToString(Inv));
                    }
                    sb.Append(']');
                }
                sb.Append("]}").Append(k + 1 < KeyframeCount ? "," : "").Append(Nl);
            }
            Ind(sb, 1).Append(']').Append(Nl);
            sb.Append('}').Append(Nl);
            return sb.ToString();
        }

        // ---- JSON import --------------------------------------------------------

        /// <summary>Strict: every count is cross-checked (keyframeDataSize against the
        /// channel counts, fileSize against align4, array shapes against the counts, every
        /// value against the s8 range), because a value that silently wraps in the (byte)
        /// casts of ToBytes would produce a plausible-looking but corrupt animation.
        /// Unknown keys (like "_readme") are ignored so annotated files stay loadable.</summary>
        public static AnmFile FromJson(string json, out string error)
        {
            error = null;
            try
            {
                JNode root = JsonParse(json);
                if (root.Kind != 'o') throw new AnmException("the top level is not a JSON object");

                var a = new AnmFile();
                a.DataOffset = (int)NeedInt(root, "dataOffset", 0, 65535);
                if (a.DataOffset != CanonicalDataOffset)
                    throw new AnmException("dataOffset must be 404 — the emitter only writes the canonical layout");
                a.RotationBoneCount = (int)NeedInt(root, "rotationBoneCount", 0, 255);
                a.TranslationBoneCount = (int)NeedInt(root, "translationBoneCount", 0, 255);
                a.KeyframeDataSize = (int)NeedInt(root, "keyframeDataSize", 0, 65535);
                if (a.KeyframeDataSize != a.RotationBoneCount * 9 + a.TranslationBoneCount * 3)
                    throw new AnmException("keyframeDataSize " + a.KeyframeDataSize.ToString(Inv) +
                                           " does not equal rotationBoneCount*9 + translationBoneCount*3");
                if (a.KeyframeDataSize == 0) throw new AnmException("no animation channels");
                a.BoneCount = (int)NeedInt(root, "boneCount", 1, 64);
                a.Pad7 = (int)NeedInt(root, "pad7", -128, 127);
                a.ActiveBones = NeedHexU32(root, "activeBones");
                a.KeyframeCount = (int)NeedInt(root, "keyframeCount", 1, 65535);
                a.ScaleLog2 = (int)NeedInt(root, "scaleLog2", 0, 15);
                a.RootYOffset = (int)NeedInt(root, "rootYOffset", 0, 255);

                long rawEnd = CanonicalDataOffset + (long)a.KeyframeDataSize * a.KeyframeCount;
                long fileSize = Align4(rawEnd);
                if (fileSize > MaxFileSize)
                    throw new AnmException("the described file would be " + fileSize.ToString(Inv) + " bytes — refusing");
                long claimed = NeedInt(root, "fileSize", 0, uint.MaxValue);
                if (claimed != fileSize)
                    throw new AnmException("fileSize " + claimed.ToString(Inv) + " does not equal align4(404 + keyframeDataSize*keyframeCount) = " +
                                           fileSize.ToString(Inv));
                a.FileSize = (int)fileSize;

                int padLen = (int)(fileSize - rawEnd);
                a.TailPad = NeedHexBytes(root, "tailPad");
                if (a.TailPad.Length != padLen)
                    throw new AnmException("tailPad holds " + a.TailPad.Length.ToString(Inv) + " byte(s) but the alignment pad is " +
                                           padLen.ToString(Inv) + " byte(s)");

                JNode bones = Need(root, "bones");
                if (bones.Kind != 'a' || bones.Items.Count != a.BoneCount)
                    throw new AnmException("\"bones\" is not an array of boneCount (" + a.BoneCount.ToString(Inv) + ") objects");
                a.Parent = new int[a.BoneCount];
                a.RotIdx = new int[a.BoneCount];
                a.TransIdx = new int[a.BoneCount];
                a.T0 = new int[a.BoneCount][];
                for (int i = 0; i < a.BoneCount; i++)
                {
                    JNode b = bones.Items[i];
                    string what = "bones[" + i.ToString(Inv) + "]";
                    if (b.Kind != 'o') throw new AnmException(what + " is not an object");
                    a.Parent[i] = (int)NeedInt(b, "parent", -128, 127);
                    a.RotIdx[i] = (int)NeedInt(b, "rotChannel", -128, 127);
                    a.TransIdx[i] = (int)NeedInt(b, "transChannel", -128, 127);
                    if (a.RotIdx[i] >= a.RotationBoneCount)
                        throw new AnmException(what + " rotChannel " + a.RotIdx[i].ToString(Inv) + " is past rotationBoneCount");
                    if (a.TransIdx[i] >= a.TranslationBoneCount)
                        throw new AnmException(what + " transChannel " + a.TransIdx[i].ToString(Inv) + " is past translationBoneCount");
                    a.T0[i] = NeedS8Array(Need(b, "translationInitial"), 3, what + ".translationInitial");
                }

                JNode kfs = Need(root, "keyframes");
                if (kfs.Kind != 'a' || kfs.Items.Count != a.KeyframeCount)
                    throw new AnmException("\"keyframes\" is not an array of keyframeCount (" + a.KeyframeCount.ToString(Inv) + ") objects");
                a._kf = new sbyte[a.KeyframeDataSize * a.KeyframeCount];
                for (int k = 0; k < a.KeyframeCount; k++)
                {
                    JNode f = kfs.Items[k];
                    string what = "keyframes[" + k.ToString(Inv) + "]";
                    if (f.Kind != 'o') throw new AnmException(what + " is not an object");
                    int baseOff = k * a.KeyframeDataSize;

                    JNode t = Need(f, "t");
                    if (t.Kind != 'a' || t.Items.Count != a.TranslationBoneCount)
                        throw new AnmException(what + ".t is not an array of " + a.TranslationBoneCount.ToString(Inv) + " triples");
                    for (int c = 0; c < a.TranslationBoneCount; c++)
                    {
                        int[] v = NeedS8Array(t.Items[c], 3, what + ".t[" + c.ToString(Inv) + "]");
                        for (int i = 0; i < 3; i++) a._kf[baseOff + c * 3 + i] = (sbyte)v[i];
                    }

                    JNode r = Need(f, "r");
                    if (r.Kind != 'a' || r.Items.Count != a.RotationBoneCount)
                        throw new AnmException(what + ".r is not an array of " + a.RotationBoneCount.ToString(Inv) + " matrices");
                    for (int c = 0; c < a.RotationBoneCount; c++)
                    {
                        int[] v = NeedS8Array(r.Items[c], 9, what + ".r[" + c.ToString(Inv) + "]");
                        for (int i = 0; i < 9; i++) a._kf[baseOff + a.TranslationBoneCount * 3 + c * 9 + i] = (sbyte)v[i];
                    }
                }

                // No source buffer exists for a JSON-born file, so RawData is the canonical
                // re-emission — which keeps the RawData property always non-null.
                a.RawData = a.ToBytes();
                return a;
            }
            catch (AnmException ex)
            {
                error = ex.Message;
                return null;
            }
        }

        // ---- re-emission --------------------------------------------------------

        /// <summary>Deterministic canonical emission: dataOffset 404, the bind region
        /// zero-padded to 404, fileSize = align4(404 + keyframe payload), tail pad bytes
        /// restored verbatim. For a Parse-built instance this reproduces the source's
        /// first FileSize bytes exactly.</summary>
        public byte[] ToBytes()
        {
            var b = new byte[FileSize];
            W16(b, 0x00, DataOffset);
            b[0x02] = (byte)RotationBoneCount;
            b[0x03] = (byte)TranslationBoneCount;
            W16(b, 0x04, KeyframeDataSize);
            b[0x06] = (byte)BoneCount;
            b[0x07] = (byte)(sbyte)Pad7;
            W32(b, 0x08, ActiveBones);
            W32(b, 0x0C, (uint)FileSize);
            W16(b, 0x10, KeyframeCount);
            b[0x12] = (byte)ScaleLog2;
            b[0x13] = (byte)RootYOffset;

            for (int i = 0; i < BoneCount; i++)
            {
                int o = HeaderSize + i * 6;
                b[o] = (byte)(sbyte)Parent[i];
                b[o + 1] = (byte)(sbyte)RotIdx[i];
                b[o + 2] = (byte)(sbyte)TransIdx[i];
                b[o + 3] = (byte)(sbyte)T0[i][0];
                b[o + 4] = (byte)(sbyte)T0[i][1];
                b[o + 5] = (byte)(sbyte)T0[i][2];
            }
            // Bind slots past boneCount stay zero — new byte[] already is.

            for (int i = 0; i < _kf.Length; i++) b[DataOffset + i] = (byte)_kf[i];
            for (int i = 0; i < TailPad.Length; i++) b[DataOffset + _kf.Length + i] = TailPad[i];
            return b;
        }

        // ---- minimal JSON reader ------------------------------------------------
        // Deliberately its own copy: the launcher's other hand-rolled parsers are private
        // to their files, and sharing would couple three tools' error behaviour together.

        /// <summary>An .anm.json arrives from hand editing or a downloaded mod, and a
        /// StackOverflowException cannot be caught on .NET 2.0+ — it kills the launcher
        /// with no message. Our own emission nests 4 deep; 64 is comfortable headroom.</summary>
        private const int JsonMaxDepth = 64;

        private class JNode
        {
            public char Kind;                 // 'o' object, 'a' array, 's' string, 'n' number, 'b' bool, 'z' null
            public string Text;               // 's'/'n'
            public List<JNode> Items;         // 'a', and the values of 'o' in key order
            public List<string> Keys;         // 'o'
        }

        private static JNode JsonParse(string s)
        {
            if (s == null) throw new AnmException("no JSON text");
            int i = 0;
            JSkipWs(s, ref i);
            JNode n = JValue(s, ref i, 0);
            JSkipWs(s, ref i);
            if (i != s.Length) throw new AnmException("trailing data after the JSON value at offset " + i.ToString(Inv));
            return n;
        }

        private static void JSkipWs(string s, ref int i)
        {
            while (i < s.Length && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        }

        private static JNode JValue(string s, ref int i, int depth)
        {
            if (i >= s.Length) throw new AnmException("unexpected end of JSON");
            if (depth > JsonMaxDepth) throw new AnmException("JSON nesting too deep");
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
                if (i >= s.Length || s[i] != '"') throw new AnmException("expected a key at offset " + i.ToString(Inv));
                node.Keys.Add(JString(s, ref i));
                JSkipWs(s, ref i);
                if (i >= s.Length || s[i] != ':') throw new AnmException("expected ':' at offset " + i.ToString(Inv));
                i++;
                JSkipWs(s, ref i);
                node.Items.Add(JValue(s, ref i, depth));
                JSkipWs(s, ref i);
                if (i < s.Length && s[i] == ',') { i++; continue; }
                if (i < s.Length && s[i] == '}') { i++; return node; }
                throw new AnmException("expected ',' or '}' at offset " + i.ToString(Inv));
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
                throw new AnmException("expected ',' or ']' at offset " + i.ToString(Inv));
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
                    int cp;
                    if (i + 4 > s.Length || !int.TryParse(s.Substring(i, 4), NumberStyles.HexNumber, Inv, out cp))
                        throw new AnmException("bad \\u escape");
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
                else throw new AnmException("bad escape '\\" + e + "'");
            }
            throw new AnmException("unterminated string");
        }

        private static string JNumber(string s, ref int i)
        {
            int start = i;
            if (i < s.Length && (s[i] == '-' || s[i] == '+')) i++;
            while (i < s.Length && (char.IsDigit(s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    ((s[i] == '-' || s[i] == '+') && (s[i - 1] == 'e' || s[i - 1] == 'E')))) i++;
            if (i == start) throw new AnmException("unexpected character '" + s[start] + "' at offset " + start.ToString(Inv));
            return s.Substring(start, i - start);
        }

        private static JNode Need(JNode o, string key)
        {
            if (o != null && o.Kind == 'o')
                for (int i = 0; i < o.Keys.Count; i++)
                    if (string.Equals(o.Keys[i], key, StringComparison.Ordinal)) return o.Items[i];
            throw new AnmException("missing \"" + key + "\"");
        }

        private static long NeedInt(JNode o, string key, long min, long max)
        {
            JNode n = Need(o, key);
            long v;
            if (n.Kind != 'n' || !long.TryParse(n.Text, NumberStyles.Integer, Inv, out v))
                throw new AnmException("\"" + key + "\" is not an integer");
            if (v < min || v > max)
                throw new AnmException("\"" + key + "\" = " + v.ToString(Inv) + " is outside " +
                                       min.ToString(Inv) + ".." + max.ToString(Inv));
            return v;
        }

        private static uint NeedHexU32(JNode o, string key)
        {
            JNode n = Need(o, key);
            uint v;
            if (n.Kind != 's' || n.Text.Length < 1 || n.Text.Length > 8 ||
                !uint.TryParse(n.Text, NumberStyles.HexNumber, Inv, out v))
                throw new AnmException("\"" + key + "\" is not a hex string of 1-8 digits");
            return v;
        }

        private static byte[] NeedHexBytes(JNode o, string key)
        {
            JNode n = Need(o, key);
            if (n.Kind != 's' || (n.Text.Length & 1) != 0)
                throw new AnmException("\"" + key + "\" is not a hex string of byte pairs");
            var b = new byte[n.Text.Length / 2];
            for (int i = 0; i < b.Length; i++)
            {
                int v;
                if (!int.TryParse(n.Text.Substring(i * 2, 2), NumberStyles.HexNumber, Inv, out v))
                    throw new AnmException("\"" + key + "\" holds a non-hex pair");
                b[i] = (byte)v;
            }
            return b;
        }

        private static int[] NeedS8Array(JNode n, int want, string what)
        {
            if (n == null || n.Kind != 'a' || n.Items.Count != want)
                throw new AnmException(what + " is not a " + want.ToString(Inv) + "-element array");
            var v = new int[want];
            for (int i = 0; i < want; i++)
            {
                long l;
                if (n.Items[i].Kind != 'n' || !long.TryParse(n.Items[i].Text, NumberStyles.Integer, Inv, out l))
                    throw new AnmException(what + " holds a non-integer value");
                if (l < -128 || l > 127)
                    throw new AnmException(what + " value " + l.ToString(Inv) + " is outside the signed-byte range [-128,127]");
                v[i] = (int)l;
            }
            return v;
        }
    }
}
