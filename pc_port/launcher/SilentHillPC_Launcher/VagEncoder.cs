using System;
using System.Collections.Generic;
using System.IO;

namespace SilentHillPC_Launcher
{
    /// <summary>16-bit PCM to PSX ADPCM.
    ///
    /// Each 16-byte block holds one predictor/shift byte, one flag byte and 28
    /// 4-bit residuals. The encoder brute-forces the block header: all 4 filters
    /// against every usable shift, decoding as it goes and keeping whichever pair
    /// reproduces the block with the least squared error. That matters because the
    /// predictor is fed by RECONSTRUCTED samples, so a locally greedy choice can
    /// drift — measuring the actual decode is the only honest way to pick.</summary>
    internal static class VagEncoder
    {
        public const int SamplesPerBlock = 28;
        public const int BlockBytes = 16;

        private static readonly int[] Filter0 = { 0, 60, 115, 98 };
        private static readonly int[] Filter1 = { 0, 0, -52, -55 };

        /// <summary>Shifts above 12 are a mute on hardware, so they are never useful.</summary>
        private const int MaxShift = 12;

        public sealed class Options
        {
            /// <summary>Make the result loop over its whole length. Off means a one-shot
            /// that stops at the end.</summary>
            public bool Loop;

            /// <summary>First sample of the loop region, when Loop is set. Rounded down
            /// to a block boundary because the flags live per block.</summary>
            public int LoopStartSample;
        }

        public static byte[] Encode(short[] pcm, Options opt)
        {
            if (opt == null) opt = new Options();
            if (pcm == null) pcm = new short[0];

            int blocks = (pcm.Length + SamplesPerBlock - 1) / SamplesPerBlock;
            if (blocks == 0) blocks = 1;

            int loopBlock = 0;
            if (opt.Loop && opt.LoopStartSample > 0)
            {
                loopBlock = opt.LoopStartSample / SamplesPerBlock;
                if (loopBlock >= blocks) loopBlock = blocks - 1;
            }

            var outBytes = new byte[blocks * BlockBytes];
            int prev1 = 0, prev2 = 0;

            for (int b = 0; b < blocks; b++)
            {
                var block = new short[SamplesPerBlock];
                for (int i = 0; i < SamplesPerBlock; i++)
                {
                    int src = b * SamplesPerBlock + i;
                    block[i] = src < pcm.Length ? pcm[src] : (short)0;
                }

                int bestFilter = 0, bestShift = 12;
                double bestErr = double.MaxValue;
                var bestNibbles = new int[SamplesPerBlock];
                int bestPrev1 = prev1, bestPrev2 = prev2;

                for (int f = 0; f < 4; f++)
                {
                    for (int s = 0; s <= MaxShift; s++)
                    {
                        int p1 = prev1, p2 = prev2;
                        double err = 0;
                        var nibbles = new int[SamplesPerBlock];

                        for (int i = 0; i < SamplesPerBlock; i++)
                        {
                            int predicted = (p1 * Filter0[f] + p2 * Filter1[f]) >> 6;
                            int residual = block[i] - predicted;

                            // Inverse of the decoder's (nib << 12) >> shift, rounded to
                            // nearest rather than truncated: truncation biases every
                            // sample the same direction and the error compounds through
                            // the predictor.
                            int n = (int)Math.Round(residual * Math.Pow(2, s) / 4096.0);
                            if (n > 7) n = 7;
                            if (n < -8) n = -8;
                            nibbles[i] = n;

                            int recon = ((n << 12) >> s) + predicted;
                            if (recon > 32767) recon = 32767;
                            if (recon < -32768) recon = -32768;

                            double d = recon - block[i];
                            err += d * d;

                            p2 = p1;
                            p1 = recon;
                        }

                        if (err < bestErr)
                        {
                            bestErr = err;
                            bestFilter = f;
                            bestShift = s;
                            Array.Copy(nibbles, bestNibbles, SamplesPerBlock);
                            bestPrev1 = p1;
                            bestPrev2 = p2;
                        }
                    }
                }

                prev1 = bestPrev1;
                prev2 = bestPrev2;

                int o = b * BlockBytes;
                outBytes[o] = (byte)((bestFilter << 4) | bestShift);
                outBytes[o + 1] = (byte)BlockFlag(b, blocks, loopBlock, opt.Loop);
                for (int i = 0; i < SamplesPerBlock; i += 2)
                {
                    int lo = bestNibbles[i] & 0x0F;
                    int hi = bestNibbles[i + 1] & 0x0F;
                    outBytes[o + 2 + i / 2] = (byte)(lo | (hi << 4));
                }
            }

            return outBytes;
        }

