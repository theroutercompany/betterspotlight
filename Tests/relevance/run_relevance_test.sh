#!/usr/bin/env bash
set -euo pipefail

# BetterSpotlight relevance runner for the M2 corpus.
#
# Usage:
#   ./run_relevance_test.sh [--mode fts5|semantic|both] [--corpus path] [--fixture path] [--output path]
#
# Flow:
#   1) Validate fixture and corpus input.
#   2) Build a temporary DB for each run mode.
#   3) Index the fixture with betterspotlight-indexer.
#   4) Run all corpus queries with betterspotlight-query.
#   5) Score results by pass_rank and edge-case rules.
#   6) Emit CSV and print category + aggregate summary.
#   7) Exit non-zero when threshold is not met.

MODE="both"
CORPUS="tests/relevance/test_corpus.json"
FIXTURE="tests/fixtures/standard_home_v1"
OUTPUT="tests/relevance/results.csv"

usage() {
	cat <<'EOF'
Usage: ./run_relevance_test.sh [--mode fts5|semantic|both] [--corpus path] [--fixture path] [--output path]

Options:
  --mode fts5       Run FTS5-only baseline
  --mode semantic   Run FTS5+semantic
  --mode both       Run both and produce comparison (default)
  --corpus          Path to test_corpus.json (default: tests/relevance/test_corpus.json)
  --fixture         Path to fixture directory (default: tests/fixtures/standard_home_v1)
  --output          Path to results CSV (default: tests/relevance/results.csv)
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--mode)
		MODE="${2:-}"
		shift 2
		;;
	--corpus)
		CORPUS="${2:-}"
		shift 2
		;;
	--fixture)
		FIXTURE="${2:-}"
		shift 2
		;;
	--output)
		OUTPUT="${2:-}"
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "Unknown argument: $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

if [[ "$MODE" != "fts5" && "$MODE" != "semantic" && "$MODE" != "both" ]]; then
	echo "Invalid --mode: $MODE" >&2
	exit 2
fi

if [[ ! -d "$FIXTURE" ]]; then
	echo "Fixture directory not found: $FIXTURE" >&2
	exit 2
fi

if [[ ! -f "$CORPUS" ]]; then
	echo "Corpus not found: $CORPUS" >&2
	exit 2
fi

if ! command -v python3 >/dev/null 2>&1; then
	echo "python3 is required" >&2
	exit 2
fi

resolve_bin() {
	local name="$1"
	local candidates=(
		"build/bin/$name"
		"build/src/services/indexer/$name"
		"build/src/services/query/$name"
	)
	for candidate in "${candidates[@]}"; do
		if [[ -x "$candidate" ]]; then
			printf '%s\n' "$candidate"
			return 0
		fi
	done
	if command -v "$name" >/dev/null 2>&1; then
		command -v "$name"
		return 0
	fi
	return 1
}

INDEXER_BIN="$(resolve_bin "betterspotlight-indexer")" || {
	echo "Missing binary: betterspotlight-indexer (expected in build/bin or PATH)" >&2
	exit 2
}
QUERY_BIN="$(resolve_bin "betterspotlight-query")" || {
	echo "Missing binary: betterspotlight-query (expected in build/bin or PATH)" >&2
	exit 2
}

required_fixture_files=(
	"Documents/budget-2026.xlsx"
	"Documents/meeting-notes-jan.md"
	"Developer/myapp/src/config_parser.cpp"
	"Developer/myapp/src/database_migration.py"
	"Desktop/todo-list.md"
	".zshrc"
)

for rel in "${required_fixture_files[@]}"; do
	if [[ ! -f "$FIXTURE/$rel" ]]; then
		echo "Fixture missing expected file: $FIXTURE/$rel" >&2
		exit 2
	fi
done

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

score_run_python='import csv
import json
import os
import subprocess
import sys
from datetime import datetime, timezone

mode = sys.argv[1]
corpus_path = sys.argv[2]
fixture = sys.argv[3]
db_path = sys.argv[4]
query_bin = sys.argv[5]
output_csv = sys.argv[6]

with open(corpus_path, "r", encoding="utf-8") as f:
    corpus = json.load(f)

queries = corpus["queries"]
threshold = 0.80

