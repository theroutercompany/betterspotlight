# M2: Relevance Testing Specification

**Scope:** 50-query test corpus, standard fixture definition, scoring methodology, baseline comparison (FTS5-only vs. FTS5+semantic), CI integration, and regression tracking.

**References:** [milestones/acceptance-criteria.md](../acceptance-criteria.md) M2 Section (80% relevance threshold, corpus composition), [foundation/ranking-scoring.md](../../foundation/ranking-scoring.md) (scoring formula, match types, boost algorithms), [vector-search.md](vector-search.md) (semantic-lexical merge), [feedback-system.md](feedback-system.md) (interaction-based boosts).

---

## 1. Purpose

BetterSpotlight M2 claims that semantic search improves relevance. That claim is meaningless without measurement. This document defines a reproducible test that answers two questions:

1. Does the combined FTS5+semantic pipeline reach the 80% threshold on a controlled fixture?
2. Does FTS5+semantic actually outperform FTS5-only (the M1 baseline)?

If the answer to question 2 is "no" on any query category, the semantic system is adding complexity without value and the merge weights need tuning.

---

## 2. Standard Test Fixture

### 2.1 Fixture Identity

| Property | Value |
|----------|-------|
| Name | `standard_home_v1` |
| Location | `tests/fixtures/standard_home_v1/` |
| Total files | ~500 (representative, not 500K) |
| Total size | ~50MB (small enough to version-control) |
| Purpose | Controlled, deterministic relevance testing |

The fixture is NOT the 500K-file performance benchmark fixture (that lives at `tests/fixtures/large_home/` and is used for latency/throughput testing). The relevance fixture is small, version-controlled, and designed so every query has exactly one correct answer.

### 2.2 Directory Structure

```
tests/fixtures/standard_home_v1/
  Documents/
    budget-2026.xlsx                    # Spreadsheet with financial data
    meeting-notes-jan.md                # Markdown meeting notes
    project-proposal.pdf                # PDF with "proposal" and "timeline"
    resume-rex-2026.docx                # Word doc
    travel-itinerary.txt                # Plain text with dates and destinations
    quarterly-review-q4.pdf             # PDF with "quarterly" and "performance"
    research-paper-ml.pdf               # Academic paper on machine learning
    tax-return-2025.pdf                 # Financial document
    wedding-guest-list.csv              # CSV with names
    cover-letter.md                     # Job application text
  Desktop/
    screenshot-2026-02-01.png           # Image file (metadata only)
    todo-list.md                        # Short markdown checklist
    quick-notes.txt                     # Informal notes with "remember to..."
    design-mockup-v3.fig                # Figma file (metadata only)
  Developer/
    myapp/
      src/
        main.cpp                        # C++ with "int main()" and app initialization
        config_parser.cpp               # Parses JSON/YAML configuration files
        database_migration.py           # Python with "ALTER TABLE", "schema"
        auth_handler.ts                 # TypeScript with "JWT", "token", "authenticate"
        api_routes.go                   # Go with HTTP handlers, "endpoint", "router"
        utils.js                        # JavaScript utility functions
        logger.rs                       # Rust logging module
      tests/
        test_config.py                  # Python test for config parser
        test_auth.spec.ts               # TypeScript test for auth
      docs/
        architecture.md                 # System design document
        deployment-guide.md             # Step-by-step deploy instructions
      Makefile                          # Build commands
      README.md                         # Project readme with "getting started"
      .env.example                      # Environment variable template
      docker-compose.yml                # Docker services definition
    scripts/
      backup.sh                         # Bash script for file backup
      deploy.sh                         # Deployment automation
      sync_database.py                  # Python database sync utility
    tools/
      performance_profiler.py           # CPU/memory profiling tool
      log_analyzer.rb                   # Ruby script that parses log files
  Projects/
    blog/
      posts/
        intro-to-rust.md                # Blog post about Rust programming
        react-hooks-guide.md            # Blog post about React hooks
        machine-learning-basics.md      # Blog post about ML fundamentals
      drafts/
        unpublished-post.md             # Draft blog post
      config.yaml                       # Blog site configuration
    research/
      data-analysis.ipynb               # Jupyter notebook (plain text version)
      experiment-results.csv            # CSV with numerical results
      literature-review.md              # Academic literature summary
  Downloads/
    invoice-january-2026.pdf            # Financial invoice
    software-license-agreement.pdf      # Legal document
    presentation-slides.pptx            # PowerPoint file
    setup-installer.dmg                 # Installer (metadata only)
    random-article.html                 # Saved web page
    podcast-episode-42.mp3              # Audio file (metadata only)
  iCloud Drive/
    shared-project-notes.md             # Collaborative notes
    family-photos/
      vacation-2025.jpg                 # Image (metadata only)
  .config/
    nvim/
      init.lua                          # Neovim configuration
    git/
      config                            # Git global config
    alacritty/
      alacritty.yml                     # Terminal emulator config
  .zshrc                                # Shell configuration
  .gitconfig                            # Git user config
  .ssh/
    config                              # SSH config (sensitive, metadata only)
```

