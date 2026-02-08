#pragma once

#include <QString>

namespace bs {

// FdaCheck -- verifies Full Disk Access permissions on macOS.
//
// macOS requires Full Disk Access (FDA) for apps to read certain
// protected directories (~/Library/Mail/, ~/Library/Messages/, etc.).
// This utility probes a known FDA-gated path to detect whether
// the running process has FDA granted.
class FdaCheck {
public:
    // Returns true if Full Disk Access appears to be granted.
    // Probes ~/Library/Mail/ (exists on all macOS systems, requires FDA).
    static bool hasFullDiskAccess();

    // Returns a user-friendly message explaining how to grant FDA.
    static QString instructionMessage();
};

} // namespace bs
