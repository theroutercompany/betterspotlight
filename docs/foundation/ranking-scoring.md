# Ranking & Scoring Specification

**Document Status:** Implementation-Ready
**Last Updated:** February 2026
**Applies to:** BetterSpotlight M1 (deterministic), M2+ (ML-augmented)

## Overview

BetterSpotlight ranks search results using a **deterministic multi-signal scoring model** in M1, upgraded to **ML-augmented scoring** in M2. This document defines both approaches, with M2 additions marked explicitly.

Core principle: **Recency, frequency, and context matter as much as keyword relevance.**

---

## Fundamental Scoring Formula

```
finalScore = baseMatchScore
           + recencyBoost
           + frequencyBoost
           + contextBoost
           + pinnedBoost
           - junkPenalty
           [+ semanticBoost]  # M2 only
```

All scores are **deterministic** (no randomness). Ties are broken by `itemId` (ascending).

**Score Range:** 0–1000+ (unbounded)

---

## Match Types (7 categories, priority order)

Results are matched against the query using these types, evaluated in priority order:

| Priority | Type | Points | Condition | Example |
|----------|------|--------|-----------|---------|
| 1 | exactNameMatch | 200 | File name equals query exactly (case-insensitive) | Query: `report`, File: `Report.pdf` |
| 2 | prefixNameMatch | 150 | File name starts with query (case-insensitive) | Query: `quart`, File: `quarterly-2025.pdf` |
| 3 | containsNameMatch | 100 | File name contains query as substring | Query: `report`, File: `q4_report_final.pdf` |
| 4 | exactPathMatch | 90 | Full path exactly matches query | Query: `/Users/alice/Documents`, File: `/Users/alice/Documents` |
| 5 | prefixPathMatch | 80 | Path starts with query | Query: `/Users/alice/Documents`, File: `/Users/alice/Documents/Work/...` |
| 6 | contentMatch | Variable* | FTS5 BM25 raw score × contentMatchWeight | Query: `quarterly`, Match in PDF body text |
| 7 | fuzzyMatch | 30 | Edit distance ≤ 2 on file name | Query: `repot`, File: `report.pdf` |

*Content match scoring: FTS5 has built-in column weights (file_name=10.0, file_path=5.0, content=1.0) that affect BM25 ranking. The contentMatchWeight (default 1.0) is a separate post-processing multiplier applied to the raw BM25 score. Typical formula: contentScore = fts5_bm25_raw_score × contentMatchWeight, resulting in scores typically 50–70 points for typical queries.

### Match Type Selection Algorithm

```
For a file and query:

1. IF file name equals query (case-insensitive)
   → exactNameMatch (200 points)

2. ELSE IF file name starts with query (case-insensitive)
   → prefixNameMatch (150 points)

3. ELSE IF file name contains query (case-insensitive)
   → containsNameMatch (100 points)

4. ELSE IF full path equals query
   → exactPathMatch (90 points)

5. ELSE IF full path starts with query
   → prefixPathMatch (80 points)

6. ELSE IF FTS5 search found this file
   → contentMatch (BM25 score × contentMatchWeight)
   → ALSO check fuzzyMatch on name in parallel

7. ELSE IF edit distance ≤ 2
   → fuzzyMatch (30 points)

8. ELSE
   → No match (excluded from results)

Special case: Multi-word queries
- Split on whitespace
- Try AND match first (all terms present)
- Fall back to OR match (any term present)
- Each term matched independently, then combined with max score
```

---

## Recency Boost

**Intent:** Recent files are more likely to be relevant.

**Formula:**
```
recencyBoost = recencyWeight × exp(−timeSinceModification / decayConstant)
```

Where:
- `recencyWeight` = configurable (default 30 points)
- `timeSinceModification` = seconds since file was last modified
- `decayConstant` = seconds (default 7 days = 604800 seconds)

**Behavior:**
- File modified today: ~30 points (full boost)
- File modified 1 day ago: ~15 points
- File modified 7 days ago: ~11 points (36% of max)
- File modified 30 days ago: ~1 point

**Calculation Example:**
```
File modified 3 days ago:
  timeSince = 3 days × 86400 sec/day = 259200 seconds
  decayConstant = 7 days × 86400 = 604800 seconds

  boost = 30 × exp(−259200 / 604800)
        = 30 × exp(−0.429)
        = 30 × 0.651
        = 19.5 points
```