### 2.3 File Content Requirements

Every file in the fixture must contain realistic, deterministic content. Specific content constraints:

| File | Must contain (verbatim, for query matching) |
|------|---------------------------------------------|
| `budget-2026.xlsx` | Column headers: "Revenue", "Expenses", "Profit" |
| `meeting-notes-jan.md` | "Action items: follow up with marketing team" |
| `project-proposal.pdf` | "Executive summary", "timeline", "budget allocation" |
| `quarterly-review-q4.pdf` | "performance metrics", "quarterly targets", "revenue growth" |
| `config_parser.cpp` | `parseConfig()`, `loadSettings()`, `readJsonFile()` |
| `database_migration.py` | `ALTER TABLE`, `CREATE INDEX`, `migrate_schema()` |
| `auth_handler.ts` | `verifyJWT()`, `refreshToken()`, `authenticate()` |
| `api_routes.go` | `http.HandleFunc()`, `/api/v1/users`, `router.GET()` |
| `backup.sh` | `rsync`, `tar -czf`, `/backup/destination` |
| `architecture.md` | "microservices", "message queue", "load balancer" |
| `deployment-guide.md` | "docker build", "kubectl apply", "CI/CD pipeline" |
| `.zshrc` | `export PATH`, `alias`, `source ~/.nvm/nvm.sh` |
| `intro-to-rust.md` | "ownership", "borrowing", "lifetime", "cargo build" |
| `react-hooks-guide.md` | "useState", "useEffect", "custom hooks" |
| `machine-learning-basics.md` | "neural network", "gradient descent", "training data" |
| `literature-review.md` | "prior work", "methodology", "findings suggest" |

### 2.4 Fixture Generation

The fixture is hand-created, not procedurally generated. Each file is authored to contain specific keywords and concepts that map to test queries. This ensures exact reproducibility.

The fixture is committed to Git as-is (no tarball needed at 50MB). Binary files (PDF, XLSX, PPTX) are committed as actual files, not placeholders, because the extractors must be able to parse them.

A generation script (`tests/fixtures/generate_standard_home.sh`) is provided to re-create the fixture from templates if needed, but the committed fixture is the source of truth.

---

## 3. Test Corpus (50 Queries)

### 3.1 Format

```json
{
  "version": "1.0",
  "fixture_id": "standard_home_v1",
  "created": "2026-02-07",
  "queries": [
    {
      "id": "q001",
      "query": "budget",
      "category": "exact_filename",
      "expected_files": [
        "Documents/budget-2026.xlsx"
      ],
      "pass_rank": 3,
      "rationale": "Exact substring in filename. Should be top result.",
      "notes": ""
    }
  ]
}
```

Fields:
- `id`: Stable identifier (never reused, even if query is removed)
- `query`: Exact text typed into search panel
- `category`: One of `exact_filename`, `partial_filename`, `content_search`, `semantic`, `edge_case`
- `expected_files`: One or more acceptable correct answers (paths relative to fixture root)
- `pass_rank`: Maximum position (1-indexed) where any expected file must appear. Default 3.
- `rationale`: Why this query should find this file
- `notes`: Known ambiguities, expected failures, or commentary

