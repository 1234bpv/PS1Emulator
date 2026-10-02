// Generates the application icon bitmaps in src/symbian/gfx/ from a vector description.
//
//   node tools-patches/mkicon.js
//
// The icon is an original drawing: a dark rounded plate, a compact disc with a
// rainbow sheen, and in front of it a light grey game pad with a cross on the
// left and four coloured buttons on the right. It deliberately uses no letters
// and no shape or colour scheme that belongs to a console maker.
//
// Each of the three sizes S60 asks for (88, 44 and 22 pixels) is drawn
// separately rather than scaled from one image: at 22 the pad outline thickens,
// the cross and the buttons grow and the disc loses its inner hub, so that the
// two objects still read.
//
// Output is a 24-bit BMP plus an 8-bit soft mask per size (what mifconv takes,
// see group\icons.mk) and a PNG preview of all three for a human to look at.
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const PLATE_T = [0x2E, 0x34, 0x44], PLATE_B = [0x11, 0x14, 0x1C];
const PLATE_EDGE = [0x4A, 0x52, 0x66];
const DISC_T = [0xF2, 0xF4, 0xF8], DISC_B = [0x9A, 0xA2, 0xB0];
const HUB = [0xD6, 0xDA, 0xE2];
const PAD_T = [0xEE, 0xF0, 0xF4], PAD_B = [0xB2, 0xB9, 0xC4];
const PAD_LINE = [0x0C, 0x0E, 0x14];
const CROSS = [0x3C, 0x42, 0x4E];
const BTN = [[0xF2, 0xC8, 0x1E], [0xE5, 0x3B, 0x3B], [0x2F, 0x7B, 0xE8], [0x3C, 0xB8, 0x5A]];

const mix = (a, b, f) => [0, 1, 2].map(k => a[k] + (b[k] - a[k]) * f);
const clamp01 = v => Math.min(1, Math.max(0, v));

function hue(h) {
    const k = n => { const m = (n + h * 6) % 6; return 1 - Math.max(0, Math.min(m, 4 - m, 1)); };
    return [k(5), k(3), k(1)].map(v => 80 + 175 * v);
}

