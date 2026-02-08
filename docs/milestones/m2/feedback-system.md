# M2: Feedback & Interaction Tracking System

**Scope:** User interaction tracking, query-result feedback loops, frequency aggregation pipeline, and the M2 extensions to the existing frequency/recency boost system.

**References:** Extends [foundation/ranking-scoring.md](../../foundation/ranking-scoring.md) Sections 3-4 (frequency tiers, recency decay) and [foundation/storage-schema.md](../../foundation/storage-schema.md) (feedback/frequencies tables). The schema and tier math are already fully specified there. This document covers what's NEW in M2.

---

## 1. What M1 Already Has vs. What M2 Adds

**M1 (exists):**
- `feedback` table: records (item_id, action, query, result_position, timestamp)
- `frequencies` table: denormalized (item_id, openCount, lastOpened, isPinned)
- `record_feedback` IPC method on QueryService
- Tiered frequency boost (10/20/30 points)
- Exponential recency decay (7-day half-life)
- Pinned items (+200 points)
- 90-day retention policy on feedback

**M2 (new, specified in this document):**
- InteractionTracker: learns which paths are selected for which queries
- Path preference model: per-directory selection frequency
- File type affinity model: per-user preference for code vs. docs vs. media
- Feedback aggregation pipeline: periodic rollup from feedback to frequencies
- App-context interaction history: which files are opened from which apps
- Privacy controls for all tracking

---

## 2. InteractionTracker

### 2.1 Purpose

The InteractionTracker answers: "When this user searched for X in the past, what did they actually click on?" This allows the system to learn user intent patterns over time.

Example: if the user searches "config" and always clicks `~/.zshrc` instead of `config.json`, the tracker learns that "config" correlates with shell configuration for this user.

### 2.2 Data Model

```cpp
struct Interaction {
    QString query;           // The search query text
    int64_t selectedItemId;  // The item the user opened
    QString selectedPath;    // Denormalized for fast lookup
    QString matchType;       // How the result was matched (exactName, content, semantic, etc.)
    int resultPosition;      // Where in the result list (1-based)
    QString frontmostApp;    // Which app was active when search was invoked
    QDateTime timestamp;
};
```

### 2.3 Storage

New SQLite table (extends the existing schema):

```sql
CREATE TABLE IF NOT EXISTS interactions (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    query         TEXT NOT NULL,
    query_normalized TEXT NOT NULL,  -- lowercase, trimmed, single-spaced
    item_id       INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    path          TEXT NOT NULL,
    match_type    TEXT NOT NULL,
    result_position INTEGER NOT NULL,
    app_context   TEXT,
    timestamp     TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_interactions_query ON interactions(query_normalized);
CREATE INDEX idx_interactions_item ON interactions(item_id);
CREATE INDEX idx_interactions_timestamp ON interactions(timestamp);
```