### 3.2 Category: Exact Filename (10 queries)

These queries match the filename directly. FTS5 alone should handle them. Semantic search should not regress these.

| ID | Query | Expected File | Rationale |
|----|-------|---------------|-----------|
| q001 | `budget` | `Documents/budget-2026.xlsx` | Exact substring in filename |
| q002 | `resume` | `Documents/resume-rex-2026.docx` | Exact substring in filename |
| q003 | `todo` | `Desktop/todo-list.md` | Exact substring in filename |
| q004 | `backup.sh` | `Developer/scripts/backup.sh` | Exact filename |
| q005 | `Makefile` | `Developer/myapp/Makefile` | Exact filename (case-sensitive) |
| q006 | `docker-compose` | `Developer/myapp/docker-compose.yml` | Exact substring, hyphenated |
| q007 | `.zshrc` | `.zshrc` | Dotfile, exact name |
| q008 | `README` | `Developer/myapp/README.md` | Exact substring, common filename |
| q009 | `invoice` | `Downloads/invoice-january-2026.pdf` | Exact substring in filename |
| q010 | `alacritty` | `.config/alacritty/alacritty.yml` | Nested config, exact name match |

### 3.3 Category: Partial Filename (10 queries)

Short prefixes or substrings. Tests the prefix/contains match types.

| ID | Query | Expected File | Rationale |
|----|-------|---------------|-----------|
| q011 | `bud` | `Documents/budget-2026.xlsx` | Prefix of "budget" |
| q012 | `quar` | `Documents/quarterly-review-q4.pdf` | Prefix of "quarterly" |
| q013 | `deploy` | `Developer/scripts/deploy.sh` OR `Developer/myapp/docs/deployment-guide.md` | Prefix matches both |
| q014 | `auth` | `Developer/myapp/src/auth_handler.ts` | Prefix of "auth_handler" |
| q015 | `test_` | `Developer/myapp/tests/test_config.py` OR `Developer/myapp/tests/test_auth.spec.ts` | Prefix pattern common in test files |
| q016 | `log` | `Developer/tools/log_analyzer.rb` OR `Developer/myapp/src/logger.rs` | Prefix/substring in filename |
| q017 | `react` | `Projects/blog/posts/react-hooks-guide.md` | Prefix in filename |
| q018 | `init` | `.config/nvim/init.lua` | Exact stem "init" in filename |
| q019 | `sync` | `Developer/scripts/sync_database.py` | Prefix of "sync_database" |
| q020 | `perf` | `Developer/tools/performance_profiler.py` | Prefix of "performance" |

### 3.4 Category: Content Search (15 queries)

These queries match content inside files, not filenames. FTS5 should find them via indexed content chunks.

| ID | Query | Expected File | Rationale |
|----|-------|---------------|-----------|
| q021 | `ALTER TABLE` | `Developer/myapp/src/database_migration.py` | SQL keyword in file content |
| q022 | `JWT` | `Developer/myapp/src/auth_handler.ts` | Authentication token in content |
| q023 | `rsync` | `Developer/scripts/backup.sh` | Command in shell script |
| q024 | `gradient descent` | `Projects/blog/posts/machine-learning-basics.md` | ML concept in blog post |
| q025 | `kubectl apply` | `Developer/myapp/docs/deployment-guide.md` | Kubernetes command in guide |
| q026 | `useEffect` | `Projects/blog/posts/react-hooks-guide.md` | React hook in blog post |
| q027 | `export PATH` | `.zshrc` | Shell variable in config |
| q028 | `revenue growth` | `Documents/quarterly-review-q4.pdf` | Business term in PDF content |
| q029 | `ownership borrowing` | `Projects/blog/posts/intro-to-rust.md` | Rust concepts in blog post |
| q030 | `microservices` | `Developer/myapp/docs/architecture.md` | Architecture term in design doc |
| q031 | `follow up marketing` | `Documents/meeting-notes-jan.md` | Action item phrase in notes |
| q032 | `http.HandleFunc` | `Developer/myapp/src/api_routes.go` | Go HTTP handler in source code |
| q033 | `executive summary` | `Documents/project-proposal.pdf` | Formal phrase in PDF |
| q034 | `prior work methodology` | `Projects/research/literature-review.md` | Academic phrases in review doc |
| q035 | `cargo build` | `Projects/blog/posts/intro-to-rust.md` | Rust build command in blog |

