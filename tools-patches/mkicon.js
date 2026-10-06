// Generates the application icon bitmaps in src/symbian/gfx/ from a vector description.
//
//   node tools-patches/mkicon.js           variant A, the one in use, into src/symbian/gfx
//   node tools-patches/mkicon.js B         variant B (A..F) into plans/reports/icon-variants/B
//   node tools-patches/mkicon.js all       every variant plus the summary preview.png
//   node tools-patches/mkicon.js readme    variant A at 256 px with alpha, src/symbian/gfx/icon.png
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
function makeIconA(detail) {
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

// ---- Variants B..F, for comparison with A (the one in use). Every maker takes the
// detail level (2: 88 px, 1: 44 px, 0: 22 px) and returns fn(x, y) -> [r, g, b] or
// null for transparent, in unit coordinates, like makeIconA.

function inTriangle(x, y, ax, ay, bx, by, cx, cy) {
    const s = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
    const t = (cx - bx) * (y - by) - (cy - by) * (x - bx);
    const u = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);
    return (s >= 0 && t >= 0 && u >= 0) || (s <= 0 && t <= 0 && u <= 0);
}
const inQuad = (x, y, p) =>
    inTriangle(x, y, p[0][0], p[0][1], p[1][0], p[1][1], p[2][0], p[2][1]) ||
    inTriangle(x, y, p[0][0], p[0][1], p[2][0], p[2][1], p[3][0], p[3][1]);
const inEllipse = (x, y, cx, cy, rx, ry) => ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1;
const inPlate = (x, y) => inRoundedRect(x, y, 0.008, 0.008, 0.992, 0.992, 0.215);
const grad = (t, b, y, y0, y1) => mix(t, b, clamp01((y - y0) / (y1 - y0)));

// B: a classic grey pad with two handles, big on a teal plate.
function makeIconB(detail) {
    const BG_T = [0x2A, 0xB5, 0xA5], BG_B = [0x0E, 0x6B, 0x7A];
    const BODY_T = [0xE2, 0xE4, 0xE8], BODY_B = [0x98, 0x9E, 0xA8];
    const LINE = [0x0A, 0x2B, 0x33], SHADOW = [0x0B, 0x50, 0x5C];
    const K = detail === 0 ? 0.040 : detail === 1 ? 0.030 : 0.024;
    const GREY = [0x5A, 0x60, 0x6A], ORANGE = [0xF0, 0x7A, 0x2A];
    const ARM = detail === 0 ? 0.095 : 0.082, ARM_W = detail === 0 ? 0.038 : 0.030;
    const OFF = detail === 0 ? 0.075 : 0.068, BR = detail === 0 ? 0.042 : 0.034;
    const LX = 0.30, RX = 0.70, PY = 0.43;
    const pad = (x, y, k) =>
        inRoundedRect(x, y, 0.14 - k, 0.26 - k, 0.86 + k, 0.60 + k, 0.13 + k) ||
        inCircle(x, y, 0.245, 0.64, 0.14 + k) || inCircle(x, y, 0.755, 0.64, 0.14 + k);
    return function (x, y) {
        if (!inPlate(x, y)) return null;
        if (pad(x, y, 0)) {
            if (Math.abs(x - LX) <= ARM && Math.abs(y - PY) <= ARM_W) return GREY;
            if (Math.abs(y - PY) <= ARM && Math.abs(x - LX) <= ARM_W) return GREY;
            for (let i = 0; i < 4; ++i) {
                const a = i * Math.PI / 2;
                if (inCircle(x, y, RX + Math.sin(a) * OFF, PY - Math.cos(a) * OFF, BR))
                    return i === 1 ? ORANGE : GREY;
            }
            if (detail === 2 && (inRoundedRect(x, y, 0.435, 0.40, 0.485, 0.425, 0.012) ||
                                 inRoundedRect(x, y, 0.515, 0.40, 0.565, 0.425, 0.012))) return GREY;
            return grad(BODY_T, BODY_B, y, 0.26, 0.78);
        }
        if (pad(x, y, K)) return LINE;
        if (pad(x, y - 0.035, K)) return SHADOW;
        return grad(BG_T, BG_B, y, 0.02, 0.98);
    };
}

