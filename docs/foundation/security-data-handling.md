# Security & Data Handling Model

**Status**: Active | **Last Updated**: 2025-02 | **Ownership**: Security Lead / Tech Lead

A threat model and operational security guide for BetterSpotlight, a local desktop file search app requiring Full Disk Access on macOS. This document focuses on realistic threats to a client-side indexing app, not security theater.

---

## Executive Summary

**Threat Profile**: BetterSpotlight reads every file on indexed disks and stores extracted metadata/text in a local SQLite database. Attack surface is primarily local: unauthorized process access to the index, malicious file triggering extractor crash, and sensitive content inadvertently exposed in search results.

**Key Principle**: Defense focuses on detection and containment rather than encryption. A user with Full Disk Access has already granted broad system permissions; our role is to minimize blast radius and prevent information leakage within the user's system.

**Known Limitations**:
- No encryption at rest (M1, M2 phase; SQLCipher deferred to M2+)
- Process isolation via separate extractor processes (crash containment)
- Content filtering via .bsignore (exclusions, not strong access control)
- No authentication between local processes (acceptable for single-user desktop app)

---

## 1. Data Collection Scope

### What BetterSpotlight Reads

**File System**:
- Every file under user-specified indexed roots (default: `~`, `/Applications`, system directories)
- File metadata: path, size, modification time, owner, permissions, type (via macOS Uniform Type Identifier)
- File contents: varies by file type (see below)

**File Contents Extraction**:
- **Text files** (.txt, .md, .rst, code): Full content (up to configurable max size, e.g., 10 MB)
- **Structured data** (.json, .csv, .yaml): Full content
- **Documents** (.pdf, .docx, .rtf): Extracted text only (not binary formatting)
- **Images** (.png, .jpg, .gif, .tiff): OCR text extraction (via Tesseract, ~25-30 MB for English language model)
- **Code** (.py, .js, .swift, .cpp, etc.): Full content including comments
- **Archives** (.zip, .tar, .gz): Metadata about archive (not extracted contents)
- **Binary files** (executables, compiled objects, libraries): Name only, no content

**Content NOT Extracted**:
- File binary data (for images, PDFs, compiled code)
- Encrypted file contents (.gpg, password-protected PDFs)
- Files matching .bsignore exclusions
- Symlink targets (name only; circular link prevention built-in)

### What BetterSpotlight Stores

**SQLite Database** (`~/Library/Application Support/BetterSpotlight/index.db`):

| Table | Content | Purpose |
|-------|---------|---------|
| `files` | `id, path, size, mtime, inode, indexed_flag` | File metadata for path matching |
| `file_metadata` | `file_id, owner, permissions, uti_type, is_symlink` | Additional file attributes |
| `content` | `file_id, text_excerpt (first 500 chars), hash, extracted_at` | Indexed text (capped excerpt) |
| `fts_idx` | FTS5 index over `content.text_excerpt` | Full-text search tokens |
| `tags` | `file_id, tag_name, user_created` | User-assigned tags (future M2) |
| `embeddings` | `file_id, embedding (float vector), model_version` | Semantic search vectors (M2+, ONNX Runtime) |
| `extraction_errors` | `file_id, error_type, error_message, attempted_at` | Log of failed extractions (debugging) |
| `index_log` | `batch_id, timestamp, files_indexed, files_deleted, duration_ms` | Indexing statistics |

**What is NOT Stored Verbatim**:
- Full file contents (capped at 500 char excerpt for display purposes)
- Binary data from images, PDFs, executables
- Encrypted file contents (extraction fails, logged as error)
- Passwords, API keys, credentials (should be in .bsignore)

**Disk Space Estimate**:
- 100k files with ~300 char average excerpt: ~30 MB SQLite database
- WAL mode temporary files (see below): +5-10 MB during active indexing
- Total for average user: 50-75 MB

---

## 2. Storage Security

### Database Location & Permissions

**Path**: `~/Library/Application Support/BetterSpotlight/`

**Directory Permissions**:
```bash
ls -ld ~/Library/Application\ Support/BetterSpotlight/
# Expected: drwx------ (0700 — owner only)

chmod 0700 ~/Library/Application\ Support/BetterSpotlight/
```

