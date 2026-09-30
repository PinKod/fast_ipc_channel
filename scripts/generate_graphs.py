import glob
import json
import os
import re
import statistics
import sys

import plotly.graph_objects as go

graphs_dir = sys.argv[1]
latest_json_path = sys.argv[2]
json_dir = sys.argv[3]
data_tag = sys.argv[4] if len(sys.argv) > 4 else None

OUTLIER_THRESHOLD = 3.0



def load_json(path):
    with open(path) as fh:
        return json.load(fh)


def load_all_jsons(directory):
    if data_tag:
        pattern = os.path.join(directory, f"benchmark_report_{data_tag}_[0-9]*.json")
    else:
        pattern = os.path.join(directory, "benchmark_report_*.json")
    paths = sorted(p for p in glob.glob(pattern) if "latest" not in os.path.basename(p))
    reports = []
    for p in paths:
        try:
            reports.append(load_json(p))
        except (json.JSONDecodeError, OSError):
            pass
    return reports


def save(fig, html_dir, pics_dir, name):
    fig.write_html(os.path.join(html_dir, name + ".html"))
    fig.write_image(os.path.join(pics_dir, name + ".png"), width=1400, height=700)



def ver_sort_key(v):
    m = re.match(r"^(c_)?v(.*)$", v)
    prefix, rest = (m.group(1) or "", m.group(2)) if m else ("", v)
    parts = re.split(r"_", rest)
    return (prefix, [(0, int(x)) if x.isdigit() else (1, x) for x in parts])


def sorted_versions(ver_list):
    return sorted(ver_list, key=ver_sort_key)


def find_outliers(versions, combined_avgs):
    if len(combined_avgs) < 2:
        return set()
    med = statistics.median(combined_avgs)
    return {v for v, a in zip(versions, combined_avgs) if a > OUTLIER_THRESHOLD * med}



def _bar_from_vr(vr, versions, title):
    p_avgs = [vr[v]["producer_statistics"]["average_time_seconds"] for v in versions]
    c_avgs = [vr[v]["consumer_statistics"]["average_time_seconds"] for v in versions]
    fig = go.Figure()
    fig.add_trace(go.Bar(name="producer", x=versions, y=p_avgs))
    fig.add_trace(go.Bar(name="consumer", x=versions, y=c_avgs))
    fig.update_layout(
        title=title,
        barmode="group",
        xaxis_title="Version",
        yaxis_title="Time (s)",
        legend_title="Component",
    )
    return fig


def make_latest_bar(report, exclude=None):
    vr = report["version_results"]
    versions = sorted(
        (v for v in vr if v not in (exclude or set())),
        key=lambda v: (
            vr[v]["producer_statistics"]["average_time_seconds"]
            + vr[v]["consumer_statistics"]["average_time_seconds"]
        ),
    )
    suffix = " — detail (outliers excluded)" if exclude else " (fastest → slowest)"
    return _bar_from_vr(vr, versions, "Latest Run — Average Time per Version" + suffix)


def make_latest_box(report, exclude=None):
    vr = report["version_results"]
    versions = sorted_versions(v for v in vr if v not in (exclude or set()))
    suffix = " — detail (outliers excluded)" if exclude else ""
    fig = go.Figure()
    for v in versions:
        fig.add_trace(go.Box(
            name=f"{v} prod",
            y=vr[v]["producer_statistics"]["raw_run_times_seconds"],
            boxmean=True,
        ))
        fig.add_trace(go.Box(
            name=f"{v} cons",
            y=vr[v]["consumer_statistics"]["raw_run_times_seconds"],
            boxmean=True,
        ))
    fig.update_layout(
        title="Latest Run — Time Distribution per Version/Component" + suffix,
        xaxis_title="Version / Component",
        yaxis_title="Time (s)",
        showlegend=False,
    )
    return fig



def _pool(reports):
    pooled_p, pooled_c = {}, {}
    for report in reports:
        for v, vdata in report.get("version_results", {}).items():
            pooled_p.setdefault(v, []).extend(vdata["producer_statistics"]["raw_run_times_seconds"])
            pooled_c.setdefault(v, []).extend(vdata["consumer_statistics"]["raw_run_times_seconds"])
    return pooled_p, pooled_c


def make_aggregate_bar(reports, exclude=None):
    pooled_p, pooled_c = _pool(reports)
    versions = sorted(
        (v for v in pooled_p if v not in (exclude or set()) and pooled_p[v] and pooled_c.get(v)),
        key=lambda v: sum(pooled_p[v]) / len(pooled_p[v]) + sum(pooled_c[v]) / len(pooled_c[v]),
    )
    p_avgs = [sum(pooled_p[v]) / len(pooled_p[v]) for v in versions]
    c_avgs = [sum(pooled_c[v]) / len(pooled_c[v]) for v in versions]
    suffix = " — detail (outliers excluded)" if exclude else ""
    fig = go.Figure()
    fig.add_trace(go.Bar(name="producer", x=versions, y=p_avgs))
    fig.add_trace(go.Bar(name="consumer", x=versions, y=c_avgs))
    fig.update_layout(
        title=f"Aggregate ({len(reports)} reports) — Pooled Average Time per Version" + suffix,
        barmode="group",
        xaxis_title="Version",
        yaxis_title="Time (s)",
        legend_title="Component",
    )
    return fig


