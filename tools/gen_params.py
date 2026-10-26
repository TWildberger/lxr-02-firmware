#!/usr/bin/env python3
"""Generate docs/params.json from the firmware sources.

The parameter numbering, ranges, labels and page layout are derived directly
from the C sources so the table never drifts from the firmware:

  Core/Inc/Parameters.h        ParamEnums (parameter numbers)
  Core/Inc/menu/menu.h         NamesEnum / shortNames / catNames / longNames / Datatypes / PageNames
  Core/Inc/menu/MenuText.h     MENU_* ids, enum label arrays, name strings
  Core/Inc/menu/menuPages.h    page / sub-page / slot layout
  Core/Src/menu/menu.c         parameter_dtypes, valueNames, getMaxEntriesForMenu
  Core/Src/menu/Cc2Text.c      modTargets, modTargetVoiceOffsets

Usage:  python3 tools/gen_params.py [--check]
  --check  exit non-zero if docs/params.json is out of date (ignores source_commit)
"""

import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# Preprocessor conditionals that guard the tables we parse.
DEFINES = {"USE_CODEC_EQ": 0, "0": 0, "1": 1}

DTYPE_NAMES = [
    "0B255", "0B127", "PM100", "MENU", "PM63", "1B16", "ON_OFF",
    "TARGET_SELECTION_LFO", "TARGET_SELECTION_VELO", "VOICE_LFO",
    "AUTOM_TARGET", "0b1", "NOTE_NAME", "0B15", "1B64", "0B100",
]

VOICE_PAGE_LABELS = ["Drum1", "Drum2", "Drum3", "Snare", "Cymbal", "HiHat closed", "HiHat open"]
VOICE_SUBPAGES = ["Osc", "AmpEG", "Mod", "FM", "Transient", "Filter", "LFO", "Mix"]
FX_SUBPAGES = ["Off", "Distortion", "Ring", "Compressor", "Delay", "Reverb", "Freeze"]

RESERVED_PREFIXES = ("PAR_UNUSED", "UNUSED_", "PAR_NRPN_", "NRPN_")
PM63_OFFSET_60 = {
    "PAR_COARSE1", "PAR_COARSE2", "PAR_COARSE3", "PAR_COARSE4", "PAR_COARSE5", "PAR_COARSE6",
    "PAR_FM_FREQ1", "PAR_FM_FREQ2", "PAR_FM_FREQ3",
    "PAR_MOD_OSC_F1", "PAR_MOD_OSC_F2", "PAR_MOD_OSC_F1_CYM", "PAR_MOD_OSC_F2_CYM",
}
MARKERS = ("NUM_PARAMS", "END_OF_SOUND_PARAMETERS", "PAR_BEGINNING_OF_GLOBALS")


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def strip_comments(src):
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            end = n if j < 0 else j + 2
            out.append("\n" * src.count("\n", i, end))
            i = end
        elif src[i] == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def preprocess(src):
    """Minimal #if/#ifdef/#else/#endif evaluator (after stripping comments)."""
    src = strip_comments(src)
    out, stack, active = [], [], True
    for line in src.split("\n"):
        m = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)", line)
        if not m:
            out.append(line if active else "")
            continue
        kind, cond = m.group(1), m.group(2).strip()
        if kind in ("if", "ifdef", "ifndef"):
            if kind == "if":
                val = bool(DEFINES.get(cond, 1))
            else:
                val = kind == "ifndef"  # include guards
            stack.append((active, val))
            active = active and val
        elif kind == "elif":
            parent, taken = stack.pop()
            val = bool(DEFINES.get(cond, 0))
            stack.append((parent, taken or val))
            active = parent and not taken and val
        elif kind == "else":
            parent, taken = stack.pop()
            stack.append((parent, True))
            active = parent and not taken
        else:
            parent, _ = stack.pop()
            active = parent
        out.append("")
    return "\n".join(out)


def braced_block(src, start_pat):
    m = re.search(start_pat, src)
    if not m:
        raise SystemExit("pattern not found: " + start_pat)
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i + 1:j]
    raise SystemExit("unbalanced braces for " + start_pat)