        private static int BlockFlag(int block, int blocks, int loopBlock, bool loop)
        {
            int flag = 0;
            if (loop && block == loopBlock) flag |= VabFile.FlagLoopStart;

            if (block == blocks - 1)
            {
                // LoopEnd always terminates; Repeat is what turns it into a loop rather
                // than a stop, and the hardware ignores Repeat without LoopEnd.
                flag |= VabFile.FlagLoopEnd;
                if (loop) flag |= VabFile.FlagRepeat;
            }
            return flag;
        }

        /// <summary>Read a mono/stereo 16-bit or 8-bit PCM WAV. Stereo is mixed down and
        /// anything else is refused, because a silently wrong interpretation of the data
        /// chunk sounds like noise and users would blame the encoder.</summary>
        public static short[] ReadWav(string path, out int sampleRate, out string error)
        {
            sampleRate = 0;
            error = null;
            byte[] d;
            try { d = File.ReadAllBytes(path); }
            catch (Exception ex) { error = ex.Message; return null; }

            if (d.Length < 44 ||
                d[0] != 'R' || d[1] != 'I' || d[2] != 'F' || d[3] != 'F' ||
                d[8] != 'W' || d[9] != 'A' || d[10] != 'V' || d[11] != 'E')
            {
                error = "Not a RIFF/WAVE file.";
                return null;
            }

            int fmtOff = -1, fmtLen = 0, dataOff = -1, dataLen = 0;
            int p = 12;
            while (p + 8 <= d.Length)
            {
                string id = "" + (char)d[p] + (char)d[p + 1] + (char)d[p + 2] + (char)d[p + 3];
                int len = BitConverter.ToInt32(d, p + 4);
                if (len < 0 || p + 8 + len > d.Length) len = d.Length - p - 8;
                if (id == "fmt ") { fmtOff = p + 8; fmtLen = len; }
                else if (id == "data") { dataOff = p + 8; dataLen = len; }
                p += 8 + len + (len & 1);
            }

            if (fmtOff < 0 || fmtLen < 16) { error = "No usable 'fmt ' chunk."; return null; }
            if (dataOff < 0) { error = "No 'data' chunk."; return null; }

            int audioFormat = BitConverter.ToUInt16(d, fmtOff);
            int channels = BitConverter.ToUInt16(d, fmtOff + 2);
            sampleRate = BitConverter.ToInt32(d, fmtOff + 4);
            int bits = BitConverter.ToUInt16(d, fmtOff + 14);

            if (audioFormat != 1)
            {
                error = "Only uncompressed PCM WAV is supported (this one is format " +
                        audioFormat + "). Re-export as PCM.";
                return null;
            }
            if (bits != 16 && bits != 8)
            {
                error = "Only 8- or 16-bit PCM is supported (this one is " + bits + "-bit).";
                return null;
            }
            if (channels < 1 || channels > 2)
            {
                error = "Only mono or stereo is supported (this one has " + channels + " channels).";
                return null;
            }

            int bytesPer = bits / 8;
            int frames = dataLen / (bytesPer * channels);
            var pcm = new short[frames];
            for (int i = 0; i < frames; i++)
            {
                int sum = 0;
                for (int c = 0; c < channels; c++)
                {
                    int off = dataOff + (i * channels + c) * bytesPer;
                    sum += bits == 16
                        ? BitConverter.ToInt16(d, off)
                        : (d[off] - 128) << 8;   // 8-bit WAV is unsigned
                }
                pcm[i] = (short)(sum / channels);
            }
            return pcm;
        }

        /// <summary>Nearest-neighbour-free linear resample. A replacement authored at
        /// 44.1 kHz has to be resampled to the rate the tone will play it at, or it comes
        /// out at the wrong speed — the VAG carries no rate of its own.</summary>
        public static short[] Resample(short[] pcm, int fromRate, int toRate)
        {
            if (pcm.Length == 0 || fromRate <= 0 || toRate <= 0 || fromRate == toRate) return pcm;

            long outLen = (long)pcm.Length * toRate / fromRate;
            if (outLen < 1) outLen = 1;
            if (outLen > 8 * 1024 * 1024) outLen = 8 * 1024 * 1024;

            var outBuf = new short[outLen];
            double step = (double)fromRate / toRate;
            for (int i = 0; i < outLen; i++)
            {
                double sp = i * step;
                int i0 = (int)sp;
                int i1 = i0 + 1 < pcm.Length ? i0 + 1 : pcm.Length - 1;
                double frac = sp - i0;
                if (i0 >= pcm.Length) { i0 = pcm.Length - 1; i1 = i0; frac = 0; }
                outBuf[i] = (short)Math.Round(pcm[i0] * (1.0 - frac) + pcm[i1] * frac);
            }
            return outBuf;
        }
    }
}
