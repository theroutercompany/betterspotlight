# BetterSpotlight

A Spotlight replacement for technical macOS users. Offline-first file search with Full Disk Access, full-text search via SQLite FTS5, configurable exclusion rules, and multi-signal ranking.

## Features

- **Offline-first**: All indexing and search happens locally on your Mac
- **Full-text search**: SQLite FTS5 with unicode61 tokenizer for 100+ file types
- **Smart ranking**: Multi-signal scoring with match type, recency, frequency, and context awareness
- **Privacy-aware**: Sensitive paths (`.ssh`, `.gnupg`, `.aws`) are metadata-only
- **Configurable**: gitignore-style exclusion rules via `.bsignore`
- **Crash-resilient**: XPC process isolation for indexer, extractor, and query services

## Requirements

- macOS 14+ (Sonoma)
- Swift 5.9
- Full Disk Access permission

## Quick Start

```bash
# Build
swift build

# Run tests
swift test

# Build release
swift build -c release
```

## Architecture

```
App (SwiftUI)  →  Status bar app, search UI
Core           →  FS monitoring, extraction, indexing, ranking
Services       →  XPC-isolated IndexerService, ExtractorService, QueryService
Shared         →  Models (Codable) + IPC protocols
```

## License

MIT License. See [LICENSE.txt](LICENSE.txt) for details.