function inRoundedRect(x, y, x0, y0, x1, y1, r) {
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    const cx = Math.min(Math.max(x, x0 + r), x1 - r);
    const cy = Math.min(Math.max(y, y0 + r), y1 - r);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

const inCircle = (x, y, cx, cy, r) => (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;

// detail 2: everything, for 88 px. 1: bolder pad details, for 44 px.
// 0: thick outline, big cross and buttons, no hub and no rim, for 22 px.
function makeIcon(detail) {
    const DCX = 0.5, DCY = 0.37, DR = 0.31;
    const HOLE = detail === 0 ? 0.075 : 0.060;
    const HUB_R = 0.125;
    const LINE = detail === 0 ? 0.040 : detail === 1 ? 0.030 : 0.024;
    const ARM = detail === 0 ? 0.075 : detail === 1 ? 0.062 : 0.056;
    const ARM_W = detail === 0 ? 0.030 : detail === 1 ? 0.024 : 0.019;
    const BTN_OFF = detail === 0 ? 0.064 : detail === 1 ? 0.056 : 0.056;
    const BTN_R = detail === 0 ? 0.036 : detail === 1 ? 0.029 : 0.030;
    const LX = 0.31, RX = 0.70, PY = detail === 0 ? 0.70 : 0.685;

    // g widens the shape evenly: the outline is the same shape taken larger.
    function inPad(x, y, g) {
        return inRoundedRect(x, y, 0.17 - g, 0.575 - g, 0.83 + g, 0.80 + g, 0.10 + g) ||
               inCircle(x, y, 0.255, 0.745, 0.135 + g) ||
               inCircle(x, y, 0.745, 0.745, 0.135 + g);
    }

    const plate = (x, y) => mix(PLATE_T, PLATE_B, clamp01((y - 0.02) / 0.96));

    return function (x, y) {
        if (!inRoundedRect(x, y, 0.008, 0.008, 0.992, 0.992, 0.215)) return null;

        if (inPad(x, y, 0)) {
            if (Math.abs(x - LX) <= ARM && Math.abs(y - PY) <= ARM_W) return CROSS;
            if (Math.abs(y - PY) <= ARM && Math.abs(x - LX) <= ARM_W) return CROSS;
            for (let i = 0; i < 4; ++i) {
                const a = i * Math.PI / 2;
                if (inCircle(x, y, RX + Math.sin(a) * BTN_OFF, PY - Math.cos(a) * BTN_OFF, BTN_R)) return BTN[i];
            }
            return mix(PAD_T, PAD_B, clamp01((y - 0.575) / 0.30));
        }
        if (inPad(x, y, LINE)) return PAD_LINE;

        const dx = x - DCX, dy = y - DCY;
        const r = Math.sqrt(dx * dx + dy * dy);
        if (r <= DR) {
            if (r <= HOLE) return plate(x, y);
            if (detail > 0 && r <= HUB_R) return HUB;
            const base = mix(DISC_T, DISC_B, clamp01((y - (DCY - DR)) / (2 * DR)));
            if (detail > 0 && r >= DR - 0.014) return mix(base, [255, 255, 255], 0.5);
            // Two opposite rainbow sectors, strongest in the middle of the data ring.
            const ang = Math.atan2(dy, dx);
            const sector = Math.pow(Math.abs(Math.cos(ang - 0.6)), 3);
            const ring = clamp01(1 - Math.abs(r - 0.215) / 0.09);
            const w = (detail === 0 ? 0.85 : 0.75) * sector * ring;
            return mix(base, hue(((ang / (2 * Math.PI)) * 2 + 1) % 1), w);
        }

        // A faint lighter edge keeps the plate from vanishing on dark wallpapers.
        if (!inRoundedRect(x, y, 0.028, 0.028, 0.972, 0.972, 0.195)) return PLATE_EDGE;
        return plate(x, y);
    };
}

// Renders one size with 4x4 supersampling. The mask keeps the real coverage:
// mifconv takes an EGray256 soft mask, so the rounded plate edge stays smooth.
function render(size, detail) {
    const fn = makeIcon(detail);
    const SS = 4;
    const rgb = Buffer.alloc(size * size * 3);
    const mask = Buffer.alloc(size * size);
    for (let py = 0; py < size; ++py)
        for (let px = 0; px < size; ++px) {
            let r = 0, g = 0, b = 0, hits = 0;
            for (let sy = 0; sy < SS; ++sy)
                for (let sx = 0; sx < SS; ++sx) {
                    const c = fn((px + (sx + 0.5) / SS) / size, (py + (sy + 0.5) / SS) / size);
                    if (c) { r += c[0]; g += c[1]; b += c[2]; ++hits; }
                }
            const o = (py * size + px) * 3;
            if (hits) {
                rgb[o] = Math.round(r / hits);
                rgb[o + 1] = Math.round(g / hits);
                rgb[o + 2] = Math.round(b / hits);
            }
            mask[py * size + px] = Math.round(255 * hits / (SS * SS));
        }
    return { rgb, mask };
}

// 8-bit greyscale BMP with an identity palette. This is the EGray256 soft mask
// mifconv looks for beside the colour bitmap, under the name <file>_mask_soft.bmp.
function writeGrayBmp(file, size, gray) {
    const rowBytes = size, pad = (4 - (rowBytes % 4)) % 4;
    const pixels = Buffer.alloc((rowBytes + pad) * size);
    for (let y = 0; y < size; ++y) {
        const src = (size - 1 - y) * size;
        gray.copy(pixels, y * (rowBytes + pad), src, src + size);
    }
    const palette = Buffer.alloc(256 * 4);
    for (let i = 0; i < 256; ++i)
        palette[i * 4] = palette[i * 4 + 1] = palette[i * 4 + 2] = i;
    const head = Buffer.alloc(54);
    head.write('BM', 0, 'latin1');
    head.writeUInt32LE(54 + palette.length + pixels.length, 2);
    head.writeUInt32LE(54 + palette.length, 10);
    head.writeUInt32LE(40, 14);
    head.writeInt32LE(size, 18);
    head.writeInt32LE(size, 22);
    head.writeUInt16LE(1, 26);
    head.writeUInt16LE(8, 28);
    head.writeUInt32LE(pixels.length, 34);
    head.writeInt32LE(2835, 38);
    head.writeInt32LE(2835, 42);
    head.writeUInt32LE(256, 46);
    head.writeUInt32LE(256, 50);
    fs.writeFileSync(file, Buffer.concat([head, palette, pixels]));
}

// 24-bit BMP, bottom-up, rows padded to four bytes.
function writeBmp(file, size, rgb) {
    const rowBytes = size * 3, pad = (4 - (rowBytes % 4)) % 4;
    const pixels = Buffer.alloc((rowBytes + pad) * size);
    for (let y = 0; y < size; ++y) {
        const src = (size - 1 - y) * rowBytes;
        const dst = y * (rowBytes + pad);
        for (let x = 0; x < size; ++x) {
            pixels[dst + x * 3]     = rgb[src + x * 3 + 2];
            pixels[dst + x * 3 + 1] = rgb[src + x * 3 + 1];
            pixels[dst + x * 3 + 2] = rgb[src + x * 3];
        }
    }
    const head = Buffer.alloc(54);
    head.write('BM', 0, 'latin1');
    head.writeUInt32LE(54 + pixels.length, 2);
    head.writeUInt32LE(54, 10);
    head.writeUInt32LE(40, 14);
    head.writeInt32LE(size, 18);
    head.writeInt32LE(size, 22);
    head.writeUInt16LE(1, 26);
    head.writeUInt16LE(24, 28);
    head.writeUInt32LE(pixels.length, 34);
    head.writeInt32LE(2835, 38);
    head.writeInt32LE(2835, 42);
    fs.writeFileSync(file, Buffer.concat([head, pixels]));
}

const CRC = new Uint32Array(256).map((_, n) => {
    let c = n;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
});
function chunk(type, data) {
    const t = Buffer.from(type, 'latin1');
    let c = 0xFFFFFFFF;
    for (const b of Buffer.concat([t, data])) c = CRC[(c ^ b) & 0xFF] ^ (c >>> 8);
    const len = Buffer.alloc(4), crc = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    crc.writeUInt32BE((c ^ 0xFFFFFFFF) >>> 0);
    return Buffer.concat([len, t, data, crc]);
}

// Preview for people: each size enlarged to the same width (nearest neighbour,
// so single pixels stay visible) over a mid-grey ground, and under it the three
// at their real size, which is how the phone will show them.
function writePreview(file, icons) {
    const W = 3 * 280 + 20, H = 280 + 100, scale = { 88: 3, 44: 6, 22: 12 };
    const img = Buffer.alloc(W * H * 3);
    for (let i = 0; i < W * H; ++i) img.fill(0x80, i * 3, i * 3 + 3);
    const put = (icon, ox, oy, k) => {
        for (let y = 0; y < icon.size * k; ++y)
            for (let x = 0; x < icon.size * k; ++x) {
                const s = (Math.floor(y / k) * icon.size + Math.floor(x / k));
                const a = icon.mask[s] / 255, o = ((oy + y) * W + ox + x) * 3;
                for (let c = 0; c < 3; ++c)
                    img[o + c] = Math.round(img[o + c] * (1 - a) + icon.rgb[s * 3 + c] * a);
            }
    };
    icons.forEach((ic, i) => {
        put(ic, 10 + i * 290, 10, scale[ic.size]);
        put(ic, 10 + i * 290 + 20, 300, 1);
    });
    const raw = Buffer.alloc((W * 3 + 1) * H);
    for (let y = 0; y < H; ++y) img.copy(raw, y * (W * 3 + 1) + 1, y * W * 3, (y + 1) * W * 3);
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 2;
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, Buffer.concat([Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
        chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}

const SIZES = [[88, 2], [44, 1], [22, 0]];
const root = path.join(__dirname, '..');
const dir = path.join(root, 'src', 'symbian', 'gfx');
fs.mkdirSync(dir, { recursive: true });
const done = [];
for (const [size, detail] of SIZES) {
    const { rgb, mask } = render(size, detail);
    writeBmp(path.join(dir, 'ps1emu_' + size + '.bmp'), size, rgb);
    writeGrayBmp(path.join(dir, 'ps1emu_' + size + '_mask_soft.bmp'), size, mask);
    done.push({ size, rgb, mask });
    console.log('ps1emu_' + size + '.bmp + soft mask');
}
writePreview(path.join(root, 'plans', 'reports', 'icon-preview.png'), done);