// C: a close-up of the disc on a light plate; the whole face is the rainbow.
function makeIconC(detail) {
    const BG_T = [0xFB, 0xFC, 0xFE], BG_B = [0xD9, 0xE0, 0xEA];
    const R = 0.445, HOLE = detail === 0 ? 0.085 : 0.068, HUB = 0.165;
    const SILVER = [0xE6, 0xE9, 0xEE], RIM = [0x9C, 0xA4, 0xB2];
    return function (x, y) {
        if (!inPlate(x, y)) return null;
        const dx = x - 0.5, dy = y - 0.5, r = Math.sqrt(dx * dx + dy * dy);
        const bg = grad(BG_T, BG_B, y, 0.02, 0.98);
        if (r > R) return bg;
        if (r <= HOLE) return bg;
        const ang = Math.atan2(dy, dx);
        if (detail > 0 && r <= HUB) return r >= HUB - 0.012 ? RIM : SILVER;
        const edge = detail === 0 ? 0.022 : 0.014;
        if (r >= R - edge) return RIM;
        const base = grad([0xF4, 0xF6, 0xFA], [0xB4, 0xBB, 0xC8], y, 0.05, 0.95);
        const w = clamp01((r - (detail > 0 ? HUB : HOLE)) / 0.05) * (detail === 0 ? 0.80 : 0.72);
        const rainbow = hue((ang / (2 * Math.PI) + 1 + 0.3 * Math.sin(2 * ang)) % 1);
        const glare = Math.pow(Math.max(0, Math.cos(ang + 0.9)), 10) * 0.55;
        return mix(mix(base, rainbow, w), [255, 255, 255], glare);
    };
}

// D: a retro television with a pad glowing on the screen, on an amber plate.
function makeIconD(detail) {
    const BG_T = [0xF7, 0xC6, 0x5C], BG_B = [0xDD, 0x86, 0x2A];
    const BODY_T = [0x44, 0x4A, 0x56], BODY_B = [0x1F, 0x23, 0x2A];
    const GLASS_T = [0x2E, 0x66, 0x5E], GLASS_B = [0x12, 0x32, 0x34];
    const GLOW_T = [0xDE, 0xFF, 0xE4], GLOW_B = [0x7C, 0xE2, 0x9C], GLOW_D = [0x2A, 0x6A, 0x58];
    const KNOB = [0xB8, 0xBE, 0xC8];
    const big = detail === 0;
    const sx0 = big ? 0.14 : 0.15, sx1 = big ? 0.86 : 0.85, sy0 = big ? 0.25 : 0.27, sy1 = big ? 0.68 : 0.64;
    const PY = big ? 0.455 : 0.46;
    const inGlow = (x, y) => big
        ? inRoundedRect(x, y, 0.29, 0.36, 0.71, 0.52, 0.06) || inCircle(x, y, 0.35, 0.49, 0.10) || inCircle(x, y, 0.65, 0.49, 0.10)
        : inRoundedRect(x, y, 0.33, PY - 0.075, 0.67, PY + 0.035, 0.05) ||
          inCircle(x, y, 0.375, PY + 0.04, 0.065) || inCircle(x, y, 0.625, PY + 0.04, 0.065);
    return function (x, y) {
        if (!inPlate(x, y)) return null;
        if (detail > 0) {
            // Antenna: two thin strokes from the top of the set.
            if (y >= 0.07 && y <= 0.21 && Math.abs((x - 0.5) - (0.21 - y) * 1.15) < 0.012) return BODY_B;
            if (y >= 0.07 && y <= 0.21 && Math.abs((x - 0.5) + (0.21 - y) * 1.15) < 0.012) return BODY_B;
        }
        if (inRoundedRect(x, y, 0.18, 0.79, 0.30, 0.87, 0.02) || inRoundedRect(x, y, 0.70, 0.79, 0.82, 0.87, 0.02))
            return BODY_B;
        if (inRoundedRect(x, y, 0.09, 0.20, 0.91, 0.80, 0.10)) {
            if (inRoundedRect(x, y, sx0, sy0, sx1, sy1, 0.06)) {
                if (inGlow(x, y)) {
                    if (detail === 2 && (
                        (Math.abs(x - 0.43) <= 0.028 && Math.abs(y - (PY - 0.01)) <= 0.008) ||
                        (Math.abs(y - (PY - 0.01)) <= 0.028 && Math.abs(x - 0.43) <= 0.008) ||
                        inCircle(x, y, 0.58, PY - 0.01, 0.011) || inCircle(x, y, 0.62, PY - 0.01, 0.011))) return GLOW_D;
                    return grad(GLOW_T, GLOW_B, y, 0.36, 0.55);
                }
                return grad(GLASS_T, GLASS_B, y, sy0, sy1);
            }
            if (detail > 0) {
                if (inCircle(x, y, 0.70, 0.735, 0.032) || inCircle(x, y, 0.80, 0.735, 0.032)) return KNOB;
                for (let i = 0; i < 3; ++i)
                    if (Math.abs(x - (0.16 + i * 0.03)) <= 0.007 && y >= 0.70 && y <= 0.77) return BODY_B;
            } else if (inCircle(x, y, 0.76, 0.76, 0.045)) return KNOB;
            return grad(BODY_T, BODY_B, y, 0.20, 0.80);
        }
        return grad(BG_T, BG_B, y, 0.02, 0.98);
    };
}