---

## Frequency Boost

**Intent:** Files you open often are more likely to be relevant to future searches.

**Data Source:** `frequencies` table

**Formula:** Tiered lookup based on `open_count`:

| Open Count Range | Tier | Boost Points |
|------------------|------|--------------|
| 0 | Unfrequent | 0 |
| 1–5 | Tier 1 | 10 |
| 6–20 | Tier 2 | 20 |
| 21+ | Tier 3 | 30 |

**Recency modifier:** Within each tier, recent opens boost more:
```
tierBoost = baseTierBoost × (0.5 + 0.5 × exp(−daysSinceLastOpen / 30))
```

If last open was recent (< 3 days), apply full tier boost. If > 30 days ago, apply ~50% of tier boost.

**Example:**
```
File with open_count = 8, last opened 1 day ago:
  Tier 2: base = 20
  modifier = 0.5 + 0.5 × exp(−1/30) = 0.5 + 0.5 × 0.967 = 0.984
  boost = 20 × 0.984 = 19.7 points
```

---

## Context Boost

Context signals are **optional**. If not provided (e.g., headless search), skip context boost entirely.

### CWD Proximity Boost (M1)

**Intent:** Files in or near your current working directory are more relevant.

**Formula:**
```
IF file is in cwdPath OR within 2 directory levels of cwdPath:
  cwdBoost = cwdBoostWeight  (default 25 points)
ELSE:
  cwdBoost = 0
```

**Examples:**
```
cwdPath = /Users/alice/Documents

- File: /Users/alice/Documents/Report.pdf
  → In cwdPath → 25 points

- File: /Users/alice/Documents/Work/Q4/Report.pdf
  → 2 levels deep → 25 points

- File: /Users/alice/Documents/Work/Q4/Archive/Report.pdf
  → 3 levels deep → 0 points

- File: /Users/alice/Desktop/Report.pdf
  → Different directory → 0 points
```

### App Context Boost (M2 only)

**Intent:** Boost files relevant to the frontmost application.

**Data Source:** `frontmostAppBundleId` from QueryContext

**App Categories:** 40+ bundle IDs mapped to categories:

| Category | Examples | Boosted File Types |
|----------|----------|-------------------|
| IDE/Editor | `com.microsoft.VSCode`, `com.jetbrains.pycharm`, `com.apple.dt.Xcode`, `vim` | `.js`, `.py`, `.cpp`, `.h`, `.swift`, `.config`, `.json`, `.yaml` |
| Terminal | `com.apple.Terminal`, `org.iterm2.iTerm2` | Executables, `.sh`, `.bash`, `.zsh`, `.cfg` |
| Document Viewer | `com.apple.Preview`, `com.microsoft.Word` | `.pdf`, `.docx`, `.doc`, `.pages`, `.txt`, `.md` |
| Design Tools | `com.figma.desktop`, `com.bohemiancoding.sketch3` | `.png`, `.svg`, `.jpg`, `.gif`, `.sketch`, `.fig` |
| Browser | `com.google.Chrome`, `org.mozilla.firefox`, `com.apple.Safari` | (No specific boost) |
| Media Player | `com.apple.QuickTimePlayer`, `org.videolan.vlc` | `.mp4`, `.mov`, `.mkv`, `.m4a` |

**Boost Amount:** 15 points per matching category (configurable, `appContextBoostWeight`)

**Example:**
```
frontmostApp = com.microsoft.VSCode (IDE)
File = Quarterly_Analysis.py

Boost = 15 points (code file matching IDE context)

---

frontmostApp = com.apple.Terminal (Terminal)
File = sync.sh

Boost = 15 points (executable matching Terminal context)
```

**Fallback:** If VectorIndex unavailable (M2 network latency), skip app boost, continue with deterministic scoring.

---

## Pinned Boost

**Intent:** User-pinned files always rank high.

**Data Source:** `is_pinned` column in `items` table

**Formula:**
```
IF is_pinned = 1:
  pinnedBoost = pinnedBoostWeight  (default 200 points)
ELSE:
  pinnedBoost = 0
```

**Behavior:** Pinned files typically appear in top 3 results unless query is very poor match.

---

## Junk Penalty