def make_aggregate_trend(reports):
    version_set = set()
    for r in reports:
        version_set.update(r.get("version_results", {}).keys())
    versions = sorted_versions(version_set)

    dated = sorted(reports, key=lambda r: r.get("benchmark_metadata", {}).get("date", ""))
    dates = [r["benchmark_metadata"]["date"] for r in dated]

    fig = go.Figure()
    for v in versions:
        combined = []
        for r in dated:
            vdata = r.get("version_results", {}).get(v)
            if vdata:
                combined.append(
                    vdata["producer_statistics"]["average_time_seconds"]
                    + vdata["consumer_statistics"]["average_time_seconds"]
                )
            else:
                combined.append(None)
        fig.add_trace(go.Scatter(name=v, x=dates, y=combined, mode="lines+markers", connectgaps=False))

    fig.update_layout(
        title="Aggregate Trend — Combined Average Time Over Runs",
        xaxis_title="Benchmark Date",
        yaxis_title="Combined Time (s)",
        legend_title="Version",
    )
    return fig



def write_index(graphs_dir):
    sections = [
        ("Latest Run", [
            ("latest_bar",        "Average time — all versions"),
            ("latest_bar_detail", "Average time — fast versions (outliers excluded)"),
            ("latest_box",        "Time distribution — all versions"),
            ("latest_box_detail", "Time distribution — fast versions (outliers excluded)"),
        ]),
        ("Aggregate (all reports)", [
            ("aggregate_bar",        "Pooled average — all versions"),
            ("aggregate_bar_detail", "Pooled average — fast versions (outliers excluded)"),
            ("aggregate_trend",      "Performance trend over time"),
        ]),
    ]

    cards = ""
    for section_title, graphs in sections:
        cards += f'<h2 style="margin:32px 0 12px">{section_title}</h2>\n<div class="row">\n'
        for name, label in graphs:
            html_rel = f"html/{name}.html"
            pic_rel  = f"pics/{name}.png"
            cards += f"""  <div class="card">
    <a href="{html_rel}" target="_blank">
      <img src="{pic_rel}" alt="{label}">
      <div class="label">{label}</div>
    </a>
  </div>
"""
        cards += "</div>\n"

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Benchmark Graphs</title>
<style>
  body {{ font-family: sans-serif; margin: 0; padding: 20px; background: #f5f5f5; color: #222; }}
  h1   {{ font-size: 1.6em; margin-bottom: 4px; }}
  h2   {{ font-size: 1.2em; color: #444; border-bottom: 1px solid #ccc; padding-bottom: 4px; }}
  .row {{ display: flex; flex-wrap: wrap; gap: 16px; margin-bottom: 8px; }}
  .card {{
    background: #fff; border: 1px solid #ddd; border-radius: 6px;
    overflow: hidden; width: 320px; box-shadow: 0 1px 4px rgba(0,0,0,.08);
    transition: box-shadow .15s;
  }}
  .card:hover {{ box-shadow: 0 3px 10px rgba(0,0,0,.18); }}
  .card a {{ display: block; text-decoration: none; color: inherit; }}
  .card img {{ width: 100%; height: 180px; object-fit: cover; display: block; }}
  .label {{ padding: 8px 10px; font-size: .85em; color: #333; }}
</style>
</head>
<body>
<h1>Benchmark Graphs</h1>
<p style="color:#666;font-size:.9em">Click any card to open the interactive HTML graph in a new tab.</p>
{cards}
</body>
</html>
"""
    with open(os.path.join(graphs_dir, "index.html"), "w") as fh:
        fh.write(html)



html_dir = os.path.join(graphs_dir, "html")
pics_dir = os.path.join(graphs_dir, "pics")
os.makedirs(html_dir, exist_ok=True)
os.makedirs(pics_dir, exist_ok=True)

latest = load_json(latest_json_path)
all_reports = load_all_jsons(json_dir)

vr = latest["version_results"]
all_vers = list(vr.keys())
combined_latest = [
    vr[v]["producer_statistics"]["average_time_seconds"]
    + vr[v]["consumer_statistics"]["average_time_seconds"]
    for v in all_vers
]
outliers_latest = find_outliers(all_vers, combined_latest)

save(make_latest_bar(latest),                          html_dir, pics_dir, "latest_bar")
save(make_latest_bar(latest, exclude=outliers_latest), html_dir, pics_dir, "latest_bar_detail")
save(make_latest_box(latest),                          html_dir, pics_dir, "latest_box")
save(make_latest_box(latest, exclude=outliers_latest), html_dir, pics_dir, "latest_box_detail")

if all_reports:
    pooled_p, pooled_c = _pool(all_reports)
    all_vers_agg = [v for v in pooled_p if pooled_p[v] and pooled_c.get(v)]
    combined_agg = [
        sum(pooled_p[v]) / len(pooled_p[v]) + sum(pooled_c[v]) / len(pooled_c[v])
        for v in all_vers_agg
    ]
    outliers_agg = find_outliers(all_vers_agg, combined_agg)

    save(make_aggregate_bar(all_reports),                         html_dir, pics_dir, "aggregate_bar")
    save(make_aggregate_bar(all_reports, exclude=outliers_agg),   html_dir, pics_dir, "aggregate_bar_detail")
    save(make_aggregate_trend(all_reports),                       html_dir, pics_dir, "aggregate_trend")

write_index(graphs_dir)

print(f"Graphs written to: {graphs_dir}", file=sys.stderr)
