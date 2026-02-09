#include "core/extraction/mdls_text_extractor.h"

#include "core/shared/logging.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>

namespace bs {

namespace {

constexpr int64_t kMaxFileSizeBytes = 50 * 1024 * 1024;
constexpr int kExtractorTimeoutMs = 30000;

const QSet<QString>& mdlsSupportedExtensions()
{
    static const QSet<QString> exts = {
        QStringLiteral("xlsx"),
        QStringLiteral("xls"),
        QStringLiteral("pptx"),
        QStringLiteral("ppt"),
        QStringLiteral("numbers"),
        QStringLiteral("pages"),
        QStringLiteral("key"),
    };
    return exts;
}

QString decodeMdlsEscapes(const QString& input)
{
    QString out;
    out.reserve(input.size());

    bool escaping = false;
    for (QChar ch : input) {
        if (!escaping) {
            if (ch == QLatin1Char('\\')) {
                escaping = true;
            } else {
                out.append(ch);
            }
            continue;
        }

        switch (ch.unicode()) {
        case 'n': out.append(QLatin1Char('\n')); break;
        case 'r': out.append(QLatin1Char('\r')); break;
        case 't': out.append(QLatin1Char('\t')); break;
        case '"': out.append(QLatin1Char('"')); break;
        case '\\': out.append(QLatin1Char('\\')); break;
        default:
            out.append(ch);
            break;
        }

        escaping = false;
    }

    if (escaping) {
        out.append(QLatin1Char('\\'));
    }

    return out;
}

QString parseMdlsTextValue(const QString& output)
{
    static const QRegularExpression kPrefix(
        QStringLiteral("kMDItemTextContent\\s*=\\s*(.*)$"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch prefixMatch = kPrefix.match(output);
    if (!prefixMatch.hasMatch()) {
        return QString();
    }

    const QString rawValue = prefixMatch.captured(1).trimmed();
    if (rawValue.isEmpty() || rawValue == QStringLiteral("(null)")) {
        return QString();
    }

    static const QRegularExpression kQuoted(
        QStringLiteral("\"((?:\\\\.|[^\\\"])*)\""),
        QRegularExpression::DotMatchesEverythingOption);

    QStringList parts;
    QRegularExpressionMatchIterator it = kQuoted.globalMatch(rawValue);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        parts.append(decodeMdlsEscapes(m.captured(1)));
    }

    if (!parts.isEmpty()) {
        return parts.join(QLatin1Char('\n')).trimmed();
    }

    return rawValue.trimmed();
}

ExtractionResult runProcess(
    const QString& program,
    const QStringList& args,
    const QString& timeoutMessage,
    QElapsedTimer& timer)
{
    QProcess process;
    process.start(program, args);

    ExtractionResult result;

    if (!process.waitForStarted(kExtractorTimeoutMs)) {
        result.status = ExtractionResult::Status::UnsupportedFormat;
        result.errorMessage = QStringLiteral("Failed to start process: %1").arg(program);
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (!process.waitForFinished(kExtractorTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        result.status = ExtractionResult::Status::Timeout;
        result.errorMessage = timeoutMessage;
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
        result.status = ExtractionResult::Status::UnsupportedFormat;
        result.errorMessage = stderrText.isEmpty()
                                  ? QStringLiteral("Process failed: %1").arg(program)
                                  : QStringLiteral("%1 failed: %2").arg(program, stderrText.left(300));
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    result.status = ExtractionResult::Status::Success;
    result.content = QString::fromUtf8(process.readAllStandardOutput());
    result.durationMs = static_cast<int>(timer.elapsed());
    return result;
}

} // anonymous namespace

bool MdlsTextExtractor::supports(const QString& extension) const
{
    return mdlsSupportedExtensions().contains(extension.toLower());
}

ExtractionResult MdlsTextExtractor::extract(const QString& filePath)
{
    QElapsedTimer timer;
    timer.start();

    ExtractionResult result;

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
        LOG_INFO(bsExtraction, "Skipping oversized file for mdls extraction: %s (%lld bytes)",
                 qUtf8Printable(filePath),
                 static_cast<long long>(info.size()));
        return result;
    }

    LOG_DEBUG(bsExtraction, "Running mdimport for %s", qUtf8Printable(filePath));
    ExtractionResult mdimportResult = runProcess(
        QStringLiteral("/usr/bin/mdimport"),
        {filePath},
        QStringLiteral("mdimport timed out"),
        timer);

    if (mdimportResult.status == ExtractionResult::Status::Timeout) {
        LOG_INFO(bsExtraction, "mdimport timed out for %s", qUtf8Printable(filePath));
        return mdimportResult;
    }

    LOG_DEBUG(bsExtraction, "Running mdls kMDItemTextContent for %s", qUtf8Printable(filePath));
    ExtractionResult mdlsResult = runProcess(
        QStringLiteral("/usr/bin/mdls"),
        {QStringLiteral("-name"), QStringLiteral("kMDItemTextContent"), filePath},
        QStringLiteral("mdls timed out"),
        timer);

    if (mdlsResult.status == ExtractionResult::Status::Timeout) {
        LOG_INFO(bsExtraction, "mdls timed out for %s", qUtf8Printable(filePath));
        return mdlsResult;
    }

    if (mdlsResult.status != ExtractionResult::Status::Success || !mdlsResult.content.has_value()) {
        LOG_DEBUG(bsExtraction, "mdls did not return content for %s", qUtf8Printable(filePath));
        result.status = ExtractionResult::Status::UnsupportedFormat;
        result.errorMessage = mdlsResult.errorMessage.value_or(QStringLiteral("mdls failed"));
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    const QString parsedText = parseMdlsTextValue(mdlsResult.content.value());
    if (parsedText.isEmpty()) {
        LOG_DEBUG(bsExtraction, "kMDItemTextContent is null/empty for %s", qUtf8Printable(filePath));
        result.status = ExtractionResult::Status::UnsupportedFormat;
        result.errorMessage = QStringLiteral("kMDItemTextContent is empty");
        result.durationMs = static_cast<int>(timer.elapsed());
        return result;
    }

    result.status = ExtractionResult::Status::Success;
    result.content = parsedText;
    result.durationMs = static_cast<int>(timer.elapsed());

    LOG_INFO(bsExtraction, "Extracted mdls text for %s (%lld chars)",
             qUtf8Printable(filePath),
             static_cast<long long>(parsedText.size()));

    return result;
}

} // namespace bs