// E: flat and quiet, four abstract button shapes in a 2x2 grid, muted colours.
function makeIconE(detail) {
    const BG_T = [0x34, 0x42, 0x4B], BG_B = [0x21, 0x2B, 0x33];
    const MUSTARD = [0xD9, 0xA4, 0x41], BLUE = [0x6F, 0x9A, 0xB8];
    const SAND = [0xCB, 0xC2, 0xAB], CLAY = [0xB8, 0x6E, 0x5C];
    const S = detail === 0 ? 0.165 : 0.145;
    const T = S * (detail === 0 ? 0.36 : 0.30);
    const C = [[0.29, 0.29], [0.71, 0.29], [0.29, 0.71], [0.71, 0.71]];
    return function (x, y) {
        if (!inPlate(x, y)) return null;
        let dx = x - C[0][0], dy = y - C[0][1];
        if (inTriangle(x, y, C[0][0], C[0][1] - S * 1.1, C[0][0] + S * 1.15, C[0][1] + S * 0.85,
                       C[0][0] - S * 1.15, C[0][1] + S * 0.85)) return MUSTARD;
        if (inCircle(x, y, C[1][0], C[1][1], S)) return BLUE;
        dx = x - C[2][0]; dy = y - C[2][1];
        const u = Math.abs(dx + dy) / Math.SQRT2, v = Math.abs(dx - dy) / Math.SQRT2;
        if ((u <= T && v <= S * 1.15) || (v <= T && u <= S * 1.15)) return SAND;
        if (inRoundedRect(x, y, C[3][0] - S * 0.95, C[3][1] - S * 0.95, C[3][0] + S * 0.95, C[3][1] + S * 0.95, 0.02))
            return CLAY;
        return grad(BG_T, BG_B, y, 0.02, 0.98);
    };
}