def run_query(query_text, limit, mode_name):
    # Query binary is expected to support JSON output. We tolerate a few shapes
    # to keep this runner compatible with iterative CLI changes.
    cmd = [query_bin, "--db", db_path, "--query", query_text, "--limit", str(limit), "--json"]
    if mode_name == "fts5":
        cmd.extend(["--semantic", "false"])
    else:
        cmd.extend(["--semantic", "true"])

    proc = subprocess.run(cmd, text=True, capture_output=True)
    if proc.returncode != 0:
        return []

    text = proc.stdout.strip()
    if not text:
        return []

    try:
        payload = json.loads(text)
    except json.JSONDecodeError:
        return []

    if isinstance(payload, dict):
        if isinstance(payload.get("results"), list):
            payload = payload["results"]
        elif isinstance(payload.get("items"), list):
            payload = payload["items"]
        else:
            return []

    if not isinstance(payload, list):
        return []

    normalized = []
    for item in payload:
        if not isinstance(item, dict):
            continue
        path = item.get("path") or item.get("relativePath") or item.get("file") or ""
        if not path:
            continue
        rel = os.path.relpath(path, fixture) if os.path.isabs(path) else path
        normalized.append({
            "path": rel,
            "fts5": float(item.get("fts5Score", 0.0)),
            "semantic": float(item.get("semanticScore", 0.0)),
            "merged": float(item.get("score", item.get("mergedScore", 0.0))),
        })
    return normalized

def category_bucket():
    return {
        "exact_filename": {"pass": 0, "total": 0},
        "partial_filename": {"pass": 0, "total": 0},
        "content_search": {"pass": 0, "total": 0},
        "semantic": {"pass": 0, "total": 0},
        "edge_case": {"pass": 0, "total": 0},
    }

summary = {
    "pass": 0,
    "total": 0,
    "by_category": category_bucket(),
    "failures": [],
}

os.makedirs(os.path.dirname(output_csv) or ".", exist_ok=True)
with open(output_csv, "w", newline="", encoding="utf-8") as f:
    writer = csv.writer(f)
    writer.writerow(["id", "query", "category", "expected_file", "actual_rank", "actual_top3", "result", "fts5_score", "semantic_score", "merged_score"])

    for q in queries:
        qid = q["id"]
        query = q["query"]
        category = q["category"]
        expected_files = q.get("expected_files", [])
        pass_rank = int(q.get("pass_rank", 3))

        summary["total"] += 1
        summary["by_category"][category]["total"] += 1

        results = run_query(query, max(pass_rank, 5), mode)
        top_ranked = results[:pass_rank]

        passed = False
        actual_rank = ""
        expected_display = expected_files[0] if expected_files else "(any)"

        if qid == "q046":
            passed = len(results) > 0
            if passed:
                actual_rank = 1
        elif qid == "q049":
            for idx, item in enumerate(results[:5], start=1):
                if item["path"].endswith(".py"):
                    passed = True
                    actual_rank = idx
                    break
        elif qid == "q050":
            cfg_rank = 10**9
            test_rank = 10**9
            for idx, item in enumerate(results, start=1):
                if item["path"] == "Developer/myapp/src/config_parser.cpp" and cfg_rank == 10**9:
                    cfg_rank = idx
                if item["path"] == "Developer/myapp/tests/test_config.py" and test_rank == 10**9:
                    test_rank = idx
            passed = cfg_rank < test_rank or (cfg_rank != 10**9 and test_rank == 10**9)
            if cfg_rank != 10**9:
                actual_rank = cfg_rank
        else:
            expected_set = set(expected_files)
            for idx, item in enumerate(top_ranked, start=1):
                if item["path"] in expected_set:
                    passed = True
                    actual_rank = idx
                    break

        result_label = "PASS" if passed else "FAIL"
        if passed:
            summary["pass"] += 1
            summary["by_category"][category]["pass"] += 1
        else:
            summary["failures"].append({
                "id": qid,
                "query": query,
                "expected": expected_display,
                "actual_top3": [i["path"] for i in results[:3]],
            })

        first = results[0] if results else {"fts5": 0.0, "semantic": 0.0, "merged": 0.0}
        writer.writerow([
            qid,
            query,
            category,
            expected_display,
            actual_rank,
            "|".join([i["path"] for i in results[:3]]),
            result_label,
            f"{first['fts5']:.4f}",
            f"{first['semantic']:.4f}",
            f"{first['merged']:.4f}",
        ])

percent = int((summary["pass"] / summary["total"]) * 100) if summary["total"] else 0
label = "FTS5-only" if mode == "fts5" else "FTS5+semantic"

