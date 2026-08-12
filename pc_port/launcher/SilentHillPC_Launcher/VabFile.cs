/* SPDX-License-Identifier: GPL-3.0-or-later */
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

    /// <summary>The SPU's note-to-pitch conversion, mirroring Note2Pitch in
    /// libsd/smf_io.c. Pitch 0x1000 is unity, and PsyCross uploads every sample at
    /// 44100 Hz then scales by pitch/4096 — so the audible rate is
    /// 44100 * pitch / 4096.
    ///
    /// The engine reads a baked PitchTbl[12][128]; this computes it instead, which
    /// is bit-exact: all 1536 entries reproduce as
    /// floor(4096 * 2^((semitone*128 + cent)/1536)).</summary>
    internal static class PsxPitch
    {
        public const int Unity = 0x1000;
        public const int BaseRate = 44100;

        private static readonly ushort[,] Tbl = BuildTable();

        private static ushort[,] BuildTable()
        {
            var t = new ushort[12, 128];
            for (int s = 0; s < 12; s++)
                for (int c = 0; c < 128; c++)
                    t[s, c] = (ushort)Math.Floor(4096.0 * Math.Pow(2.0, (s * 128.0 + c) / 1536.0));
            return t;
        }

        public static int Note2Pitch(int note, int cent, int sampleNote, int sampleCent)
        {
            int totalCent = cent + sampleCent;
            int steps = (totalCent < 0 ? totalCent + 127 : totalCent) >> 7;
            int finalNote = note + steps;
            int centOffset = totalCent - (steps << 7);
            if (centOffset < 0) centOffset = 0;
            if (centOffset > 127) centOffset = 127;

            int diff = finalNote - sampleNote;
            if (diff >= 0)
            {
                int shift = diff / 12;
                // A big upward interval would shift the 16-bit pitch into nothing;
                // the hardware register saturates rather than wrapping to silence.
                if (shift > 3) return 0x3FFF;
                int p = Tbl[diff % 12, centOffset] << shift;
                return p > 0x3FFF ? 0x3FFF : p;
            }

            int abs = -diff;
            return Tbl[(12 - (abs % 12)) % 12, centOffset] >> ((abs + 11) / 12);
        }

        public static double RateHz(int pitch)
        {
            return BaseRate * (double)pitch / Unity;
        }
    }

    /// <summary>A sound id that plays a given sample, and the rate it plays at.</summary>
    internal sealed class SfxMatch
    {
        public SfxRow Row;
        public int Pitch;          // SPU pitch register value; 0x1000 is unity
        public double RateHz;

        public string Label
        {
            get { return Row.Name ?? ("sfx " + Row.Id); }
        }

        /// <summary>Slot names from e_AudioType. 0 and 2 each have two spellings in the
        /// enum (base/music-key, ambient/special-screen); the broader one is used.</summary>
        public static string SlotName(int slot)
        {
            switch (slot)
            {
                case 0: return "base";
                case 1: return "weapon";
                case 2: return "ambient";
                case 3: return "music";
                default: return "slot " + slot;
            }
        }
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

        // Block flag BITS, matching PsyCross's decoder (PsyX_SPUAL.cpp): the values
        // are combined, not enumerated, so tests must be bitwise. Treating them as
        // whole numbers ("flag == 7") misses every ordinary end block.
        public const int FlagLoopEnd = 1 << 0;   // sample stops here
        public const int FlagRepeat = 1 << 1;    // ...and jumps to the loop start
        public const int FlagLoopStart = 1 << 2;

        private static bool ScanForLoop(byte[] d, VabVag vag)
        {
            for (int b = 0; b < vag.BlockCount; b++)
            {
                if ((d[vag.Offset + b * 16 + 1] & FlagRepeat) != 0) return true;
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

                // The terminating block's audio is part of the sample — PsyCross
                // finishes decoding it and only then stops, so stopping first would
                // clip the tail off every preview.
                if ((flag & FlagLoopEnd) != 0) break;
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

        /// <summary>Which tone a program plays for a given note. Every tone in these
        /// banks has a one-note range (48..48, 49..49, ...), so the note identifies the
        /// tone exactly — there is deliberately NO fallback. Returning "some tone in the
        /// program" instead made every sound id match every bank.</summary>
        public VabTone ResolveTone(int program, int note)
        {
            foreach (VabTone t in Tones)
            {
                if (t.Program == program && note >= t.MinNote && note <= t.MaxNote) return t;
            }
            return null;
        }

        /// <summary>The sound-table rows that land on a given sample in this bank, with
        /// the exact rate each plays at.
        ///
        /// A bank file does not record which slot it gets loaded into — the weapon slot
        /// holds PISTOL/SHOTGUN/RIFEL/SAW depending on what Harry carries, and the
        /// ambient slot changes per map — so a row is matched on program + note and the
        /// slot is reported rather than assumed. Two independent constraints have to
        /// agree: the note must land in a tone's range, AND the row's own tone index
        /// must be that tone's slot. Either alone lets rows from other slots through.
        ///
        /// Pass a slot to restrict further, or -1 for any.</summary>
        public List<SfxMatch> MatchesFor(int vagIndex, int slotFilter)
        {
            var hits = new List<SfxMatch>();
            foreach (SfxRow row in SfxTable.Rows)
            {
                if (row.Id == SfxTable.SfxBase) continue;
                if (slotFilter >= 0 && row.Slot != slotFilter) continue;

                VabTone tone = ResolveTone(row.Program, row.Note);
                if (tone == null || tone.Vag != vagIndex) continue;
                if (tone.Slot != row.VabIndex) continue;

                int pitch = PsxPitch.Note2Pitch(row.Note, 0, tone.CenterNote, tone.CenterFine);
                hits.Add(new SfxMatch
                {
                    Row = row,
                    Pitch = pitch,
                    RateHz = PsxPitch.RateHz(pitch),
                });
            }
            return hits;
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

        /// <summary>Largest a single sample can be: the size table stores length/8 in a
        /// u16, so 65535 * 8 is the hard ceiling regardless of anything else.</summary>
        public const int MaxVagBytes = 65535 * 8;

        /// <summary>Write the bank back out with some samples replaced.
        ///
        /// Bodies are stored back to back and addressed by a running sum, so replacing
        /// one sample with a different-sized one moves every sample after it. That is
        /// why this rebuilds the whole file rather than patching in place: the size
        /// table (one-based, length/8) and the header's own size field both have to
        /// agree with the new layout or the engine walks into the wrong sample.</summary>
        public byte[] Rebuild(Dictionary<int, byte[]> replacements, out string error)
        {
            error = null;
            if (replacements == null) replacements = new Dictionary<int, byte[]>();

            var bodies = new List<byte[]>(VagCount);
            int total = 0;
            foreach (VabVag vag in Vags)
            {
                byte[] body;
                if (!replacements.TryGetValue(vag.Index, out body) || body == null)
                {
                    body = RawVag(vag.Index);
                }

                if ((body.Length & 0x0F) != 0)
                {
                    error = "Sample " + vag.Index + " is " + body.Length +
                            " bytes, which is not a whole number of 16-byte ADPCM blocks.";
                    return null;
                }
                if (body.Length > MaxVagBytes)
                {
                    error = "Sample " + vag.Index + " is " + body.Length.ToString("N0") +
                            " bytes; the bank format cannot store more than " +
                            MaxVagBytes.ToString("N0") + " per sample.";
                    return null;
                }

                bodies.Add(body);
                total += body.Length;
            }

            int toneTable = HeaderSize + ProgramTableSize;
            int toneBytes = ProgramCount * TonesPerProgram * ToneEntrySize;
            int sizeTable = toneTable + toneBytes;
            int bodiesOff = sizeTable + SizeTableEntries * 2;

            var outBuf = new byte[bodiesOff + total];
            Array.Copy(_data, 0, outBuf, 0, bodiesOff);

            // Rewrite the whole size table rather than only the changed rows: a stale
            // entry past VagCount would be read as a real length by anything that
            // trusts the table over the header.
            for (int i = 0; i < SizeTableEntries; i++)
            {
                ushort v = 0;
                if (i >= 1 && i <= bodies.Count) v = (ushort)(bodies[i - 1].Length / 8);
                outBuf[sizeTable + i * 2] = (byte)(v & 0xFF);
                outBuf[sizeTable + i * 2 + 1] = (byte)(v >> 8);
            }

            int w = bodiesOff;
            foreach (byte[] b in bodies)
            {
                Array.Copy(b, 0, outBuf, w, b.Length);
                w += b.Length;
            }

            byte[] fsize = BitConverter.GetBytes(outBuf.Length);
            Array.Copy(fsize, 0, outBuf, 12, 4);

            return outBuf;
        }
    }
}
