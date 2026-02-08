#pragma once

#include "core/shared/types.h"
#include "core/fs/path_rules.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace bs {

// FileScanner — recursive directory walker with metadata extraction.
//
// Walks a directory tree using QDirIterator, applies PathRules validation,
// extracts FileMetadata for each valid entry, and classifies ItemKind
// from file extensions.
class FileScanner {
public:
    // Construct a scanner with the given path rules.
    // If nullptr, a default PathRules instance is created internally.
    explicit FileScanner(const PathRules* rules = nullptr);

    // Recursively scan a directory and return metadata for all valid files.
    // Applies PathRules to skip excluded paths. Directories are not included
    // in the result set (they are traversed but not emitted).
    std::vector<FileMetadata> scanDirectory(const std::string& root) const;

    // Classify a file's ItemKind based on its extension and permissions.
    // mode is the POSIX file mode (from stat); used to detect executables.
    static ItemKind classifyItemKind(const std::string& extension,
                                     mode_t mode = 0);

private:
    // Recursive helper that prunes excluded directories before entering them.
    void scanRecursive(const QString& dirPath,
                       std::vector<FileMetadata>& results,
                       uint64_t& scannedCount,
                       uint64_t& excludedCount,
                       int depth = 0) const;

    // Build the extension -> ItemKind lookup table.
    static std::unordered_map<std::string, ItemKind> buildExtensionMap();

    // Singleton-like extension map (built once, immutable).
    static const std::unordered_map<std::string, ItemKind>& extensionMap();

    static constexpr int kMaxDepth = 64;

    const PathRules* m_rules;
    PathRules m_ownedRules;  // used when no external rules provided
};

} // namespace bs
