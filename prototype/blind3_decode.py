"""Join the user's blind-test answers with the key and summarise them.

    source .venv/bin/activate && python prototype/blind3_decode.py path/to/blind3-results-DATE.json

The key (prototype/out/analyse/blind3/key.json) says what every opaque id was; open it only after the listening. The page and the key carry
the same build id, and a results file from another build is refused (the letters would mean something else).

Prints, per test, the options in the user's order with their kind and settings and any comments; then the pooled picture by option kind
(mean rank, share ranked first or tied first), the controls (the flipped option should come last; a repeat should rank like what it repeats),
and by part. Ranks are 1 = best; ties share a number; unranked options are left out of the means. Writes prototype/out/analyse/blind3/decoded.md.
"""
import json
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

KEY = Path(__file__).resolve().parent / "out" / "analyse" / "blind3" / "key.json"


def main():
    res = json.loads(Path(sys.argv[1]).read_text())
    key = json.loads(KEY.read_text())
    if res.get("build") != key["build"]:
        sys.exit(f"build mismatch: results {res.get('build')} against key {key['build']}")
    tests = key["tests"]
    out = ["# Blind listening results\n"]
    by_kind = defaultdict(list)           # kind -> [(rank, n ranked, first?)]
    by_part_kind = defaultdict(lambda: defaultdict(list))
    repeats, flips = [], []
    for t in res["tests"]:
        k = tests.get(t["id"])
        if k is None or not any(t["ranks"].values()):
            continue
        ranks = {o: r for o, r in t["ranks"].items() if r}
        n = len(ranks)
        best = min(ranks.values())
        out.append(f"## {t['id']}: {k['test']} (part {k['part']}; level match {'on' if res.get('levelMatchAtExport') else 'off'}; "
                   f"listened {sum(t.get('secs', {}).values()):.0f} s, {t.get('switches', 0)} switches)\n")
        out.append("| rank | kind | setting | comment |\n|---|---|---|---|")
        for o, r in sorted(ranks.items(), key=lambda kv: kv[1]):
            opt = k["options"][o]
            out.append(f"| {r} | {opt['kind']} | {opt['desc']} | {(t.get('comments', {}).get(o) or '').replace(chr(10), ' ')} |")
            by_kind[opt["kind"]].append((r, n, r == best))
            by_part_kind[k["part"]][opt["kind"]].append((r, n, r == best))
        if t.get("note"):
            out.append(f"\nNote: {t['note']}")
        by = {k["options"][o]["kind"]: r for o, r in ranks.items()}
        if "flipped" in by:
            flips.append((t["id"], by["flipped"], n))
        if "repeat" in by:
            lead = k["options"] and next((x for x in ("pick1", "paper", "off") if x in by), None)
            repeats.append((t["id"], by["repeat"], by.get(lead), lead))
        out.append("")
    out.append("## By option kind (all tests)\n")
    out.append("| kind | times ranked | mean rank | mean rank as a share of the options | ranked first (or tied) |\n|---|---|---|---|---|")
    for kind, v in sorted(by_kind.items(), key=lambda kv: np.mean([r / n for r, n, _ in kv[1]])):
        out.append(f"| {kind} | {len(v)} | {np.mean([r for r, _, _ in v]):.2f} | {np.mean([r / n for r, n, _ in v]):.2f} | "
                   f"{sum(f for _, _, f in v)} of {len(v)} |")
    out.append("\n## Controls\n")
    out.append("Flipped (should be last): " + ", ".join(f"{i}: {r} of {n}" for i, r, n in flips))
    out.append("Repeats (should rank like the option repeated): " + ", ".join(f"{i}: repeat {r}, {lead} {rl}" for i, r, rl, lead in repeats))
    for part in sorted(by_part_kind):
        out.append(f"\n## Part {part}\n\n| kind | n | mean rank share | first |\n|---|---|---|---|")
        for kind, v in sorted(by_part_kind[part].items(), key=lambda kv: np.mean([r / n for r, n, _ in kv[1]])):
            out.append(f"| {kind} | {len(v)} | {np.mean([r / n for r, n, _ in v]):.2f} | {sum(f for _, _, f in v)} of {len(v)} |")
    text = "\n".join(out) + "\n"
    (KEY.parent / "decoded.md").write_text(text)
    print(text)


if __name__ == "__main__":
    main()