**Database File Permissions**:
```bash
ls -l ~/Library/Application\ Support/BetterSpotlight/index.db
# Expected: -rw------- (0600 — owner read/write only)

chmod 0600 ~/Library/Application\ Support/BetterSpotlight/index.db*
```

**Why Strict Permissions Matter**:
- macOS does not enforce mandatory access control (MAC) at the filesystem level for local access (unlike Linux SELinux)
- Any process running as the same user can read files with group/other bits set
- Strict 0600 permissions prevent accidental exposure (e.g., scripts, other apps, or compromised processes)

**Enforcement in Code**:
```cpp
// Pseudocode: set permissions on first app launch
QFile dbFile(dbPath);
if (dbFile.exists()) {
    dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
}
```

### Encryption at Rest

**Current Status (M1)**: No encryption.

**Rationale for M1**:
- Full Disk Access already requires user trust; FileVault provides OS-level encryption if desired
- SQLCipher adds ~20% performance overhead; premature optimization
- Complexity: managing encryption keys, handling key rotation, and securely zeroing memory requires careful design

**Known Limitation**: Database file is readable by any process running as the user. Shared machines (e.g., family laptop) are at risk. User should enable FileVault for full-disk encryption.

**Future (M2)**: Implement SQLCipher encryption with a master passphrase stored in Keychain. Deferred due to complexity and uncertain user demand.

### Temporary Files

**WAL Mode** (Write-Ahead Logging):

SQLite uses WAL mode for performance. This creates temporary files:
- `index.db-wal` (Write-Ahead Log)
- `index.db-shm` (Shared Memory)

These files contain **unencrypted index data** and must have the same strict permissions as the main database:

```cpp
// Ensure WAL files inherit permissions
sqlite3_wal_autocheckpoint(db, 0); // Manual checkpoint control
// After checkpoint, verify permissions on -wal and -shm files
chmod(walPath, 0600);
chmod(shmPath, 0600);
```

**Cleanup**: WAL files are automatically cleaned up during normal shutdown. If the app crashes, files remain until next launch (acceptable; they contain only index data).

---

## 3. Sensitive Content Handling

### .bsignore File

**Purpose**: User-specified exclusion patterns. Files matching patterns are NOT indexed at all.

