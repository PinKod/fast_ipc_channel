"""Usage: generate_combined_report.py <small_json> <big_json> <out_txt> <out_json> <small_tag> <big_tag>"""
import json
import sys


def avg_time(entry):
    return (
        entry["producer_statistics"]["average_time_seconds"]
        + entry["consumer_statistics"]["average_time_seconds"]
    )


def main():
    small_path, big_path, out_txt, out_json, small_tag, big_tag = sys.argv[1:7]

    with open(small_path) as fh:
        small = json.load(fh)["version_results"]
    big = {}
    if big_path and big_path != "-":
        try:
            with open(big_path) as fh:
                big = json.load(fh)["version_results"]
        except (OSError, json.JSONDecodeError):
            big = {}

    small_avgs = {v: avg_time(e) for v, e in small.items()}
    big_avgs = {v: avg_time(e) for v, e in big.items()}

    small_fastest = min(small_avgs.values()) if small_avgs else None
    big_fastest = min(big_avgs.values()) if big_avgs else None

    rows = []
    for version in sorted(set(small_avgs) | set(big_avgs)):
        s_avg = small_avgs.get(version)
        b_avg = big_avgs.get(version)
        s_ratio = (s_avg / small_fastest) if (s_avg is not None and small_fastest) else None
        b_ratio = (b_avg / big_fastest) if (b_avg is not None and big_fastest) else None
        combined = (s_ratio + b_ratio) / 2 if (s_ratio is not None and b_ratio is not None) else None
        rows.append(
            {
                "version": version,
                f"{small_tag}_avg_seconds": s_avg,
                f"{small_tag}_ratio_to_fastest": s_ratio,
                f"{big_tag}_avg_seconds": b_avg,
                f"{big_tag}_ratio_to_fastest": b_ratio,
                "combined_score": combined,
            }
        )

    def sort_key(r):
        if r["combined_score"] is not None:
            return (0, r["combined_score"])
        return (1, r[f"{small_tag}_ratio_to_fastest"] or float("inf"))

    rows.sort(key=sort_key)

    with open(out_json, "w") as fh:
        json.dump({"rows": rows, "small_tag": small_tag, "big_tag": big_tag}, fh, indent=2)

    lines = []
    lines.append("=" * 96)
    lines.append("COMBINED RANKING (normalized: ratio to fastest on each dataset, averaged)".center(96))
    lines.append("=" * 96)
    lines.append(
        f"{'Version':<10} | {small_tag+' avg(s)':>14} | {small_tag+' ratio':>10} | "
        f"{big_tag+' avg(s)':>14} | {big_tag+' ratio':>10} | {'combined':>10}"
    )
    lines.append("-" * 96)
    for r in rows:
        s_avg = r[f"{small_tag}_avg_seconds"]
        s_ratio = r[f"{small_tag}_ratio_to_fastest"]
        b_avg = r[f"{big_tag}_avg_seconds"]
        b_ratio = r[f"{big_tag}_ratio_to_fastest"]
        combined = r["combined_score"]
        s_avg_s = f"{s_avg:.6f}" if s_avg is not None else "-"
        s_ratio_s = f"{s_ratio:.3f}x" if s_ratio is not None else "-"
        b_avg_s = f"{b_avg:.6f}" if b_avg is not None else "-"
        b_ratio_s = f"{b_ratio:.3f}x" if b_ratio is not None else "-"
        combined_s = f"{combined:.3f}" if combined is not None else "N/A (not in top-N this run)"
        lines.append(
            f"{r['version']:<10} | {s_avg_s:>14} | {s_ratio_s:>10} | "
            f"{b_avg_s:>14} | {b_ratio_s:>10} | {combined_s:>10}"
        )
    lines.append("=" * 96)

    with open(out_txt, "w") as fh:
        fh.write("\n".join(lines) + "\n")

    print(f"Combined report: {out_txt}")


if __name__ == "__main__":
    main()
