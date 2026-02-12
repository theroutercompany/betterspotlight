#pragma once

#include <QString>
#include <cstdint>
#include <optional>
#include <string>

namespace bs {

// File type classification (doc 03 Stage 4, doc 04 Section 3.1)
enum class ItemKind {
    Directory,
    Text,
    Code,
    Markdown,
    Pdf,
    Image,
    Archive,
    Binary,
    Unknown,
};

QString itemKindToString(ItemKind kind);
ItemKind itemKindFromString(const QString& str);

// Sensitivity level for indexed items (doc 04 Section 3.1)
enum class Sensitivity {
    Normal,
    Sensitive,
    Hidden,
};

QString sensitivityToString(Sensitivity s);
Sensitivity sensitivityFromString(const QString& str);

// Path validation result (doc 03 Stage 3)
enum class ValidationResult {
    Include,
    MetadataOnly,
    Exclude,
};

// Work queue item type (doc 03 Stage 2)
struct WorkItem {
    enum class Type {
        Delete,
        ModifiedContent,
        NewFile,
        RescanDirectory,
    };

    Type type;
    std::string filePath;
    std::optional<uint64_t> knownModTime;
    std::optional<uint64_t> knownSize;
    int retryCount = 0;
    bool rebuildLane = false;
};

// Filesystem metadata extracted in Stage 4
struct FileMetadata {
    std::string filePath;
    std::string fileName;
    std::string extension;
    uint64_t fileSize = 0;
    double createdAt = 0.0;
    double modifiedAt = 0.0;
    uint16_t permissions = 0;
    bool isReadable = false;
    ItemKind itemKind = ItemKind::Unknown;
};

} // namespace bs
