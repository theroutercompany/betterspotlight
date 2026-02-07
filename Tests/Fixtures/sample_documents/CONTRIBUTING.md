# Contributing to BetterSpotlight

Thank you for your interest in contributing to BetterSpotlight! This document
provides guidelines and information for contributors.

## Getting Started

1. Fork the repository
2. Clone your fork: `git clone https://github.com/your-username/betterspotlight.git`
3. Create a feature branch: `git checkout -b feature/your-feature-name`
4. Make your changes
5. Run tests: `swift test`
6. Submit a pull request

## Development Setup

### Requirements
- macOS 14+ (Sonoma)
- Xcode 15+ or Swift 5.9+ toolchain
- Full Disk Access (for integration tests)

### Building
```bash
swift build          # Debug build
swift build -c release  # Release build
```

### Testing
```bash
swift test                          # All tests
swift test --filter CoreTests       # Unit tests
swift test --filter IntegrationTests # Integration tests
```

## Code Style

- Follow Swift API Design Guidelines
- Use `actor` for thread-safe mutable state
- All cross-process types must be `Codable` and `Sendable`
- Maximum line length: 120 characters
- Use meaningful names; avoid abbreviations

## Architecture Rules

- **Dependency direction**: App -> Core/Services/Shared -> Shared
- **No upward dependencies**: Shared must not import Core or Services
- **Process isolation**: Each XPC service runs independently
- **Sensitive paths**: Never extract content from `.ssh`, `.gnupg`, `.aws`

## Commit Messages

Follow conventional commits:
- `feat:` new feature
- `fix:` bug fix
- `docs:` documentation only
- `refactor:` code restructuring
- `test:` adding or fixing tests
- `chore:` maintenance tasks

## Reporting Issues

- Use GitHub Issues
- Include macOS version, Swift version, and reproduction steps
- For crashes, include the crash log if available

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