Retention: 180 days (longer than feedback's 90 days, because interaction patterns need more history to be useful).

### 2.4 Query Normalization

Before storing or looking up queries:
1. Lowercase
2. Trim leading/trailing whitespace
3. Collapse multiple spaces to single space
4. Strip trailing wildcards (`*`)
5. Do NOT stem (preserving exact user intent)

This ensures "Config", "config", and " config " all map to the same normalized form.

### 2.5 Interaction-Based Boost

At search time, after scoring but before final ranking:

```
For each result in scored_results:
  interaction_count = SELECT COUNT(*) FROM interactions
                      WHERE query_normalized = ? AND item_id = ?
                      AND timestamp > datetime('now', '-90 days')

  if interaction_count > 0:
    interaction_boost = min(interaction_count * 5, 25)
    // 5 points per past interaction, capped at 25
    result.score += interaction_boost
```

This is intentionally conservative. 25 points maximum means interaction history can nudge results up by 1-2 positions, not dominate the ranking.

### 2.6 Performance

The interaction lookup adds a SQLite query per result. For 20 results, that's 20 queries. With the index on `(query_normalized, item_id)`, each query completes in < 0.1ms. Total overhead: < 2ms. Acceptable.

If performance becomes a concern at scale, pre-compute interaction counts into a cache table during the aggregation pipeline (Section 5).

---

## 3. Path Preference Model

### 3.1 Purpose

Tracks which directories the user frequently selects files from. If a user consistently opens files from `~/Projects/myapp/`, that directory gets a boost for future searches.

### 3.2 Implementation

Derived from the `interactions` table, not stored separately:

```sql
-- Top 50 directories by selection frequency (last 90 days)
SELECT
    substr(path, 1, instr(path, '/') - 1) AS parent_dir,  -- simplified; actual impl uses dirname()
    COUNT(*) AS selection_count
FROM interactions
WHERE timestamp > datetime('now', '-90 days')
GROUP BY parent_dir
ORDER BY selection_count DESC
LIMIT 50
```

At search time, check if each result's parent directory is in the top 50. If so:

```
path_preference_boost = min(selection_count / 5, 15)
// 1 point per 5 selections, capped at 15
```

### 3.3 Caching

The top-50 directory list changes slowly. Cache it in memory and refresh every 10 minutes (or on explicit invalidation when new interactions are recorded).

---

## 4. File Type Affinity Model

### 4.1 Purpose

Detects whether this user is primarily a developer (opens code), a writer (opens documents), or mixed. Adjusts default scoring accordingly.

### 4.2 Implementation

Derived from interaction history:

```sql
SELECT
    CASE
        WHEN path LIKE '%.py' OR path LIKE '%.js' OR path LIKE '%.ts'
             OR path LIKE '%.cpp' OR path LIKE '%.go' OR path LIKE '%.rs'
        THEN 'code'
        WHEN path LIKE '%.md' OR path LIKE '%.txt' OR path LIKE '%.pdf'
             OR path LIKE '%.docx'
        THEN 'document'
        WHEN path LIKE '%.png' OR path LIKE '%.jpg' OR path LIKE '%.svg'
        THEN 'media'
        ELSE 'other'
    END AS file_type,
    COUNT(*) AS opens
FROM interactions
WHERE timestamp > datetime('now', '-30 days')
GROUP BY file_type
ORDER BY opens DESC
```

If one category has > 60% of total opens, set that as the `primary_affinity`. Apply a small boost (+5 points) to results matching the primary affinity.

This is an extremely lightweight signal. It exists so that a developer who searches "readme" sees `.md` files before `.pdf` readme files, all else being equal.

---

## 5. Feedback Aggregation Pipeline

### 5.1 Purpose

The `feedback` table collects raw events. The `frequencies` table holds aggregated counts. The aggregation pipeline rolls up feedback into frequencies.

### 5.2 Trigger

Run aggregation:
- Every 60 minutes (timer-based)
- On app startup
- On explicit trigger from Index Health UI

### 5.3 Algorithm

```sql
-- For each item with feedback since last aggregation:
INSERT OR REPLACE INTO frequencies (item_id, open_count, last_opened, is_pinned)
SELECT
    item_id,
    COUNT(*) FILTER (WHERE action = 'open'),
    MAX(timestamp) FILTER (WHERE action = 'open'),
    COALESCE(
        (SELECT action = 'pin' FROM feedback f2
         WHERE f2.item_id = feedback.item_id
         AND f2.action IN ('pin', 'unpin')
         ORDER BY f2.timestamp DESC LIMIT 1),
        0
    )
FROM feedback
WHERE item_id IN (
    SELECT DISTINCT item_id FROM feedback
    WHERE timestamp > ?  -- last_aggregation_timestamp
)
GROUP BY item_id;

-- Update last_aggregation_timestamp
INSERT OR REPLACE INTO settings (key, value) VALUES ('last_feedback_aggregation', ?);
```

### 5.4 Cleanup

After aggregation, delete feedback older than 90 days:

```sql
DELETE FROM feedback WHERE timestamp < datetime('now', '-90 days');
DELETE FROM interactions WHERE timestamp < datetime('now', '-180 days');
```

---

## 6. Privacy Controls

All tracking features are governed by privacy settings:

| Setting | Default | Effect when OFF |
|---------|---------|-----------------|
| `enableFeedbackLogging` | true | No feedback recorded. Frequency boosts use stale data. |
| `enableInteractionTracking` | true | No interaction tracking. InteractionTracker boost disabled. |
| `enablePathPreferences` | true | Path preference boost disabled. |
| `enableFileTypeAffinity` | true | File type affinity boost disabled. |
| `feedbackRetentionDays` | 90 | Shorter retention = less data stored |
| `interactionRetentionDays` | 180 | Same |

When the user disables a tracking feature:
1. Stop collecting new data for that feature
2. Do NOT delete existing data (user may re-enable)
3. Disable the corresponding boost in scoring
4. If user explicitly requests data deletion (via Index Health UI), delete from the relevant tables

### 6.1 Data Export

For transparency, provide an IPC method to export all stored interaction data:

```json
{
  "method": "export_interaction_data",
  "params": { "format": "json" }
}
```

Returns all interactions and feedback as JSON. This supports GDPR-style data access requests if BetterSpotlight is ever used in regulated environments.

---

## 7. IPC Interface Extensions

New methods on QueryService:

```json
// Record interaction (extends existing record_feedback)
{
  "method": "record_interaction",
  "params": {
    "query": "config",
    "item_id": 12345,
    "path": "/Users/rex/.zshrc",
    "match_type": "content",
    "result_position": 3,
    "app_context": "com.apple.Terminal"
  }
}

// Get path preferences (for debug/settings UI)
{
  "method": "get_path_preferences",
  "params": { "limit": 50 }
}

// Get file type affinity
{
  "method": "get_file_type_affinity",
  "params": {}
}

// Trigger aggregation
{
  "method": "run_aggregation",
  "params": {}
}
```

---

## 8. File Layout

```
src/core/feedback/
  interaction_tracker.h       // Records and queries interactions
  interaction_tracker.cpp
  feedback_aggregator.h       // Periodic feedback → frequencies rollup
  feedback_aggregator.cpp
  path_preferences.h          // Directory frequency cache
  path_preferences.cpp
  type_affinity.h             // File type affinity model
  type_affinity.cpp
```