// F: a console box in isometry, with a disc showing under the lid, on a violet plate.
function makeIconF(detail) {
    const BG_T = [0x6C, 0x59, 0xBA], BG_B = [0x2E, 0x24, 0x66];
    const TOP = [[0.5, 0.12], [0.88, 0.31], [0.5, 0.50], [0.12, 0.31]];
    const LEFT = [[0.12, 0.31], [0.5, 0.50], [0.5, 0.90], [0.12, 0.70]];
    const RIGHT = [[0.5, 0.50], [0.88, 0.31], [0.88, 0.70], [0.5, 0.90]];
    const DARK = [0x36, 0x3C, 0x48], DISC = [0xE8, 0xEB, 0xF0];
    const LID_X = detail === 0 ? 0.22 : 0.20, LID_Y = LID_X / 2;
    const DISC_X = LID_X - 0.035, DISC_Y = DISC_X / 2;
    return function (x, y) {
        if (!inPlate(x, y)) return null;
        if (inQuad(x, y, TOP)) {
            if (inEllipse(x, y, 0.5, 0.31, DISC_X * 0.22, DISC_Y * 0.22)) return DARK;
            if (inEllipse(x, y, 0.5, 0.31, DISC_X, DISC_Y)) return mix(DISC, [0xB0, 0xB8, 0xC6], clamp01((y - 0.22) / 0.18));
            if (inEllipse(x, y, 0.5, 0.31, LID_X, LID_Y)) return DARK;
            return grad([0xF4, 0xF5, 0xF9], [0xCE, 0xD3, 0xDC], y, 0.12, 0.50);
        }
        if (inQuad(x, y, LEFT)) {
            const s = y - 0.31 - (x - 0.12) * 0.5;
            if (detail === 2 && x >= 0.18 && x <= 0.42 &&
                ((s >= 0.14 && s <= 0.158) || (s >= 0.19 && s <= 0.208) || (s >= 0.24 && s <= 0.258)))
                return [0x74, 0x7B, 0x89];
            return grad([0xA8, 0xAF, 0xBC], [0x86, 0x8D, 0x9B], y, 0.31, 0.90);
        }
        if (inQuad(x, y, RIGHT)) {
            if (detail > 0 && inQuad(x, y, [[0.58, 0.66], [0.80, 0.55], [0.80, 0.59], [0.58, 0.70]])) return DARK;
            if (detail > 0 && inCircle(x, y, 0.60, 0.78, 0.020)) return [0xF2, 0xB6, 0x3E];
            if (detail === 0 && inCircle(x, y, 0.69, 0.66, 0.045)) return [0xF2, 0xB6, 0x3E];
            return grad([0x70, 0x77, 0x86], [0x52, 0x58, 0x66], y, 0.31, 0.90);
        }
        return grad(BG_T, BG_B, y, 0.02, 0.98);
    };
}

const MAKERS = { A: makeIconA, B: makeIconB, C: makeIconC, D: makeIconD, E: makeIconE, F: makeIconF };