def eval_c_int(expr, symbols):
    tokens = re.findall(r"[A-Za-z_]\w*|0x[0-9a-fA-F]+|\d+|<<|>>|[-+()|&*]", expr)
    py = []
    for t in tokens:
        if re.match(r"[A-Za-z_]", t):
            if t not in symbols:
                raise SystemExit("unknown symbol in expression: " + t)
            py.append(str(symbols[t]))
        else:
            py.append(t)
    return int(eval(" ".join(py), {"__builtins__": {}}))


def parse_enum(src, name):
    body = braced_block(src, r"enum\s+" + name + r"\s*\{")
    values, order, cur = {}, [], -1
    for item in body.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" in item:
            k, v = [x.strip() for x in item.split("=", 1)]
            cur = eval_c_int(v, values)
        else:
            k, cur = item, cur + 1
        values[k] = cur
        order.append(k)
    return values, order


def parse_string_table(src, name):
    body = braced_block(src, r"const\s+char\s+" + name + r"\s*\[\s*\]\s*\[\s*\d+\s*\]\s*=")
    out = []
    for e in re.findall(r"\{\s*(\"(?:[^\"\\]|\\.)*\"|[^{}]*?)\s*\}", body):
        e = e.strip()
        out.append(e[1:-1].encode().decode("unicode_escape") if e.startswith('"') else e)
    return out


def label_array(src, name):
    """MenuText arrays: first entry is {count}, then labels. The count is authoritative."""
    entries = parse_string_table(src, name)
    count = int(entries[0], 0)
    labels = [e.strip() for e in entries[1:1 + count]]
    if len(labels) != count:
        raise SystemExit("%s: count %d but only %d labels" % (name, count, len(labels)))
    return labels


def split_top_level(body):
    groups, depth, start = [], 0, None
    for i, c in enumerate(body):
        if c == "{":
            if depth == 0:
                start = i + 1
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                groups.append(body[start:i])
    return groups


def git_rev():
    try:
        return subprocess.check_output(["git", "-C", ROOT, "rev-parse", "HEAD"], text=True).strip()
    except Exception:
        return None


