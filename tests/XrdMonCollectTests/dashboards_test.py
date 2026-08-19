#!/usr/bin/env python3
"""Validate the dashboards and index template shipped with xrdmoncollect.

Nothing else checks these files: they are data, not code, so a typo in a
metric name or a duplicated panel id survives every build and only shows up
as an empty panel in somebody's Grafana. The checks here are cheap and catch
the two ways these files actually rot.

The interesting one is coverage. Metrics are added to the decoder far more
often than dashboards are revisited, so this asserts in *both* directions:
every metric a panel queries has to exist in the sources, and every metric the
sources register has to appear on some dashboard.

Usage: dashboards_test.py <source-root>
"""

import json
import pathlib
import re
import sys

# Grafana's own variables, which are never declared in templating.list.
BUILTIN_VARS = {
    "__rate_interval", "__interval", "__interval_ms", "__range", "__range_s",
    "__range_ms", "__auto_interval", "__from", "__to", "__all", "__name",
    "__field", "__series", "__value", "__dashboard", "__user", "__org",
    "__timeFilter", "timeFilter", "__auto",
}

# Metrics that deliberately appear on no dashboard. Keep this empty if you
# can: an entry here is a metric nobody can see.
UNCHARTED = set()

errors = []
notes = []


def fail(msg):
    errors.append(msg)


# Files that register metrics. Every registration -- whether it goes straight
# to counterSeries() or through one of the local helpers (OBS, perOp, delta)
# -- passes the short name first and the help text second, so that pair is
# what we match on rather than the name of the call, which varies.
REGISTRARS = ("XrdMonCollect.cc", "XrdMonDecode.cc")
REGISTER_CALL = re.compile(
    r'\(\s*"([a-z][a-z0-9_]{2,})"\s*,\s*(?:k[A-Z]\w*Help|"[^"]*\s[^"]*")',
    re.S)
REGISTRY_API = re.compile(r'\b(?:counterSeries|gaugeSeries|histogramSeries|'
                          r'observeCounter|observeGauge)\b')
# Labels Prometheus attaches at scrape time, plus the global site label and
# the histogram bucket bound. None of them appear in a registration.
IMPLICIT_LABELS = {"instance", "job", "site", "le"}


def label_set(text, pos):
    """The label names of the registration whose arguments start at `pos`.

    Deliberately narrow: the label vector has to follow the help text
    immediately as a brace literal. A registration that passes its labels as a
    variable, or builds them with a helper that prepends a common prefix,
    returns None and goes unchecked -- reading those would need to understand
    the helper, and guessing would report labels that are really there as
    missing.
    """
    tail = text[pos:pos + 600]
    opening = re.match(r'\s*,\s*(?=\{\{)', tail)
    if not opening:
        return None
    depth, end = 0, opening.end()
    start = end
    while end < len(tail):
        if tail[end] == "{":
            depth += 1
        elif tail[end] == "}":
            depth -= 1
            if depth == 0:
                break
        end += 1
    else:
        return None
    return set(re.findall(r'\{\s*"([a-z_]+)"', tail[start:end + 1]))


