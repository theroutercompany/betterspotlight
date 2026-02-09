#include "core/fs/path_rules.h"
#include "core/shared/logging.h"

#include <QFileInfo>
#include <algorithm>
#include <cstring>

namespace bs {

PathRules::PathRules()
{
    // ── Default exclusions (30+ patterns) ──────────────────────────
    // These match gitignore-style patterns against path suffixes.
    m_defaultExclusions = {
        // Version control internals
        ".git/objects/**",
        ".git/hooks/**",
        ".git/refs/**",
        ".git/logs/**",
        ".git/info/**",
        ".git/packed-refs",
        ".git/COMMIT_EDITMSG",
        ".git/MERGE_MSG",
        ".git/index",

        // Node / JS
        "node_modules/**",
        ".yarn/cache/**",
        ".next/**",
        ".nuxt/**",
        "bower_components/**",

        // Build outputs
        "dist/**",
        "build/**",
        "DerivedData/**",
        "cmake-build-*/**",

        // Python
        "__pycache__/**",
        ".tox/**",
        "*.pyc",
        "*.pyo",
        ".eggs/**",
        "*.egg-info/**",

        // Rust / Cargo
        ".cargo/**",
        "target/release/**",
        "target/debug/**",

        // Virtual environments
        "venv/**",
        ".venv/**",

        // Vendor / dependencies
        "vendor/**",

        // macOS system
        "Library/Caches/**",
        "Library/Containers/**",
        "Library/Group Containers/**",
        "Library/Developer/**",
        "Library/Logs/**",
        "Library/Mail/**",
        "Library/Messages/**",
        "Library/Saved Application State/**",
        "Library/Application Support/AddressBook/**",
        "Library/Application Support/CallHistoryDB/**",
        "Library/Application Support/MobileSync/**",
        "Library/Application Support/com.apple.*/**",
        ".DS_Store",
        ".localized",
        "._*",
        ".TemporaryItems/**",
        ".Trashes/**",
        ".fseventsd/**",
        ".Spotlight-V100/**",
        "Thumbs.db",

        // Cloud sync artifacts
        ".dropbox.cache/**",
        ".dropbox/",

        // Cloud temp files
        "Icon\r",
        ".gdoc.tmp",
        ".gsheet.tmp",

        // IDE / editor
        ".idea/**",
        ".vscode/**",
        "*.swp",
        "*.swo",
        "*~",
    };

    // ── Sensitive path patterns ────────────────────────────────────
    m_sensitivePatterns = {
        ".ssh/",
        ".gnupg/",
        ".gpg/",
        ".aws/",
        "Library/Preferences/",
        "Library/Keychains/",
    };
}

ValidationResult PathRules::validate(const std::string& filePath,
                                     uint64_t fileSize) const
{
    // Decision table (doc 03 Stage 3), evaluated in order:

    // 1. .bsignore match -> Exclude
    if (m_bsignoreParser.matches(filePath)) {
        return ValidationResult::Exclude;
    }

    // 2. Built-in exclusion match -> Exclude
    if (matchesDefaultExclusion(filePath)) {
        return ValidationResult::Exclude;
    }

    // 3. Cloud artifact -> Exclude
    if (isCloudArtifact(filePath)) {
        return ValidationResult::Exclude;
    }

    // 4. Hidden path (dot-prefixed directory) -> Exclude
    //    Only hidden directories, not files that happen to start with '.'.
    //    Cloud and bsignore-handled paths are already caught above.
    if (isHiddenPath(filePath)) {
        return ValidationResult::Exclude;
    }

    // 5. Sensitive path -> MetadataOnly
    if (isSensitivePath(filePath)) {
        return ValidationResult::MetadataOnly;
    }

    // 6. Size > 5 GB -> Exclude
    if (fileSize > 0 && fileSize > kMaxFileSize) {
        return ValidationResult::Exclude;
    }

    // 7. Otherwise -> Include
    return ValidationResult::Include;
}

Sensitivity PathRules::classifySensitivity(const std::string& filePath) const
{
    if (isSensitivePath(filePath)) {
        return Sensitivity::Sensitive;
    }
    if (isHiddenPath(filePath)) {
        return Sensitivity::Hidden;
    }
    return Sensitivity::Normal;
}

bool PathRules::isCloudFolder(const std::string& filePath) const
{
    // Detect cloud-synced folder roots by checking for provider marker files.
    // We check if the path contains known cloud provider directory names.

    // Dropbox: path contains a directory with .dropbox or .dropbox-dist
    if (filePath.find("Dropbox/") != std::string::npos ||
        filePath.find("Dropbox\\") != std::string::npos) {
        return true;
    }

    // Google Drive: path contains "Google Drive" or "My Drive"
    if (filePath.find("Google Drive/") != std::string::npos ||
        filePath.find("GoogleDrive/") != std::string::npos ||
        filePath.find("My Drive/") != std::string::npos) {
        return true;
    }

    // OneDrive
    if (filePath.find("OneDrive/") != std::string::npos ||
        filePath.find("OneDrive -") != std::string::npos) {
        return true;
    }

    // iCloud Drive
    if (filePath.find("iCloud Drive/") != std::string::npos ||
        filePath.find("Mobile Documents/") != std::string::npos) {
        return true;
    }

    return false;
}

bool PathRules::isCloudArtifact(const std::string& filePath) const
{
    // Cloud provider metadata / artifact files that should never be indexed.

    // Dropbox
    if (filePath.find(".dropbox.cache/") != std::string::npos) {
        return true;
    }
    // .dropbox directory itself (sync metadata)
    if (filePath.size() >= 9) {
        auto pos = filePath.rfind('/');
        std::string basename;
        if (pos != std::string::npos) {
            basename = filePath.substr(pos + 1);
        } else {
            basename = filePath;
        }
        if (basename == ".dropbox" || basename == ".dropbox-dist") {
            return true;
        }
    }

    // Google Drive
    if (filePath.find(".~google-drive-root") != std::string::npos) {
        return true;
    }
    {
        auto pos = filePath.rfind('/');
        std::string basename;
        if (pos != std::string::npos) {
            basename = filePath.substr(pos + 1);
        } else {
            basename = filePath;
        }
        if (basename == ".gdoc.tmp" || basename == ".gsheet.tmp") {
            return true;
        }
    }

    // OneDrive
    if (filePath.find("OneDrive_folder_placeholder.ini") != std::string::npos) {
        return true;
    }

    // iCloud
    if (filePath.find(".icloud_folder_attributes.plist") != std::string::npos) {
        return true;
    }
    // iCloud placeholder files (.icloud extension in Mobile Documents)
    {
        auto pos = filePath.rfind('.');
        if (pos != std::string::npos) {
            std::string ext = filePath.substr(pos);
            if (ext == ".icloud" &&
                filePath.find("Mobile Documents/") != std::string::npos) {
                return true;
            }
        }
    }

    return false;
}

void PathRules::loadBsignore(const std::string& bsignorePath)
{
    if (m_bsignoreParser.loadFromFile(bsignorePath)) {
        LOG_INFO(bsFs, "Loaded .bsignore from %s (%zu patterns)",
                 bsignorePath.c_str(), m_bsignoreParser.patterns().size());
    }
}

bool PathRules::matchesDefaultExclusion(const std::string& path) const
{
    for (const auto& pattern : m_defaultExclusions) {
        if (matchSimpleGlob(pattern, path)) {
            return true;
        }
    }
    return false;
}

bool PathRules::isHiddenPath(const std::string& path) const
{
    static const std::vector<std::string> allowedDotDirs = {
        // Dev toolchains — index normally
        ".config/",
        ".local/",
        ".cargo/",
        ".rustup/",
        ".npm/",
        ".nvm/",
        ".pyenv/",
        ".rbenv/",
        ".sdkman/",
        ".gradle/",
        ".m2/",
        ".docker/",
        ".kube/",
        ".terraform.d/",
        ".bundle/",
        // Sensitive dirs — must pass through to isSensitivePath() for MetadataOnly
        ".ssh/",
        ".gnupg/",
        ".gpg/",
        ".aws/",
    };

    // Check only directory components (not the filename itself).
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) {
            end = path.size();
        }

