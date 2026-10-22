"""Read-only audit of saved well predictions and service caches against taxonomy.

Use --projects for project folders or their parent directory (repeatable),
--cache for a well-facies response cache and --report for an audit JSON.
No predictions, project catalog, source files or credentials are modified.
"""
import argparse
from collections import Counter
from contextlib import closing
import json
from pathlib import Path
import sqlite3


RESOURCES = Path(__file__).resolve().parents[1] / "resources/geology"


def hierarchy(row):
    return (row.get("facies", ""), row.get("subfacies", row.get("sub_facies", "")),
            row.get("microfacies", row.get("micro_facies", row.get("name", ""))))


def resolve(schema, label, code=None):
    label = str(label).strip()
    matches = [r for r in schema if label in [r.get("name"), hierarchy(r)[2],
               " / ".join(hierarchy(r)), *r.get("aliases", [])]]
    if code is not None:
        matches = [r for r in matches if str(r.get("code")) == str(code)]
    return matches[0] if len(matches) == 1 else None


def audit(project_roots, cache, defaults, templates):
    report = {"projects": [], "cache": {}, "issues": [], "warnings": []}
    system_paths = {hierarchy(r) for r in templates}
    counts = Counter()

    def check(row, label, schema, location, prediction_code=True):
        # Interval attributes keep interpretation code/parents separately from
        # predicted_facies. Differences are scientific opinions, not corruption.
        code = row.get("facies_code") if prediction_code else None
        leaf = resolve(schema, label, code)
        if leaf is None:
            known = resolve(defaults, label)
            target = report["warnings"] if known and code is None else report["issues"]
            target.append({**location, "label": label, "code": code,
                           "reason": "Known system class is absent from the old project taxonomy" if known and code is None else
                                     "Unknown/ambiguous label or code mismatch in project taxonomy"})
        elif hierarchy(leaf) not in system_paths:
            report["issues"].append({**location, "label": label, "reason": "Project class is outside the installed template library"})
        elif prediction_code:
            parent = (row.get("facies"), row.get("sub_facies", row.get("subfacies")))
            if any(value and value != hierarchy(leaf)[i] for i, value in enumerate(parent)):
                report["issues"].append({**location, "label": label, "reason": "Stored parent hierarchy differs from label/code"})
        counts[str(label)] += 1

    catalogs = sorted({p.resolve() for root in project_roots for p in root.rglob("catalog.sqlite")})
    for catalog in catalogs:
        root = catalog.parents[2]
        summary = {"project": str(root), "saved_point_results": 0, "saved_interval_results": 0,
                   "saved_formation_results": 0, "predicted_wells": []}
        seen_wells = set()
        with closing(sqlite3.connect(f"{catalog.as_uri()}?mode=ro", uri=True)) as db:
            current = {}
            query = "select a.type,v.id,v.path,v.extra_json from assets a join versions v on v.asset_id=a.id order by v.version_number"
            versions = list(db.execute(query))
            for kind, _, _, extra in versions:
                data = json.loads(extra)
                if kind == "facies_schema":
                    current[str(data.get("horizon", ""))] = data.get("facies", defaults)
            for kind, vid, path, extra in versions:
                if kind != "wells_prediction":
                    continue
                data = json.loads(extra)
                file = Path(path) if Path(path).is_absolute() else root / path
                if not file.exists():
                    report["issues"].append({"project": str(root), "version": vid, "reason": "Saved prediction file missing"})
                    continue
                document = json.loads(file.read_text(encoding="utf-8-sig"))
                schema = data.get("facies") or current.get(str(data.get("horizon", "")), defaults)
                for feature in document.get("features", []):
                    row = feature.get("properties", {})
                    label = row.get("micro_facies") or row.get("microfacies") or row.get("predicted_facies") or row.get("facies_name")
                    if not label or row.get("prediction_status") in ("skipped", "missing"):
                        continue
                    check(row, label, schema, {"project": str(root), "version": vid, "well": row.get("well_name")})
                    summary["saved_point_results"] += 1
            well_names = {r[0] for r in db.execute("select name from entities where entity_type='well'")}
        for file_name, category, label_key in [("well-log-attributes.gpkg", "saved_interval_results", "predicted_facies"),
                                                ("well-formation-facies.gpkg", "saved_formation_results", "predicted_dominant")]:
            file = root / "artifacts/layers" / file_name
            if not file.exists():
                continue
            with closing(sqlite3.connect(f"{file.as_uri()}?mode=ro", uri=True)) as db:
                db.row_factory = sqlite3.Row
                for (table,) in db.execute("select table_name from gpkg_contents where data_type='features'"):
                    fields = {r[1] for r in db.execute('pragma table_info("' + table.replace('"', '""') + '")')}
                    if label_key not in fields:
                        continue
                    query = 'select * from "' + table.replace('"', '""') + '" where "' + label_key + '" is not null and "' + label_key + '"<>\'\''
                    for record in db.execute(query):
                        row = dict(record); label = row[label_key]
                        horizon = row.get("horizon", row.get("formation", ""))
                        check(row, label, current.get(str(horizon), defaults),
                              {"project": str(root), "table": table, "well": row.get("well_name"), "horizon": horizon}, False)
                        seen_wells.add(row.get("well_name")); summary[category] += 1
        summary["predicted_wells"] = sorted(x for x in seen_wells if x)
        summary["wells_without_saved_interval_or_formation_prediction"] = sorted(well_names - seen_wells)
        report["projects"].append(summary)
    cache_count = cache_rows = real_rows = test_files = 0
    real_wells = set()
    if cache:
        for file in sorted(cache.glob("*.json")):
            data = json.loads(file.read_text(encoding="utf-8-sig")); cache_count += 1
            test = data.get("model", {}).get("name") == "测试微相"
            test_files += test
            for row in data.get("predictions", []):
                label = row.get("label")
                if not label:
                    continue
                check(row, label, defaults, {"cache": file.name, "well": row.get("wellName"), "depth": row.get("depth")})
                cache_rows += 1
                if not test:
                    real_rows += 1; real_wells.add(row.get("wellName"))
    report["cache"] = {"files": cache_count, "prediction_rows": cache_rows, "known_test_model_files": test_files,
                       "non_test_prediction_rows": real_rows, "non_test_wells": sorted(x for x in real_wells if x)}
    report["labels"] = dict(counts)
    report["ok"] = not report["issues"]
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--projects", type=Path, action="append", required=True)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    defaults = json.loads((RESOURCES / "facies-hierarchy.json").read_text(encoding="utf-8"))
    templates = defaults + json.loads((RESOURCES / "facies-templates-extra.json").read_text(encoding="utf-8"))
    result = audit(args.projects, args.cache, defaults, templates)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"ok": result["ok"], "projects": len(result["projects"]), "cache": result["cache"],
                      "labels": result["labels"], "issues": len(result["issues"]), "warnings": len(result["warnings"])}, ensure_ascii=False))
    raise SystemExit(0 if result["ok"] else 1)
