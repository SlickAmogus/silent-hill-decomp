using System;
using System.Collections.Generic;
using System.IO;

namespace SilentHillPC_Launcher
{
    /// <summary>One playable sample inside a bank: a VAG body plus the tones that
    /// reference it. The VAG is the layer a modder actually replaces — everything
    /// above it (sfxId -> program -> tone) is only routing.</summary>
    internal sealed class VabVag
    {
        public int Index;          // 1-based, as the size table and tones address it
        public int Offset;         // byte offset of the ADPCM body within the file
        public int Length;         // byte length of the ADPCM body
        public readonly List<VabTone> Tones = new List<VabTone>();

        public int BlockCount { get { return Length / 16; } }

        /// <summary>Whether any 16-byte block carries a loop-start or repeat flag.
        /// Sustained sounds (radio static, ambience) rely on these, and a WAV round
        /// trip through a naive encoder loses them.</summary>
        public bool Loops;
    }

    internal sealed class VabTone
    {
        public int Program;        // owning program index
        public int Slot;           // tone slot within the program (0..15)
        public int Vag;            // 1-based VAG index this tone plays
        public int Volume;
        public int Pan;
        public int CenterNote;     // note at which the sample plays back unshifted
        public int CenterFine;     // fine tune
        public int MinNote, MaxNote;
        public int Adsr1, Adsr2;
    }

    /// <summary>Reader for the PSX VAB sound banks this game ships (90 of them in
    /// SND/). Layout verified against all 90 banks AND against the engine's own
    /// arithmetic in libsd/smf_io.c, which locates the size table at
    /// vh_addr + (ps * 512) + 2080 — i.e. 32-byte header + 2048-byte program table
    /// + the tone table.
    ///
    /// The trap worth knowing: the VAG size table is ONE-BASED. Entry 0 is a dummy
    /// and sample n's length is table[n] * 8. Treating it as 0-based shifts every
    /// body offset and silently produces noise rather than a clean failure.</summary>
    internal sealed class VabFile
    {
        public const int HeaderSize = 32;
        public const int ProgramTableSize = 128 * 16;   // always 128 entries, occupied or not
        public const int ToneEntrySize = 32;
        public const int TonesPerProgram = 16;
        public const int SizeTableEntries = 256;

        public string Path;
        public int Version;
        public int VabId;
        public int DeclaredSize;      // header's own size field
        public int ProgramCount;      // "ps" — also the tone table's block count
        public int ToneCount;         // "ts"
        public int VagCount;          // "vs"
        public int MasterVolume;

        public readonly List<VabVag> Vags = new List<VabVag>();
        public readonly List<VabTone> Tones = new List<VabTone>();

        private byte[] _data;

        public byte[] Raw { get { return _data; } }
        public int BodiesOffset { get; private set; }

        public static VabFile Load(string path, out string error)
        {
            error = null;
            byte[] d;
            try { d = File.ReadAllBytes(path); }
            catch (Exception ex) { error = "Could not read the file:\n\n" + ex.Message; return null; }

            if (d.Length < HeaderSize + ProgramTableSize)
            {
                error = "Too small to be a VAB sound bank (" + d.Length + " bytes).";
                return null;
            }

            // The magic reads "pBAV" in file order — the PSX header stores the tag as
            // a little-endian word, so a byte-wise read sees it reversed.
            if (d[0] != 'p' || d[1] != 'B' || d[2] != 'A' || d[3] != 'V')
            {
                error = "Not a VAB sound bank: expected the 'pBAV' tag, found '" +
                        SafeTag(d) + "'.";
                return null;
            }

            var v = new VabFile();
            v._data = d;
            v.Path = path;
            v.Version = BitConverter.ToInt32(d, 4);
            v.VabId = BitConverter.ToInt32(d, 8);
            v.DeclaredSize = BitConverter.ToInt32(d, 12);
            v.ProgramCount = BitConverter.ToInt16(d, 18);
            v.ToneCount = BitConverter.ToInt16(d, 20);
            v.VagCount = BitConverter.ToInt16(d, 22);
            v.MasterVolume = d[24];

            if (v.ProgramCount <= 0 || v.ProgramCount > 128)
            {
                error = "Program count out of range (" + v.ProgramCount + ").";
                return null;
            }
            if (v.VagCount <= 0 || v.VagCount >= SizeTableEntries)
            {
                error = "Sample count out of range (" + v.VagCount + ").";
                return null;
            }

            int toneTable = HeaderSize + ProgramTableSize;
            int toneBytes = v.ProgramCount * TonesPerProgram * ToneEntrySize;
            int sizeTable = toneTable + toneBytes;
            int bodies = sizeTable + SizeTableEntries * 2;

            if (bodies > d.Length)
            {
                error = "Truncated: the sample table would end past the end of the file.";
                return null;
            }
            v.BodiesOffset = bodies;

            // Bodies are packed in VAG order, so an offset is the running sum of the
            // preceding lengths. One replaced sample therefore moves every later one.
            int running = 0;
            for (int n = 1; n <= v.VagCount; n++)
            {
                int len = BitConverter.ToUInt16(d, sizeTable + n * 2) * 8;
                var vag = new VabVag { Index = n, Offset = bodies + running, Length = len };
                running += len;
                v.Vags.Add(vag);
            }

            if (bodies + running > d.Length)
            {
                error = "Truncated: sample bodies run " + (bodies + running - d.Length) +
                        " bytes past the end of the file.";
                return null;
            }

            for (int p = 0; p < v.ProgramCount; p++)
            {
                for (int t = 0; t < TonesPerProgram; t++)
                {
                    int off = toneTable + (p * TonesPerProgram + t) * ToneEntrySize;
                    int vagIdx = BitConverter.ToInt16(d, off + 22);
                    if (vagIdx <= 0 || vagIdx > v.VagCount) continue;

                    var tone = new VabTone
                    {
                        Program = p,
                        Slot = t,
                        Vag = vagIdx,
                        Volume = d[off + 2],
                        Pan = d[off + 3],
                        CenterNote = d[off + 4],
                        CenterFine = d[off + 5],
                        MinNote = d[off + 6],
                        MaxNote = d[off + 7],
                        Adsr1 = BitConverter.ToUInt16(d, off + 16),
                        Adsr2 = BitConverter.ToUInt16(d, off + 18),
                    };
                    v.Tones.Add(tone);
                    v.Vags[vagIdx - 1].Tones.Add(tone);
                }
            }

            foreach (VabVag vag in v.Vags) vag.Loops = ScanForLoop(d, vag);
            return v;
        }

