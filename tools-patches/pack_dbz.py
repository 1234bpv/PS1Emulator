#!/usr/bin/env python3
import os
import sys
import struct
import zlib
import time

SECTOR = 2352
PBP_BLK = 16  # sectors per block (block_shift = 4)
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"

def bcd(val):
    return ((val // 10) << 4) | (val % 10)

def msf_bcd(sectors):
    m, rest = divmod(sectors, 75 * 60)
    s, f = divmod(rest, 75)
    return bytes((bcd(m) if m < 100 else 0x99, bcd(s), bcd(f)))

def deflate(buf, level=9):
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    return c.compress(buf) + c.flush()

def zero_ecc_block(buf):
    b = bytearray(buf)
    n = 0
    for off in range(0, len(b) - SECTOR + 1, SECTOR):
        if b[off:off + 12] == SYNC and b[off + 15] == 2 and not (b[off + 18] & 0x20):
            b[off + 2076:off + 2352] = b"\0" * 276
            n += 1
    return bytes(b), n

def main():
    cue_path = r"D:\ps1-images\DragonBallZ\Dragon Ball Z - Idainaru Dragon Ball Densetsu (Japan).cue"
    track1_bin = r"D:\ps1-images\DragonBallZ\Dragon Ball Z - Idainaru Dragon Ball Densetsu (Japan) (Track 01).bin"
    out_pbp = r"D:\ps1-images\DragonBallZ\Dragon Ball Z - Idainaru Dragon Ball Densetsu (Japan).pbp"

    # Exact track sector counts from the 20 files
    tracks_info = [
        (1, 2, 78977, 0),       # Track 1: DATA, 78977 sectors, pregap 0
        (2, 1, 896, 150),       # Track 2: AUDIO, 896 sectors, pregap 150 (INDEX 01 00:02:00)
        (3, 1, 5042, 0),        # Track 3: AUDIO
        (4, 1, 8418, 0),
        (5, 1, 1126, 0),
        (6, 1, 868, 0),
        (7, 1, 7529, 0),
        (8, 1, 8148, 0),
        (9, 1, 3185, 0),
        (10, 1, 10802, 0),
        (11, 1, 4276, 0),
        (12, 1, 4276, 0),
        (13, 1, 4276, 0),
        (14, 1, 4276, 0),
        (15, 1, 4276, 0),
        (16, 1, 4276, 0),
        (17, 1, 4276, 0),
        (18, 1, 4276, 0),
        (19, 1, 596, 0),
        (20, 1, 13225, 0),
    ]

    # Calculate CD geometry
    # TOC calculations mirror parsecue in cdriso.c:537 and completeCue in imgconv.cpp:348
    # toc_sector starts at 2 * 75 = 150
    toc_tracks = []
    toc_sector = 2 * 75
    for trk_num, trk_type, trk_len, pregap in tracks_info:
        toc_sector += pregap
        start_sec = toc_sector
        start_offset_sec = start_sec - 2 * 75
        toc_tracks.append({
            "num": trk_num,
            "type": trk_type, # 1 = CDDA, 2 = DATA
            "start": start_sec,
            "start_offset": start_offset_sec,
            "length": trk_len
        })
        toc_sector += trk_len

    total_cd_sectors = toc_sector
    print(f"Total CD sectors: {total_cd_sectors} ({msf_bcd(total_cd_sectors).hex()})")
    for t in toc_tracks[:5]:
        print(f"  Trk {t['num']:02d}: type={t['type']} start={t['start']} (MSF {msf_bcd(t['start']).hex()}) len={t['length']}")
    print(f"  ... Trk 20: start={toc_tracks[-1]['start']} len={toc_tracks[-1]['length']}")

    # Number of 16-sector blocks needed for full image
    total_blocks = (total_cd_sectors + PBP_BLK - 1) // PBP_BLK
    track1_sectors = tracks_info[0][2]
    track1_blocks = (track1_sectors + PBP_BLK - 1) // PBP_BLK

    print(f"Packaging {track1_blocks} blocks of Track 01 and {total_blocks - track1_blocks} silent audio blocks (total {total_blocks} blocks)...")

    psar = 0x28
    hdr = struct.pack("<4sI8I", b"\x00PBP", 0x10000, *([psar] * 8))

    index = []
    t0 = time.time()
    zero_block_comp = deflate(b"\0" * (SECTOR * PBP_BLK), 9)

    with open(track1_bin, "rb") as fi, open(out_pbp, "wb") as fo:
        fo.seek(psar + 0x100000)
        off = 0

        # 1. Write Track 01 blocks
        for b_idx in range(track1_blocks):
            raw = fi.read(SECTOR * PBP_BLK)
            if len(raw) < SECTOR * PBP_BLK:
                raw += b"\0" * (SECTOR * PBP_BLK - len(raw))
            raw, _ = zero_ecc_block(raw)
            comp = deflate(raw, 9)
            fo.write(comp)
            index.append((off, len(comp)))
            off += len(comp)

        # 2. Write silent blocks for tracks 2-20
        for b_idx in range(track1_blocks, total_blocks):
            fo.write(zero_block_comp)
            index.append((off, len(zero_block_comp)))
            off += len(zero_block_comp)

        end = fo.tell()

        # Write PBP header
        fo.seek(0)
        fo.write(hdr)
        fo.seek(psar)
        fo.write(b"PSISOIMG0000")

        # Write TOC at psar + 0x800
        # Entry 0: dummy
        # Entry 1: track count in index1[0]
        # Entry 2: lead-out cd length in index1
        # Entry 3..22: tracks 1..20
        toc = bytearray()
        toc += b"\0" * 10
        # Entry 1: track count
        toc += bytes((0, 0, 0)) + b"\0\0\0" + b"\0" + bytes((bcd(len(toc_tracks)), 0, 0))
        # Entry 2: cd_length
        toc += bytes((0, 0, 0)) + b"\0\0\0" + b"\0" + msf_bcd(total_cd_sectors)
        # Tracks
        for t in toc_tracks:
            entry = bytearray(10)
            entry[0] = t["type"] # 1 = CDDA, 2 = DATA
            entry[2] = bcd(t["num"])
            entry[3:6] = msf_bcd(t["start_offset"]) # index0: offset in image
            entry[7:10] = msf_bcd(t["start"])       # index1: sector on CD
            toc += entry

        fo.seek(psar + 0x800)
        fo.write(toc)

        # Write block index at psar + 0x4000
        fo.seek(psar + 0x4000)
        for o, s in index:
            fo.write(struct.pack("<II24x", o, s))
        fo.write(struct.pack("<II24x", off, 0)) # terminator

    elapsed = time.time() - t0
    pbp_size = os.path.getsize(out_pbp)
    print(f"Created {out_pbp}: {pbp_size} bytes ({pbp_size / 1024 / 1024:.2f} MB) in {elapsed:.1f}s")

if __name__ == "__main__":
    main()
