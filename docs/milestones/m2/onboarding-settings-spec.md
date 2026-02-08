# M2: Onboarding Flow & Settings UI Specification

**Scope:** First-run experience (3-step onboarding), complete Settings UI (5 tabs with M2 additions), and Index Health dashboard.

**References:** [foundation/architecture-overview.md](../../foundation/architecture-overview.md) Sections 1.2-1.4 (UI overview), [milestones/acceptance-criteria.md](../acceptance-criteria.md) M2 Section (onboarding/settings criteria), [operations/swift-deprecation-audit.md](../../operations/swift-deprecation-audit.md) (OnboardingView.swift, SettingsView.swift, IndexHealthView.swift references).

---

## 1. Onboarding Flow

### 1.1 Trigger

Onboarding launches on first app start (when `onboarding_completed` is not set in settings table). It runs BEFORE any indexing begins.

If the user quits during onboarding, the incomplete state is preserved. On next launch, onboarding resumes from where the user left off.

### 1.2 Step 1: Welcome

**Purpose:** Explain what BetterSpotlight does and what permissions it needs.

```
┌──────────────────────────────────────────────┐
│                                              │
│        [App Icon]                            │
│                                              │
│   Welcome to BetterSpotlight                 │
│                                              │
│   Fast, private file search for your Mac.    │
│   Everything stays on your machine.          │
│                                              │
│   BetterSpotlight needs Full Disk Access     │
│   to index your files. We'll set that up     │
│   in the next step.                          │
│                                              │
│                          [Get Started →]     │
│                                              │
└──────────────────────────────────────────────┘
```

No configuration. Single "Get Started" button. Back button disabled (first step).

### 1.3 Step 2: Full Disk Access

**Purpose:** Guide the user through granting FDA permission.

```
┌──────────────────────────────────────────────┐
│                                              │
│   Grant Full Disk Access                     │
│                                              │
│   BetterSpotlight needs access to your       │
│   files to build a search index.             │
│                                              │
│   1. Click "Open System Settings" below      │
│   2. Find BetterSpotlight in the list        │
│   3. Toggle it ON                            │
│   4. Come back here and click "Verify"       │
│                                              │
│   [Open System Settings]  [Verify Access ✓]  │
│                                              │
│   Status: ⚠ Not yet granted                  │
│                                              │
│   [← Back]  [Skip for now]  [Continue →]     │
│                                              │
└──────────────────────────────────────────────┘
```

Behavior:
- "Open System Settings" executes: `open "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"`
- "Verify Access" calls `FdaCheck::hasFullDiskAccess()` and updates status
- Status shows: "Not yet granted" (yellow) or "Access granted" (green)
- "Skip for now" is available but shows warning: "Without Full Disk Access, many files won't appear in search results."
- "Continue" is enabled regardless of FDA status (user can grant later)

### 1.4 Step 3: Home Map

**Purpose:** Let the user choose which directories to index (and in M2, which to embed).

```
┌──────────────────────────────────────────────────────┐
│                                                      │
│   Choose what to index                               │
│                                                      │
│   We found these folders in your home directory.     │
│   Choose how each should be handled:                 │
│                                                      │
│   ┌─────────────────────────────────────────────┐    │
│   │ 📁 Documents        [Index + Embed ▾]       │    │
│   │ 📁 Desktop          [Index + Embed ▾]       │    │
│   │ 📁 Downloads        [Index Only ▾]          │    │
│   │ 📁 Developer        [Index + Embed ▾]       │    │
│   │ 📁 Projects         [Index + Embed ▾]       │    │
│   │ ☁️ iCloud Drive      [Index Only ▾]          │    │
│   │ ☁️ Dropbox           [Index Only ▾]          │    │
│   │ 🔒 .ssh             [Skip ▾]                │    │
│   │ 🔒 .gnupg           [Skip ▾]                │    │
│   │ 📁 Library          [Skip ▾]                │    │
│   │ 📁 .config          [Index Only ▾]          │    │
│   └─────────────────────────────────────────────┘    │
│                                                      │
│   Dropdown options:                                  │
│     • Index + Embed  (full search + semantic)        │
│     • Index Only     (keyword search only)           │
│     • Skip           (not indexed)                   │
│                                                      │
│   [← Back]                    [Start Indexing →]     │
│                                                      │
└──────────────────────────────────────────────────────┘
```

Behavior:
- Enumerate `~/` top-level directories via `QDir::entryList()`
- Apply `suggestedClassification()` to pre-select defaults:
  - Known dev directories (Developer, Projects, Code) → Index + Embed
  - Known document directories (Documents, Desktop) → Index + Embed
  - Cloud folders (detected via `isCloudFolder()`) → Index Only (semantic on cloud files is wasteful due to sync churn)
  - Sensitive directories (.ssh, .gnupg, .aws) → Skip
  - System directories (Library, .Trash) → Skip
  - Everything else → Index Only
