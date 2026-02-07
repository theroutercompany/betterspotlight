#pragma once

#include "core/shared/types.h"
#include "core/fs/bsignore_parser.h"
#include <string>
#include <vector>

namespace bs {

// PathRules — exclusion rules, sensitivity classification, and cloud detection.
//
// Validation decision table (doc 03 Stage 3, evaluated in order):
//   1. .bsignore match             -> Exclude
//   2. Built-in exclusion match    -> Exclude
//   3. Cloud artifact              -> Exclude
//   4. Hidden path (dot-prefixed)  -> Exclude (hidden dirs only; not
//      cloud/bsignore-handled paths)
//   5. Sensitive path              -> MetadataOnly
//   6. Size > 5 GB                 -> Exclude
//   7. Otherwise                   -> Include
class PathRules {
public:
    PathRules();

    // Validate a file path against all rules.
    // fileSize is optional; pass 0 to skip the size check.
    ValidationResult validate(const std::string& filePath,
                              uint64_t fileSize = 0) const;

    // Classify the sensitivity level of a path.
    Sensitivity classifySensitivity(const std::string& filePath) const;

    // Returns true if the path appears to be inside a cloud-synced folder
    // (Dropbox, Google Drive, OneDrive, iCloud).
    bool isCloudFolder(const std::string& filePath) const;

    // Returns true if the path is a cloud provider artifact/metadata file
    // that should not be indexed.
    bool isCloudArtifact(const std::string& filePath) const;

    // Load additional exclusion patterns from a .bsignore file.
    void loadBsignore(const std::string& bsignorePath);

    // Maximum file size for indexing (5 GB).
    static constexpr uint64_t kMaxFileSize = 5ULL * 1024 * 1024 * 1024;

private:
    // Check whether a path matches any of the default exclusion patterns.
    bool matchesDefaultExclusion(const std::string& path) const;

    // Check whether any path component starts with a dot (hidden).
    bool isHiddenPath(const std::string& path) const;

    // Check whether the path is under a sensitive directory.
    bool isSensitivePath(const std::string& path) const;

    // Simple glob matching for default exclusion patterns.
    bool matchSimpleGlob(const std::string& pattern,
                         const std::string& path) const;

    // Recursive glob implementation for individual pattern/path pair.
    bool matchSimpleGlobImpl(const std::string& pattern,
                             const std::string& path) const;

    std::vector<std::string> m_defaultExclusions;
    std::vector<std::string> m_sensitivePatterns;
    BsignoreParser m_bsignoreParser;
};

} // namespace bs