**Intent:** Suppress noisy directories (build artifacts, caches, dependencies) that clutter results.

**Junk Patterns:** Directories matching these patterns incur penalty:

| Pattern | Example Paths | Rationale |
|---------|-----------------|-----------|
| `node_modules` | `/project/node_modules/lib.js` | NPM dependencies |
| `.build` | `/project/.build/cache.dat` | Compiled artifacts |
| `__pycache__` | `/project/__pycache__/module.pyc` | Python bytecode |
| `.cache` | `/home/.cache/app.dat` | User cache |
| `DerivedData` | `/Users/alice/Library/Developer/Xcode/DerivedData/...` | Xcode build cache |
| `.Trash` | `/Users/alice/.Trash/old.txt` | Deleted files |
| `vendor/bundle` | `/rails-app/vendor/bundle/...` | Ruby dependencies |
| `.git` | `/.git/objects/...` | Version control internals |

**Formula:**
```
IF file path contains any junk pattern:
  junkPenalty = junkPenaltyWeight  (default 50 points)
ELSE:
  junkPenalty = 0
```

**Configuration:** Junk patterns and penalty weight are user-configurable. Can be disabled entirely (set weight to 0).

**Example:**
```
Query: "config"
Match 1: /Users/alice/Documents/app.config
  baseMatch: 150 (prefixNameMatch)
  junkPenalty: 0
  finalScore: 150

Match 2: /Users/alice/node_modules/.cache/babel-config.js
  baseMatch: 100 (containsNameMatch)
  junkPenalty: 50
  finalScore: 50  (heavily suppressed)
```

---

## Semantic Similarity Boost (M2 only)

**Intent:** Match queries by semantic meaning, not just keywords.

**Data Source:** VectorIndex embeddings (pre-computed during indexing)

**Query Processing:**
```
1. Embed the query string using the same embedding model as indexed files
2. Compute cosine similarity between query embedding and each result embedding
3. Apply semantic boost only if similarity > 0.7 (configurable threshold)
```

**Formula:**
```
IF cosineSimilarity > similarityThreshold (0.7):
  semanticBoost = semanticWeight × linearScale(cosineSimilarity)
  where linearScale = (similarity − threshold) / (1 − threshold)
ELSE:
  semanticBoost = 0
```

**Examples:**
```
semanticWeight = 40 points

Query: "performance analysis"
File: "metrics_report.pdf"
Embedding similarity: 0.82

  scale = (0.82 − 0.7) / (1 − 0.7) = 0.12 / 0.3 = 0.4
  boost = 40 × 0.4 = 16 points

---

Query: "performance analysis"
File: "cat_photos.jpg"
Embedding similarity: 0.45 (< 0.7 threshold)

  boost = 0 (below threshold)
```

