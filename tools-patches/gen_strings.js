// Translation table generator: src/symbian/strings.json -> src/symbian/i18n.cpp
// and the TLangId block of src/symbian/i18n.h.
//
// Non-ASCII text goes into the C++ source only as TText arrays of hex code points:
// GCCE 3.4.3 must never be trusted to convert UTF-8 literals, and a wrong
// conversion would only show on the phone screen. Readable text stays in JSON.
//
// A string missing in a language falls back to the English one. The string ids
// and their order are taken from the TStrId enum in i18n.h, which stays
// hand-written; the JSON must hold exactly those ids.
//
// The language list and its order must match SymbianOverclock/tools/gen_strings.js
// in content. Order here is the saved setting index (lang= in ps1_settings.txt),
// so new languages are only ever appended.
//
// Run from the repository root: node tools-patches/gen_strings.js
'use strict';
const fs = require('fs');

const JSON_PATH = 'src/symbian/strings.json';
const CPP_PATH = 'src/symbian/i18n.cpp';
const H_PATH = 'src/symbian/i18n.h';

// code: [enum suffix, native name, Symbian TLanguage values that select it]
const LANG = {
    en: ['English', 'English', ['ELangEnglish', 'ELangAmerican', 'ELangAustralian',
        'ELangCanadianEnglish', 'ELangInternationalEnglish', 'ELangSouthAfricanEnglish',
        'ELangNewZealand']],
    uk: ['Ukrainian', 'Українська', ['ELangUkrainian']],
    zh: ['Chinese', '简体中文', ['ELangPrcChinese', 'ELangTaiwanChinese', 'ELangHongKongChinese']],
    de: ['German', 'Deutsch', ['ELangGerman', 'ELangSwissGerman', 'ELangAustrian']],
    fr: ['French', 'Français', ['ELangFrench', 'ELangSwissFrench', 'ELangBelgianFrench',
        'ELangInternationalFrench', 'ELangCanadianFrench']],
    es: ['Spanish', 'Español', ['ELangSpanish', 'ELangInternationalSpanish',
        'ELangLatinAmericanSpanish']],
    ru: ['Russian', 'Русский', ['ELangRussian']],
    pt: ['Portuguese', 'Português', ['ELangPortuguese', 'ELangBrazilianPortuguese']],
    it: ['Italian', 'Italiano', ['ELangItalian', 'ELangSwissItalian']],
    pl: ['Polish', 'Polski', ['ELangPolish']],
    tr: ['Turkish', 'Türkçe', ['ELangTurkish', 'ELangCyprusTurkish']],
    ja: ['Japanese', '日本語', ['ELangJapanese']],
    hi: ['Hindi', 'हिन्दी', ['ELangHindi']],
    ar: ['Arabic', 'العربية', ['ELangArabic']],
    ko: ['Korean', '한국어', ['ELangKorean']],
    cs: ['Czech', 'Čeština', ['ELangCzech']],
    nl: ['Dutch', 'Nederlands', ['ELangDutch', 'ELangBelgianFlemish']],
    sv: ['Swedish', 'Svenska', ['ELangSwedish', 'ELangFinlandSwedish']],
    ro: ['Romanian', 'Română', ['ELangRomanian']],
    hu: ['Hungarian', 'Magyar', ['ELangHungarian']],
    el: ['Greek', 'Ελληνικά', ['ELangGreek', 'ELangCyprusGreek']],
    fi: ['Finnish', 'Suomi', ['ELangFinnish']],
    da: ['Danish', 'Dansk', ['ELangDanish']],
    no: ['Norwegian', 'Norsk', ['ELangNorwegian', 'ELangNorwegianNynorsk']],
    sk: ['Slovak', 'Slovenčina', ['ELangSlovak']],
    bg: ['Bulgarian', 'Български', ['ELangBulgarian']],
    hr: ['Croatian', 'Hrvatski', ['ELangCroatian']],
    sl: ['Slovenian', 'Slovenščina', ['ELangSlovenian']],
    sr: ['Serbian', 'Srpski', ['ELangSerbian']],
    et: ['Estonian', 'Eesti', ['ELangEstonian']],
    lv: ['Latvian', 'Latviešu', ['ELangLatvian']],
    lt: ['Lithuanian', 'Lietuvių', ['ELangLithuanian']],
    ca: ['Catalan', 'Català', ['ELangCatalan']],
    // No TLanguage value is mapped: the S60 3rd Edition SDK may not define ELangVietnamese,
    // so Vietnamese is chosen by hand in System > Language and never picked automatically.
    vi: ['Vietnamese', 'Tiếng Việt', []],
};

const data = JSON.parse(fs.readFileSync(JSON_PATH, 'utf8'));
const langs = data.languages;
if (langs[0] !== 'en') throw new Error('English must be the first language');
for (const l of langs) if (!LANG[l]) throw new Error('no metadata for language ' + l);

let header = fs.readFileSync(H_PATH, 'utf8');
const ids = [...header.match(/enum TStrId\s*\{([\s\S]*?)\}/)[1].matchAll(/\b(EStr\w+)\b/g)]
    .map(m => m[1]).filter(x => x !== 'EStrCount');
const jsonIds = Object.keys(data.strings);
for (const id of ids) if (!(id in data.strings)) throw new Error('missing in JSON: ' + id);
for (const id of jsonIds) if (!ids.includes(id)) throw new Error('not in TStrId: ' + id);
for (const id of ids) if (typeof data.strings[id].en !== 'string') throw new Error('no English text: ' + id);