### 3.5 Category: Semantic / Conceptual (10 queries)

These queries have NO exact keyword match in the expected file. They rely on semantic similarity to surface results. This is the category that justifies M2's existence.

| ID | Query | Expected File | Rationale |
|----|-------|---------------|-----------|
| q036 | `settings` | `Developer/myapp/src/config_parser.cpp` OR `.config/alacritty/alacritty.yml` | "settings" is semantically close to "config" |
| q037 | `credentials` | `Developer/myapp/.env.example` | ".env" files store credentials; semantic association |
| q038 | `server deployment` | `Developer/myapp/docs/deployment-guide.md` OR `Developer/scripts/deploy.sh` | "server deployment" ~ "deploy", "docker build" |
| q039 | `database schema` | `Developer/myapp/src/database_migration.py` | "schema" ~ "ALTER TABLE", "migrate_schema()" |
| q040 | `performance optimization` | `Developer/tools/performance_profiler.py` | "optimization" ~ "profiling" |
| q041 | `money expenses` | `Documents/budget-2026.xlsx` | "money expenses" ~ "Revenue", "Expenses", "Profit" |
| q042 | `deep learning` | `Projects/blog/posts/machine-learning-basics.md` | "deep learning" ~ "neural network", "gradient descent" |
| q043 | `API endpoints` | `Developer/myapp/src/api_routes.go` | "endpoints" ~ "router", "HandleFunc", "/api/v1" |
| q044 | `shell configuration` | `.zshrc` | "shell configuration" ~ shell config file, "alias", "export" |
| q045 | `version control` | `.gitconfig` OR `.config/git/config` | "version control" ~ git config |

### 3.6 Category: Edge Cases (5 queries)

Tricky inputs: unicode, special characters, very short queries, and ambiguous terms.

| ID | Query | Expected File | Rationale |
|----|-------|---------------|-----------|
| q046 | `r` | (any `.r` file or reasonable match) | Single-character query; tests graceful handling, not specific ranking |
| q047 | `meeting notes jan` | `Documents/meeting-notes-jan.md` | Multi-word with abbreviated month |
| q048 | `"project proposal"` | `Documents/project-proposal.pdf` | Quoted phrase (FTS5 phrase match) |
| q049 | `*.py` | (any Python file) | Glob-like pattern; may or may not be interpreted |
| q050 | `config -test` | `Developer/myapp/src/config_parser.cpp` (NOT test_config.py) | Negation intent; tests whether scoring suppresses test files |

Edge case scoring: q046 passes if any result is returned without error. q049 passes if any `.py` file appears in top 5. q050 passes if the non-test config file ranks above the test file.

---

## 4. Scoring Methodology

### 4.1 Pass/Fail per Query

```
For each query q in corpus:
  1. Execute search(q.query) against the indexed fixture
  2. Extract top q.pass_rank results (default: top 3)
  3. For each expected_file in q.expected_files:
     if any expected_file appears in the top results:
       q.result = PASS
       break
  4. If no expected_file found:
     q.result = FAIL
```

### 4.2 Aggregate Score

```
total_pass = count(q where q.result == PASS)
total_applicable = count(q where q.category != "edge_case" or q has strict expectation)
pass_rate = total_pass / 50

M2 threshold: pass_rate >= 0.80 (40/50)
```

Edge cases (q046-q050) still count toward the 50-query total. They have relaxed expectations but are not excluded from scoring.

### 4.3 Per-Category Breakdown

Report pass rates by category to identify weak areas:

