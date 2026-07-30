#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""WAV -> PlayStation 1 .VAG (PS-ADPCM) converter.

Encodes a mono PCM WAV into the 48-byte-header .VAG format the SPU expects,
matching the loop-flag convention this project's sound.c driver relies on:
every ADPCM block carries flag 0 except the final block, which carries 0x03
(loop-end + repeat). The driver points the channel's LSAX (loop-start) register
at the sample's start, so on reaching that final block the SPU jumps back to
the beginning - a hardware loop with no per-frame code.

--loop makes the loop SEAMLESS: instead of a hard cut at the wrap point, the
head of the clip is crossfaded with the material immediately following the loop
length, so the last sample flows naturally into the first.

Requires numpy and scipy (for WAV reading/resampling); ffmpeg is used to
normalise arbitrary inputs to mono 16-bit if scipy can't read them directly.

Usage:
    wav2vag.py in.wav out.vag --name NEWBGM --rate 44100 --seconds 20 --loop
"""

import argparse
import struct
import subprocess
import sys
import tempfile
import os

import numpy as np

# PS-ADPCM predictor coefficients (x64). filter index 0..4.
FILTER_POS = [0, 60, 115, 98, 122]
FILTER_NEG = [0, 0, -52, -55, -60]


def read_wav_mono(path, target_rate):
    """Return float32 mono samples in [-1,1] at target_rate."""
    try:
        from scipy.io import wavfile
        from scipy.signal import resample_poly
        rate, data = wavfile.read(path)
    except Exception:
        # Fall back to ffmpeg -> temp wav that scipy can definitely read.
        from scipy.io import wavfile
        from scipy.signal import resample_poly
        tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
        tmp.close()
        subprocess.run(
            ["ffmpeg", "-y", "-i", path, "-ac", "1", "-ar", str(target_rate),
             "-acodec", "pcm_s16le", tmp.name],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        rate, data = wavfile.read(tmp.name)
        os.unlink(tmp.name)

    data = np.asarray(data)

    # Capture integer normalisation info BEFORE any stereo downmix. This
    # matters because np.mean() on an integer array upcasts its result to
    # float64 - so checking np.issubdtype(...) AFTER averaging stereo down
    # to mono silently fails for every integer PCM WAV with more than one
    # channel, and the code falls into the "already float" branch, which
    # does nothing but cast - leaving samples in their raw +/-32768 range
    # instead of normalised to +/-1.0. encode_vag() then clips that against
    # int16 range and most of the signal ends up pinned at full scale. This
    # is exactly what corrupted an earlier BGM track this project shipped
    # (traced by decoding it back: 72% of samples were clipped, RMS at 97%
    # of full scale) - it went unnoticed here because that track happened
    # to be converted from an MP3, which forces the ffmpeg fallback path
    # below (ffmpeg's own -ac 1 downmixes to mono before scipy ever reads
    # it, so the bug's trigger condition - scipy reading multi-channel
    # integer PCM directly - was never hit). Any stereo WAV fed in directly
    # hits it, which is what happened here.
    #
    # Separately: UNSIGNED integer PCM (8-bit WAV is virtually always
    # unsigned; 16/24/32-bit WAV is virtually always signed) stores silence
    # at the midpoint of its range - 128 for 8-bit, not 0 - with the actual
    # waveform swinging symmetrically around that midpoint. Dividing by max
    # alone (correct for signed PCM, which IS already zero-centered) leaves
    # a huge DC bias for unsigned PCM instead: an 8-bit WAV normalised this
    # way came out ranging ~0.1..0.68 with a mean of ~0.50 instead of
    # symmetric +/-1.0 around 0, and encoding that heavily-biased signal is
    # exactly what "MGS SFX sounds noisy and distorted" turned out to be -
    # traced by decoding read_wav_mono()'s own output directly and finding
    # that mean.
    is_unsigned = np.issubdtype(data.dtype, np.unsignedinteger)
    is_signed   = np.issubdtype(data.dtype, np.signedinteger)

    if is_unsigned:
        # e.g. uint8: range 0..255, silence at 128, half-range 128.
        midpoint = (int(np.iinfo(data.dtype).max) + 1) // 2
        offset, maxv = midpoint, float(midpoint)
    elif is_signed:
        offset, maxv = 0, float(np.iinfo(data.dtype).max)
    else:
        offset, maxv = 0, None

    if data.ndim > 1:                       # stereo -> mono
        data = data.mean(axis=1)

    if maxv is not None:
        data = (data.astype(np.float64) - offset) / maxv
    else:
        data = data.astype(np.float64)

    if rate != target_rate:
        from math import gcd
        from scipy.signal import resample_poly
        g = gcd(int(rate), int(target_rate))
        data = resample_poly(data, target_rate // g, rate // g)
    return data.astype(np.float64)


def make_seamless_loop(samples, loop_len, xfade):
    """Take `loop_len` samples that loop seamlessly.

    final[0]      == src[loop_len]      (so src[loop_len-1] -> final[0] is a
                                         natural consecutive-sample step)
    final[xfade]  == src[xfade]         (back on the original timeline)
    so the wrap point end->start has no discontinuity.
    """
    if len(samples) < loop_len + xfade:
        # not enough tail to crossfade - just hard-cut (SPU still loops, may
        # click slightly at the seam).
        return samples[:loop_len]
    out = samples[:loop_len].copy()
    i = np.arange(xfade)
    w = i / xfade                            # 0 -> 1
    out[:xfade] = samples[loop_len:loop_len + xfade] * (1.0 - w) \
        + samples[:xfade] * w
    return out


# Precompute the 65 candidate (filter,shift) configurations once.
_CFG_FILT = np.array([f for f in range(5) for _ in range(13)], np.int64)
_CFG_SHIFT = np.array([s for _ in range(5) for s in range(13)], np.int64)
_CFG_FP = np.array([FILTER_POS[f] for f in _CFG_FILT], np.int64)
_CFG_FN = np.array([FILTER_NEG[f] for f in _CFG_FILT], np.int64)
_CFG_RSH = (12 - _CFG_SHIFT).astype(np.int64)                 # right shift amount
_CFG_HALF = np.where(_CFG_SHIFT <= 11,
                     (1 << np.clip(11 - _CFG_SHIFT, 0, 31)), 0).astype(np.int64)
_NCFG = _CFG_FILT.shape[0]


def encode_block(block28, h1, h2):
    """Encode 28 samples choosing the best of all 65 filter/shift configs.
    Vectorised across configs; the 28-sample recurrence is the only loop.
    Returns (15 bytes = filter/shift byte + 14 data bytes, new_h1, new_h2)."""
    p1 = np.full(_NCFG, h1, np.int64)
    p2 = np.full(_NCFG, h2, np.int64)
    err = np.zeros(_NCFG, np.int64)
    nibs = np.empty((28, _NCFG), np.int64)

    for i, s in enumerate(block28):
        predict = (p1 * _CFG_FP + p2 * _CFG_FN + 32) >> 6
        diff = s - predict
        q = (diff + _CFG_HALF) >> _CFG_RSH
        q = np.clip(q, -8, 7)
        recon = np.clip(predict + (q << _CFG_RSH), -32768, 32767)
        e = s - recon
        err += e * e
        nibs[i] = q & 0xF
        p2 = p1
        p1 = recon

    best = int(np.argmin(err))
    filt = int(_CFG_FILT[best])
    shift = int(_CFG_SHIFT[best])
    col = nibs[:, best]
    out = bytearray(14)
    for i in range(14):
        out[i] = int(col[2 * i]) | (int(col[2 * i + 1]) << 4)
    return bytes([(filt << 4) | shift]) + bytes(out), int(p1[best]), int(p2[best])


def encode_vag(samples_f, rate, name, loop):
    # to int16
    s = np.clip(np.round(samples_f * 32767.0), -32768, 32767).astype(np.int64)
    # pad to multiple of 28
    pad = (-len(s)) % 28
    if pad:
        s = np.concatenate([s, np.zeros(pad, np.int64)])
    nblocks = len(s) // 28

    body = bytearray()
    h1 = h2 = 0
    for b in range(nblocks):
        blk = s[b * 28:(b + 1) * 28]
        enc, h1, h2 = encode_block(blk.tolist(), h1, h2)  # 15 bytes: head + 14 data
        head = enc[0:1]
        data = enc[1:15]
        if b == nblocks - 1:
            flag = 0x03 if loop else 0x01   # loop end+repeat -> LSAX, or one-shot end
        else:
            flag = 0x00
        body += head + bytes([flag]) + data

    header = struct.pack(
        ">4sIII I 12x 16s",
        b"VAGp", 0x20, 0, len(body), rate,
        name.encode("ascii")[:16].ljust(16, b"\x00"),
    )
    return header + bytes(body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile")
    ap.add_argument("outfile")
    ap.add_argument("--name", default="SAMPLE")
    ap.add_argument("--rate", type=int, default=44100)
    ap.add_argument("--seconds", type=float, default=None,
                    help="trim/loop length in seconds")
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--xfade-ms", type=float, default=60.0)
    args = ap.parse_args()

    samples = read_wav_mono(args.infile, args.rate)
    if args.seconds:
        loop_len = int(args.seconds * args.rate)
        xfade = int(args.xfade_ms / 1000.0 * args.rate)
        if args.loop:
            samples = make_seamless_loop(samples, loop_len, xfade)
        else:
            samples = samples[:loop_len]

    vag = encode_vag(samples, args.rate, args.name, args.loop)
    with open(args.outfile, "wb") as f:
        f.write(vag)
    print(f"wrote {args.outfile}: {len(vag)} bytes, {len(samples)} samples "
          f"@ {args.rate} Hz, loop={args.loop}")


if __name__ == "__main__":
    main()