// Renders one size with 4x4 supersampling. The mask keeps the real coverage:
// mifconv takes an EGray256 soft mask, so the rounded plate edge stays smooth.
function render(size, detail, maker) {
    const fn = maker(detail);
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

const GLYPH = {
    A: ['01110', '10001', '10001', '11111', '10001', '10001', '10001'],
    B: ['11110', '10001', '10001', '11110', '10001', '10001', '11110'],
    C: ['01110', '10001', '10000', '10000', '10000', '10001', '01110'],
    D: ['11110', '10001', '10001', '10001', '10001', '10001', '11110'],
    E: ['11111', '10000', '10000', '11110', '10000', '10000', '11111'],
    F: ['11111', '10000', '10000', '11110', '10000', '10000', '10000'],
};

// Summary sheet of all variants: a row per variant (letter, 88/44/22 at real size,
// 88 enlarged twice) and below it a mock-up of the S60 menu grid on a dark and a
// light ground, at the two sizes the grid actually uses.
function writeSummary(file, all) {
    const letters = Object.keys(all);
    const W = 940, ROW = 200, GRID = 190, H = letters.length * ROW + GRID + 20;
    const img = Buffer.alloc(W * H * 3);
    const fillRect = (x0, y0, w, h, c) => {
        for (let y = y0; y < y0 + h; ++y)
            for (let x = x0; x < x0 + w; ++x) img.set(c, (y * W + x) * 3);
    };
    fillRect(0, 0, W, H, [0x80, 0x80, 0x80]);
    const put = (icon, ox, oy, k) => {
        for (let y = 0; y < icon.size * k; ++y)
            for (let x = 0; x < icon.size * k; ++x) {
                const s = Math.floor(y / k) * icon.size + Math.floor(x / k);
                const a = icon.mask[s] / 255, o = ((oy + y) * W + ox + x) * 3;
                for (let c = 0; c < 3; ++c)
                    img[o + c] = Math.round(img[o + c] * (1 - a) + icon.rgb[s * 3 + c] * a);
            }
    };
    const by = (X, size) => all[X].find(i => i.size === size);
    letters.forEach((X, i) => {
        const y0 = i * ROW + 10;
        GLYPH[X].forEach((row, gy) => [...row].forEach((bit, gx) => {
            if (bit === '1') fillRect(20 + gx * 8, y0 + 70 + gy * 8, 8, 8, [0x10, 0x10, 0x10]);
        }));
        put(by(X, 88), 90, y0 + 40, 1);
        put(by(X, 44), 210, y0 + 40, 1);
        put(by(X, 22), 290, y0 + 40, 1);
        put(by(X, 88), 360, y0, 2);
    });
    const gy = letters.length * ROW + 10;
    [[10, [0x12, 0x14, 0x1A]], [480, [0xEA, 0xEC, 0xF0]]].forEach(([gx, bg]) => {
        fillRect(gx, gy, 450, GRID - 10, bg);
        letters.forEach((X, i) => {
            put(by(X, 44), gx + 15 + i * 72, gy + 20, 1);
            put(by(X, 22), gx + 15 + i * 72 + 11, gy + 90, 1);
        });
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

function renderAll(letter) {
    return SIZES.map(([size, detail]) => ({ size, ...render(size, detail, MAKERS[letter]) }));
}

function writeSet(dir, icons) {
    fs.mkdirSync(dir, { recursive: true });
    for (const { size, rgb, mask } of icons) {
        writeBmp(path.join(dir, 'ps1emu_' + size + '.bmp'), size, rgb);
        writeGrayBmp(path.join(dir, 'ps1emu_' + size + '_mask_soft.bmp'), size, mask);
        console.log(path.join(dir, 'ps1emu_' + size + '.bmp') + ' + soft mask');
    }
}

// No argument: variant A into src/symbian/gfx, as before. A letter: that variant
// into plans/reports/icon-variants/<letter>. "all": every variant plus the summary.
// RGBA PNG of one rendered size, for the README.
function writeRgbaPng(file, icon) {
    const n = icon.size, raw = Buffer.alloc((n * 4 + 1) * n);
    for (let y = 0; y < n; ++y)
        for (let x = 0; x < n; ++x) {
            const s = y * n + x, o = y * (n * 4 + 1) + 1 + x * 4;
            raw[o] = icon.rgb[s * 3]; raw[o + 1] = icon.rgb[s * 3 + 1]; raw[o + 2] = icon.rgb[s * 3 + 2];
            raw[o + 3] = icon.mask[s];
        }
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(n, 0); ihdr.writeUInt32BE(n, 4); ihdr[8] = 8; ihdr[9] = 6;
    fs.writeFileSync(file, Buffer.concat([Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
        chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
    console.log(file);
}

const arg = (process.argv[2] || '').toUpperCase();
if (arg === 'README') {
    writeRgbaPng(path.join(root, 'src', 'symbian', 'gfx', 'icon.png'), { size: 256, ...render(256, 2, MAKERS.A) });
} else if (!arg) {
    const icons = renderAll('A');
    writeSet(path.join(root, 'src', 'symbian', 'gfx'), icons);
    writePreview(path.join(root, 'plans', 'reports', 'icon-preview.png'), icons);
} else if (arg === 'ALL') {
    const all = {};
    for (const X of Object.keys(MAKERS)) {
        all[X] = renderAll(X);
        writeSet(path.join(root, 'plans', 'reports', 'icon-variants', X), all[X]);
    }
    writeSummary(path.join(root, 'plans', 'reports', 'icon-variants', 'preview.png'), all);
} else if (MAKERS[arg]) {
    writeSet(path.join(root, 'plans', 'reports', 'icon-variants', arg), renderAll(arg));
} else {
    console.error('usage: node mkicon.js [A..F | all]');
    process.exit(1);
}
