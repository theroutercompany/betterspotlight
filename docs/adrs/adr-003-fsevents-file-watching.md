# ADR-003: FSEvents for File Watching

**Date:** 2026-01-19 (carried from original design)
**Status:** Accepted

## Context

The application must detect file system changes across the user's home directory (potentially 500K+ files) to maintain index freshness and currency. Detection must satisfy:
- Minimal CPU and battery impact
- Coalesced event delivery (not per-file notifications that create thundering herd)
- Reliable operation across APFS, HFS+, and network-mounted volumes (NFS, SMB)
- Latency: changes reflected in index within seconds

## Decision

Use the macOS FSEvents C API (CoreServices framework) as the primary file system change detection mechanism.

## Alternatives Considered

**kqueue**
- Rejected. Per-file descriptor model does not scale to 500K+ files; exhausts file descriptor limits (typically 256 per process on macOS); high memory overhead per watched file; unsuitable for full-home monitoring.

**Polling (periodic full scan)**
- Rejected. High CPU and battery cost; slow change detection (typically minutes); poor user experience; incompatible with real-time index freshness goals.

**Qt QFileSystemWatcher**
- Rejected for primary use. Built-in Qt class wraps kqueue on macOS; inherits same scaling limitations; acceptable only for watching small, specific directory sets, not full-home surveillance.

**Spotlight's mdimporter infrastructure**
- Rejected. Creates dependency on Spotlight's index quality, reliability, and update speed—the very system we are designed to replace or supplement.

## Consequences

### Positive

- Kernel-level efficiency; minimal CPU and battery overhead.
- Coalesced event delivery prevents thundering herd; events batched by directory.
- Historical event replay from FSEvents journal allows recovery from temporary disconnections or index staleness.
- Automatic handling of volume mounts, unmounts, and reconnections.
- No file descriptor exhaustion concerns; scales naturally to millions of files.

### Negative

- macOS-only implementation; cross-platform support (Linux: inotify/fanotify; Windows: ReadDirectoryChangesW) requires platform abstraction layer.
- FSEvents reports changes at directory granularity, not per-file; requires re-scan of affected directories to determine which specific files changed.
- Directory-level reporting creates transient ambiguity when multiple files change rapidly; application must deduplicate and batch index updates.
- FSEvents journal is kept for a limited time (system-dependent); recovery of very old changes is not possible.

## Implementation Notes

- Wrap FSEvents in a platform-agnostic FileSystemWatcher interface to prepare for cross-platform support.
- Implement directory-level change coalescing with timer (e.g., 500ms) to batch updates and reduce index write frequency.
- Design re-scan strategy: on event, read directory contents, diff against last known state, queue changes for indexing.
- Monitor FSEvents latency; alert if system is unable to keep up with file system activity.
