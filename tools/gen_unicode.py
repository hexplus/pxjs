#!/usr/bin/env python3
"""Generates src/px_unicode.c: the Unicode data RegExp needs, from the
Unicode Character Database (UCD), pinned to one version (the one the
pinned Test262 checks) and verified by hash:

    python3 tools/gen_unicode.py --fetch      downloads the files into third_party/ucd/
    python3 tools/gen_unicode.py > src/px_unicode.c

The UCD files are covered by the Unicode license (third_party/ucd/license.txt,
also src/LICENSE-UNICODE.txt). The output is deterministic.

What it emits (all read by px_regexp.c):
  - General_Category of every code point, as runs;
  - Script of every code point, as runs, and the Script_Extensions lists;
  - the binary properties ECMAScript names (table "Binary Unicode property
    aliases"), each stored as the symmetric difference from a similar set
    (a group of categories, or an earlier property) -- about a quarter of
    the size of plain range lists;
  - simple case folding (CaseFolding.txt C+S, for /iu) and the
    single-code-unit uppercase mapping of the BMP (for /i without u), as
    runs of equal deltas.

Numbers are variable-length: a byte below 0x80 is its value; 10xxxxxx yy is
((x << 8) | y); 11xxxxxx yy zz is ((x << 16) | (y << 8) | z)."""
import hashlib
import os
import re
import sys
import urllib.request

VERSION = "17.0.0"
BASE = "https://www.unicode.org/Public/%s/ucd/" % VERSION
FILES = {  # name in third_party/ucd -> (path under BASE, SHA-256)
    "UnicodeData.txt": ("UnicodeData.txt", "2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c"),
    "Scripts.txt": ("Scripts.txt", "9f5e50d3abaee7d6ce09480f325c706f485ae3240912527e651954d2d6b035bf"),
    "ScriptExtensions.txt": ("ScriptExtensions.txt",
                             "ec2107e58825a1586acee8e0911ce18260394ac8b87e535ca325f1ccbeb06bc6"),
    "PropList.txt": ("PropList.txt", "130dcddcaadaf071008bdfce1e7743e04fdfbc910886f017d9f9ac931d8c64dd"),
    "DerivedCoreProperties.txt": ("DerivedCoreProperties.txt",
                                  "24c7fed1195c482faaefd5c1e7eb821c5ee1fb6de07ecdbaa64b56a99da22c08"),
    "DerivedNormalizationProps.txt": ("DerivedNormalizationProps.txt",
                                      "71fd6a206a2c0cdd41feb6b7f656aa31091db45e9cedc926985d718397f9e488"),
    "DerivedBinaryProperties.txt": ("extracted/DerivedBinaryProperties.txt",
                                    "13dd09d35a9377e33eb388a01e6581d4bfec6b2685316078c341982fa444071a"),
    "emoji-data.txt": ("emoji/emoji-data.txt", "2cb2bb9455cda83e8481541ecf5b6dfda66a3bb89efa3fa7c5297eccf607b72b"),
    "CaseFolding.txt": ("CaseFolding.txt", "ff8d8fefbf123574205085d6714c36149eb946d717a0c585c27f0f4ef58c4183"),
    "SpecialCasing.txt": ("SpecialCasing.txt", "efc25faf19de21b92c1194c111c932e03d2a5eaf18194e33f1156e96de4c9588"),
    "PropertyValueAliases.txt": ("PropertyValueAliases.txt",
                                 "64e9a5f76f7a1e8b5a47d6a1f9a26522a251208f5276bdfa1559dac7cf2e827a"),
}
D = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "third_party", "ucd")


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


if sys.argv[1:] == ["--fetch"]:
    os.makedirs(D, exist_ok=True)
    for name, (url, digest) in sorted(FILES.items()):
        path = os.path.join(D, name)
        if not os.path.exists(path) or sha256(path) != digest:
            urllib.request.urlretrieve(BASE + url, path)
    urllib.request.urlretrieve("https://www.unicode.org/license.txt", os.path.join(D, "license.txt"))