def load_sources(root):
    """Map every registered metric to the file that registers it.

    The exposed name is <prefix>_<subsystem>_<name>: the prefix is fixed as
    "xrootd", and the subsystem is whichever subsystem() call most recently
    preceded the registration, so the shoveler's families are not confused
    with the collector's. The decoder reaches its subsystem through a pointer
    and never names it, so everything there is the collector's.
    """
    metrics, labels = {}, {}
    share = root / "src/XrdApps/XrdMonCollect"
    for path in sorted(share.glob("*.cc")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if path.name not in REGISTRARS:
            if REGISTRY_API.search(text):
                fail(f"{path.name} registers metrics but is not scanned -- "
                     f"add it to REGISTRARS in this test")
            continue
        subsystems = [(m.start(), m.group(1))
                      for m in re.finditer(r'subsystem\("(\w+)"\)', text)]
        for m in REGISTER_CALL.finditer(text):
            sub = "collector"
            for pos, name in subsystems:
                if pos < m.start():
                    sub = name
            full = f"xrootd_{sub}_{m.group(1)}"
            metrics[full] = path.name
            found = label_set(text, m.end())
            if found is not None:
                labels[full] = found | IMPLICIT_LABELS
    return metrics, labels


def metric_names(expr):
    """Metric names in a PromQL expression, minus histogram suffixes."""
    out = set()
    for name in re.findall(r'\bxrootd_[a-z0-9_]+', expr):
        out.add(re.sub(r'_(bucket|sum|count)$', '', name))
    return out


def walk(node, key, out):
    if isinstance(node, dict):
        for k, v in node.items():
            if k == key and isinstance(v, str):
                out.append(v)
            else:
                walk(v, key, out)
    elif isinstance(node, list):
        for v in node:
            walk(v, key, out)


def check_dashboard(path, seen_uids, registered, labels, charted):
    try:
        dash = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        fail(f"{path.name}: not valid JSON: {e}")
        return

    for field in ("uid", "title", "panels", "templating", "tags"):
        if field not in dash:
            fail(f"{path.name}: missing top-level '{field}'")
    if "uid" not in dash:
        return

    uid = dash["uid"]
    if uid in seen_uids:
        fail(f"{path.name}: uid '{uid}' already used by {seen_uids[uid]}")
    seen_uids[uid] = path.name

    if "xrootd" not in dash.get("tags", []):
        fail(f"{path.name}: tags must include 'xrootd' -- the cross-dashboard "
             f"link dropdown finds siblings by that tag")

    # Panel ids must be unique, including the row panels.
    ids = [p["id"] for p in dash.get("panels", []) if "id" in p]
    for pid in {i for i in ids if ids.count(i) > 1}:
        fail(f"{path.name}: panel id {pid} used {ids.count(pid)} times")

    # Every panel type has to be declared, or import warns.
    declared = {r["id"] for r in dash.get("__requires", [])}
    used = {p["type"] for p in dash.get("panels", [])} - {"row"}
    for kind in sorted(used - declared):
        fail(f"{path.name}: panel type '{kind}' is used but not in __requires")
    for kind in sorted(declared - used - {"grafana", "prometheus", "loki"}):
        fail(f"{path.name}: __requires declares unused panel type '{kind}'")

    # Every $variable has to be declared.
    declared_vars = {v["name"] for v in dash["templating"]["list"]}
    body = json.dumps(dash)
    for var in set(re.findall(r'\$\{?(\w+)', body)):
        if var not in declared_vars and var not in BUILTIN_VARS:
            fail(f"{path.name}: '${var}' is used but not declared in "
                 f"templating.list")

    # Metric names, for Prometheus dashboards only; the Loki ones use LogQL
    # over document attributes, which have no registry to check against.
    inputs = dash.get("__inputs", [])
    if not any(i.get("pluginId") == "prometheus" for i in inputs):
        return
    exprs = []
    walk(dash.get("panels", []), "expr", exprs)
    walk(dash["templating"]["list"], "query", exprs)
    walk(dash["templating"]["list"], "definition", exprs)
    for expr in exprs:
        for name in metric_names(expr):
            if name not in registered:
                fail(f"{path.name}: '{name}' is not a metric xrdmoncollect "
                     f"emits")
            charted.add(name)
        check_labels(path.name, expr, labels)


def check_labels(fname, expr, labels):
    """Selectors and by() clauses must name labels the metric really has.

    Getting this wrong costs nothing at query time -- Prometheus matches
    nothing and the panel is simply empty -- so it is the mistake most likely
    to ship unnoticed.
    """
    def known(name):
        return labels.get(re.sub(r'_(bucket|sum|count)$', '', name))

    for name, selector in re.findall(r'(xrootd_[a-z0-9_]+)\{([^}]*)\}', expr):
        have = known(name)
        if have is None:
            continue
        for extra in sorted(set(re.findall(r'(\w+)\s*=~?"', selector)) - have):
            fail(f"{fname}: {name} has no '{extra}' label "
                 f"(it has {', '.join(sorted(have))})")

    for group, body in re.findall(r'\bby\s*\(([^)]*)\)\s*\((.*)', expr):
        wanted = {g.strip() for g in group.split(",") if g.strip()}
        for name in set(re.findall(r'xrootd_[a-z0-9_]+', body)):
            have = known(name)
            if have is None:
                continue
            for extra in sorted(wanted - have):
                fail(f"{fname}: cannot group {name} by '{extra}' "
                     f"(it has {', '.join(sorted(have))})")


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    share = root / "src/XrdApps/XrdMonCollect"
    if not share.is_dir():
        print(f"not a source tree: {root}", file=sys.stderr)
        return 2

    registered, labels = load_sources(root)
    if len(registered) < 40:
        fail(f"only {len(registered)} metrics found in the sources -- the "
             f"scan is broken, not the dashboards")
    notes.append(f"{len(registered)} metrics registered in the sources, "
                 f"{len(labels)} with a checkable label set")

    seen_uids, charted = {}, set()
    dashboards = sorted(share.glob("grafana-*.json"))
    if not dashboards:
        fail("no grafana-*.json found")
    for path in dashboards:
        check_dashboard(path, seen_uids, registered, labels, charted)
    notes.append(f"{len(dashboards)} dashboards, {len(charted)} metrics charted")

    for name in sorted(set(registered) - charted - UNCHARTED):
        fail(f"{name} ({registered[name]}) appears on no dashboard")

    # The OpenSearch saved objects and index template are plain JSON/NDJSON.
    for path in sorted(share.glob("opensearch-*.ndjson")):
        for lineno, line in enumerate(path.read_text(encoding="utf-8")
                                      .splitlines(), 1):
            if not line.strip():
                continue
            try:
                json.loads(line)
            except json.JSONDecodeError as e:
                fail(f"{path.name}:{lineno}: not valid JSON: {e}")
    try:
        json.loads((share / "opensearch-template.json")
                   .read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as e:
        fail(f"opensearch-template.json: {e}")

    for note in notes:
        print(f"ok: {note}")
    for err in errors:
        print(f"FAIL: {err}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
