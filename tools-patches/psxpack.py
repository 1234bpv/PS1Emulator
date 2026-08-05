#!/usr/bin/env python3
# Pack a raw 2352-byte-per-sector PS1 .bin into a container this fork's cdriso.c
# can read: CISO (.cbn) or PSISOIMG (.pbp). Also verifies a packed file by
# decoding it exactly the way cdread_compressed() does and comparing sectors
# against the source.
#
# Constraints taken from libpcsxcore/cdriso.c, not from the public format spec:
#   .cbn  handlecbin(): block_shift = 0, so cdbuffer_size_expect = 2352 ->
#         block_size MUST be 2352 (usual PSP ciso tools emit 2048 and are unusable).
#         Index has index_len+1 entries; the last one bounds the last block.
#   .pbp  handlepbp(): block_shift = 4 -> 16 sectors = 37632 bytes per block.
#         The loop breaks on the first size==0 entry and uses THAT entry's offset
#         as the end of the last block, so the terminator must carry the end
#         offset with size 0.
#   Both go through uncompress2_pcsx() -> inflateInit2(-15): raw DEFLATE,
#         no zlib header, no adler32.

import os, struct, sys, time, zlib

SECTOR = 2352
PBP_BLK = 16
MSB = 0x80000000


def msf_bcd(sectors):
    m, rest = divmod(sectors, 75 * 60)
    s, f = divmod(rest, 75)
    return bytes((int(str(m), 16) if m < 100 else 0x99,
                  int(str(s), 16), int(str(f), 16)))


def deflate(buf, level):
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    return c.compress(buf) + c.flush()


def pack_cbn(src, dst, level):
    total = os.path.getsize(src)
    if total % SECTOR:
        sys.exit("source is not a multiple of 2352 bytes")
    n = total // SECTOR
    hdr = struct.pack("<4sIQIBBH", b"CISO", 24, total, SECTOR, 1, 0, 0)
    index_bytes = (n + 1) * 4
    data_start = len(hdr) + index_bytes

    index = []
    plain = 0
    t0 = time.time()
    with open(src, "rb") as fi, open(dst, "wb") as fo:
        fo.seek(data_start)
        off = data_start
        while True:
            raw = fi.read(SECTOR)
            if not raw:
                break
            comp = deflate(raw, level)
            if len(comp) >= SECTOR:
                index.append(off | MSB)      # MSB set = stored plain
                fo.write(raw)
                off += SECTOR
                plain += 1
            else:
                index.append(off)
                fo.write(comp)
                off += len(comp)
            if len(index) % 20000 == 0:
                report(len(index), n, off, t0)
        index.append(off)                    # bound of the last block
        fo.seek(0)
        fo.write(hdr)
        fo.write(struct.pack("<%dI" % len(index), *index))
    print("\n%s: %d sectors, %d stored plain, %d -> %d bytes (%.1f%%)"
          % (dst, n, plain, total, off, 100.0 * off / total))


def zero_ecc_block(buf):
    """Zero the 276 ECC bytes of every MODE2/FORM1 sector in place.

    Nothing in software reads ECC - the drive hardware did the correction - so the
    emulator never misses it, and a run of zeros costs DEFLATE almost nothing.
    This needs no reader change at all: the block still inflates to 16 raw sectors.
    Deliberately NOT touched: EDC (a game could checksum it) and the sync/header
    (ISOgetBuffer hands the header to the game).
    """
    b = bytearray(buf)
    n = 0
    for off in range(0, len(b) - SECTOR + 1, SECTOR):
        if b[off:off + 12] == SYNC and b[off + 15] == 2 and not (b[off + 18] & 0x20):
            b[off + 2076:off + 2352] = b"\0" * 276
            n += 1
    return bytes(b), n