for name, (url, digest) in sorted(FILES.items()):
    path = os.path.join(D, name)
    if not os.path.exists(path) or sha256(path) != digest:
        sys.exit("gen_unicode.py: %s is missing or not the pinned Unicode %s file: run with --fetch" % (path, VERSION))
if sys.argv[1:] == ["--fetch"]:
    sys.exit(0)

MAXCP = 0x110000

CATS = ["Cc", "Cf", "Cn", "Co", "Cs", "Ll", "Lm", "Lo", "Lt", "Lu", "Mc", "Me", "Mn", "Nd", "Nl", "No",
        "Pc", "Pd", "Pe", "Pf", "Pi", "Po", "Ps", "Sc", "Sk", "Sm", "So", "Zl", "Zp", "Zs"]

# ECMAScript's binary properties and their aliases (ES2023 table 67).
BINARY = [
    ("ASCII",), ("ASCII_Hex_Digit", "AHex"), ("Alphabetic", "Alpha"), ("Any",), ("Assigned",),
    ("Bidi_Control", "Bidi_C"), ("Bidi_Mirrored", "Bidi_M"), ("Case_Ignorable", "CI"), ("Cased",),
    ("Changes_When_Casefolded", "CWCF"), ("Changes_When_Casemapped", "CWCM"),
    ("Changes_When_Lowercased", "CWL"), ("Changes_When_NFKC_Casefolded", "CWKCF"),
    ("Changes_When_Titlecased", "CWT"), ("Changes_When_Uppercased", "CWU"), ("Dash",),
    ("Default_Ignorable_Code_Point", "DI"), ("Deprecated", "Dep"), ("Diacritic", "Dia"), ("Emoji",),
    ("Emoji_Component", "EComp"), ("Emoji_Modifier", "EMod"), ("Emoji_Modifier_Base", "EBase"),
    ("Emoji_Presentation", "EPres"), ("Extended_Pictographic", "ExtPict"), ("Extender", "Ext"),
    ("Grapheme_Base", "Gr_Base"), ("Grapheme_Extend", "Gr_Ext"), ("Hex_Digit", "Hex"),
    ("IDS_Binary_Operator", "IDSB"), ("IDS_Trinary_Operator", "IDST"), ("ID_Continue", "IDC"),
    ("ID_Start", "IDS"), ("Ideographic", "Ideo"), ("Join_Control", "Join_C"),
    ("Logical_Order_Exception", "LOE"), ("Lowercase", "Lower"), ("Math",),
    ("Noncharacter_Code_Point", "NChar"), ("Pattern_Syntax", "Pat_Syn"), ("Pattern_White_Space", "Pat_WS"),
    ("Quotation_Mark", "QMark"), ("Radical",), ("Regional_Indicator", "RI"), ("Sentence_Terminal", "STerm"),
    ("Soft_Dotted", "SD"), ("Terminal_Punctuation", "Term"), ("Unified_Ideograph", "UIdeo"),
    ("Uppercase", "Upper"), ("Variation_Selector", "VS"), ("White_Space", "space"),
    ("XID_Continue", "XIDC"), ("XID_Start", "XIDS"),
]


def lines(name):
    for line in open(os.path.join(D, name), encoding="utf-8"):
        line = line.split("#")[0].strip()
        if line:
            yield [f.strip() for f in line.split(";")]


def cprange(s):
    r = s.split("..")
    return int(r[0], 16), int(r[-1], 16)


def to_ranges(members):
    out = []
    for c in sorted(members):
        if out and c == out[-1][1] + 1:
            out[-1][1] = c
        else:
            out.append([c, c])
    return out


def varint(n):
    assert 0 <= n < (1 << 22)
    if n < 0x80:
        return [n]
    if n < 0x4000:
        return [0x80 | (n >> 8), n & 0xFF]
    return [0xC0 | (n >> 16), (n >> 8) & 0xFF, n & 0xFF]


def enc_ranges(members):
    out, prev = [], 0
    for lo, hi in to_ranges(members):
        out += varint(lo - prev) + varint(hi - lo)
        prev = hi + 1
    return out