def build():
    params_h = preprocess(read("Core/Inc/Parameters.h"))
    menu_h = preprocess(read("Core/Inc/menu/menu.h"))
    menutext_h = preprocess(read("Core/Inc/menu/MenuText.h"))
    pages_h = preprocess(read("Core/Inc/menu/menuPages.h"))
    menu_c = preprocess(read("Core/Src/menu/menu.c"))
    cc2text_c = preprocess(read("Core/Src/menu/Cc2Text.c"))

    par, par_order = parse_enum(params_h, "ParamEnums")
    texts, _ = parse_enum(menu_h, "NamesEnum")
    shorts, _ = parse_enum(menu_h, "shortNamesEnum")
    cats, _ = parse_enum(menu_h, "catNamesEnum")
    longs, _ = parse_enum(menu_h, "longNamesEnum")
    _, dtype_order = parse_enum(menu_h, "Datatypes")
    pages_enum, _ = parse_enum(menu_h, "PageNames")
    menu_ids = {k: int(v) for k, v in re.findall(r"#define\s+(MENU_\w+)\s+(\d+)", menutext_h)}

    if [d[len("DTYPE_"):] for d in dtype_order if d.startswith("DTYPE_")] != DTYPE_NAMES:
        raise SystemExit("Datatypes enum changed: %r" % dtype_order)

    num_params = par["NUM_PARAMS"]
    end_sound = par["END_OF_SOUND_PARAMETERS"]
    glob_start = par["PAR_BEGINNING_OF_GLOBALS"]

    symbol_for = {}
    for k in par_order:
        if k not in MARKERS:
            symbol_for.setdefault(par[k], k)

    short_names = parse_string_table(menutext_h, "shortNames")
    cat_names = parse_string_table(menutext_h, "catNames")
    long_names = parse_string_table(menutext_h, "longNames")
    for tbl, enum, nm in ((short_names, shorts, "shortNames"), (cat_names, cats, "catNames"),
                          (long_names, longs, "longNames")):
        if len(tbl) != len(enum):
            raise SystemExit("%s has %d entries but its enum has %d" % (nm, len(tbl), len(enum)))

    # menu id -> label array (mirrors getMaxEntriesForMenu in menu.c)
    gm = braced_block(menu_c, r"static\s+uint8_t\s+getMaxEntriesForMenu\s*\([^)]*\)\s*\{")
    menus = {}
    for mname, arr in re.findall(r"case\s+(MENU_\w+)\s*:\s*return\s*[^;]*?(\w+)\[0\]\[0\]", gm):
        menus[menu_ids[mname]] = {"symbol": mname, "array": arr, "labels": label_array(menutext_h, arr)}

    dt_body = braced_block(menu_c, r"parameter_dtypes\s*\[\s*NUM_PARAMS\s*\]\s*=")
    dt_items = [x.strip() for x in dt_body.split(",") if x.strip()]
    if len(dt_items) != num_params:
        raise SystemExit("parameter_dtypes has %d entries, NUM_PARAMS=%d" % (len(dt_items), num_params))
    param_dtype = []
    for it in dt_items:
        m = re.match(r"DTYPE_(\w+)\s*(?:\|\s*\(\s*(MENU_\w+)\s*<<\s*8\s*\))?$", it)
        if not m:
            raise SystemExit("cannot parse dtype entry: " + it)
        param_dtype.append((m.group(1), menu_ids[m.group(2)] if m.group(2) else None))

    vn_body = braced_block(menu_c, r"const\s+Name\s+valueNames\s*\[\s*NUM_NAMES\s*\]\s*=")
    value_names = []
    for g in split_top_level(vn_body):
        s, c, l = [x.strip() for x in g.split(",")]
        value_names.append({
            "short": short_names[shorts[s]].strip(),
            "category": cat_names[cats[c]].strip(),
            "long": long_names[longs[l]].strip(),
        })
    if len(value_names) != texts["NUM_NAMES"]:
        raise SystemExit("valueNames has %d entries, NUM_NAMES=%d" % (len(value_names), texts["NUM_NAMES"]))

    mod_targets = []
    for g in split_top_level(braced_block(cc2text_c, r"const\s+ModTarg\s+modTargets\s*\[\s*\]\s*=")):
        t, p = [x.strip() for x in g.split(",")]
        mod_targets.append((texts[t], par[p]))
    voice_offsets = [[int(x) for x in g.split(",")]
                     for g in split_top_level(braced_block(cc2text_c, r"modTargetVoiceOffsets\s*\[\s*6\s*\]\s*="))]

    def mt_voice(idx):
        for v, (s, e) in enumerate(voice_offsets):
            if s <= idx <= e:
                return v + 1
        return None

    # menuPages: page -> subpage -> 8 TEXT_* followed by 8 PAR_* (missing trailing entries are 0)
    page_name_by_index = {v: k for k, v in pages_enum.items()}
    layout, text_by_param = {}, {}
    mp_body = braced_block(pages_h, r"const\s+Page\s+menuPages\s*\[")
    for page_idx, page in enumerate(split_top_level(mp_body)):
        pname = page_name_by_index[page_idx]
        for sub_idx, sub in enumerate(split_top_level(page)):
            toks = [x.strip() for x in sub.split(",") if x.strip()]
            tops, bots = toks[:8], toks[8:16]
            for slot, b in enumerate(bots):
                pn = par[b]
                if pn == par["PAR_NONE"]:
                    continue
                if page_idx < 7:
                    plabel, slabel = VOICE_PAGE_LABELS[page_idx], VOICE_SUBPAGES[sub_idx]
                elif pname == "FX_MODE_PAGE":
                    plabel, slabel = "FX", FX_SUBPAGES[sub_idx]
                else:
                    plabel = "Global" if pname == "MENU_MIDI_PAGE" else pname[:-len("_PAGE")].replace("_", " ").title()
                    slabel = str(sub_idx + 1)
                vn = value_names[texts[tops[slot]]]
                text_by_param.setdefault(pn, vn)
                layout.setdefault(pn, []).append({
                    "page": pname, "page_label": plabel, "subpage": sub_idx, "subpage_label": slabel,
                    "slot": slot, "label": vn["short"],
                })
    for t, p in mod_targets:
        text_by_param.setdefault(p, value_names[t])

    n_mt = len(mod_targets)
    fixed_ranges = {
        "0B255": (0, 255), "VOICE_LFO": (1, 6), "1B16": (1, 16), "1B64": (1, 64), "0B15": (0, 15),
        "ON_OFF": (0, 1), "0b1": (0, 1), "0B100": (0, 100),
        "AUTOM_TARGET": (0, n_mt - 1), "TARGET_SELECTION_LFO": (0, n_mt - 1),
        "TARGET_SELECTION_VELO": (0, n_mt - 1),
    }

    params = []
    for pn in range(num_params):
        sym = symbol_for.get(pn)
        dt, menu_id = param_dtype[pn]
        locs = layout.get(pn, [])
        lo, hi = (0, len(menus[menu_id]["labels"]) - 1) if dt == "MENU" else fixed_ranges.get(dt, (0, 127))
        scope = "sound" if pn < end_sound else ("global" if pn >= glob_start else "pattern")
        reserved = sym is None or sym in ("PAR_NONE", "PAR_BANK_CHANGE") or sym.startswith(RESERVED_PREFIXES)
        e = {"id": pn, "symbol": sym, "scope": scope, "reserved": reserved}
        if pn in text_by_param:
            e["name"] = dict(text_by_param[pn])
        e.update({"dtype": dt, "min": lo, "max": hi})
        if dt == "MENU":
            e["menu"] = menus[menu_id]["symbol"]
            e["labels"] = menus[menu_id]["labels"]
            if e["menu"] == "MENU_WAVEFORM":
                e["note"] = ("max grows with the number of user samples on the SD card; values >= %d "
                             "select user samples s0, s1, ..." % len(e["labels"]))
        if dt == "PM63":
            e["display_offset"] = -60 if sym in PM63_OFFSET_60 else -63
        elif dt == "0b1":
            e["display_offset"] = 1
        elif dt in ("TARGET_SELECTION_LFO", "TARGET_SELECTION_VELO", "AUTOM_TARGET"):
            e["value_is"] = "modTargets index"
        elif dt == "NOTE_NAME":
            e["value_is"] = "MIDI note number" + (" (0 = any)" if sym and "MIDI_NOTE" in sym else "")
        if sym == "PAR_BPM":
            e["note"] = "beats per minute"
        if sym and sym.startswith("PAR_ENGINE_"):
            e["note"] = ("Opt-in feedback FM for Drum1-3: 0 = legacy, 1 = feedback FM. "
                         "Persisted only with the EngV01 kit trailer; old/unversioned kits load as legacy.")
        if sym and sym.startswith("PAR_TIMBRE_DECAY_"):
            e["note"] = ("Feedback FM timbre envelope T60: 0.003 + 1.997*(value/127)^2 seconds. "
                         "Independent of pitch/amp decay. Not in the legacy modulation target table.")
        voices = sorted({VOICE_PAGE_LABELS.index(l["page_label"]) + 1 for l in locs if l["page_label"] in VOICE_PAGE_LABELS})
        if voices:
            e["voice"] = 6 if voices == [6, 7] else voices[0]
        if locs:
            e["ui"] = locs
        midi = None
        if scope == "sound" and not reserved:
            if pn < 127:
                midi = {"type": "cc", "number": pn + 1}
            elif pn >= 128:
                n = pn - 128
                midi = {"type": "nrpn", "number": n, "msb": n >> 7, "lsb": n & 0x7F}
            if midi and dt in ("TARGET_SELECTION_LFO", "TARGET_SELECTION_VELO"):
                midi["value_is"] = "target parameter number (firmware converts it to a modTargets index)"
        e["midi"] = midi
        if scope == "sound":
            e["snd_offset"] = 8 + pn
        elif scope == "global":
            e["glo_cfg_offset"] = pn - glob_start
        params.append(e)

    stock_globals = (par["PAR_SYSEX_ENABLE"] if "PAR_SYSEX_ENABLE" in par else num_params) - glob_start
    return {
        "$comment": "Generated by tools/gen_params.py from the firmware sources. Do not edit by hand.",
        "format_version": 1,
        "kit_engine_extension": {
            "tag": "EngV01", "tag_offset": 8 + end_sound,
            "snd_bytes": 8 + end_sound + 6,
            "sysex_capability_bit": 7,
            "selector_ids": [par["PAR_ENGINE_1"], par["PAR_ENGINE_2"], par["PAR_ENGINE_3"]],
            "timbre_decay_ids": [par["PAR_TIMBRE_DECAY_1"], par["PAR_TIMBRE_DECAY_2"], par["PAR_TIMBRE_DECAY_3"]],
            "legacy": "No tag: reset only ids 221..226 to zero; other sound bytes and all existing ids stay unchanged.",
        },
        "firmware": {"base": "Erica Synths LXR-02 v1.7", "source_commit": git_rev()},
        "counts": {
            "NUM_PARAMS": num_params,
            "END_OF_SOUND_PARAMETERS": end_sound,
            "PAR_BEGINNING_OF_GLOBALS": glob_start,
            "globals": num_params - glob_start,
            "stock_v1_7_globals": stock_globals,
            "mod_targets": n_mt,
        },
        "scopes": {
            "sound": "0..%d: kit (edit buffer) parameters, stored in .snd files and kit SysEx dumps" % (end_sound - 1),
            "pattern": "%d..%d: sequencer/step/performance UI parameters; not stored in .snd" % (end_sound, glob_start - 1),
            "global": "%d..%d: global settings stored in glo.cfg" % (glob_start, num_params - 1),
        },
        "midi_rules": {
            "cc": "Sound params 0..126 map to CC (id+1) on the voice's channel or the global channel. "
                  "CC0 is bank select (kit load), so param 127 is not reachable via CC.",
            "nrpn": "Sound params 128..%d map to NRPN (id-128): CC99=msb, CC98=lsb, CC6=value (7-bit)." % (end_sound - 1),
            "mute_nrpn": "NRPN 1000..1006 mute voice 1..7 (value 0/1).",
            "freeze_nrpn": "NRPN 210 (CC99=1, CC98=82) engages the freeze FX (PAR_FX_TYPE 6): CC6 value 0 = release, "
                           "1..8 = 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T, 9..127 = rate set by PAR_FX_P1. "
                           "Not a parameter. Same values as SysEx FX_FREEZE (docs/freeze.md).",
            "panel_edits": "Stock firmware does not transmit CC/NRPN for panel edits. Use SysEx parameter "
                           "echo (docs/sysex.md) on firmware that supports it.",
            "mod_target_values": "VEL_DEST_x / TARGET_LFOx / P1_DEST / P2_DEST hold a modTargets index in memory, "
                                 ".snd files and SysEx; over NRPN the value is the target parameter number.",
        },
        "dtypes": {d: i for i, d in enumerate(DTYPE_NAMES)},
        "menus": {str(k): {"symbol": v["symbol"], "labels": v["labels"]} for k, v in sorted(menus.items())},
        "mod_targets": [
            {"index": i, "param": p, "symbol": symbol_for.get(p), "label": value_names[t]["short"],
             "long": value_names[t]["long"], "voice": mt_voice(i)}
            for i, (t, p) in enumerate(mod_targets)
        ],
        "mod_target_voice_ranges": [{"voice": i + 1, "start": s, "end": e} for i, (s, e) in enumerate(voice_offsets)],
        "params": params,
    }


def main():
    doc = build()
    out_path = os.path.join(ROOT, "docs", "params.json")
    if "--check" in sys.argv:
        with open(out_path, encoding="utf-8") as f:
            old = json.load(f)
        old["firmware"]["source_commit"] = doc["firmware"]["source_commit"] = None
        if old != doc:
            print("docs/params.json is out of date; run tools/gen_params.py")
            return 1
        print("docs/params.json is up to date")
        return 0
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(json.dumps(doc, indent=1, ensure_ascii=False) + "\n")
    c = doc["counts"]
    print("wrote docs/params.json: %d params (%d sound, %d global), %d mod targets"
          % (c["NUM_PARAMS"], c["END_OF_SOUND_PARAMETERS"], c["globals"], c["mod_targets"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
