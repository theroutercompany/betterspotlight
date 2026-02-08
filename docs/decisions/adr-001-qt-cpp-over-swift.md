# ADR-001: Qt 6/C++ over Swift/SwiftUI

**Date:** 2026-02-06
**Status:** Accepted

## Context

The project was initially scaffolded in Swift/SwiftUI with XPC services. The primary developer is fluent in C/C++ but not in Swift. SwiftUI on macOS has documented persistent issues with complex desktop UIs, a pattern reported by veteran macOS engineers through macOS 26. The development team consists of 2-3 people.

The scaffold is approximately 75-80% complete but the technology choice is creating friction with developer productivity and long-term maintainability concerns.

## Decision

Rewrite the application in Qt 6/C++ using Qt Quick (QML) for the user interface.

## Alternatives Considered

**Continue with Swift/SwiftUI**
- Rejected due to: developer unfamiliarity with Swift, SwiftUI instability for complex macOS desktop applications, inability to leverage the team's C/C++ expertise, and persistent layout/rendering issues in desktop-class UIs.

**Electron/TypeScript**
- Rejected categorically. The team has strong objections to JavaScript/TypeScript, and Electron's memory overhead contradicts the performance-first goals of a replacement for the lightweight native Spotlight.

**Tauri/Rust**
- Considered but rejected. Rust learning curve presents significant onboarding friction for the team, the desktop widget ecosystem is less mature than Qt's, and webview-based UI lacks the native feel required for a system-level utility.

**AppKit (without SwiftUI)**
- Rejected. Would solve SwiftUI's instability issues but still requires Swift proficiency and removes cross-platform potential without solving the core developer productivity problem.

## Consequences

### Positive

- Primary developer works in a language with existing proficiency, reducing cognitive load.
- Qt 6.10+ has strong native macOS integration, including Liquid Glass support and native SearchField component.
- Cross-platform potential for future Linux and Windows support without major architectural changes.
- Excellent C++ performance characteristics, suitable for real-time file system operations and large-scale indexing.
- Rich ecosystem of libraries and tools; well-documented for desktop development.

### Negative

- Existing Swift scaffold (~75-80% complete) becomes technical debt and is retired.
- Qt applications on macOS have subtle non-native visual tells and behavior differences (diminishing concern for technical power-user audience).
- Qt licensing requires commercial license (€530/year for small business) or LGPL compliance with dynamic linking.
- Build system complexity increases (CMake vs. SPM/Xcode); project setup and CI/CD are more involved.
- Smaller macOS development community compared to Swift, fewer macOS-specific examples.

## Implementation Notes

- Abstract platform-specific code behind interfaces to facilitate future cross-platform extensions.
- Evaluate Qt's licensing terms early to determine commercial vs. LGPL distribution strategy.
- CMake configuration should enforce macOS 12.0+ as minimum deployment target (Liquid Glass support).