# ---------------------------------------------------------------- data

gc = ["Cn"] * MAXCP
first = None
upper_simple = {}
for f in lines("UnicodeData.txt"):
    cp = int(f[0], 16)
    if f[1].endswith("First>"):
        first = cp
        continue
    lo = first if f[1].endswith("Last>") else cp
    for c in range(lo, cp + 1):
        gc[c] = f[2]
    if f[12]:
        upper_simple[cp] = int(f[12], 16)
cat_sets = {c: set() for c in CATS}
for cp in range(MAXCP):
    cat_sets[gc[cp]].add(cp)

# General_Category values: the 30 categories (in CATS order), then groups.
gc_values = {}  # short name -> [names], mask
for f in lines("PropertyValueAliases.txt"):
    if f[0] == "gc":
        gc_values[f[1]] = f[1:]
GROUPS = {"C": ["Cc", "Cf", "Cn", "Co", "Cs"], "L": ["Ll", "Lm", "Lo", "Lt", "Lu"], "LC": ["Ll", "Lt", "Lu"],
          "M": ["Mc", "Me", "Mn"], "N": ["Nd", "Nl", "No"], "P": ["Pc", "Pd", "Pe", "Pf", "Pi", "Po", "Ps"],
          "S": ["Sc", "Sk", "Sm", "So"], "Z": ["Zl", "Zp", "Zs"]}
gc_list = [(gc_values[c], 1 << CATS.index(c)) for c in CATS]
for g, members in GROUPS.items():
    gc_list.append((gc_values[g], sum(1 << CATS.index(c) for c in members)))
assert len(gc_list) == len(gc_values), "unexpected General_Category values"

# Scripts: every sc value (id = index), runs over all code points.
sc_list = []
sc_id = {}
for f in lines("PropertyValueAliases.txt"):
    if f[0] == "sc":
        sc_id[f[1]] = len(sc_list)
        sc_list.append(f[1:])
long_to_short = {names[1]: names[0] for names in sc_list}
sc = [sc_id["Zzzz"]] * MAXCP
for f in lines("Scripts.txt"):
    lo, hi = cprange(f[0])
    for c in range(lo, hi + 1):
        sc[c] = sc_id[long_to_short[f[1]]]
assert len(sc_list) < 256
scx = []  # (lo, hi, [ids])
for f in lines("ScriptExtensions.txt"):
    lo, hi = cprange(f[0])
    scx.append((lo, hi, sorted(sc_id[s] for s in f[1].split())))
scx.sort()

# Binary properties.
props = {}
for name in ("PropList.txt", "DerivedCoreProperties.txt", "DerivedNormalizationProps.txt",
             "DerivedBinaryProperties.txt", "emoji-data.txt"):
    for f in lines(name):
        if len(f) != 2:
            continue  # properties with values (NFKC_CF, ...)
        lo, hi = cprange(f[0])
        props.setdefault(f[1], set()).update(range(lo, hi + 1))
props["Any"] = set(range(MAXCP))
props["ASCII"] = set(range(0x80))
props["Assigned"] = set(range(MAXCP)) - cat_sets["Cn"]

# Candidate bases: groups of categories (and complements), then earlier properties.
def mask_set(mask):
    s = set()
    for i, c in enumerate(CATS):
        if mask >> i & 1:
            s |= cat_sets[c]
    return s


def cmask(*cs):
    return sum(1 << CATS.index(c) for c in cs)


ALL = (1 << len(CATS)) - 1
base_masks = [cmask(c) for c in CATS] + [
    cmask("Lu", "Ll", "Lt", "Lm", "Lo"), cmask("Lu", "Ll", "Lt", "Lm", "Lo", "Nl"), cmask("Lu", "Ll", "Lt"),
    cmask("Lu", "Ll", "Lt", "Lm", "Lo", "Nl", "Mn", "Mc", "Nd", "Pc"), cmask("Mn", "Me"), cmask("Mn", "Mc", "Me"),
    cmask("Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po"), ALL & ~cmask("Cn"),
    ALL & ~cmask("Cc", "Cf", "Cs", "Co", "Cn", "Zl", "Zp", "Mn", "Me")]