```
exact_filename:    ?/10  (baseline: should be 10/10)
partial_filename:  ?/10  (baseline: should be 9/10+)
content_search:    ?/15  (baseline: should be 13/15+)
semantic:          ?/10  (this is where M2 proves value)
edge_case:         ?/5   (lenient expectations)
```

If `exact_filename` or `partial_filename` categories regress compared to FTS5-only, the semantic merge is introducing bugs and needs fixing before the semantic category is evaluated.

### 4.4 A/B Comparison

Every relevance test run produces TWO result sets:

1. **FTS5-only (M1 baseline):** Disable semantic search (`embedding_enabled = false`), run the same 50 queries.
2. **FTS5+semantic (M2 candidate):** Enable semantic search, run the same 50 queries.

Report:
```
                    FTS5-only    FTS5+semantic    Delta
exact_filename      10/10        10/10            0
partial_filename     9/10         9/10            0
content_search      12/15        13/15           +1
semantic             2/10         8/10           +6
edge_case            3/5          4/5            +1
TOTAL               36/50        44/50           +8
```

Pass criterion for M2: FTS5+semantic total >= 40/50 AND FTS5+semantic total > FTS5-only total.

If the second condition fails, semantic search is not providing value and should not ship.

---

## 5. Test Runner

### 5.1 Script

`tests/relevance/run_relevance_test.sh`

```
Usage:
  ./run_relevance_test.sh [--mode fts5|semantic|both] [--corpus path] [--fixture path]

Options:
  --mode fts5       Run FTS5-only baseline
  --mode semantic   Run FTS5+semantic
  --mode both       Run both and produce comparison (default)
  --corpus          Path to test_corpus.json (default: tests/relevance/test_corpus.json)
  --fixture         Path to fixture directory (default: tests/fixtures/standard_home_v1/)
  --output          Path to results CSV (default: tests/relevance/results.csv)
```

### 5.2 Execution Flow

```
1. Verify fixture directory exists and contains expected files
2. Create temporary BetterSpotlight database
3. Index the fixture directory (FTS5 + optional embeddings)
4. Wait for indexing to complete (poll IndexerService health)
5. For each query in corpus:
   a. Send search IPC to QueryService
   b. Capture top N results (paths + scores)
   c. Compare against expected_files
   d. Record PASS/FAIL + actual results
6. Generate results CSV
7. Generate summary report (stdout)
8. Exit with code 0 if pass_rate >= threshold, else exit 1
```

### 5.3 Output Format

**Results CSV:**

```csv
id,query,category,expected_file,actual_rank,actual_top3,result,fts5_score,semantic_score,merged_score
q001,budget,exact_filename,Documents/budget-2026.xlsx,1,"Documents/budget-2026.xlsx|Documents/tax-return-2025.pdf|Downloads/invoice-january-2026.pdf",PASS,187.5,0.0,187.5
q036,settings,semantic,"Developer/myapp/src/config_parser.cpp",2,"...",PASS,0.0,0.82,32.8
```

**Summary report (stdout):**

```
=== BetterSpotlight Relevance Test ===
Fixture:    standard_home_v1
Mode:       FTS5+semantic
Date:       2026-02-07T15:30:00Z

Results by category:
  exact_filename:    10/10 (100%)
  partial_filename:   9/10 ( 90%)
  content_search:    13/15 ( 87%)
  semantic:           8/10 ( 80%)
  edge_case:          4/5  ( 80%)

TOTAL: 44/50 (88%)  ✓ PASS (threshold: 80%)

Failures:
  q015: test_   expected: test_config.py   actual top 3: [test_auth.spec.ts, ...]
  q046: r       expected: (any)            actual top 3: [] (no results)
  ...

A/B comparison:
  FTS5-only:      36/50 (72%)
  FTS5+semantic:  44/50 (88%)
  Delta:          +8 queries improved
  Regressions:    0 queries worse
```

---

## 6. Regression Tracking

### 6.1 Baseline File

`tests/relevance/baselines.json`

