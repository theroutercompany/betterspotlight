#include "core/extraction/text_extractor.h"
#include "core/shared/logging.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStringConverter>

namespace bs {

namespace {

// 50 MB — files beyond this are too large for full-text indexing
constexpr int64_t kMaxFileSizeBytes = 50 * 1024 * 1024;

const QSet<QString>& officeExtensions()
{
    static const QSet<QString> exts = {
        QStringLiteral("doc"),
        QStringLiteral("docx"),
        QStringLiteral("rtf"),
        QStringLiteral("odt"),
    };
    return exts;
}

} // anonymous namespace

const QSet<QString>& TextExtractor::supportedExtensions()
{
    // Built once, lives for the process lifetime.
    static const QSet<QString> exts = {
        // Plain text
        QStringLiteral("txt"),
        QStringLiteral("text"),
        QStringLiteral("log"),
        QStringLiteral("readme"),
        QStringLiteral("changelog"),
        QStringLiteral("license"),
        QStringLiteral("authors"),
        QStringLiteral("todo"),
        QStringLiteral("notes"),

        // Markup / documentation
        QStringLiteral("md"),
        QStringLiteral("markdown"),
        QStringLiteral("rst"),
        QStringLiteral("adoc"),
        QStringLiteral("asciidoc"),
        QStringLiteral("textile"),
        QStringLiteral("org"),
        QStringLiteral("wiki"),
        QStringLiteral("tex"),
        QStringLiteral("latex"),
        QStringLiteral("bib"),
        QStringLiteral("doc"),
        QStringLiteral("docx"),
        QStringLiteral("rtf"),
        QStringLiteral("odt"),

        // Web / markup
        QStringLiteral("html"),
        QStringLiteral("htm"),
        QStringLiteral("xhtml"),
        QStringLiteral("xml"),
        QStringLiteral("xsl"),
        QStringLiteral("xslt"),
        QStringLiteral("svg"),
        QStringLiteral("css"),
        QStringLiteral("scss"),
        QStringLiteral("sass"),
        QStringLiteral("less"),
        QStringLiteral("styl"),

        // JavaScript / TypeScript
        QStringLiteral("js"),
        QStringLiteral("jsx"),
        QStringLiteral("ts"),
        QStringLiteral("tsx"),
        QStringLiteral("mjs"),
        QStringLiteral("cjs"),
        QStringLiteral("vue"),
        QStringLiteral("svelte"),

        // C / C++
        QStringLiteral("c"),
        QStringLiteral("h"),
        QStringLiteral("cpp"),
        QStringLiteral("cxx"),
        QStringLiteral("cc"),
        QStringLiteral("hpp"),
        QStringLiteral("hxx"),
        QStringLiteral("hh"),
        QStringLiteral("ipp"),
        QStringLiteral("inl"),

        // C# / .NET
        QStringLiteral("cs"),
        QStringLiteral("csx"),
        QStringLiteral("fs"),
        QStringLiteral("fsx"),
        QStringLiteral("fsi"),
        QStringLiteral("vb"),

        // Java / JVM
        QStringLiteral("java"),
        QStringLiteral("kt"),
        QStringLiteral("kts"),
        QStringLiteral("scala"),
        QStringLiteral("sc"),
        QStringLiteral("groovy"),
        QStringLiteral("gradle"),
        QStringLiteral("clj"),
        QStringLiteral("cljs"),
        QStringLiteral("cljc"),
        QStringLiteral("edn"),

        // Systems languages
        QStringLiteral("rs"),
        QStringLiteral("go"),
        QStringLiteral("swift"),
        QStringLiteral("m"),
        QStringLiteral("mm"),
        QStringLiteral("zig"),
        QStringLiteral("nim"),
        QStringLiteral("d"),
        QStringLiteral("v"),

        // Scripting
        QStringLiteral("py"),
        QStringLiteral("pyi"),
        QStringLiteral("pyw"),
        QStringLiteral("rb"),
        QStringLiteral("rbw"),
        QStringLiteral("pl"),
        QStringLiteral("pm"),
        QStringLiteral("t"),
        QStringLiteral("php"),
        QStringLiteral("phps"),
        QStringLiteral("lua"),
        QStringLiteral("tcl"),
        QStringLiteral("r"),
        QStringLiteral("rmd"),
        QStringLiteral("jl"),
        QStringLiteral("ex"),
        QStringLiteral("exs"),
        QStringLiteral("erl"),
        QStringLiteral("hrl"),
        QStringLiteral("hs"),
        QStringLiteral("lhs"),
        QStringLiteral("ml"),
        QStringLiteral("mli"),
        QStringLiteral("sml"),

        // Shell
        QStringLiteral("sh"),
        QStringLiteral("bash"),
        QStringLiteral("zsh"),
        QStringLiteral("fish"),
        QStringLiteral("csh"),
        QStringLiteral("ksh"),
        QStringLiteral("bat"),
        QStringLiteral("cmd"),
        QStringLiteral("ps1"),
        QStringLiteral("psm1"),
        QStringLiteral("psd1"),

        // Data / config
        QStringLiteral("json"),
        QStringLiteral("jsonl"),
        QStringLiteral("jsonc"),
        QStringLiteral("json5"),
        QStringLiteral("yaml"),
        QStringLiteral("yml"),
        QStringLiteral("toml"),
        QStringLiteral("ini"),
        QStringLiteral("cfg"),
        QStringLiteral("conf"),
        QStringLiteral("config"),
        QStringLiteral("properties"),
        QStringLiteral("env"),
        QStringLiteral("csv"),
        QStringLiteral("tsv"),
        QStringLiteral("sql"),
        QStringLiteral("graphql"),
        QStringLiteral("gql"),
        QStringLiteral("proto"),
        QStringLiteral("thrift"),
        QStringLiteral("avsc"),

        // Build / CI
        QStringLiteral("cmake"),
        QStringLiteral("make"),
        QStringLiteral("makefile"),
        QStringLiteral("mk"),
        QStringLiteral("dockerfile"),
        QStringLiteral("vagrantfile"),
        QStringLiteral("rakefile"),
        QStringLiteral("gemfile"),
        QStringLiteral("podfile"),
        QStringLiteral("fastfile"),

        // Misc
        QStringLiteral("diff"),
        QStringLiteral("patch"),
        QStringLiteral("gitignore"),
        QStringLiteral("gitattributes"),
        QStringLiteral("gitmodules"),
        QStringLiteral("editorconfig"),
        QStringLiteral("htaccess"),
        QStringLiteral("nginx"),
        QStringLiteral("tf"),
        QStringLiteral("tfvars"),
        QStringLiteral("hcl"),
        QStringLiteral("plist"),
        QStringLiteral("pbxproj"),
    };
    return exts;
}

bool TextExtractor::supports(const QString& extension) const
{
    return supportedExtensions().contains(extension.toLower());
}

ExtractionResult TextExtractor::extract(const QString& filePath)
{
    QElapsedTimer timer;
    timer.start();

    ExtractionResult result;

    // Check file accessibility and size
    QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        result.status = ExtractionResult::Status::Inaccessible;
        result.errorMessage = QStringLiteral("File does not exist or is not a regular file");
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (!info.isReadable()) {
        result.status = ExtractionResult::Status::Inaccessible;
        result.errorMessage = QStringLiteral("File is not readable");
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (info.size() > kMaxFileSizeBytes) {
        result.status = ExtractionResult::Status::SizeExceeded;
        result.errorMessage = QString("File size %1 bytes exceeds limit of %2 bytes")
                                  .arg(info.size())
                                  .arg(kMaxFileSizeBytes);
        result.durationMs = static_cast<int>(timer.elapsed());
        LOG_INFO(bsExtraction, "Skipping oversized file: %s (%lld bytes)",
                 qUtf8Printable(filePath), static_cast<long long>(info.size()));
        return result;
    }

    const QString extension = info.suffix().toLower();
    if (officeExtensions().contains(extension)) {
        // macOS-native Office/RTF/ODT conversion path.
        QProcess textutil;
        QStringList args = {
            QStringLiteral("-convert"),
            QStringLiteral("txt"),
            QStringLiteral("-stdout"),
            filePath,
        };
        textutil.start(QStringLiteral("/usr/bin/textutil"), args);

        if (!textutil.waitForFinished(30000)) {
            textutil.kill();
            textutil.waitForFinished();
            result.status = ExtractionResult::Status::Timeout;
            result.errorMessage = QStringLiteral("textutil conversion timed out");
            result.durationMs = static_cast<int>(timer.elapsed());
            return result;
        }

        const int exitCode = textutil.exitCode();
        const bool ok = (textutil.exitStatus() == QProcess::NormalExit && exitCode == 0);
        if (!ok) {
            const QString stderrText = QString::fromUtf8(textutil.readAllStandardError()).trimmed();
            result.status = ExtractionResult::Status::UnsupportedFormat;
            result.errorMessage = stderrText.isEmpty()
                                      ? QStringLiteral("textutil conversion failed")
                                      : QStringLiteral("textutil conversion failed: %1")
                                            .arg(stderrText.left(200));
            result.durationMs = static_cast<int>(timer.elapsed());
            return result;
        }

        QByteArray rawBytes = textutil.readAllStandardOutput();
        QString decoded = QString::fromUtf8(rawBytes);
        if (decoded.isNull()) {
            decoded = QString::fromLatin1(rawBytes);
        }

        result.status = ExtractionResult::Status::Success;
        result.content = std::move(decoded);
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    // Open and read file contents
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.status = ExtractionResult::Status::Inaccessible;
        result.errorMessage = QString("Failed to open file: %1").arg(file.errorString());
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    const QByteArray rawBytes = file.readAll();
    file.close();

    if (rawBytes.isEmpty()) {
        // Empty file is valid — just has no content
        result.status = ExtractionResult::Status::Success;
        result.content = QString();
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    // Attempt UTF-8 decoding first
    QString decoded;
    {
        auto toUtf8 = QStringDecoder(QStringDecoder::Utf8,
                                     QStringDecoder::Flag::Stateless);
        decoded = toUtf8(rawBytes);

        if (toUtf8.hasError()) {
            // UTF-8 failed — fall back to Latin-1 (always succeeds, byte-for-byte)
            decoded = QString::fromLatin1(rawBytes);
            LOG_DEBUG(bsExtraction, "UTF-8 decode failed for %s, using Latin-1 fallback",
                      qUtf8Printable(filePath));
        }
    }

    result.status = ExtractionResult::Status::Success;
    result.content = std::move(decoded);
    result.durationMs = static_cast<int>(timer.elapsed());

    LOG_DEBUG(bsExtraction, "Extracted %lld chars from %s in %d ms",
              static_cast<long long>(result.content->size()),
              qUtf8Printable(filePath),
              result.durationMs);

    return result;
}

} // namespace bs
