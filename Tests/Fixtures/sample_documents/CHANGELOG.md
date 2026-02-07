# Changelog

All notable changes to BetterSpotlight will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial SwiftUI scaffold with status bar integration
- XPC service architecture for Indexer, Extractor, and Query services
- SQLite FTS5 storage layer with WAL mode
- FSEvents wrapper for file change detection
- PathRules engine with 30+ default exclusion patterns
- Multi-signal ranking with 7 match types
- Onboarding window for first-run experience

### Changed
- Nothing yet

### Fixed
- XPC proxy error handling for connection failures
- Codable conformance for all cross-process models

## [0.1.0] - 2024-01-15

### Added
- Project scaffolding with Swift Package Manager
- Four-layer architecture: App, Core, Services, Shared
- Basic project documentation in `docs/`
- Architecture Decision Records (ADRs)
- .md for AI-assisted development

### Infrastructure
- Xcode project with proper signing entitlements
- SPM dependency on SQLite.swift 0.15.0
- CI-ready test structure with unit and integration test targets

[Unreleased]: https://github.com/user/betterspotlight/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/user/betterspotlight/releases/tag/v0.1.0