**Location**: `~/.bsignore` (user's home directory for M1; future M2+ may support per-directory .bsignore files)

**Default Exclusions** (shipped in app):
```
# SSH & Cryptography
.ssh/*
.gnupg/*
.ssh/config
.ssh/authorized_keys

# AWS & Cloud Credentials
.aws/credentials
.aws/config
.azure/*

# Environment & Secrets
.env
.env.local
.env.*.local
*.pem
*.key
id_rsa*
id_dsa*
id_ecdsa*
id_ed25519*

# Known Hosts
known_hosts

# API Keys & Tokens
.config/hub
.kube/config
.docker/config.json

# Transitive: Node, Python
node_modules/**
venv/**
.venv/**
__pycache__/**

# System
.Trash
.TemporaryItems
.DS_Store
Thumbs.db

# Application Caches
*.cache
.pytest_cache/**
.mypy_cache/**
```

**User Customization**:
```
# Example: index.bsignore
# Index everything except my private documents
!/Users/alice/Documents/Private/*
```

**Matching Semantics**:
- Gitignore-style patterns (fnmatch with `*`, `**`, `?`)
- `#` comments
- Lines prefixed with `!` are un-ignored (whitelisted)
- Matching is performed at index time; updates take effect on next full re-index

**Enforcement**:
```cpp
bool isIgnored(const QString& filePath, const QStringList& ignorePatterns) {
    for (const auto& pattern : ignorePatterns) {
        if (pattern.startsWith('!')) {
            // Whitelist pattern
            if (fnmatch(pattern.mid(1), filePath)) return false;
        } else if (fnmatch(pattern, filePath)) {
            return true;
        }
    }
    return false;
}
```

### "Guarded" File Classification

**Concept**: A file can be indexed for **path/name search** but content extraction is skipped.

**Use Case**: User wants to find a file named "credential-manager.py" but doesn't want the file's contents searchable (e.g., it contains hardcoded test credentials).

**Implementation** (Future M2):
```
# .bsignore with 'guarded' directive
guarded: .env*, *.key, *.pem
```

**Behavior**:
- File path is indexed (users can find by filename)
- Content is NOT extracted or tokenized
- FTS5 index does not contain content tokens
- Search results show path only, no snippet preview

**Guarded Table Schema**:
```sql
CREATE TABLE guarded_paths (
    file_id INTEGER PRIMARY KEY,
    path TEXT,
    classification TEXT  -- 'guarded', 'always_exclude', 'always_extract'
);
```

### Preview Masking in UI

**Threat**: Shoulder surfing or accidental exposure of sensitive content in search result snippets.

**Mitigation**: Search results from guarded or sensitive-classified paths show path only, no text excerpt.

**Example**:
```
Search: "password"

❌ BAD (current):
Path: /Users/alice/.env
Preview: DATABASE_PASSWORD=SuperSecret123...

✅ GOOD (future):
Path: /Users/alice/.env
Preview: [Content hidden - guarded file]
```

**Implementation**:
```cpp
QString resultSnippet(const SearchResult& result) {
    if (isGuardedPath(result.path)) {
        return "[Content masked - sensitive file]";
    }
    return result.excerpt;
}
```

---

## 4. Process Isolation & Crashes

### Architecture Overview

BetterSpotlight runs as multiple processes to isolate failures:

```
Main App (GUI)
  ├── Indexer Service (background indexing, FSEvents monitoring)
  ├── Extractor Service (text extraction from files)
  └── Searcher Service (query execution, result ranking)
```

**Communication**: Unix domain sockets (localhost, no network exposure).

### Failure Isolation

**Threat**: A malicious or corrupted file (e.g., extremely large PDF, malformed image) triggers a crash in the extractor.

**Mitigation via Process Isolation**:

| Component | Process | Isolation |
|-----------|---------|-----------|
| GUI/Main App | `BetterSpotlight.app/Contents/MacOS/BetterSpotlight` | OS process boundary. Crash in extractor does NOT crash UI. |
| Indexer Service | `BetterSpotlight.app/Contents/Helpers/ExtractorService` | Separate process. Inherits FDA permission from parent. |
| Extractor Service | `BetterSpotlight.app/Contents/Helpers/ExtractorService --worker` | Worker pool (e.g., 2-4 workers). One worker crash → restarted, others continue. |

**Crash Handling**:
```cpp
// Extractor Service: wrap extraction in try-catch
void extractFile(const QString& filePath) {
    try {
        if (filePath.endsWith(".pdf")) {
            extractPDF(filePath);
        } else if (filePath.endsWith(".png")) {
            extractOCR(filePath);
        }
    } catch (const std::exception& e) {
        // Log error, mark file as extraction_failed
        logExtractionError(filePath, e.what());
        // Continue to next file (do not crash worker)
    }
}
```

**Restart Policy**:
- Main app monitors Indexer Service. If Indexer dies, restart with exponential backoff.
- Indexer monitors Extractor workers. If a worker dies, restart immediately.
- If Extractor dies >5 times in 1 minute, pause indexing and notify user.

### IPC Security (Unix Domain Sockets)

**Communication Method**: Unix domain sockets (AF_UNIX) instead of TCP.

**Why Not TCP**:
- TCP binds to localhost, potentially exposed to network (if firewall is misconfigured)
- Unix sockets are filesystem-based, inherently local-only

**Socket Permissions**:
```bash
ls -l /var/tmp/BetterSpotlight-*.sock
# Expected: srwx------ (0700)
```

**Enforcement in Code**:
```cpp
// Server side: bind socket
int socketFd = socket(AF_UNIX, SOCK_STREAM, 0);
sockaddr_un addr = {};
strcpy(addr.sun_path, "/var/tmp/BetterSpotlight-ipc.sock");
bind(socketFd, (sockaddr*)&addr, sizeof(addr));
chmod(addr.sun_path, 0700); // Restrict to owner
listen(socketFd, 5);
```

### Authentication Between Processes

**Current**: No authentication. Assumption is single-user desktop.

**Rationale**:
- All processes run as the same user (UID)
- Shared memory and sockets are already protected by user-level permissions
- Adding authentication (tokens, signatures) adds complexity without security benefit for local-only processes

**If Future Threat Model Changes**:
- For multi-user shared desktop, add simple token-based auth: main app generates token on launch, passed to child processes via IPC handshake

---

## 5. Network Posture

### Offline-First Architecture

BetterSpotlight is designed to work entirely offline.

**Zero Outbound Connections** (in core operation):
- No telemetry
- No analytics
- No crash reporting to external servers
- No update checking (initial release; future versions may add opt-in checking)

**Qt Network Module** (if included):
- Currently unused in core
- Included for future features (e.g., sharing indexes, collaborative search)
- Does not make outbound calls without explicit user action

### Update Checking (Future Feature)

If implemented, update checking must be:

1. **Opt-in**: User explicitly enables in Settings
2. **Transparent**: User sees what data is sent (e.g., app version, OS version, no file index)
3. **No Automatic Installation**: User approves downloads and installation
4. **HTTPS Only**: Encrypted connection to update server

**Example (Future)**:
```cpp
// Check for updates (opt-in)
if (settings.checkForUpdates()) {
    QNetworkAccessManager mgr;
    QNetworkRequest req(QUrl("https://updates.betterspotlight.app/latest"));
    QNetworkReply* reply = mgr.get(req);
    // User explicitly triggered check; user sees update notification
}
```

### No External Dependencies at Runtime

- No cloud sync (data stays local)
- No online activation or licensing (licensing is handled locally or via file)
- No remote logging or monitoring

---

## 6. Threat Scenarios & Mitigations

### Threat 1: Unauthorized Process Access to Index Database

**Scenario**: Another process running as the same user reads the SQLite database file.

**Risk**: Index exposes file paths and content excerpts that the user may have believed private.

**Existing Mitigations**:
- File permissions (0600) prevent other users from reading
- FileVault encryption (user responsibility) prevents physical disk theft

**Residual Risk**: Malware or compromised application running as the user can read the index.

**Acceptable**?  Yes, because:
- Full Disk Access grant already allows the app to read all files; an attacker with that permission can read the original files
- Index is a summary; original files contain more sensitive data
- User controls which files are indexed via .bsignore

**Future Mitigations** (M2+):
- SQLCipher encryption with passphrase stored in Keychain
- Per-session in-memory decryption (decrypt on app launch, zero after app closes)

---

### Threat 2: Malicious File Triggers Extractor Crash

**Scenario**: A corrupted PDF, extremely large file, or specially crafted image causes the Tesseract OCR or PDF extractor to crash, potentially exploiting a buffer overflow.

**Risk**: Crash of extractor process; in worst case, code execution if vulnerability exists in extraction library.

**Existing Mitigations**:
- **Process isolation**: Extractor runs in separate process. Crash does not affect GUI or other services.
- **Resource limits**: Set memory/CPU limits on extractor worker processes (via setrlimit or cgroups on macOS).
- **Timeouts**: Extraction task has timeout (e.g., 10 seconds per file). If extraction hangs, worker is killed and restarted.
- **Input validation**: Sanity checks before passing file to extractor (max file size, check magic bytes for file type).

**Implementation**:
```cpp
// Pseudocode: Extract with timeout
void extractWithTimeout(const QString& filePath) {
    QProcess extractor;
    extractor.start("./ExtractorService", {"--extract", filePath});

    if (!extractor.waitForFinished(10000)) { // 10 second timeout
        extractor.kill();
        logExtractionError(filePath, "Timeout");
        return;
    }

    if (extractor.exitCode() != 0) {
        logExtractionError(filePath, extractor.readAllStandardError());
    }
}
```

**Residual Risk**: Zero-day vulnerability in extraction library (Tesseract, Poppler, etc.) could still be exploited. However, blast radius is limited to the extractor process; filesystem is protected by macOS sandbox (if implemented).

**Future Mitigations** (M2+):
- Run extractors in macOS App Sandbox (restrict file access to indexed directories only)
- Use native macOS extractors where available (PDFKit, Vision framework for OCR)

---

### Threat 3: Sensitive Content in Search Result Previews (Shoulder Surfing)

**Scenario**: User searches for a file and a search result snippet reveals sensitive information (e.g., API key, password).

**Risk**: Bystander sees sensitive data displayed on screen.

**Existing Mitigations**:
- .bsignore exclusions prevent indexing of known sensitive paths
- Default .bsignore includes .ssh/, .aws/credentials, .env, etc.

**Residual Risk**: User does not add custom exclusions; searches for a string that matches both public and private files.

**Example**:
```
Search: "token"

Result 1: /Users/alice/Desktop/Article-token-economics.md (safe)
Result 2: /Users/alice/.env (contains API_TOKEN=secret123) — EXPOSED
```

**Future Mitigations** (M2):
- Implement "guarded" file classification (content indexed but preview masked)
- Add warnings in Settings: "Files matching patterns will not have previews shown"
- Option to blur/mask previews in results

---

### Threat 4: Index Reveals File Existence (Information Leakage)

**Scenario**: User believes a file is "hidden" (not indexed by Spotlight), but BetterSpotlight indexes it by default, making the file discoverable via search.

**Risk**: Unintended disclosure of file existence.

**Example**: User creates `~/.private/secret-project/notes.txt` and assumes it's not indexed. BetterSpotlight finds it because `.private` is not in .bsignore by default.

**Existing Mitigations**:
- .bsignore exclusions
- Documentation: "BetterSpotlight indexes all non-excluded files. Use .bsignore to hide sensitive paths."

**Residual Risk**: User misconfigures .bsignore or forgets to exclude a directory.

**Future Mitigations** (M2):
- Smarter defaults: Auto-exclude common sensitive directories (e.g., `~/Private`, `~/Confidential`)
- UI warning: "The following paths are publicly indexed: ..." (show summary of what's indexed)

---

### Threat 5: Extraction of Encrypted PDFs or Password-Protected Files

**Scenario**: A PDF is encrypted with a user password. The extractor attempts to read it and either crashes or exposes the encrypted content.

**Risk**: Extractor crash (from Threat 2) or unintended extraction of encrypted data.

**Existing Mitigations**:
- Extraction libraries (Poppler, MuPDF, PDFKit) detect encryption and skip extraction
- Extraction failure is logged as `extraction_error` with reason "File is encrypted"
- File is still indexed by path/name (user can find the file), but content is not searchable

**Implementation**:
```cpp
bool canExtractPDF(const QString& pdfPath) {
    // Use Poppler or PDFKit to check if PDF is encrypted
    Poppler::Document doc = Poppler::Document::load(pdfPath);
    if (doc && doc->isEncrypted()) {
        logExtractionError(pdfPath, "PDF is encrypted; skipping content extraction");
        return false;
    }
    return true;
}
```

**Residual Risk**: None. Encrypted files are skipped gracefully.

---

## 7. Recommendations for Future Milestones

### Milestone 2 (M2): Enhanced Privacy Controls

- [ ] **SQLCipher Integration**: Encrypt index database at rest with user passphrase stored in Keychain
- [ ] **Guarded File Classification**: Content indexed for path matching, but extraction skipped for sensitive paths
- [ ] **Preview Masking**: Search results from guarded/sensitive paths show path only
- [ ] **Secure Memory Handling**: Zero buffers containing extracted text after indexing to prevent memory dumps leaking content
- [ ] **Configurable Extraction**: User can exclude specific file extensions from content extraction (e.g., "index .pdf paths, but don't extract text")

**Effort**: ~4 weeks (2 developers)

---

### Milestone 3 (M3): Sandboxing & Hardening

- [ ] **App Sandbox**: Run main app and extractors in macOS App Sandbox, restrict file access to indexed roots only
- [ ] **Code Signing**: Sign binaries with developer certificate; verify signatures on service launch
- [ ] **Hardened Runtime**: Enable Hardened Runtime entitlements (restrict dynamic code loading, socket filter, etc.)
- [ ] **Crash Reporting** (Optional): Add opt-in crash reporter (e.g., Crashpad) with full user control

**Effort**: ~3 weeks (1-2 developers)

---

### Milestone 4 (M4): Native Extraction Services

- [ ] **Replace Tesseract with Vision.framework**: Native Apple OCR (better privacy, performance, M1 optimized)
- [ ] **Replace Poppler with PDFKit**: Native Apple PDF handling (zero external dependencies)
- [ ] **Reduce External Dependencies**: Smaller attack surface, better OS integration

**Effort**: ~6 weeks (1-2 developers, requires rewrite of extraction layer)

---

## 8. Security Testing & Validation

### Automated Security Checks (CI)

- [ ] **Dependency Scanning**: Run `cargo-audit` or `pip audit` equivalents monthly; flag vulnerable versions
- [ ] **Static Analysis**: LLVM's `clang --analyze` on C++ code to detect memory safety issues
- [ ] **Fuzzing**: Fuzz PDF/OCR extraction with malformed files (e.g., libFuzzer with corpus of broken PDFs)
- [ ] **Permission Verification**: Test suite verifies database file permissions (0600) after app launch

### Manual Security Audit

Before M1 release:
- [ ] Code review of all file I/O and memory handling (focus on extractors)
- [ ] Threat model walkthrough with team and external security reviewer (if budget allows)
- [ ] Penetration testing: attempt to escape process isolation, read unencrypted index, etc.

---

## 9. User Documentation

### Privacy Policy (In-App)

**Example**:
```
BetterSpotlight Privacy
======================

Data Collection:
- BetterSpotlight indexes files on your computer to make them searchable.
- It reads file paths, sizes, modification times, and content (text files, PDFs, images).
- It does NOT transmit data to external servers.

Data Storage:
- Indexed data is stored in ~/Library/Application Support/BetterSpotlight/index.db
- Database file is readable only by your user (owner-only permissions, 0600).
- To fully protect the index, enable FileVault full-disk encryption.

Sensitive Files:
- BetterSpotlight skips indexing files matching ~/.bsignore patterns.
- Default patterns exclude .ssh/, .aws/credentials, .env, and other sensitive paths.
- Add custom exclusions to ~/.bsignore to hide additional files.

No External Data:
- BetterSpotlight does not collect analytics, telemetry, or crash reports.
- The app is offline-first and does not require internet access.
```

### .bsignore Documentation

**Example: User Guide**:
```
Configuring BetterSpotlight Exclusions
======================================

Edit ~/.bsignore to control which files are indexed.

Examples:
  .ssh/*           # Exclude SSH keys and config
  .env*            # Exclude environment files
  *.key            # Exclude private keys
  ~/Private/*      # Exclude a private directory
  node_modules/**  # Exclude node_modules (large, usually not searched)

Rules:
- Lines starting with # are comments
- * matches any characters except /
- ** matches any characters including /
- Lines starting with ! un-ignore (whitelist) a pattern
- Matching is case-sensitive

Example with whitelisting:
  ~/secrets/*      # Exclude all secrets
  !~/secrets/public-list.txt  # But index this one file
```

---

## 10. Compliance & Legal

### GDPR Considerations

**Is BetterSpotlight a "data processor"?** No, because:
- Data is collected and processed on the user's device
- No data is transferred to a third party
- No data controller relationship exists

**Does GDPR apply?** Minimally. Local file indexing is not a data processing activity under GDPR.

### Data Breach Notification

Since BetterSpotlight does not transmit data externally:
- No breach of our servers is possible
- User is responsible for protecting their device (FileVault, security updates)
- If a user's device is compromised, they should follow macOS recovery procedures

---

## References

- macOS Security & Privacy: https://support.apple.com/en-us/102186
- OWASP Top 10 Desktop: https://owasp.org/www-project-top-10/
- SQLite Security: https://www.sqlite.org/security.html
- FSF: LGPL Dynamic Linking: https://www.gnu.org/licenses/lgpl-faq.html#SourceCodeForm
- macOS App Sandbox: https://developer.apple.com/documentation/xcode/configuring-the-macos-app-sandbox