```json
{
  "baselines": [
    {
      "date": "2026-02-10",
      "commit": "abc123f",
      "mode": "fts5_only",
      "total": 36,
      "by_category": {
        "exact_filename": 10,
        "partial_filename": 9,
        "content_search": 12,
        "semantic": 2,
        "edge_case": 3
      }
    },
    {
      "date": "2026-02-10",
      "commit": "abc123f",
      "mode": "fts5_semantic",
      "total": 44,
      "by_category": {
        "exact_filename": 10,
        "partial_filename": 9,
        "content_search": 13,
        "semantic": 8,
        "edge_case": 4
      }
    }
  ]
}
```

### 6.2 Regression Rules

CI compares each run against the latest baseline:

1. **Total regressions > 2:** CI fails. Something broke.
2. **Any exact_filename regression:** CI fails. Fundamental matching is broken.
3. **Semantic category improvement + other regressions:** CI warns. Merge weights may need rebalancing.
4. **Total improvement >= 3 with no regressions:** Auto-update baseline.

### 6.3 Baseline Updates

Baselines are updated:
- Automatically when CI detects improvement with no regressions
- Manually via PR when queries are added/modified or fixture changes
- On milestone sign-off

---

## 7. CI Integration

### 7.1 GitHub Actions Step

Add to `.github/workflows/ci.yml`:

```yaml
  relevance-test:
    name: Relevance Test (M2)
    runs-on: macos-14
    needs: [build]
    steps:
      - uses: actions/checkout@v4
      - name: Download build artifact
        uses: actions/download-artifact@v4
        with:
          name: betterspotlight-build
      - name: Index fixture
        run: |
          ./build/bin/betterspotlight-indexer \
            --root tests/fixtures/standard_home_v1/ \
            --db /tmp/test-index.db \
            --wait-for-complete
      - name: Run relevance test
        run: |
          ./tests/relevance/run_relevance_test.sh \
            --mode both \
            --output /tmp/relevance-results.csv
      - name: Upload results
        uses: actions/upload-artifact@v4
        with:
          name: relevance-results
          path: /tmp/relevance-results.csv
```

### 7.2 Gating

- M2 PRs that touch scoring, ranking, embedding, or merge logic MUST pass the relevance test
- M2 PRs that only touch UI, settings, or onboarding can skip (but shouldn't)
- The relevance test is not gating for M1 branches (FTS5-only expected score is below 80%)

---

## 8. Query Maintenance

### 8.1 Adding Queries

When adding a new query:
1. Assign the next unused ID (q051, q052, ...)
2. Ensure the expected file exists in the fixture with the right content
3. Run the test locally to verify PASS before committing
4. Update baselines.json via PR

### 8.2 Removing Queries

Never reuse a query ID. Mark removed queries as `"deprecated": true` in the corpus. The scorer skips deprecated queries.

### 8.3 Modifying the Fixture

Fixture changes are high-risk. They invalidate baselines and may flip multiple query results. Process:
1. Make the fixture change
2. Run the full relevance test locally
3. Review every query that changed result
4. Update baselines.json
5. Commit all three (fixture, results, baselines) in a single PR

---

## 9. Scaling to M3

M3 expands the corpus to 100 queries and tightens the threshold to 95%. The M2 infrastructure is designed to scale:

- Corpus format supports any number of queries
- CSV output handles arbitrary row counts
- A/B comparison works unchanged
- Baselines store per-milestone snapshots

New M3 categories (planned):
- 10 more exact filename queries
- 10 more partial filename queries
- 10 more content queries
- 10 more semantic queries
- 10 more edge cases (CJK, emoji, long queries, path-like queries)

The M3 fixture expands to `standard_home_v2` with additional directories and file types.

---

## 10. File Layout

```
tests/relevance/
  test_corpus.json              # The 50 queries
  run_relevance_test.sh         # Test runner script
  baselines.json                # Historical baseline results
  results.csv                   # Latest run output (gitignored)

tests/fixtures/
  standard_home_v1/             # The controlled fixture (committed)
    Documents/
    Desktop/
    Developer/
    Projects/
    Downloads/
    iCloud Drive/
    .config/
    .zshrc
    .gitconfig
  generate_standard_home.sh     # Fixture regeneration script
```