def pack_pbp(src, dst, level, zecc=False):
    total = os.path.getsize(src)
    if total % SECTOR:
        sys.exit("source is not a multiple of 2352 bytes")
    n = total // SECTOR
    nblk = (n + PBP_BLK - 1) // PBP_BLK
    if nblk > (0x100000 - 0x4000) // 32:
        sys.exit("image too large for the pbp index")

    psar = 0x28
    # PBP header: magic, version, 8 section offsets; the last one is PSAR.
    hdr = struct.pack("<4sI8I", b"\x00PBP", 0x10000, *([psar] * 8))

    index = []
    zeroed = 0
    t0 = time.time()
    with open(src, "rb") as fi, open(dst, "wb") as fo:
        fo.seek(psar + 0x100000)
        off = 0                              # offsets are relative to cdimg_base
        while True:
            raw = fi.read(SECTOR * PBP_BLK)
            if not raw:
                break
            if len(raw) < SECTOR * PBP_BLK:  # last block: pad to full size
                raw += b"\0" * (SECTOR * PBP_BLK - len(raw))
            if zecc:
                raw, nz = zero_ecc_block(raw)
                zeroed += nz
            comp = deflate(raw, level)
            fo.write(comp)
            index.append((off, len(comp)))
            off += len(comp)
            if len(index) % 2000 == 0:
                report(len(index), nblk, off, t0)
        end = fo.tell()

        fo.seek(0)
        fo.write(hdr)
        fo.seek(psar)
        fo.write(b"PSISOIMG0000")

        # TOC at +0x800. Entries are sizeof(toc_entry) = 10 bytes (all char, no
        # padding): type, pad0, track, index0[3], pad1, index1[3].
        toc = bytearray()
        toc += b"\0" * 10                                    # skipped by the reader
        toc += bytes((0, 0, 0)) + b"\0\0\0" + b"\0" + bytes((1, 0, 0))   # numtracks
        toc += bytes((0, 0, 0)) + b"\0\0\0" + b"\0" + msf_bcd(n)         # cd_length
        toc += bytes((2, 0, 1)) + msf_bcd(0) + b"\0" + msf_bcd(150)      # track 1, data
        fo.seek(psar + 0x800)
        fo.write(toc)

        fo.seek(psar + 0x4000)
        for o, s in index:
            fo.write(struct.pack("<II24x", o, s))
        fo.write(struct.pack("<II24x", off, 0))              # terminator carries the end
    print("\n%s: %d blocks of %d sectors%s, %d -> %d bytes (%.1f%%)"
          % (dst, nblk, PBP_BLK, ", ECC zeroed in %d sectors" % zeroed if zecc else "",
             total, end, 100.0 * end / total))


# --- ECM: strip the fields the reader can recompute ------------------------
# Reference implementation, mirrored by ecm_rebuild_sector() in cdriso.c. Used
# here to decide per sector whether stripping is lossless, and by verify().

SYNC = b"\x00" + b"\xff" * 10 + b"\x00"

_edc_lut = []
for _i in range(256):
    _e = _i
    for _ in range(8):
        _e = (_e >> 1) ^ (0xD8018001 if _e & 1 else 0)
    _edc_lut.append(_e)

_ecc_f = [0] * 256
_ecc_b = [0] * 256
for _i in range(256):
    _j = ((_i << 1) ^ (0x11D if _i & 0x80 else 0)) & 0xFF
    _ecc_f[_i] = _j
    _ecc_b[_i ^ _j] = _i


def ecm_edc(buf):
    c = 0
    for b in buf:
        c = (c >> 8) ^ _edc_lut[(c ^ b) & 0xFF]
    return c