- User can override any suggestion
- Dropdown is a `ComboBox` in QML
- Selections are saved to settings as `indexRoots` array with per-root config

### 1.5 Completion

After "Start Indexing":
1. Save all settings (index roots, FDA status, onboarding_completed = true)
2. Start IndexerService and ExtractorService
3. Show search panel with a subtle "Indexing in progress..." status indicator
4. User can start searching immediately (partial results from metadata scan)

---

## 2. Settings UI

Accessible via: status bar icon menu → "Settings..." OR keyboard shortcut (configurable, default Cmd+,).

### 2.1 General Tab

| Control | Type | Setting Key | Default |
|---------|------|-------------|---------|
| Global hotkey | Key recorder | `hotkey` | Cmd+Space |
| Launch at login | Toggle | `launchAtLogin` | false |
| Show in Dock | Toggle | `showInDock` | false |
| Check for updates | Toggle | `checkForUpdates` | true |
| Max results | Slider (5-50) | `maxResults` | 20 |

**Hotkey recorder:** Custom QML component that captures key press and displays it as text (e.g., "Cmd+Space"). Must call `isHotkeyAvailable()` and show warning if conflict detected.

### 2.2 Indexing Tab

| Control | Type | Setting Key | Default |
|---------|------|-------------|---------|
| Index roots | List with add/remove | `indexRoots` | From onboarding |
| Per-root mode | Dropdown per root | `indexRoots[].mode` | From onboarding |
| Enable PDF extraction | Toggle | `enablePdf` | true |
| Enable OCR | Toggle | `enableOcr` | false |
| OCR languages | Multi-select | `ocrLanguages` | ["eng"] |
| Enable semantic search | Toggle | `embeddingEnabled` | true |
| Embedding model | Read-only label | - | BGE-small-en-v1.5 |
| Max file size | Slider (1-500 MB) | `maxFileSizeMB` | 50 |
| Pause indexing | Button | - | Sends IPC pause |

**Per-root mode dropdown** mirrors onboarding: Index + Embed, Index Only, Skip.

**Add root:** Opens native folder picker (`QFileDialog::getExistingDirectory`). New root starts indexing immediately.

**Remove root:** Removes from settings, deletes index entries for that root, removes embeddings.

### 2.3 Exclusions Tab

| Control | Type | Setting Key |
|---------|------|-------------|
| Pattern list | Editable text list | `excludePatterns` |
| .bsignore file path | Read-only label + "Edit" button | `~/.bsignore` |
| Pattern syntax help | Collapsible help text | - |

**Pattern list** shows all active exclusion patterns (default + .bsignore + user-added). User-added patterns are marked with a delete button. Default patterns are grayed out (not deletable).

**"Edit .bsignore"** button opens `~/.bsignore` in the default text editor (`open -e ~/.bsignore`).

**Syntax help** shows: `*` matches anything, `**` matches directories recursively, `!` negates, `#` comments.

**Live validation:** As user types a pattern, show a count of how many currently indexed files would match. Helps prevent overly broad patterns.

### 2.4 Privacy Tab

| Control | Type | Setting Key | Default |
|---------|------|-------------|---------|
| Enable feedback logging | Toggle | `enableFeedbackLogging` | true |
| Enable interaction tracking | Toggle | `enableInteractionTracking` | true |
| Feedback retention | Dropdown (30/60/90/180 days) | `feedbackRetentionDays` | 90 |
| Sensitive paths | Editable list | `sensitivePaths` | .ssh, .gnupg, .aws, Library/Keychains |
| Clear all feedback data | Button | - | Deletes feedback + interactions |
| Export my data | Button | - | JSON export to Downloads |

**Clear all feedback data:** Confirmation dialog required. Irreversible. Resets frequency boosts to zero.

**Export my data:** Calls `export_interaction_data` IPC method, saves JSON to `~/Downloads/betterspotlight-data-export.json`.

### 2.5 Index Health Tab

