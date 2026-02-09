#include "core/fs/file_scanner.h"
#include "core/shared/logging.h"

#include <QDir>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <sys/stat.h>

namespace bs {

FileScanner::FileScanner(const PathRules* rules)
    : m_rules(rules)
{
    if (!m_rules) {
        m_rules = &m_ownedRules;
    }
}

std::vector<FileMetadata> FileScanner::scanDirectory(
    const std::string& root) const
{
    std::vector<FileMetadata> results;

    QString qRoot = QString::fromStdString(root);
    QDir rootDir(qRoot);
    if (!rootDir.exists()) {
        LOG_WARN(bsFs, "Scan root does not exist: %s", root.c_str());
        return results;
    }

    LOG_INFO(bsFs, "Starting directory scan: %s", root.c_str());

    uint64_t scannedCount = 0;
    uint64_t excludedCount = 0;

    scanRecursive(qRoot, results, scannedCount, excludedCount);

    LOG_INFO(bsFs, "Scan complete: %s — %" PRIu64 " files, "
                   "%" PRIu64 " excluded",
             root.c_str(), scannedCount, excludedCount);

    return results;
}

void FileScanner::scanRecursive(const QString& dirPath,
                                std::vector<FileMetadata>& results,
                                uint64_t& scannedCount,
                                uint64_t& excludedCount,
                                int depth) const
{
    if (depth >= kMaxDepth) {
        LOG_WARN(bsFs, "Max scan depth (%d) reached at: %s", kMaxDepth,
                 qUtf8Printable(dirPath));
        return;
    }

    QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
        QDir::Name);

    for (const QFileInfo& fi : entries) {
        std::string filePath = fi.absoluteFilePath().toStdString();

        if (fi.isDir()) {
            // Skip symlinked directories to prevent infinite recursion from
            // symlink cycles (e.g. a symlink pointing to a parent directory).
            if (fi.isSymLink()) {
                ++excludedCount;
                continue;
            }

            // PRUNE: check path rules BEFORE recursing into the directory.
            // This avoids walking thousands of files in excluded trees
            // (e.g., .git/, .config/gcloud/virtenv/, Library/Containers/).
            //
            // Append trailing "/" so that glob patterns like "Library/Caches/**"
            // match the directory itself (the /** requires at least a slash
            // after the directory name to start matching).
            std::string dirPathWithSlash = filePath + "/";
            ValidationResult validation = m_rules->validate(dirPathWithSlash);
            if (validation == ValidationResult::Exclude) {
                ++excludedCount;
                continue;
            }

            // Progress logging every 10 000 files
            if ((scannedCount + excludedCount) % 10000 == 0 && (scannedCount + excludedCount) > 0) {
                LOG_INFO(bsFs, "Scan progress: %" PRIu64 " files, %" PRIu64 " excluded, entering %s",
                         scannedCount, excludedCount, filePath.c_str());
            }

            scanRecursive(fi.absoluteFilePath(), results, scannedCount,
                          excludedCount, depth + 1);
            continue;
        }

        // Regular file
        uint64_t fileSize = static_cast<uint64_t>(fi.size());
        ValidationResult validation = m_rules->validate(filePath, fileSize);
        if (validation == ValidationResult::Exclude) {
            ++excludedCount;
            continue;
        }

        ++scannedCount;

        FileMetadata meta;
        meta.filePath = filePath;
        meta.fileName = fi.fileName().toStdString();
        QString ext = fi.suffix().toLower();
        meta.extension = ext.isEmpty() ? std::string() : ("." + ext).toStdString();
        meta.fileSize = fileSize;
        meta.createdAt = static_cast<double>(
            fi.birthTime().toSecsSinceEpoch());
        meta.modifiedAt = static_cast<double>(
            fi.lastModified().toSecsSinceEpoch());

        struct stat st{};
        if (stat(filePath.c_str(), &st) == 0) {
            meta.permissions = static_cast<uint16_t>(st.st_mode & 07777);
            meta.isReadable = (st.st_mode & S_IRUSR) != 0;
            meta.itemKind = classifyItemKind(meta.extension, st.st_mode);
        } else {
            meta.permissions = 0;
            meta.isReadable = fi.isReadable();
            meta.itemKind = classifyItemKind(meta.extension, 0);
        }

        results.push_back(std::move(meta));
    }
}

ItemKind FileScanner::classifyItemKind(const std::string& extension,
                                       mode_t mode)
{
    // Check executable bit first (only for regular files with no extension
    // or unknown extension).
    if (extension.empty() && mode != 0 && (mode & S_IXUSR)) {
        return ItemKind::Binary;
    }

    if (extension.empty()) {
        return ItemKind::Unknown;
    }

    // Strip leading dot: the spec stores extensions as ".txt" but the
    // internal lookup table uses bare extensions like "txt".
    std::string lower = extension;
    if (!lower.empty() && lower[0] == '.') {
        lower.erase(0, 1);
    }

    // Normalise to lowercase for lookup.
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    const auto& map = extensionMap();
    auto it = map.find(lower);
    if (it != map.end()) {
        return it->second;
    }

    // Executable bit with an unrecognised extension.
    if (mode != 0 && (mode & S_IXUSR)) {
        return ItemKind::Binary;
    }

    return ItemKind::Unknown;
}