def _ecc_pass(src, major_count, minor_count, major_mult, minor_inc, dest, doff):
    size = major_count * minor_count
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            t = src[index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= t
            b ^= t
            a = _ecc_f[a]
        a = _ecc_b[_ecc_f[a] ^ b]
        dest[doff + major] = a
        dest[doff + major + major_count] = a ^ b


def ecm_ecc(sec):
    # The Q pass reads 52*43 = 2236 bytes from the same origin, i.e. the P parity
    # the first pass just wrote - so P has to be in place before Q runs.
    src = bytearray(sec[12:2248])
    src[0:4] = b"\0\0\0\0"
    out = bytearray(276)
    _ecc_pass(src, 86, 24, 2, 86, out, 0)
    src[2064:2236] = out[0:172]
    _ecc_pass(src, 52, 43, 86, 88, out, 172)
    return bytes(out)


def ecm_type(sec, lba, mode):
    """0 = MODE2/FORM1, 1 = FORM2, 2 = keep verbatim (not reconstructible)."""
    if sec[0:12] != SYNC or sec[15] != 2 or sec[16:20] != sec[20:24]:
        return 2
    f = lba + 150
    m, f = divmod(f, 60 * 75)
    s, f = divmod(f, 75)
    if sec[12:15] != bytes((int(str(m), 16) if m < 100 else -1,
                            int(str(s), 16), int(str(f), 16))):
        return 2
    if sec[18] & 0x20:
        # FORM2 EDC is optional; the reader writes zero, so anything else is lost.
        return 1 if sec[2348:2352] == b"\0\0\0\0" else 2
    if ecm_edc(sec[16:2072]) != int.from_bytes(sec[2072:2076], "little"):
        return 2
    # mode >= 1 zeroes ECC on read, so its stored value no longer has to match.
    if mode == 0 and ecm_ecc(sec) != sec[2076:2352]:
        return 2
    return 0


def ecm_strip(sec, t):
    if t == 0:
        return b"\0" + sec[16:20] + sec[24:2072]
    if t == 1:
        return b"\x01" + sec[16:20] + sec[24:2348]
    return b"\x02" + sec


def pack_cise(src, dst, level, spb=16, mode=1):
    total = os.path.getsize(src)
    if total % SECTOR:
        sys.exit("source is not a multiple of 2352 bytes")
    n = total // SECTOR
    nblk = (n + spb - 1) // spb
    block_size = spb * SECTOR
    # total_bytes must cover whole blocks: the reader derives the index length by
    # integer division, so a partial tail block would be unreachable.
    total_bytes = nblk * spb * SECTOR
    hdr = struct.pack("<4sIQIBBBB", b"CISE", 24, total_bytes, block_size, 2, 0, mode, 0)
    data_start = len(hdr) + (nblk + 1) * 4

    index, kinds = [], [0, 0, 0]
    t0 = time.time()
    with open(src, "rb") as fi, open(dst, "wb") as fo:
        fo.seek(data_start)
        off = data_start
        for b in range(nblk):
            blk = bytearray()
            for j in range(spb):
                lba = b * spb + j
                sec = fi.read(SECTOR) if lba < n else b""
                if len(sec) < SECTOR:
                    sec = (sec + b"\0" * SECTOR)[:SECTOR]   # pad the tail block
                t = ecm_type(sec, lba, mode)
                kinds[t] += 1
                blk += ecm_strip(sec, t)
            comp = deflate(bytes(blk), level)
            if len(comp) >= len(blk):
                index.append(off | MSB)
                fo.write(blk)
                off += len(blk)
            else:
                index.append(off)
                fo.write(comp)
                off += len(comp)
            if (b + 1) % 2000 == 0:
                report(b + 1, nblk, off, t0)
        index.append(off)
        fo.seek(0)
        fo.write(hdr)
        fo.write(struct.pack("<%dI" % len(index), *index))
    print("\n%s: %d blocks of %d sectors, mode=%d, form1=%d form2=%d raw=%d"
          % (dst, nblk, spb, mode, kinds[0], kinds[1], kinds[2]))
    print("%d -> %d bytes (%.1f%%)" % (total, off, 100.0 * off / total))


def report(done, n, off, t0):
    el = time.time() - t0
    sys.stdout.write("\r  %d/%d  %.0f MB out  %.0f s  eta %.0f s"
                     % (done, n, off / 1e6, el, el * (n - done) / max(done, 1)))
    sys.stdout.flush()


# --- verification: mirrors cdread_compressed() including its quirks ----------

def ecm_rebuild(sec, lba, mode):
    """Host mirror of ecm_rebuild_sector() in cdriso.c."""
    sec = bytearray(sec)
    sec[0:12] = SYNC
    sec[20:24] = sec[16:20]
    if sec[18] & 0x20:
        sec[2348:2352] = b"\0\0\0\0"
    else:
        edc = ecm_edc(sec[16:2072]) if mode < 2 else 0
        sec[2072:2076] = edc.to_bytes(4, "little")
        sec[12:16] = b"\0\0\0\0"
        sec[2076:2352] = ecm_ecc(sec) if mode == 0 else b"\0" * 276
    f = lba + 150
    m, f = divmod(f, 60 * 75)
    s, f = divmod(f, 75)
    sec[12:15] = bytes((int(str(m), 16), int(str(s), 16), int(str(f), 16)))
    sec[15] = 2
    return bytes(sec)


def ecm_expand(src, nsectors, lba0, mode):
    out, off = [], 0
    for i in range(nsectors):
        t = src[off]
        off += 1
        if t == 2:
            out.append(bytes(src[off:off + SECTOR]))
            off += SECTOR
            continue
        body = 2048 if t == 0 else 2324
        sec = bytearray(SECTOR)
        sec[16:20] = src[off:off + 4]
        sec[24:24 + body] = src[off + 4:off + 4 + body]
        off += 4 + body
        out.append(ecm_rebuild(sec, lba0 + i, mode))
    return b"".join(out)


def open_cbn(f):
    magic, hsize, total, bsize, ver, align, m0, _ = struct.unpack("<4sIQIBBBB", f.read(24))
    if magic not in (b"CISO", b"CISE"):
        sys.exit("bad magic")
    ecm = None
    if magic == b"CISE":
        ecm = m0
        if bsize % SECTOR or bsize // SECTOR not in (1, 2, 4, 8, 16):
            sys.exit("bad cise block size %d" % bsize)
    elif bsize != SECTOR:
        sys.exit("block_size %d: the reader expects 2352" % bsize)
    shift = (bsize // SECTOR).bit_length() - 1
    if hsize not in (0, 24):
        f.seek(hsize)
    n = total // bsize
    raw = struct.unpack("<%dI" % (n + 1), f.read((n + 1) * 4))
    return shift, [((v & 0x7fffffff) << align) | ((v & MSB) and MSB or 0) for v in raw], \
        n << shift, ecm


def open_pbp(f):
    psar = struct.unpack("<I", f.read(40)[36:40])[0]
    f.seek(psar)
    if f.read(10) != b"PSISOIMG00":
        sys.exit("bad psar sig")
    f.seek(psar + 0x800 + 20)
    cd_length = f.read(10)[7:10]
    n = (cd_length[0] >> 4) * 10 + (cd_length[0] & 15)
    n = n * 60 * 75 + ((cd_length[1] >> 4) * 10 + (cd_length[1] & 15)) * 75 \
        + ((cd_length[2] >> 4) * 10 + (cd_length[2] & 15))
    f.seek(psar + 0x4000)
    base, table = psar + 0x100000, []
    for _ in range((0x100000 - 0x4000) // 32):
        o, s = struct.unpack("<II", f.read(32)[:8])
        if s == 0:
            table.append(base + o)
            break
        table.append(base + o)
    return 4, table, n, None


def verify(src, dst, sample):
    src_n = os.path.getsize(src) // SECTOR
    with open(dst, "rb") as f:
        shift, table, n, ecm = (open_pbp if dst.lower().endswith(".pbp") else open_cbn)(f)
        fi = open(src, "rb")
        step = max(1, n // sample) if sample else 1
        checked = bad = zecc = 0
        t0 = time.time()
        for sector in range(0, min(n, src_n), step):
            block = sector >> shift
            start = table[block] & ~MSB
            size = (table[block + 1] & ~MSB) - start
            f.seek(start)
            buf = f.read(size)
            if not (table[block] & MSB):
                buf = zlib.decompressobj(-15).decompress(buf)
            if ecm is not None:
                buf = ecm_expand(buf, 1 << shift, block << shift, ecm)
            expect_len = SECTOR << shift
            if len(buf) != expect_len:
                print("block %d: %d != %d bytes" % (block, len(buf), expect_len))
                bad += 1
                continue
            got = buf[(sector & ((1 << shift) - 1)) * SECTOR:][:SECTOR]
            fi.seek(sector * SECTOR)
            want = fi.read(SECTOR)
            # mode 1 zeroes ECC on read and mode 2 also zeroes EDC, so those
            # ranges are expected to differ from the source - compare the rest.
            end = SECTOR
            if ecm and not (want[18] & 0x20):
                end = 2076 if ecm == 1 else 2072
            elif (not (want[18] & 0x20) and got[2076:2352] == b"\0" * 276
                  and want[2076:2352] != b"\0" * 276):
                end = 2076          # packed by pbpz: ECC deliberately zeroed
                zecc += 1
            if got[:end] != want[:end]:
                print("sector %d mismatch (first %d bytes)" % (sector, end))
                bad += 1
            checked += 1
        fi.close()
    note = ""
    if ecm:
        note = "  [ECC%s not compared: zeroed by mode %d]" % ("/EDC" if ecm == 2 else "", ecm)
    elif zecc:
        note = "  [%d sectors carry zeroed ECC, compared up to byte 2076]" % zecc
    print("verified %d sectors (every %d of %d), %d bad, %.1f s%s"
          % (checked, step, n, bad, time.time() - t0, note))
    return bad == 0


def unpack(img, out):
    """Decode a container back to a raw .bin. No archiver can do this: the blocks
    are headerless raw DEFLATE behind an offset table, so there is nothing for
    7-Zip to recognise. NOT bit-identical when the image was made with zeroed ECC
    (pbpz, or cise mode >= 1) - the sectors come back playable but with zero ECC.
    """
    with open(img, "rb") as f:
        shift, table, n, ecm = (open_pbp if img.lower().endswith(".pbp") else open_cbn)(f)
        written = 0
        limit = n * SECTOR
        with open(out, "wb") as fo:
            for b in range(len(table) - 1):
                start = table[b] & ~MSB
                size = (table[b + 1] & ~MSB) - start
                f.seek(start)
                buf = f.read(size)
                if not (table[b] & MSB):
                    buf = zlib.decompressobj(-15).decompress(buf)
                if ecm is not None:
                    buf = ecm_expand(buf, 1 << shift, b << shift, ecm)
                if written + len(buf) > limit:      # drop the padding of the tail block
                    buf = buf[:limit - written]
                fo.write(buf)
                written += len(buf)
                if b % 2000 == 0:
                    sys.stdout.write("\r  block %d/%d" % (b, len(table) - 1))
                    sys.stdout.flush()
    print("\n%s -> %s, %d bytes (%d sectors)" % (img, out, written, written // SECTOR))


def bench(dst, count):
    # sequential decode cost of `count` blocks, to price inflate on the host
    with open(dst, "rb") as f:
        shift, table, n, ecm = (open_pbp if dst.lower().endswith(".pbp") else open_cbn)(f)
        nblk = min(count, len(table) - 1)
        t0 = time.time()
        out = 0
        for b in range(nblk):
            start = table[b] & ~MSB
            f.seek(start)
            buf = f.read((table[b + 1] & ~MSB) - start)
            if not (table[b] & MSB):
                buf = zlib.decompressobj(-15).decompress(buf)
            out += len(buf)
    el = time.time() - t0
    print("%s: %d blocks, %.1f MB out in %.2f s = %.1f MB/s (host, includes file IO)"
          % (dst, nblk, out / 1e6, el, out / 1e6 / el))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: psxpack.py {cbn|pbp|pbpz|cise|verify|unpack|bench} <src> [dst] [level|sample]\n"
                 "  unpack: container -> raw .bin (nothing else reads these formats)\n"
                 "  pbpz: like pbp but with the ECC of every MODE2/FORM1 sector zeroed -\n"
                 "        58.4% vs 65.3% on Gran Turismo, and the reader needs no change\n"
                 "  cise: ECM-stripped CISO, 16 sectors per block; 4th arg is the\n"
                 "        rebuild mode - 0 honest, 1 zero ECC (default), 2 zero ECC+EDC")
    cmd, src = sys.argv[1], sys.argv[2]
    if cmd == "verify":
        sys.exit(0 if verify(src, sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 0) else 1)
    if cmd == "unpack":
        unpack(src, sys.argv[3])
        sys.exit(0)
    if cmd == "bench":
        bench(src, int(sys.argv[3]) if len(sys.argv) > 3 else 2000)
        sys.exit(0)
    dst = sys.argv[3]
    if cmd == "cise":
        pack_cise(src, dst, 9, 16, int(sys.argv[4]) if len(sys.argv) > 4 else 1)
    else:
        level = int(sys.argv[4]) if len(sys.argv) > 4 else 9
        if cmd == "cbn":
            pack_cbn(src, dst, level)
        else:
            pack_pbp(src, dst, level, cmd == "pbpz")
