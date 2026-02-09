#pragma once

#include <string>
#include <vector>

namespace bs {

// BsignoreParser — gitignore-style pattern matching for .bsignore files.
//
// Supported syntax:
//   *        matches any characters except /
//   **       matches any characters including / (directory traversal)
//   ?        matches a single character (not /)
//   # ...    comment (entire line ignored)
//   /suffix  trailing slash means "directory only" (not enforced at match
//            time — caller should append / for directories)
//
// Negation patterns (!pattern) are parsed but treated as no-ops for M1.
// Empty lines are ignored.
class BsignoreParser {
public:
    BsignoreParser() = default;

    // Load patterns from a file. Returns true on success.
    // Existing patterns are replaced.
    bool loadFromFile(const std::string& path);

    // Load patterns from a string (one pattern per line).
    // Existing patterns are replaced.
    bool loadFromString(const std::string& content);

    // Clear all loaded patterns.
    void clear();

    // Test whether a file path matches any loaded pattern.
    // The path should be relative to the .bsignore location,
    // or an absolute path — patterns are matched against any
    // suffix of the path.
    bool matches(const std::string& filePath) const;

    // Access the loaded patterns.
    const std::vector<std::string>& patterns() const { return m_patterns; }

private:
    // Parse raw text into patterns, handling comments and blank lines.
    void parseLines(const std::string& content);

    // Match a single glob pattern against a path.
    // Supports *, **, and ? wildcards.
    bool matchGlob(const std::string& pattern,
                   const std::string& path) const;

    // Recursive helper for glob matching with backtracking.
    bool matchGlobImpl(const char* pattern, const char* path) const;

    std::vector<std::string> m_patterns;
};

} // namespace bs