std::unordered_map<std::string, ItemKind> FileScanner::buildExtensionMap()
{
    std::unordered_map<std::string, ItemKind> map;

    // ── Text files ────────────────────────────────────────────────
    for (const auto* ext : {
        "txt", "log", "csv", "tsv",
        "ini", "cfg", "conf", "properties",
        "yaml", "yml", "toml",
        "json", "jsonl", "ndjson", "json5",
        "xml", "xsl", "xslt", "xsd", "dtd",
        "html", "htm", "xhtml",
        "css", "scss", "sass", "less", "styl",
        "svg",
        "rtf",
        "doc", "docx", "odt",
        "tex", "bib", "sty", "cls",
        "env", "envrc",
        "editorconfig", "gitignore", "gitattributes", "gitmodules",
        "dockerignore", "hgignore",
        "makefile", "cmake",
        "dockerfile",
        "vagrantfile",
        "procfile",
        "gemfile",
        "rakefile",
        "podfile",
        "license",
        "changelog",
        "authors",
        "todo",
        "readme",
        "manifest",
        "lock",
        "bat", "cmd", "ps1", "psm1",
        "fish",
        "zsh", "bash", "bashrc", "zshrc", "profile",
        "sh",
        "awk", "sed",
        "diff", "patch",
        "plist",
         "reg",
         "inf", "desktop",
         "xlsx", "xls", "pptx", "ppt",
         "numbers", "pages", "key",
         "service", "timer", "socket", "path",
     }) {
         map[ext] = ItemKind::Text;
     }

    // ── Code files ────────────────────────────────────────────────
    for (const auto* ext : {
        // C / C++
        "c", "h", "cpp", "cxx", "cc", "c++",
        "hpp", "hxx", "hh", "h++",
        "inl", "ipp", "tcc", "tpp",
        // Objective-C / Objective-C++
        "m", "mm",
        // Swift
        "swift",
        // Rust
        "rs",
        // Go
        "go",
        // Python
        "py", "pyi", "pyw", "pyx", "pxd",
        // JavaScript / TypeScript
        "js", "jsx", "mjs", "cjs",
        "ts", "tsx", "mts", "cts",
        // Java / Kotlin / Scala
        "java", "kt", "kts", "scala", "sc",
        "groovy", "gradle",
        // C#
        "cs", "csx",
        // F#
        "fs", "fsi", "fsx",
        // Ruby
        "rb", "erb", "rake",
        // PHP
        "php", "phtml", "php3", "php4", "php5", "phps",
        // Perl
        "pl", "pm", "t", "pod",
        // Lua
        "lua",
        // R
        "r", "rmd",
        // Julia
        "jl",
        // Haskell
        "hs", "lhs",
        // Elixir / Erlang
        "ex", "exs", "erl", "hrl",
        // Clojure
        "clj", "cljs", "cljc", "edn",
        // OCaml / ReasonML
        "ml", "mli", "re", "rei",
        // Dart
        "dart",
        // Zig
        "zig",
        // Nim
        "nim", "nims",
        // V
        "v",
        // D
        "d",
        // Assembly
        "asm", "s",
        // SQL
        "sql",
        // GraphQL
        "graphql", "gql",
        // Protocol Buffers / Thrift / FlatBuffers
        "proto", "thrift", "fbs",
        // Shader languages
        "glsl", "hlsl", "wgsl", "vert", "frag", "comp",
        // Config as code
        "tf", "hcl",
        "nix",
        // Build systems
        "bzl", "bazel",
        "meson",
        // Templating
        "j2", "jinja", "jinja2",
        "mustache", "handlebars", "hbs",
        "ejs",
        "liquid",
        // Lisp family
        "el", "lisp", "cl", "scm", "rkt",
        // Fortran
        "f", "f90", "f95", "f03", "f08", "for",
        // COBOL
        "cob", "cbl",
        // Pascal / Delphi
        "pas", "pp", "dpr",
        // Ada
        "adb", "ads",
        // Smalltalk
        "st",
        // Tcl
        "tcl",
        // Verilog / VHDL
        "sv", "svh", "vhd", "vhdl",
        // Wasm
        "wat", "wast",
    }) {
        map[ext] = ItemKind::Code;
    }

    // ── Markdown ──────────────────────────────────────────────────
    for (const auto* ext : {
        "md", "mdx", "markdown", "mdown", "mkd", "mkdn",
    }) {
        map[ext] = ItemKind::Markdown;
    }

    // ── PDF ───────────────────────────────────────────────────────
    map["pdf"] = ItemKind::Pdf;

    // ── Image ─────────────────────────────────────────────────────
    for (const auto* ext : {
        "png", "jpg", "jpeg", "webp", "bmp",
        "tiff", "tif", "gif",
        "heif", "heic",
        "ico", "icns",
        "psd", "ai", "eps",
        "raw", "cr2", "nef", "arw", "dng",
        "exr", "hdr",
    }) {
        map[ext] = ItemKind::Image;
    }

    // ── Archive ───────────────────────────────────────────────────
    for (const auto* ext : {
        "zip", "tar", "gz", "bz2", "xz", "zst",
        "rar", "7z",
        "dmg", "iso", "img",
        "cab", "msi",
        "deb", "rpm", "pkg",
        "jar", "war", "ear",
        "whl",
        "apk", "ipa",
    }) {
        map[ext] = ItemKind::Archive;
    }

    // ── Binary (by extension) ─────────────────────────────────────
    for (const auto* ext : {
        "exe", "dll", "so", "dylib", "a", "lib",
        "o", "obj",
        "class",
        "pyc", "pyo",
        "wasm",
        "bin",
        "dat",
        "db", "sqlite", "sqlite3",
    }) {
        map[ext] = ItemKind::Binary;
    }

    return map;
}

const std::unordered_map<std::string, ItemKind>& FileScanner::extensionMap()
{
    static const auto map = buildExtensionMap();
    return map;
}

} // namespace bs