        if (end > start) {
            const bool isLastComponent = (end == path.size());
            const std::string component = path.substr(start, end - start);
            if (!isLastComponent && component[0] == '.') {
                std::string dotDirPattern = component;
                dotDirPattern.push_back('/');

                const bool isAllowedDotDir =
                    std::find(allowedDotDirs.begin(),
                              allowedDotDirs.end(),
                              dotDirPattern) != allowedDotDirs.end();
                if (!isAllowedDotDir) {
                    return true;
                }
            }
        }

        if (end == path.size()) {
            break;
        }
        start = end + 1;
    }

    return false;
}

bool PathRules::isSensitivePath(const std::string& path) const
{
    for (const auto& pattern : m_sensitivePatterns) {
        if (path.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool PathRules::matchSimpleGlob(const std::string& pattern,
                                const std::string& path) const
{
    // For default exclusion patterns, we use the BsignoreParser's glob
    // matching logic. We create a temporary parser-like match.
    // However, to avoid overhead, we implement a simplified version here.

    // If pattern has no '/', match against each path component.
    if (pattern.find('/') == std::string::npos) {
        // Match against basename or each component.
        size_t pos = 0;
        while (pos < path.size()) {
            size_t slash = path.find('/', pos);
            std::string component;
            if (slash == std::string::npos) {
                component = path.substr(pos);
            } else {
                component = path.substr(pos, slash - pos);
            }
            if (!component.empty() && matchSimpleGlobImpl(pattern, component)) {
                return true;
            }
            if (slash == std::string::npos) {
                break;
            }
            pos = slash + 1;
        }
        return false;
    }

    // Pattern contains '/' — match against path suffixes.
    if (matchSimpleGlobImpl(pattern, path)) {
        return true;
    }

    size_t pos = 0;
    while (pos < path.size()) {
        pos = path.find('/', pos);
        if (pos == std::string::npos) {
            break;
        }
        ++pos;
        if (pos < path.size() &&
            matchSimpleGlobImpl(pattern, path.substr(pos))) {
            return true;
        }
    }

    return false;
}

// We need a private helper — declared inline since it's only used here.
// This mirrors BsignoreParser::matchGlobImpl but is a member of PathRules.
namespace {

bool matchGlobHelper(const char* pattern, const char* path)
{
    while (*pattern && *path) {
        if (*pattern == '*') {
            if (*(pattern + 1) == '*') {
                pattern += 2;
                if (*pattern == '/') {
                    ++pattern;
                }
                if (*pattern == '\0') {
                    return true;
                }
                for (const char* p = path; *p; ++p) {
                    if (matchGlobHelper(pattern, p)) {
                        return true;
                    }
                }
                return matchGlobHelper(pattern, path + strlen(path));
            }

            ++pattern;
            if (*pattern == '\0') {
                return strchr(path, '/') == nullptr;
            }
            for (const char* p = path; *p && *p != '/'; ++p) {
                if (matchGlobHelper(pattern, p)) {
                    return true;
                }
            }
            return matchGlobHelper(pattern, path);
        }

        if (*pattern == '?') {
            if (*path == '/') {
                return false;
            }
            ++pattern;
            ++path;
            continue;
        }

        if (*pattern != *path) {
            return false;
        }

        ++pattern;
        ++path;
    }

    while (*pattern == '*') {
        ++pattern;
    }

    return *pattern == '\0' && *path == '\0';
}

} // anonymous namespace

// Trampoline to anonymous-namespace helper.
bool PathRules::matchSimpleGlobImpl(const std::string& pattern,
                                    const std::string& path) const
{
    return matchGlobHelper(pattern.c_str(), path.c_str());
}

} // namespace bs