print("=== BetterSpotlight Relevance Test ===")
print(f"Fixture:    {os.path.basename(os.path.normpath(fixture))}")
print(f"Mode:       {label}")
print(f"Date:       {datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}")
print()
print("Results by category:")
for cat in ["exact_filename", "partial_filename", "content_search", "semantic", "edge_case"]:
    p = summary["by_category"][cat]["pass"]
    t = summary["by_category"][cat]["total"]
    pct = int((p / t) * 100) if t else 0
    print(f"  {cat:16s} {p:2d}/{t:<2d} ({pct:3d}%)")

status = "PASS" if summary["total"] and (summary["pass"] / summary["total"]) >= threshold else "FAIL"
print()
print(f"TOTAL: {summary['pass']}/{summary['total']} ({percent}%)  {status} (threshold: 80%)")
print()
print("Failures:")
if summary["failures"]:
    for f in summary["failures"]:
        top = ", ".join(f["actual_top3"])
        print(f"  {f['id']}: {f['query']}   expected: {f['expected']}   actual top 3: [{top}]")
else:
    print("  (none)")

print(f"PASS_COUNT={summary['pass']}")
print(f"TOTAL_COUNT={summary['total']}")
'

run_mode() {
	local mode_name="$1"
	local csv_file="$2"
	local db_path="$tmp_dir/relevance_${mode_name}.db"

	rm -f "$db_path"
	local embedding_flag="true"
	if [[ "$mode_name" == "fts5" ]]; then
		embedding_flag="false"
	fi

	# Index the full fixture and wait for ingestion completion before querying.
	"$INDEXER_BIN" --root "$FIXTURE" --db "$db_path" --wait-for-complete --embedding-enabled "$embedding_flag"

	# Execute and score all queries, then print report and sentinel counters.
	python3 -c "$score_run_python" "$mode_name" "$CORPUS" "$FIXTURE" "$db_path" "$QUERY_BIN" "$csv_file"
}

parse_count() {
	local key="$1"
	local text="$2"
	awk -F= -v k="$key" '$1==k {print $2}' <<<"$text" | tail -n 1
}

if [[ "$MODE" == "fts5" ]]; then
	out="$(run_mode "fts5" "$OUTPUT")"
	printf '%s\n' "$out"
	p="$(parse_count PASS_COUNT "$out")"
	t="$(parse_count TOTAL_COUNT "$out")"
	python3 -c 'import sys; p=int(sys.argv[1]); t=int(sys.argv[2]); sys.exit(0 if t and (p/t)>=0.80 else 1)' "$p" "$t"
	exit $?
fi

if [[ "$MODE" == "semantic" ]]; then
	out="$(run_mode "semantic" "$OUTPUT")"
	printf '%s\n' "$out"
	p="$(parse_count PASS_COUNT "$out")"
	t="$(parse_count TOTAL_COUNT "$out")"
	python3 -c 'import sys; p=int(sys.argv[1]); t=int(sys.argv[2]); sys.exit(0 if t and (p/t)>=0.80 else 1)' "$p" "$t"
	exit $?
fi

fts5_csv="$tmp_dir/results_fts5.csv"
fts5_out="$(run_mode "fts5" "$fts5_csv")"
sem_out="$(run_mode "semantic" "$OUTPUT")"

printf '%s\n\n' "$fts5_out"
printf '%s\n' "$sem_out"

fts5_pass="$(parse_count PASS_COUNT "$fts5_out")"
fts5_total="$(parse_count TOTAL_COUNT "$fts5_out")"
sem_pass="$(parse_count PASS_COUNT "$sem_out")"
sem_total="$(parse_count TOTAL_COUNT "$sem_out")"

python3 -c 'import sys
fp,ft,sp,st=map(int, sys.argv[1:5])
f_pct=int((fp/ft)*100) if ft else 0
s_pct=int((sp/st)*100) if st else 0
delta=sp-fp
print()
print("A/B comparison:")
print(f"  FTS5-only:      {fp}/{ft} ({f_pct}%)")
print(f"  FTS5+semantic:  {sp}/{st} ({s_pct}%)")
print(f"  Delta:          {delta:+d} queries improved")
print(f"  Regressions:    {0 if delta>=0 else -delta} queries worse")
sys.exit(0 if st and (sp/st)>=0.80 else 1)
' "$fts5_pass" "$fts5_total" "$sem_pass" "$sem_total"
