import json
import sys
import statistics

RANKS = ["1st", "2nd", "3rd"]

output_file = sys.argv[1]
all_count = int(sys.argv[2])
all_files = sys.argv[3:3 + all_count]
last10_files = sys.argv[3 + all_count:]


def load_reports(files):
    reports = []
    for f in files:
        try:
            with open(f) as fh:
                reports.append(json.load(fh))
        except (json.JSONDecodeError, FileNotFoundError):
            pass
    return reports


def aggregate(reports):
    pooled = {}
    for report in reports:
        for ver, vdata in report.get("version_results", {}).items():
            if ver not in pooled:
                pooled[ver] = {"p_times": [], "c_times": []}
            pooled[ver]["p_times"].extend(vdata["producer_statistics"]["raw_run_times_seconds"])
            pooled[ver]["c_times"].extend(vdata["consumer_statistics"]["raw_run_times_seconds"])

    stats = {}
    for ver, data in pooled.items():
        p_times = data["p_times"]
        c_times = data["c_times"]
        p_avg = statistics.mean(p_times) if p_times else 0
        c_avg = statistics.mean(c_times) if c_times else 0
        p_med = statistics.median(p_times) if p_times else 0
        c_med = statistics.median(c_times) if c_times else 0
        p_min = min(p_times) if p_times else 0
        c_min = min(c_times) if c_times else 0
        stats[ver] = {
            "p_avg": p_avg, "c_avg": c_avg,
            "p_med": p_med, "c_med": c_med,
            "p_min": p_min, "c_min": c_min,
            "combined_avg": p_avg + c_avg,
            "combined_med": p_med + c_med,
        }

    if not stats:
        return None

    fastest_avg = sorted(stats.items(), key=lambda kv: kv[1]["combined_avg"])[:3]
    slowest_avg = sorted(stats.items(), key=lambda kv: kv[1]["combined_avg"], reverse=True)[:3]
    fastest_med = sorted(stats.items(), key=lambda kv: kv[1]["combined_med"])[:3]

    # Top-3 fastest single runs across all (ver, component) pairs
    single_runs = []
    for ver, s in stats.items():
        single_runs.append((ver, "producer", s["p_min"]))
        single_runs.append((ver, "consumer", s["c_min"]))
    fastest_single = sorted(single_runs, key=lambda t: t[2])[:3]

    return {
        "fastest_avg": fastest_avg,
        "slowest_avg": slowest_avg,
        "fastest_single": fastest_single,
        "fastest_med": fastest_med,
    }


def fmt_section(title, result, num_reports):
    sep = "=" * 80
    if result is None:
        return f"{sep}\n{title} ({num_reports} reports) — no data\n{sep}"

    lines = [sep, f"{title} ({num_reports} reports)".center(80), sep]

    lines.append("  Fastest by average (top 3):")
    for rank, (ver, s) in zip(RANKS, result["fastest_avg"]):
        lines.append(f"    {rank}  {ver:<6s} ({s['p_avg']:.6f}s + {s['c_avg']:.6f}s = {s['combined_avg']:.6f}s)")

    lines.append("  Slowest by average (top 3):")
    for rank, (ver, s) in zip(RANKS, result["slowest_avg"]):
        lines.append(f"    {rank}  {ver:<6s} ({s['p_avg']:.6f}s + {s['c_avg']:.6f}s = {s['combined_avg']:.6f}s)")

    lines.append("  Fastest single run (top 3):")
    for rank, (ver, comp, t) in zip(RANKS, result["fastest_single"]):
        lines.append(f"    {rank}  {ver:<6s} {comp:<8s} {t:.6f}s")

    lines.append("  Fastest by median (top 3):")
    for rank, (ver, s) in zip(RANKS, result["fastest_med"]):
        lines.append(f"    {rank}  {ver:<6s} ({s['p_med']:.6f}s + {s['c_med']:.6f}s = {s['combined_med']:.6f}s)")

    lines.append(sep)
    return "\n".join(lines)


all_reports = load_reports(all_files)
last10_reports = load_reports(last10_files)

all_result = aggregate(all_reports)
last10_result = aggregate(last10_reports)

sections = [
    fmt_section("AGGREGATE OVERVIEW - ALL REPORTS", all_result, len(all_reports)),
    "",
    fmt_section("AGGREGATE OVERVIEW - LAST 10 REPORTS", last10_result, len(last10_reports)),
]

with open(output_file, "w") as fh:
    fh.write("\n".join(sections) + "\n")

print(f"Report overview saved to: {output_file}", file=sys.stderr)