        private static string SafeTag(byte[] d)
        {
            var chars = new char[4];
            for (int i = 0; i < 4; i++)
                chars[i] = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '?';
            return new string(chars);
        }

        /// <summary>Block flag 1 = loop end, 2 = loop-and-repeat, 3 = loop start,
        /// 7 = end of sample. Anything with a repeat or start marker is a sustained
        /// sound rather than a one-shot.</summary>
        private static bool ScanForLoop(byte[] d, VabVag vag)
        {
            for (int b = 0; b < vag.BlockCount; b++)
            {
                int flag = d[vag.Offset + b * 16 + 1];
                if (flag == 2 || flag == 3 || flag == 6) return true;
            }
            return false;
        }

        // PSX ADPCM predictor coefficients, in 64ths.
        private static readonly int[] Filter0 = { 0, 60, 115, 98 };
        private static readonly int[] Filter1 = { 0, 0, -52, -55 };

        /// <summary>Decode one sample's ADPCM to 16-bit mono PCM. Each 16-byte block
        /// is a shift/filter byte, a flag byte, then 14 bytes holding 28 nibbles.</summary>
        public short[] Decode(int vagIndex)
        {
            VabVag vag = Vags[vagIndex - 1];
            var outBuf = new short[vag.BlockCount * 28];
            int w = 0;
            int prev1 = 0, prev2 = 0;

            for (int b = 0; b < vag.BlockCount; b++)
            {
                int p = vag.Offset + b * 16;
                int shift = _data[p] & 0x0F;
                int filter = (_data[p] >> 4) & 0x0F;
                int flag = _data[p + 1];

                if (flag == 7) break; // end marker: nothing after it is audio

                if (filter > 3) filter = 3;
                // A shift of 13..15 is not meaningful; hardware treats it as a mute.
                bool mute = shift > 12;

                for (int i = 0; i < 14; i++)
                {
                    int by = _data[p + 2 + i];
                    for (int half = 0; half < 2; half++)
                    {
                        int nib = half == 0 ? (by & 0x0F) : (by >> 4);
                        if (nib > 7) nib -= 16;

                        int s;
                        if (mute)
                        {
                            s = 0;
                        }
                        else
                        {
                            s = (nib << 12) >> shift;
                            s += (prev1 * Filter0[filter] + prev2 * Filter1[filter]) >> 6;
                            if (s > 32767) s = 32767;
                            if (s < -32768) s = -32768;
                        }

                        prev2 = prev1;
                        prev1 = s;
                        outBuf[w++] = (short)s;
                    }
                }
            }

            if (w == outBuf.Length) return outBuf;
            var trimmed = new short[w];
            Array.Copy(outBuf, trimmed, w);
            return trimmed;
        }

        /// <summary>The rate at which a sample plays when triggered at its own centre
        /// note. The SPU's unity pitch is 44100 Hz, and the game re-pitches per sound
        /// from the note in g_Vab_InfoTable — so this is the sample's natural rate, not
        /// necessarily the rate you will hear in game.</summary>
        public const int UnityRate = 44100;

        public static byte[] BuildWav(short[] pcm, int sampleRate)
        {
            int dataBytes = pcm.Length * 2;
            using (var ms = new MemoryStream(44 + dataBytes))
            using (var bw = new BinaryWriter(ms))
            {
                bw.Write(new[] { 'R', 'I', 'F', 'F' });
                bw.Write(36 + dataBytes);
                bw.Write(new[] { 'W', 'A', 'V', 'E' });
                bw.Write(new[] { 'f', 'm', 't', ' ' });
                bw.Write(16);
                bw.Write((short)1);            // PCM
                bw.Write((short)1);            // mono
                bw.Write(sampleRate);
                bw.Write(sampleRate * 2);      // byte rate
                bw.Write((short)2);            // block align
                bw.Write((short)16);           // bits
                bw.Write(new[] { 'd', 'a', 't', 'a' });
                bw.Write(dataBytes);
                foreach (short s in pcm) bw.Write(s);
                bw.Flush();
                return ms.ToArray();
            }
        }

        /// <summary>Raw ADPCM body, for users who want to edit or re-inject the exact
        /// bytes rather than round-trip through PCM.</summary>
        public byte[] RawVag(int vagIndex)
        {
            VabVag vag = Vags[vagIndex - 1];
            var b = new byte[vag.Length];
            Array.Copy(_data, vag.Offset, b, 0, vag.Length);
            return b;
        }
    }
}