base_sets = [(m, mask_set(m)) for m in base_masks]

bin_data = []
done = []
for names in BINARY:
    s = props[names[0]]
    best = (enc_ranges(s), [0])
    for m, bs in base_sets:
        e = enc_ranges(s ^ bs)
        if len(e) + 5 < len(best[0]) + len(best[1]):
            best = (e, [1, m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF, (m >> 24) & 0xFF])
    for i, prev in enumerate(done):
        e = enc_ranges(s ^ prev)
        if len(e) + 2 < len(best[0]) + len(best[1]):
            best = (e, [2, i])
    bin_data.append(best[1] + best[0])
    done.append(s)

# Case mappings as runs: (start, count, stride, delta).
fold = {}
for f in lines("CaseFolding.txt"):
    if f[1] in ("C", "S"):
        fold[int(f[0], 16)] = int(f[2], 16)
special_upper = {}
for f in lines("SpecialCasing.txt"):
    if len(f) > 4 and f[4]:
        continue  # conditional mappings
    special_upper[int(f[0], 16)] = [int(x, 16) for x in f[3].split()]
upper = {}
for cp in range(0x10000):
    u = special_upper.get(cp, [upper_simple.get(cp, cp)])
    # ECMAScript Canonicalize (non-unicode /i): one code unit, and no mapping into ASCII from outside it
    if len(u) == 1 and u[0] <= 0xFFFF and u[0] != cp and not (cp >= 128 and u[0] < 128):
        upper[cp] = u[0]
for m in (fold, upper):
    for c, t in m.items():
        assert m.get(t, t) == t, "case mapping not idempotent at %X" % c


def mapping_runs(m):
    runs = []
    for c in sorted(m):
        d = m[c] - c
        if runs:
            s, n, st, dd = runs[-1]
            if dd == d and n < 1024 and ((n == 1 and c - s in (1, 2)) or (n > 1 and c == s + n * st)):
                runs[-1] = (s, n + 1, c - s if n == 1 else st, d)
                continue
        runs.append((c, 1, 1, d))
    words = []
    for s, n, st, d in runs:
        words += [(s << 11) | ((n - 1) << 1) | (st - 1), d & 0xFFFFFFFF]
    return words


# ---------------------------------------------------------------- output

def runs_of(values, index=None):
    out, prev_start, prev, n = [], 0, None, 0
    for cp in range(MAXCP):
        if values[cp] != prev:
            out += varint(cp - prev_start)
            if index is not None and n % 64 == 0:
                index += [cp, len(out)]  # every 64th run: its start and its value's offset
            out.append(values[cp])
            prev_start, prev, n = cp, values[cp], n + 1
    return out


gc_index = []
gc_runs = runs_of([CATS.index(c) for c in gc], gc_index)
sc_runs = runs_of(sc)
scx_data, prev = [], 0
for lo, hi, ids in scx:
    scx_data += varint(lo - prev) + varint(hi - lo) + [len(ids)] + ids
    prev = hi + 1
bin_offs, bin_bytes = [], []
for d in bin_data:
    bin_offs.append(len(bin_bytes))
    bin_bytes += d
bin_offs.append(len(bin_bytes))
fold_words = mapping_runs(fold)
upper_words = mapping_runs(upper)

out = []


def emit_bytes(name, data):
    out.append("const uint8_t %s[%d] = {" % (name, len(data)))
    for i in range(0, len(data), 20):
        out.append("    " + ",".join(str(b) for b in data[i:i + 20]) + ",")
    out.append("};")


def emit_words(name, ctype, data, fmt):
    out.append("const %s %s[%d] = {" % (ctype, name, len(data)))
    for i in range(0, len(data), 8):
        out.append("    " + ", ".join(fmt % w for w in data[i:i + 8]) + ",")
    out.append("};")


def emit_names(name, entries):
    blob = "".join(",".join(n for n in names if n) + "\\0" for names in entries)
    out.append("const char %s[] =" % name)
    line = '    "'
    for part in re.findall(r"[^\\]*?\\0", blob):
        if len(line) + len(part) > 116:
            out.append(line + '"')
            line = '    "'
        line += part
    out.append(line + '";')


