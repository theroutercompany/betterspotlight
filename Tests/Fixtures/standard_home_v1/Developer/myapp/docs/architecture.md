# Architecture Overview

The service is organized as modular microservices that communicate through IPC.
A dedicated message queue buffers indexing work when bursts arrive from FSEvents.
An API layer sits behind an internal load balancer for service-to-service calls.

## Components
- Indexer: scans and chunks files
- Extractor: handles format-specific parsing
- Query service: executes lexical + semantic ranking
- Health monitor: publishes status metrics

## Data flow
1. Watch filesystem changes
2. Enqueue tasks
3. Extract metadata and content
4. Write FTS5 + vector records
5. Serve ranked query results
# note line 1
# note line 2
