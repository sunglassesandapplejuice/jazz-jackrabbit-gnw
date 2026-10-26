#!/usr/bin/env python3
"""Extract the pre-rendered .MUS pieces from a Jazz.jj pack and convert them
to listenable .wav files.

Usage:
    python3 tools/mus2wav.py Jazz.jj listen/
"""
import os
import struct
import sys
import wave

GWMU_ADPCM_BLOCK = 1024
GWMU_ADPCM_BLOCK_BYTES = 4 + GWMU_ADPCM_BLOCK // 2

IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
            37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
            157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
            544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
            1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
            4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
            12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
            29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def rd16(b, o):
    return b[o] | (b[o + 1] << 8)


def rd32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def decode_mus(data):
    assert data[:4] == b"GWMU"
    version = rd16(data, 4)
    header = 20 if version == 2 else 16
    codec = rd16(data, 6)
    rate = rd32(data, 8)
    samples = rd32(data, 12)
    payload = data[header:]
    if codec == 0:  # PCM16
        out = list(struct.unpack("<%dh" % samples, payload[:samples * 2]))
        return rate, out
    # ADPCM
    out = []
    predictor = 0
    step_index = 0
    pos = 0
    for _ in range(samples):
        in_block = pos % GWMU_ADPCM_BLOCK
        bs = (pos // GWMU_ADPCM_BLOCK) * GWMU_ADPCM_BLOCK_BYTES
        block = payload[bs:bs + GWMU_ADPCM_BLOCK_BYTES]
        if in_block == 0:
            predictor = rd16(block, 0)
            if predictor >= 32768:
                predictor -= 65536
            step_index = block[2]
            if step_index > 88:
                step_index = 88
        byte = block[4 + in_block // 2]
        code = (byte >> 4) if (in_block & 1) else (byte & 15)
        step = IMA_STEP[step_index]
        diff = step >> 3
        if code & 4:
            diff += step
        if code & 2:
            diff += step >> 1
        if code & 1:
            diff += step >> 2
        pp = predictor - diff if (code & 8) else predictor + diff
        if pp > 32767:
            pp = 32767
        if pp < -32768:
            pp = -32768
        predictor = pp
        idx = step_index + IMA_INDEX[code & 7]
        step_index = 0 if idx < 0 else (88 if idx > 88 else idx)
        out.append(predictor)
        pos += 1
    return rate, out


def extract(pack_path, out_dir):
    data = open(pack_path, "rb").read()
    assert data[:8] == b"GWJJ01\0\0"
    count = struct.unpack_from("<I", data, 8)[0]
    total = struct.unpack_from("<I", data, 12)[0]
    assert total == len(data)
    os.makedirs(out_dir, exist_ok=True)
    off = 16
    entries = []
    for i in range(count):
        name = data[off:off + 24].split(b"\0")[0].decode()
        o, s = struct.unpack_from("<II", data, off + 24)
        entries.append((name, o, s))
        off += 32
    mus_entries = [e for e in entries if e[0].endswith(".MUS")]
    print("found %d .MUS entries" % len(mus_entries))
    for name, o, s in mus_entries:
        blob = data[o:o + s]
        if blob[:4] != b"GWMU":
            print("  skip (not GWMU):", name)
            continue
        rate, samples = decode_mus(blob)
        wav_path = os.path.join(out_dir, name.replace(".MUS", ".wav"))
        with wave.open(wav_path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            import array
            arr = array.array("h", samples)
            w.writeframes(arr.tobytes())
        print("  wrote %s (%.1f s)" % (os.path.basename(wav_path), len(samples) / rate))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    extract(sys.argv[1], sys.argv[2])