function isAscii(s) { return /^[\x20-\x7e]*$/.test(s); }
function cLiteral(s) { return '"' + s.replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"'; }
function hexArray(s) {
    const cps = [];
    for (const ch of s) {
        const c = ch.codePointAt(0);
        if (c > 0xffff) throw new Error('outside the BMP: ' + s);
        cps.push('0x' + c.toString(16).toUpperCase().padStart(4, '0'));
    }
    cps.push('0');
    return '{ ' + cps.join(', ') + ' }';
}
function define(name, s) {
    if (isAscii(s)) return `static const TText *const ${name} = _S(${cLiteral(s)});\n`;
    return `static const TText ${name}[] = ${hexArray(s)};\n`;
}

let c = `// GENERATED by tools-patches/gen_strings.js from src/symbian/strings.json. Do not edit.
#include "i18n.h"

// Translation tables for ${langs.length} languages. Non-ASCII strings are TText arrays of
// explicit code points, so the text does not depend on how GCCE reads the source.
// A string with no translation points at the English one.

// --- Language names ---
`;
for (const l of langs) c += define('KLangName_' + l, LANG[l][1]);

for (const l of langs) {
    c += `\n// --- ${LANG[l][0]} (${l}) ---\n`;
    for (const id of ids) {
        const v = data.strings[id][l];
        if (l === 'en' || (typeof v === 'string' && v !== data.strings[id].en))
            c += define(`KStr_${l}_${id}`, l === 'en' ? data.strings[id].en : v);
    }
}

for (const l of langs) {
    c += `\nstatic const TText *const KTable_${l}[EStrCount] =\n\t{\n`;
    for (const id of ids) {
        const v = data.strings[id][l];
        const own = l === 'en' || (typeof v === 'string' && v !== data.strings[id].en);
        c += `\t${own ? `KStr_${l}_${id}` : `KStr_en_${id}`},\n`;
    }
    c += '\t};\n';
}

c += '\nstatic const TText *const *const KAllTables[ELangIdCount] =\n\t{\n';
for (const l of langs) c += `\tKTable_${l},\n`;
c += '\t};\n\nstatic const TText *const KAllLangNames[ELangIdCount] =\n\t{\n';
for (const l of langs) c += `\tKLangName_${l},\n`;
c += '\t};\n\n// File name of the help text, E:\\PS1\\help\\<code>.txt.\nstatic const char *const KLangCode[ELangIdCount] =\n\t{\n';
for (const l of langs) c += `\t"${l}",\n`;
c += `\t};

static TInt gActiveLang = 0;
static TPtrC gActiveStrings[EStrCount];
static TPtrC gLangNamePtrs[ELangIdCount];
static TBool gInitialized = EFalse;

static void I18nEnsureInit()
	{
	if (!gInitialized)
		{
		for (TInt i = 0; i < ELangIdCount; i++)
			gLangNamePtrs[i].Set(KAllLangNames[i]);
		I18nSetLanguage(gActiveLang);
		gInitialized = ETrue;
		}
	}

void I18nSetLanguage(TInt aLang)
	{
	if (aLang < 0 || aLang >= ELangIdCount)
		aLang = 0;
	gActiveLang = aLang;
	const TText *const *table = KAllTables[gActiveLang];
	for (TInt i = 0; i < EStrCount; i++)
		gActiveStrings[i].Set(table[i]);
	}

TInt I18nLanguageCount()
	{
	return ELangIdCount;
	}

const TDesC &I18nLanguageName(TInt aLang)
	{
	I18nEnsureInit();
	if (aLang < 0 || aLang >= ELangIdCount)
		return gLangNamePtrs[0];
	return gLangNamePtrs[aLang];
	}

const char *I18nLanguageCode(TInt aLang)
	{
	if (aLang < 0 || aLang >= ELangIdCount)
		aLang = ELangIdEnglish;
	return KLangCode[aLang];
	}

const TDesC &Tr(TInt aId)
	{
	I18nEnsureInit();
	if (aId < 0 || aId >= EStrCount)
		return KNullDesC;
	return gActiveStrings[aId];
	}

TInt I18nDetectLanguage()
	{
	switch (User::Language())
		{
`;
for (const l of langs.slice(1)) {
    const tl = LANG[l][2];
    tl.forEach((t, i) => {
        c += i === tl.length - 1 ? `\t\tcase ${t}: return ELangId${LANG[l][0]};\n` : `\t\tcase ${t}:\n`;
    });
}
c += '\t\tdefault: return ELangIdEnglish;\n\t\t}\n\t}\n';
fs.writeFileSync(CPP_PATH, c, 'utf8');

// Keep the header's own line endings: the block is spliced into a CRLF file.
const nl = header.includes('\r\n') ? '\r\n' : '\n';
let e = `enum TLangId${nl}\t{${nl}`;
for (const l of langs) e += `\tELangId${LANG[l][0]},${nl}`;
e += `\tELangIdCount${nl}\t}`;
const before = header;
header = header.replace(/enum TLangId\s*\{[\s\S]*?\r?\n\t\}/, e);
if (header === before && !before.includes(e)) throw new Error('TLangId block not found in i18n.h');
fs.writeFileSync(H_PATH, header, 'utf8');

const missing = langs.map(l => `${l}:${ids.filter(id => typeof data.strings[id][l] !== 'string').length}`);
console.log(`${ids.length} strings, ${langs.length} languages; untranslated ${missing.join(' ')}`);