**Fallback (Deterministic):** If VectorIndex is unavailable, missing, or embedding fails:
- **Skip semantic boost entirely** (don't fall back to heuristics)
- **Use deterministic match score only**
- **No error; transparent fallback**

This ensures BetterSpotlight works reliably without ML components.

---

## Complete Scoring Examples

### Example 1: PDF Report

```
Query:        "quarterly report"
File:         /Users/alice/Documents/2025-Q4-Report.pdf
Query Context: cwdPath = /Users/alice/Documents

Match Type Analysis:
  - Exact name match? No (query is phrase, file is hyphenated)
  - Prefix name match? Yes ("2025" starts with... no)
  - Contains name match? Yes ("report" in "Q4-Report")

  baseMatchScore = 100 (containsNameMatch)

Recency Boost:
  modTime = 2025-12-20T10:15:00Z
  now     = 2025-12-22T14:30:00Z
  timeSince = 2 days = 172800 seconds

  boost = 30 × exp(−172800 / 604800)
        = 30 × exp(−0.286)
        = 30 × 0.751
        = 22.5 points

Frequency Boost:
  open_count = 7 → Tier 2 (base 20)
  lastOpenDate = 2025-12-22T09:30:00Z (today!)

  modifier = 0.5 + 0.5 × exp(−0/30) = 1.0
  boost = 20 × 1.0 = 20 points

Context Boost (CWD):
  file is in cwdPath
  boost = 25 points

Pinned:
  is_pinned = false
  boost = 0

Junk Penalty:
  No junk patterns matched
  penalty = 0

Semantic Boost (M2):
  similarity = 0.85 > 0.7
  scale = (0.85 − 0.7) / 0.3 = 0.5
  boost = 40 × 0.5 = 20 points

---

finalScore = 100 + 22.5 + 20 + 25 + 0 − 0 + 20 = 187.5 points
```

### Example 2: Old Cached File

```
Query:        "config"
File:         /Users/alice/node_modules/.cache/app-config.txt
Query Context: cwdPath = /Users/alice/project

Match Type Analysis:
  - Exact name match? No
  - Prefix name match? No ("app" vs "config")
  - Contains name match? Yes ("config" in filename)

  baseMatchScore = 100

Recency Boost:
  modTime = 2025-08-15 (4+ months ago)
  timeSince = 130 days ≈ 11.23M seconds

  boost = 30 × exp(−11.23M / 604800)
        = 30 × exp(−18.56)
        ≈ 0 (nearly zero)

Frequency Boost:
  open_count = 0 (never opened, in cache)
  boost = 0

Context Boost (CWD):
  file is NOT in cwdPath
  boost = 0

Pinned:
  is_pinned = false
  boost = 0

Junk Penalty:
  Contains "node_modules" pattern
  penalty = 50

Semantic Boost:
  File is likely irrelevant anyway
  similarity = 0.4 < 0.7
  boost = 0

---

finalScore = 100 + ~0 + 0 + 0 + 0 − 50 + 0 = ~50 points
(Heavily suppressed due to junk pattern)
```

### Example 3: Pinned File

```
Query:        "notes"
File:         /Users/alice/.pinned/daily_standup.md
Query Context: cwdPath = /Users/alice/project

Match Type Analysis:
  - Exact name match? No
  - Prefix name match? No
  - Contains name match? No ("notes" not in "daily_standup.md")
  - Fuzzy match? Yes (edit distance = 2 or less? No, too different)

  baseMatchScore = 0 (no match by keyword)

Pinned:
  is_pinned = true
  boost = 200 points

---

finalScore = 0 + 0 + 0 + 0 + 200 − 0 = 200 points
(Appears in top results despite no keyword match)
```

---

## Weight Configuration

All 16 configurable weights are stored in the `settings` table in SQLite. Users can adjust via Settings UI. Changes take effect immediately for new searches (in-flight searches unaffected).

### Match Type Weights

```sql
INSERT INTO settings (key, value, type) VALUES
  ('exactNameWeight', '200', 'int'),
  ('prefixNameWeight', '150', 'int'),
  ('containsNameWeight', '100', 'int'),
  ('exactPathWeight', '90', 'int'),
  ('prefixPathWeight', '80', 'int'),
  ('contentMatchWeight', '1.0', 'float'),  -- multiplies FTS5 BM25
  ('fuzzyMatchWeight', '30', 'int');
```

### Boost Weights

```sql
INSERT INTO settings (key, value, type) VALUES
  ('recencyWeight', '30', 'int'),
  ('recencyDecayDays', '7', 'int'),
  ('frequencyTier1Boost', '10', 'int'),
  ('frequencyTier2Boost', '20', 'int'),
  ('frequencyTier3Boost', '30', 'int'),
  ('cwdBoostWeight', '25', 'int'),
  ('appContextBoostWeight', '15', 'int'),      -- M2 only
  ('semanticWeight', '40', 'int'),             -- M2 only
  ('semanticSimilarityThreshold', '0.7', 'float'),  -- M2 only
  ('pinnedBoostWeight', '200', 'int'),
  ('junkPenaltyWeight', '50', 'int');
```

**UI for Weights:**
- Settings panel with sliders for each weight (0–100 or 0–300)
- Real-time preview: search query shows before/after scores with new weights
- "Reset to Defaults" button

---

## Query Processing Pipeline

### Step 1: Parse Query

```
Input: "quarterly report"

Steps:
  1. Detect quoted substrings: none
  2. Detect prefix indicators (e.g., "path:..."): none
  3. Split on whitespace: ["quarterly", "report"]
  4. Lower-case: ["quarterly", "report"]
  5. Remove stop words (optional): ["quarterly", "report"]

Output:
  {
    "type": "multi_term",
    "terms": ["quarterly", "report"],
    "isPhrase": false
  }
```

### Step 2: Build FTS5 Query

```
For multi-term query like "quarterly report":

Try first:
  FTS5 expression: quarterly AND report
  (all terms must match)

If no results, fall back to:
  FTS5 expression: quarterly OR report
  (any term matches)

For phrase query like "quarterly report" (quoted):
  FTS5 expression: "quarterly report"
  (exact phrase match)

For prefix query like "q*":
  FTS5 expression: q*
  (matches q, quarterly, quiet, etc.)
```

### Step 3: Execute FTS5 Search

```
Query FTS5:
  SELECT itemId, rank FROM search_index
  WHERE search_index MATCH 'quarterly OR report'
  ORDER BY rank

Returns results with BM25 scores
```

### Step 4: Compute Full Score

```
For each result from FTS5:
  1. Determine match type (name, path, content, fuzzy)
  2. Fetch recency, frequency, pinned status from items table
  3. Apply recency boost formula
  4. Apply frequency boost (tier lookup)
  5. Apply context boost (CWD + app) if available
  6. Apply semantic boost (M2 only) if similarity > threshold
  7. Apply junk penalty if applicable
  8. Sum: baseScore + boosts − penalties
  9. Clamp to [0, ∞) (no negative scores)

Result: itemId → finalScore mapping
```

### Step 5: Sort & Slice

```
1. Sort results by finalScore descending
2. Break ties by itemId ascending
3. Return top N results (default 20, configurable 1–100)
4. Include metadata for each result:
   - Snippet with highlights
   - Match type
   - Score breakdown (for debugging)
```

### Step 6: Format Response

Each result includes:

```json
{
  "itemId": 4521,
  "path": "/Users/alice/Documents/2025-Q4-Report.pdf",
  "name": "2025-Q4-Report.pdf",
  "kind": "pdf",
  "matchType": "containsNameMatch",
  "score": 187.5,
  "snippet": "...Quarterly report on key metrics and performance...",
  "highlights": [
    {"offset": 0, "length": 9},   // "Quarterly"
    {"offset": 48, "length": 6}   // "report"
  ],
  "metadata": {
    "fileSize": 2097152,
    "modificationDate": "2025-12-20T10:15:00Z"
  },
  "isPinned": false,
  "frequency": {
    "openCount": 7,
    "lastOpenDate": "2025-12-22T09:30:00Z"
  },
  "scoreBreakdown": {
    "baseMatchScore": 100,
    "recencyBoost": 22.5,
    "frequencyBoost": 20,
    "contextBoost": 25,
    "pinnedBoost": 0,
    "junkPenalty": 0,
    "semanticBoost": 20
  }
}
```

---

## Feedback Loop

### Recording Feedback

When user **opens a result**, QueryService records:

```
recordFeedback(itemId, "opened", query, position)
```

This triggers:

1. **Increment open_count** in `frequencies` table
2. **Update last_indexed** timestamp
3. **Insert into feedback** table with full context:
   ```
   {
     itemId: 4521,
     action: "opened",
     query: "quarterly report",
     position: 0,      // rank in result list
     timestamp: 1702777800,
     queryDuration: 45, // ms
     appContext: "com.microsoft.VSCode"
   }
   ```

### Using Feedback (M2+)

- Feedback table informs ranking model retraining
- Identifies **false positives** (files opened despite poor ranking)
- Identifies **false negatives** (files not returned despite opening unrelated result)
- Over time, model adapts to user's search patterns

### Fallback (Deterministic)

If feedback data is unavailable, frequency boost uses raw `open_count`. M1 does not use feedback for learning.

---

## Performance Considerations

### Optimization Strategy

1. **Cache BM25 scores:** FTS5 computes once per search, reused for all results
2. **Lazy metadata fetch:** Only fetch frecency/pinned status for top 100 results
3. **Index query in parallel:** Score computation parallelizable per result
4. **Vector similarity batching (M2):** Embed query once, batch compute cosine similarity

### Complexity

- **FTS5 search:** O(log n) via index
- **Scoring loop:** O(k) where k = result count (typically 20–100)
- **Sorting:** O(k log k) where k << total index size
- **Total:** Sub-100ms for typical queries on 100k+ files

---

## Tie-Breaking

When two files have identical `finalScore`:

```
Sort by: (finalScore DESC, itemId ASC)
```

This ensures:
1. Consistent, deterministic ordering
2. Newer items (higher itemId = indexed later) appear later for exact ties
3. No random jitter

---

## Debugging & Transparency

### Score Breakdown API

QueryService includes `scoreBreakdown` in search results for transparency:

```json
"scoreBreakdown": {
  "baseMatchScore": 100,
  "recencyBoost": 22.5,
  "frequencyBoost": 20,
  "contextBoost": 25,
  "pinnedBoost": 0,
  "junkPenalty": 0,
  "semanticBoost": 20
}
```

### Debug Logs

Scoring logic can emit structured logs:

```
[SCORE] itemId=4521 path="/Users/alice/Documents/2025-Q4-Report.pdf"
  match=containsNameMatch(100)
  recency=22.5 (modTime=2d ago)
  frequency=20 (tier2, lastOpen=today)
  context=25 (cwdBoost)
  pinned=0
  junk=0
  semantic=20 (sim=0.85)
  final=187.5
```

---

## Configurable Scenarios

### Conservative Ranking (Less Boost)

For users who want keyword matches to dominate:

```
Set all boost weights to 0:
  recencyWeight: 0
  frequencyTier1/2/3: 0
  cwdBoostWeight: 0
  appContextBoostWeight: 0
  semanticWeight: 0

Keep match type weights, junk penalty:
  exactNameWeight: 200
  prefixNameWeight: 150
  containsNameWeight: 100
  junkPenaltyWeight: 50
```

### Aggressive Context Ranking (Maximum Boost)

For power users who want maximum personalization:

```
Increase all boost weights:
  recencyWeight: 50
  frequencyTier1/2/3: 20, 40, 60
  cwdBoostWeight: 50
  appContextBoostWeight: 30
  semanticWeight: 80
  semanticSimilarityThreshold: 0.6  # Lower threshold, catch more semantic matches
```

---

## Algorithm Evolution (M2+)

### M1: Deterministic Baseline

- Fixed, hand-tuned weights
- No learning, no personalization beyond frequency/recency
- Reliable, predictable, easily debuggable

### M2: ML Augmentation

- Deterministic baseline (always available)
- Add semantic similarity (embeddings)
- Add app context detection
- Feedback table seeds future learning

### Future: Learned Ranking (M3+)

- Train ranking model on feedback data
- Learn per-user weight adjustments
- Online learning: retrain weekly
- Fallback to M1 if model unavailable

---

## Testing & Validation

### Unit Tests

- **Match type detection:** Verify each query/file pair maps to correct match type
- **Scoring formula:** Manual calculation vs. implementation
- **Boost formulas:** Exponential decay, tier lookup, modifiers
- **Weight application:** Load from settings, apply correctly

### Integration Tests

- **End-to-end:** Index files, search, verify results and scores
- **Sensitivity:** Adjust weights, verify expected ranking changes
- **Feedback loop:** Open file, verify frequency boost increases
- **Fallbacks:** Disable VectorIndex (M2), verify deterministic path works

### Regression Tests

- **Score stability:** Same query on unchanged index should return same scores
- **Tie-breaking:** Identical scores ordered by itemId
- **No score inversions:** If A ranks higher than B, should remain higher after weight change (monotonicity)

---

## Appendix: Settings Table Schema

```sql
CREATE TABLE settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL,
  type TEXT NOT NULL,  -- 'int', 'float', 'bool', 'string'
  defaultValue TEXT,
  category TEXT,       -- 'matching', 'boost', 'context', 'ml'
  description TEXT,
  minValue REAL,
  maxValue REAL,
  createdAt TIMESTAMP,
  updatedAt TIMESTAMP
);

-- Sample rows:
INSERT INTO settings VALUES
  ('exactNameWeight', '200', 'int', '200', 'matching', 'Points for exact file name match', 0, 500, NOW(), NOW()),
  ('recencyWeight', '30', 'int', '30', 'boost', 'Max recency boost (exponential decay over time)', 0, 100, NOW(), NOW()),
  ('contentMatchWeight', '1.0', 'float', '1.0', 'matching', 'Multiplier for FTS5 BM25 score', 0.0, 10.0, NOW(), NOW()),
  ('semanticWeight', '40', 'int', '40', 'ml', 'M2+ semantic similarity boost', 0, 100, NOW(), NOW());
```

---

**End of Document**
