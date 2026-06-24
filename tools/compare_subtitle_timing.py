#!/usr/bin/env python3
"""Compare two subtitle-timing CSV dumps produced for the same source file:

  A.csv - mmts-dsfilter's own schedule (tools/mmts_compare_dump.exe)
  B.csv - arib-splitter's decode path, via dantto4k's converted TS
          (libaribcaption's test_caption_timeline_probe.exe)

Both share the schema: index,streamIndex,startMs,endMs,durationMs,text

Cues are aligned by text similarity (difflib), not by index, because either
side can legitimately drop or add a cue relative to the other. A single
robust (median) global offset is factored out before flagging anomalies, so
an expected fixed startup offset (e.g. dantto4k skipping ahead to the first
RAP) is not confused with an actual relative-timing bug.
"""
import argparse
import csv
import difflib
import statistics
import sys


class Cue:
    __slots__ = ("index", "stream_index", "start_ms", "end_ms", "duration_ms", "text")

    def __init__(self, row):
        self.index = int(row["index"])
        self.stream_index = int(row["streamIndex"])
        self.start_ms = int(row["startMs"])
        self.end_ms = int(row["endMs"])
        self.duration_ms = int(row["durationMs"])
        self.text = row["text"]


def load_csv(path):
    with open(path, newline="", encoding="utf-8") as f:
        return [Cue(row) for row in csv.DictReader(f)]


def normalize_text(text):
    return " ".join(text.split())


def align(cues_a, cues_b):
    """Returns (matched_pairs, only_a, only_b). matched_pairs preserves the
    chronological order of cues_a."""
    keys_a = [normalize_text(c.text) for c in cues_a]
    keys_b = [normalize_text(c.text) for c in cues_b]
    matcher = difflib.SequenceMatcher(None, keys_a, keys_b, autojunk=False)

    matched_pairs = []
    only_a = []
    only_b = []
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            for i, j in zip(range(i1, i2), range(j1, j2)):
                matched_pairs.append((cues_a[i], cues_b[j]))
        elif tag == "replace":
            # Ambiguous block: best-effort pairwise alignment within the
            # block, leftover entries on the longer side count as unmatched.
            n = min(i2 - i1, j2 - j1)
            for k in range(n):
                matched_pairs.append((cues_a[i1 + k], cues_b[j1 + k]))
            only_a.extend(cues_a[i1 + n:i2])
            only_b.extend(cues_b[j1 + n:j2])
        elif tag == "delete":
            only_a.extend(cues_a[i1:i2])
        elif tag == "insert":
            only_b.extend(cues_b[j1:j2])
    return matched_pairs, only_a, only_b


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv_a", help="mmts-dsfilter schedule CSV (Tool A)")
    parser.add_argument("csv_b", help="arib-splitter/libaribcaption schedule CSV (Tool B)")
    parser.add_argument("--tolerance-ms", type=int, default=300,
                        help="Flag a matched cue if |residual start/end delta| exceeds this (default: 300ms)")
    parser.add_argument("--report", help="Optional path to write a detailed per-cue CSV report")
    args = parser.parse_args()

    cues_a = load_csv(args.csv_a)
    cues_b = load_csv(args.csv_b)
    if not cues_a or not cues_b:
        print(f"ERROR: empty input (A={len(cues_a)} cues, B={len(cues_b)} cues)", file=sys.stderr)
        return 1

    matched_pairs, only_a, only_b = align(cues_a, cues_b)
    if not matched_pairs:
        print("ERROR: no cues could be matched between A and B at all "
              "(check that both dumps are for the same source file)", file=sys.stderr)
        return 1

    start_deltas = [a.start_ms - b.start_ms for a, b in matched_pairs]
    global_offset_ms = statistics.median(start_deltas)

    anomalies = []
    report_rows = []
    for a, b in matched_pairs:
        residual_start = (a.start_ms - b.start_ms) - global_offset_ms
        residual_end = (a.end_ms - b.end_ms) - global_offset_ms
        flagged = abs(residual_start) > args.tolerance_ms or abs(residual_end) > args.tolerance_ms
        if flagged:
            anomalies.append((a, b, residual_start, residual_end))
        report_rows.append((a, b, residual_start, residual_end, flagged))

    print(f"Matched cues: {len(matched_pairs)}")
    print(f"Only in A (mmts-dsfilter), unmatched: {len(only_a)}")
    print(f"Only in B (arib-splitter), unmatched: {len(only_b)}")
    print(f"Global offset (median, A - B): {global_offset_ms:+.0f} ms "
          f"(expected fixed startup skew; subtracted before flagging)")
    print(f"Tolerance: {args.tolerance_ms} ms")
    print(f"Anomalies beyond tolerance: {len(anomalies)}")
    print()

    if only_a:
        print("--- Cues only in A (mmts-dsfilter showed something arib-splitter didn't) ---")
        for c in only_a[:20]:
            print(f"  [A #{c.index}] start={c.start_ms}ms end={c.end_ms}ms text={c.text!r}")
        if len(only_a) > 20:
            print(f"  ... and {len(only_a) - 20} more")
        print()

    if only_b:
        print("--- Cues only in B (arib-splitter showed something mmts-dsfilter didn't) ---")
        for c in only_b[:20]:
            print(f"  [B #{c.index}] start={c.start_ms}ms end={c.end_ms}ms text={c.text!r}")
        if len(only_b) > 20:
            print(f"  ... and {len(only_b) - 20} more")
        print()

    if anomalies:
        print("--- Timing anomalies (residual beyond tolerance) ---")
        for a, b, rs, re_ in anomalies[:50]:
            print(f"  [A #{a.index} / B #{b.index}] residualStart={rs:+.0f}ms "
                  f"residualEnd={re_:+.0f}ms text={a.text!r}")
        if len(anomalies) > 50:
            print(f"  ... and {len(anomalies) - 50} more")
        print()

    if args.report:
        with open(args.report, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(["aIndex", "bIndex", "aStartMs", "bStartMs", "residualStartMs",
                            "aEndMs", "bEndMs", "residualEndMs", "flagged", "text"])
            for a, b, rs, re_, flagged in report_rows:
                writer.writerow([a.index, b.index, a.start_ms, b.start_ms, f"{rs:.0f}",
                                a.end_ms, b.end_ms, f"{re_:.0f}", int(flagged), a.text])
        print(f"Detailed report written to {args.report}")

    ok = not anomalies and not only_a and not only_b
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
