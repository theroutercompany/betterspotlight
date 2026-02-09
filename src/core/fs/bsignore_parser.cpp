#include "core/fs/bsignore_parser.h"
#include "core/shared/logging.h"

#include <QFile>
#include <QTextStream>
#include <cstring>
#include <sstream>

namespace bs {

bool BsignoreParser::loadFromFile(const std::string& path)
{
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        LOG_WARN(bsFs, "Failed to open .bsignore file: %s", path.c_str());
        return false;
    }

    QTextStream in(&file);
    std::string content = in.readAll().toStdString();
    file.close();

    parseLines(content);
    LOG_INFO(bsFs, "Loaded %zu patterns from %s",
             m_patterns.size(), path.c_str());
    return true;
}

bool BsignoreParser::loadFromString(const std::string& content)
{
    parseLines(content);
    return true;
}

void BsignoreParser::clear()
{
    m_patterns.clear();
}

void BsignoreParser::parseLines(const std::string& content)
{
    m_patterns.clear();

    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        // Trim trailing whitespace (including \r from CRLF files).
        while (!line.empty() &&
               (line.back() == ' ' || line.back() == '\t' ||
                line.back() == '\r')) {
            line.pop_back();
        }

        // Trim leading whitespace.
        size_t start = 0;
        while (start < line.size() &&
               (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        if (start > 0) {
            line = line.substr(start);
        }

        // Skip empty lines and comments.
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Negation patterns (!) are parsed but ignored for M1.
        if (line[0] == '!') {
            LOG_DEBUG(bsFs, "Ignoring negation pattern: %s", line.c_str());
            continue;
        }

        m_patterns.push_back(line);
    }
}

bool BsignoreParser::matches(const std::string& filePath) const
{
    for (const auto& pattern : m_patterns) {
        std::string effectivePattern = pattern;

        // Strip trailing '/' (directory-only indicator) — we don't
        // differentiate at match time; the caller can append '/' for dirs.
        if (!effectivePattern.empty() && effectivePattern.back() == '/') {
            effectivePattern.pop_back();
        }

        if (matchGlob(effectivePattern, filePath)) {
            return true;
        }
    }
    return false;
}

bool BsignoreParser::matchGlob(const std::string& pattern,
                                const std::string& path) const
{
    // If pattern contains no leading '/', it can match against any path
    // component suffix. Try matching against the full path and also
    // against each suffix starting after a '/'.
    if (pattern.find('/') == std::string::npos) {
        // Simple filename-only pattern — match against each path component.
        // Also try the full path for single-component paths.
        size_t pos = 0;
        while (pos < path.size()) {
            size_t slash = path.find('/', pos);
            std::string component;
            if (slash == std::string::npos) {
                component = path.substr(pos);
            } else {
                component = path.substr(pos, slash - pos);
            }
            if (!component.empty() &&
                matchGlobImpl(pattern.c_str(), component.c_str())) {
                return true;
            }
            if (slash == std::string::npos) {
                break;
            }
            pos = slash + 1;
        }
        return false;
    }

    // Pattern contains '/' — match against suffixes of the path.
    // First try the full path.
    if (matchGlobImpl(pattern.c_str(), path.c_str())) {
        return true;
    }

    // Try each suffix starting after a '/'.
    size_t pos = 0;
    while (pos < path.size()) {
        pos = path.find('/', pos);
        if (pos == std::string::npos) {
            break;
        }
        ++pos; // skip the '/'
        if (pos < path.size() &&
            matchGlobImpl(pattern.c_str(), path.c_str() + pos)) {
            return true;
        }
    }

    return false;
}

bool BsignoreParser::matchGlobImpl(const char* pattern,
                                    const char* path) const
{
    while (*pattern && *path) {
        if (*pattern == '*') {
            // Check for '**' (matches everything including /).
            if (*(pattern + 1) == '*') {
                pattern += 2;

                // '**/' or '**' at end — skip the optional trailing '/'.
                if (*pattern == '/') {
                    ++pattern;
                }

                // '**' at end of pattern matches everything remaining.
                if (*pattern == '\0') {
                    return true;
                }

                // Try matching the rest of the pattern at every position.
                for (const char* p = path; *p; ++p) {
                    if (matchGlobImpl(pattern, p)) {
                        return true;
                    }
                }
                // Also try matching at the very end (empty remaining path).
                return matchGlobImpl(pattern, path + strlen(path));
            }

            // Single '*' — matches any characters except '/'.
            ++pattern;

            // '*' at end of pattern matches everything remaining (no /).
            if (*pattern == '\0') {
                // Succeeds only if no '/' in remaining path.
                return strchr(path, '/') == nullptr;
            }

            // Try matching the rest at every position (but not past '/').
            for (const char* p = path; *p && *p != '/'; ++p) {
                if (matchGlobImpl(pattern, p)) {
                    return true;
                }
            }
            // Also try matching at current position (zero-length match
            // already tried by for loop at p=path, but handle edge case
            // where path starts with the next pattern char).
            return matchGlobImpl(pattern, path);
        }

        if (*pattern == '?') {
            // '?' matches any single character except '/'.
            if (*path == '/') {
                return false;
            }
            ++pattern;
            ++path;
            continue;
        }

        // Literal character match.
        if (*pattern != *path) {
            return false;
        }

        ++pattern;
        ++path;
    }

    // Consume trailing '*' or '**' in pattern.
    while (*pattern == '*') {
        ++pattern;
    }

    return *pattern == '\0' && *path == '\0';
}

} // namespace bs
