# ADR-004: Process Isolation Architecture

**Date:** 2026-02-06
**Status:** Accepted (approach modified from original)

## Context

File content extraction (PDF parsing, OCR, text decoding) and indexing operations are crash-prone tasks that can be triggered by malformed, corrupted, or adversarial files. A crash in the extraction or indexing pipeline must not crash the main UI process, as this degrades user experience and loses search functionality.

The original Swift scaffold used XPC services for inter-process communication. With the Qt/C++ rewrite, the IPC mechanism must be reconsidered for ergonomics, maintainability, and future cross-platform potential.

## Decision

Implement separate processes for Indexer, Extractor, and Query services, communicating with the main UI application via Unix domain sockets using a lightweight, application-defined message protocol (length-prefixed format, e.g., JSON or Protocol Buffers Lite).

The main Qt UI application launches and manages the lifecycle of service processes.

## Alternatives Considered

**C-based XPC API (xpc.h)**
- Considered. Provides launchd integration and automatic process restart. Less ergonomic than NSXPCConnection; lower-level error handling required. Locks out cross-platform IPC support. Complex bridging from C++.

**In-process threading**
- Rejected. Simpler implementation but a crash in any extraction/indexing worker thread crashes the entire process, including UI. Violates the core isolation requirement. Shared memory state increases deadlock and data race risk.

**D-Bus**
- Rejected. Standard IPC on Linux but non-native and cumbersome on macOS; adds significant dependency; overkill for local, co-located processes.

**gRPC**
- Rejected. Designed for distributed systems and network RPC; too heavyweight for local IPC between processes on the same machine; unnecessary protobuf build overhead; over-engineered for this use case.

## Consequences

### Positive

- Unix domain sockets are cross-platform primitive available on macOS, Linux, and adaptable on Windows; supports future porting.
- Low overhead for local communication; well-understood, mature networking primitive.
- No external dependencies (sockets are system API); tight control over message format and protocol evolution.
- Process crash isolation is guaranteed; OS enforces process boundaries and resource cleanup.
- Straightforward debugging: standard Unix tools (netstat, lsof) provide visibility into socket communication.

### Negative

- No automatic process restart; application must implement watchdog/supervisor logic to detect and restart crashed services.
- No built-in serialization; application must define and implement message format (recommend Protocol Buffers Lite, FlatBuffers, or simple length-prefixed JSON).
- More boilerplate code than XPC for service lifecycle management (process spawning, signal handling, socket binding).
- Error handling and timeouts must be explicitly programmed; requires careful design to avoid deadlocks or orphaned processes.
- Debugging distributed process issues is more complex than in-process threading.

## Implementation Notes

- Define a message protocol with type discriminator (e.g., IndexRequest, ExtractRequest, QueryRequest) and length prefix to handle streaming.
- Implement a Service base class managing socket lifecycle, message dispatch, and error recovery.
- Use a supervisor thread in the main UI process to monitor service process health; restart on unexpected termination.
- Implement request timeouts (e.g., 30s) to detect hung or deadlocked services.
- Consider using Protocol Buffers Lite or FlatBuffers for binary message encoding; simpler than full gRPC stack.
- Log all IPC traffic in debug builds for troubleshooting.
