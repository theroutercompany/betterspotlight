#include "extractor_service.h"
#include "core/ipc/message.h"
#include "core/shared/logging.h"
#include "core/shared/types.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>

#include <sys/stat.h>
#include <pwd.h>

namespace bs {

ExtractorService::ExtractorService(QObject* parent)
    : ServiceBase(QStringLiteral("extractor"), parent)
{
    LOG_INFO(bsIpc, "ExtractorService created");
}

QJsonObject ExtractorService::handleRequest(const QJsonObject& request)
{
    QString method = request.value(QStringLiteral("method")).toString();
    uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
    QJsonObject params = request.value(QStringLiteral("params")).toObject();

    if (method == QLatin1String("extractText"))      return handleExtractText(id, params);
    if (method == QLatin1String("extractMetadata"))   return handleExtractMetadata(id, params);
    if (method == QLatin1String("isSupported"))       return handleIsSupported(id, params);
    if (method == QLatin1String("cancelExtraction"))  return handleCancelExtraction(id, params);

    // Fall through to base (ping, shutdown, unknown)
    return ServiceBase::handleRequest(request);
}

QJsonObject ExtractorService::handleExtractText(uint64_t id, const QJsonObject& params)
{
    QString path = params.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'path' parameter"));
    }

    QString kindStr = params.value(QStringLiteral("kind")).toString();
    if (kindStr.isEmpty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'kind' parameter"));
    }

    ItemKind kind = itemKindFromString(kindStr);

    LOG_INFO(bsIpc, "Extracting text from: %s (kind=%s)",
             qPrintable(path), qPrintable(kindStr));

    ExtractionResult extraction = m_extractor.extract(path, kind);

    if (extraction.status != ExtractionResult::Status::Success) {
        // Map extraction status to IPC error code
        IpcErrorCode errorCode = IpcErrorCode::InternalError;
        switch (extraction.status) {
        case ExtractionResult::Status::Timeout:
            errorCode = IpcErrorCode::Timeout;
            break;
        case ExtractionResult::Status::UnsupportedFormat:
            errorCode = IpcErrorCode::Unsupported;
            break;
        case ExtractionResult::Status::Inaccessible:
            errorCode = IpcErrorCode::PermissionDenied;
            break;
        case ExtractionResult::Status::CorruptedFile:
            errorCode = IpcErrorCode::CorruptedIndex;
            break;
        case ExtractionResult::Status::SizeExceeded:
            errorCode = IpcErrorCode::InvalidParams;
            break;
        default:
            errorCode = IpcErrorCode::InternalError;
            break;
        }

        return IpcMessage::makeError(
            id, errorCode,
            extraction.errorMessage.value_or(QStringLiteral("Extraction failed")));
    }

    QJsonObject result;
    result[QStringLiteral("text")] = extraction.content.value_or(QString());
    result[QStringLiteral("metadata")] = QJsonObject();
    result[QStringLiteral("chunks")] = QJsonArray();
    result[QStringLiteral("duration")] = extraction.durationMs;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject ExtractorService::handleExtractMetadata(uint64_t id, const QJsonObject& params)
{
    QString path = params.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'path' parameter"));
    }

    QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        return IpcMessage::makeError(id, IpcErrorCode::NotFound,
                                     QStringLiteral("File not found: %1").arg(path));
    }

    // Determine ItemKind from extension
    QString extension = fileInfo.suffix().toLower();
    ItemKind kind = ItemKind::Unknown;
    if (fileInfo.isDir()) {
        kind = ItemKind::Directory;
    } else if (extension == QLatin1String("pdf")) {
        kind = ItemKind::Pdf;
    } else if (extension == QLatin1String("md") || extension == QLatin1String("markdown")) {
        kind = ItemKind::Markdown;
    } else if (extension == QLatin1String("png") || extension == QLatin1String("jpg")
               || extension == QLatin1String("jpeg") || extension == QLatin1String("webp")
               || extension == QLatin1String("bmp") || extension == QLatin1String("tiff")
               || extension == QLatin1String("tif")) {
        kind = ItemKind::Image;
    } else if (extension == QLatin1String("zip") || extension == QLatin1String("tar")
               || extension == QLatin1String("gz") || extension == QLatin1String("7z")
               || extension == QLatin1String("rar")) {
        kind = ItemKind::Archive;
    } else if (extension == QLatin1String("txt") || extension == QLatin1String("csv")
               || extension == QLatin1String("log") || extension == QLatin1String("ini")
               || extension == QLatin1String("cfg") || extension == QLatin1String("conf")) {
        kind = ItemKind::Text;
    } else if (!extension.isEmpty()) {
        // Default non-empty extensions to Code for common source files
        kind = ItemKind::Code;
    }

    // Get owner name via stat()
    QString ownerName;
    bool isExecutable = false;
    bool isSymlink = fileInfo.isSymLink();
    QString symlinkTarget;

    struct stat st;
    QByteArray pathUtf8 = path.toUtf8();
    if (lstat(pathUtf8.constData(), &st) == 0) {
        struct passwd* pw = getpwuid(st.st_uid);
        if (pw) {
            ownerName = QString::fromUtf8(pw->pw_name);
        }
        isExecutable = (st.st_mode & S_IXUSR) != 0;
    }

    if (isSymlink) {
        symlinkTarget = fileInfo.symLinkTarget();
    }

    QElapsedTimer timer;
    timer.start();

    QJsonObject result;
    result[QStringLiteral("fileName")] = fileInfo.fileName();
    result[QStringLiteral("extension")] = extension;
    result[QStringLiteral("fileSize")] = static_cast<qint64>(fileInfo.size());
    result[QStringLiteral("creationDate")] = static_cast<double>(
        fileInfo.birthTime().toMSecsSinceEpoch()) / 1000.0;
    result[QStringLiteral("modificationDate")] = static_cast<double>(
        fileInfo.lastModified().toMSecsSinceEpoch()) / 1000.0;
    result[QStringLiteral("owner")] = ownerName;
    result[QStringLiteral("isExecutable")] = isExecutable;
    result[QStringLiteral("isSymlink")] = isSymlink;
    result[QStringLiteral("symlinkTarget")] = symlinkTarget;
    result[QStringLiteral("itemKind")] = itemKindToString(kind);
    result[QStringLiteral("duration")] = static_cast<int>(timer.elapsed());
    return IpcMessage::makeResponse(id, result);
}

QJsonObject ExtractorService::handleIsSupported(uint64_t id, const QJsonObject& params)
{
    QString extension = params.value(QStringLiteral("extension")).toString();
    if (extension.isEmpty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'extension' parameter"));
    }

    // Strip leading dot if present
    if (extension.startsWith(QLatin1Char('.'))) {
        extension = extension.mid(1);
    }
    extension = extension.toLower();

    // Check each extractor backend for support
    TextExtractor textExtractor;
    PdfExtractor pdfExtractor;
    OcrExtractor ocrExtractor;

    bool supported = textExtractor.supports(extension)
                     || pdfExtractor.supports(extension)
                     || ocrExtractor.supports(extension);

    // Classify extension to ItemKind
    ItemKind kind = ItemKind::Unknown;
    if (pdfExtractor.supports(extension)) {
        kind = ItemKind::Pdf;
    } else if (ocrExtractor.supports(extension)) {
        kind = ItemKind::Image;
    } else if (textExtractor.supports(extension)) {
        kind = ItemKind::Text;
    }

    QJsonObject result;
    result[QStringLiteral("supported")] = supported;
    result[QStringLiteral("kind")] = itemKindToString(kind);
    return IpcMessage::makeResponse(id, result);
}

QJsonObject ExtractorService::handleCancelExtraction(uint64_t id, const QJsonObject& params)
{
    Q_UNUSED(params)

    m_extractor.requestCancel();

    QJsonObject result;
    result[QStringLiteral("cancelled")] = true;
    return IpcMessage::makeResponse(id, result);
}

} // namespace bs
