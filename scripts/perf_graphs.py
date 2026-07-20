import os
import sys

import plotly.graph_objects as go

tsv_path = sys.argv[1]
graphs_dir = sys.argv[2]

os.makedirs(graphs_dir, exist_ok=True)

rows = []
with open(tsv_path) as fh:
    header = fh.readline()  # skip header
    for line in fh:
        line = line.strip()
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) < 6:
            continue
        symbol, pct, samples, dso, role, version = parts[0], float(parts[1]), int(parts[2]), parts[3], parts[4], parts[5]
        rows.append({
            "symbol": symbol,
            "pct": pct,
            "samples": samples,
            "dso": dso,
            "role": role,
            "version": version,
        })

if not rows:
    print("No data to plot", file=sys.stderr)
    sys.exit(0)

roles = sorted(set(r["role"] for r in rows))
versions = sorted(set(r["version"] for r in rows))

for role in roles:
    role_rows = [r for r in rows if r["role"] == role]
    role_rows.sort(key=lambda r: r["pct"], reverse=True)
    top = role_rows[:20]

    symbols = [r["symbol"] for r in top]
    pcts = [r["pct"] for r in top]
    samples_list = [r["samples"] for r in top]
    hover = [
        f"{r['symbol']}<br>{r['pct']:.2f}% ({r['samples']} samples)<br>{r['dso']}"
        for r in top
    ]

    ver_label = versions[0] if len(versions) == 1 else "+".join(versions)

    fig = go.Figure(go.Bar(
        x=pcts,
        y=list(reversed(symbols)),
        orientation="h",
        hovertext=list(reversed(hover)),
        hoverinfo="text",
        marker_color="#58a6ff",
        text=[f"{p:.1f}%" for p in pcts],
        textposition="outside",
    ))
    fig.update_layout(
        title=f"{ver_label} {role} — Top {len(top)} Functions by CPU Time",
        xaxis_title="CPU Cycles (%)",
        yaxis=dict(autorange="reversed", categoryorder="total ascending"),
        height=max(400, len(top) * 28),
        width=1000,
        margin=dict(l=300, r=60, t=50, b=50),
        template="plotly_dark",
        paper_bgcolor="#0d1117",
        plot_bgcolor="#161b22",
        font=dict(color="#c9d1d9"),
    )

    name = f"cpu_time_{role}"
    fig.write_html(os.path.join(graphs_dir, name + ".html"))
    fig.write_image(os.path.join(graphs_dir, name + ".png"), width=1000, height=max(400, len(top) * 28))
    print(f"  Chart saved: {name}.html / .png", file=sys.stderr)

if len(roles) > 1:
    figs = []
    for role in roles:
        role_rows = [r for r in rows if r["role"] == role]
        role_rows.sort(key=lambda r: r["pct"], reverse=True)
        top = role_rows[:20]
        symbols = [r["symbol"] for r in top]
        pcts = [r["pct"] for r in top]
        hover = [
            f"{r['symbol']}<br>{r['pct']:.2f}% ({r['samples']} samples)<br>{r['dso']}"
            for r in top
        ]
        figs.append((role, symbols, pcts, hover))

    import plotly.subplots as sp

    fig = sp.make_subplots(
        rows=1, cols=2,
        subplot_titles=[f"{v} {r}" for r, v, _, _ in
                        [(f[0], "+".join(versions), None, None) for f in figs]],
        horizontal_spacing=0.15,
    )

    for i, (role, symbols, pcts, hover) in enumerate(figs):
        fig.add_trace(
            go.Bar(
                x=pcts,
                y=list(reversed(symbols)),
                orientation="h",
                hovertext=list(reversed(hover)),
                hoverinfo="text",
                marker_color="#58a6ff" if role == "producer" else "#3fb950",
                text=[f"{p:.1f}%" for p in pcts],
                textposition="outside",
                name=role,
                showlegend=True,
            ),
            row=1, col=i + 1,
        )

    ver_label = "+".join(versions)
    fig.update_layout(
        title=f"{ver_label} — CPU Time by Function (Producer vs Consumer)",
        height=700,
        width=1600,
        template="plotly_dark",
        paper_bgcolor="#0d1117",
        plot_bgcolor="#161b22",
        font=dict(color="#c9d1d9"),
        barmode="overlay",
    )
    fig.update_yaxes(autorange="reversed")

    name = "cpu_time_combined"
    fig.write_html(os.path.join(graphs_dir, name + ".html"))
    fig.write_image(os.path.join(graphs_dir, name + ".png"), width=1600, height=700)
    print(f"  Combined chart saved: {name}.html / .png", file=sys.stderr)

print(f"Graphs written to: {graphs_dir}", file=sys.stderr)