total = len(gc_runs) + len(sc_runs) + len(scx_data) + len(bin_bytes) + 2 * len(bin_offs) + 4 * (
    len(fold_words) + len(upper_words) + len(gc_list))
out.append("/* GENERATED by tools/gen_unicode.py from the Unicode %s Character Database." % VERSION)
out.append(" * Do not edit: regenerate. The formats are described in the generator; the")
out.append(" * reader is px_regexp.c. About %d bytes of tables plus the names. */" % total)
out.append("")
out.append("#include <stdint.h>")
out.append("")
out.append("/* General_Category values: the 30 categories, in the order the runs")
out.append(" * number them, then the groups; their category masks. */")
emit_names("px_ucd_gc_names", [n for n, _ in gc_list])
emit_words("px_ucd_gc_masks", "uint32_t", [m for _, m in gc_list], "0x%08X")
out.append("")
out.append("/* Runs over U+0000..U+10FFFF: (start - previous start, category). */")
emit_bytes("px_ucd_gc", gc_runs)
out.append("")
out.append("/* Script values (the index is the id), runs as for categories, and the")
out.append(" * code points whose Script_Extensions differ from their Script:")
out.append(" * (start - previous end - 1, length - 1, count, ids...). */")
emit_names("px_ucd_sc_names", sc_list)
emit_bytes("px_ucd_sc", sc_runs)
emit_bytes("px_ucd_scx", scx_data)
out.append("")
out.append("/* Binary properties: kind 0 (a range list), 1 (the symmetric difference")
out.append(" * from the categories of a 4-byte mask) or 2 (from an earlier property,")
out.append(" * one byte), then (start - previous end - 1, length - 1) ranges. */")
emit_names("px_ucd_bin_names", BINARY)
emit_words("px_ucd_bin_offs", "uint16_t", bin_offs, "%d")
emit_bytes("px_ucd_bin", bin_bytes)
out.append("")
out.append("/* Case mappings as runs, two words each: start << 11 | (count - 1) << 1 |")
out.append(" * (stride - 1), then the delta. Simple case folding (all planes) and the")
out.append(" * uppercase mapping /i uses without the u flag (BMP). */")
emit_words("px_ucd_fold", "uint32_t", fold_words, "0x%08X")
emit_words("px_ucd_upper", "uint32_t", upper_words, "0x%08X")
out.append("")
out.append("const uint32_t px_ucd_sizes[] = {%d, %d, %d, %d, %d, %d};" % (
    len(gc_runs), len(sc_runs), len(scx_data), len(BINARY), len(fold_words) // 2, len(upper_words) // 2))
out.append("")
out.append("/* Where every 64th category run starts, and its category's offset in px_ucd_gc. */")
emit_words("px_ucd_gc_index", "uint32_t", gc_index, "%d")
out.append("""
/* The General_Category of a code point: an index into the first 30
 * entries of px_ucd_gc_names (the lexer uses it for identifiers). */
int px_unicode_category(uint32_t cp) {
    uint32_t       lo = 0, hi = %d, start;
    const uint8_t *p, *end = px_ucd_gc + sizeof px_ucd_gc;
    int            cat;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (px_ucd_gc_index[2 * mid] <= cp) lo = mid;
        else hi = mid;
    }
    start = px_ucd_gc_index[2 * lo];
    p     = px_ucd_gc + px_ucd_gc_index[2 * lo + 1];
    cat   = *p++;
    while (p < end) {
        uint32_t d = *p++;
        if (d >= 0xC0) {
            d = ((d & 0x3F) << 16) | ((uint32_t)p[0] << 8) | p[1];
            p += 2;
        } else if (d >= 0x80) {
            d = ((d & 0x3F) << 8) | *p++;
        }
        if (start + d > cp) break;
        start += d;
        cat = *p++;
    }
    return cat;
}""" % (len(gc_index) // 2))
print("\n".join(out))