```
┌──────────────────────────────────────────────────┐
│  Index Health                          [Refresh] │
│                                                  │
│  Status: ● Healthy                               │
│                                                  │
│  ┌─ Statistics ──────────────────────────────┐   │
│  │ Indexed files:     487,231                │   │
│  │ Content chunks:    1,423,891              │   │
│  │ Embedded vectors:  487,231 (100%)         │   │
│  │ Database size:     1.8 GB                 │   │
│  │ Vector index:      198 MB                 │   │
│  │ Last scan:         2 minutes ago          │   │
│  └───────────────────────────────────────────┘   │
│                                                  │
│  ┌─ Index Roots ─────────────────────────────┐   │
│  │ ~/Documents    487K files  ● Healthy      │   │
│  │ ~/Developer    312K files  ● Healthy      │   │
│  │ ~/Desktop       1.2K files ● Healthy      │   │
│  │ ~/Downloads     8.4K files ⚠ 12 failures  │   │
│  └───────────────────────────────────────────┘   │
│                                                  │
│  ┌─ Queue ───────────────────────────────────┐   │
│  │ Pending:  0    In progress:  0            │   │
│  │ Embedding queue:  0                       │   │
│  └───────────────────────────────────────────┘   │
│                                                  │
│  ┌─ Recent Errors (12) ──────────────────────┐   │
│  │ /Users/rex/Downloads/corrupt.pdf          │   │
│  │   → Extraction failed: invalid PDF header │   │
│  │ /Users/rex/Downloads/huge_video.mov       │   │
│  │   → Skipped: file exceeds 50MB limit      │   │
│  │ [Show all...]                             │   │
│  └───────────────────────────────────────────┘   │
│                                                  │
│  ┌─ Actions ─────────────────────────────────┐   │
│  │ [Reindex Folder...]  [Rebuild All]        │   │
│  │ [Rebuild Vector Index]  [Clear Cache]     │   │
│  └───────────────────────────────────────────┘   │
│                                                  │
└──────────────────────────────────────────────────┘
```

**Status indicator:**
- Green "Healthy": no errors, indexing complete, embedding complete
- Yellow "Degraded": > 5% failure rate OR embedding < 100%
- Red "Unhealthy": > 20% failure rate OR database integrity check failed
- Blue "Rebuilding": active reindex or rebuild in progress

**Refresh** button: calls `get_health` IPC method on all services.

**Reindex Folder:** folder picker, then sends `reindex` IPC with path.

**Rebuild All:** confirmation dialog, then drops and recreates all index tables. This is destructive and takes a long time.

**Rebuild Vector Index:** confirmation dialog, then triggers hnswlib rebuild (see vector-search.md Section 2.4).

**Clear Cache:** removes CoreML compiled model cache, hnswlib temp files. Does not affect index data.

---

## 3. Settings Persistence Architecture

### 3.1 Storage Locations

Two storage mechanisms (intentional separation):

| What | Where | Why |
|------|-------|-----|
| UI/app preferences | `~/Library/Application Support/betterspotlight/settings.json` | Human-readable, easily reset, git-ignorable |
| Ranking weights + feature flags | SQLite `settings` table | Queryable by services, atomic with index operations |

### 3.2 settings.json Schema

```json
{
  "version": 2,
  "general": {
    "hotkey": "Cmd+Space",
    "launchAtLogin": false,
    "showInDock": false,
    "checkForUpdates": true,
    "maxResults": 20
  },
  "indexing": {
    "roots": [
      {"path": "/Users/rex/Documents", "mode": "index_embed"},
      {"path": "/Users/rex/Developer", "mode": "index_embed"},
      {"path": "/Users/rex/Downloads", "mode": "index_only"}
    ],
    "enablePdf": true,
    "enableOcr": false,
    "ocrLanguages": ["eng"],
    "embeddingEnabled": true,
    "maxFileSizeMB": 50
  },
  "exclusions": {
    "userPatterns": ["*.log", "tmp/"],
    "bsignorePath": "~/.bsignore"
  },
  "privacy": {
    "enableFeedbackLogging": true,
    "enableInteractionTracking": true,
    "feedbackRetentionDays": 90,
    "sensitivePaths": [".ssh", ".gnupg", ".aws", "Library/Keychains"]
  },
  "onboarding": {
    "completed": true,
    "fdaGranted": true,
    "completedAt": "2026-02-07T10:30:00Z"
  }
}
```

### 3.3 Settings Change Propagation

When a setting changes in the UI:

1. SettingsManager writes to `settings.json`
2. SettingsManager emits `settingsChanged(key)` signal
3. Relevant services receive the change via IPC (`update_settings` method)
4. Services reload the affected configuration without restart

Exception: hotkey changes require re-registering the Carbon event handler. This happens in-process (no IPC needed).

Exception: index root additions/removals trigger service-level actions (start/stop indexing for that root).

---

## 4. QML Component Structure

```
src/app/qml/
  Main.qml                  // Existing: search window
  SearchPanel.qml            // Existing: search UI
  StatusBar.qml              // Existing: tray icon
  onboarding/
    OnboardingWindow.qml     // Container with step navigation
    WelcomeStep.qml          // Step 1
    FdaStep.qml              // Step 2
    HomeMapStep.qml           // Step 3
  settings/
    SettingsWindow.qml       // Container with tab bar
    GeneralTab.qml
    IndexingTab.qml
    ExclusionsTab.qml
    PrivacyTab.qml
    IndexHealthTab.qml
  components/
    HotkeyRecorder.qml      // Custom key capture component
    DirectoryPicker.qml     // Root folder management component
    PatternEditor.qml       // Exclusion pattern list editor
```

---

## 5. File Layout (C++ Backend)

```
src/app/
  onboarding_controller.h    // Drives onboarding state machine
  onboarding_controller.cpp
  settings_controller.h      // QML-exposed settings model
  settings_controller.cpp
```
