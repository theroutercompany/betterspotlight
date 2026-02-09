#include "core/query/query_parser.h"

#include <QSet>

namespace bs {

namespace {

const QSet<QString>& knownTypeTokens()
{
    static const QSet<QString> kKnownTypes = {
        QStringLiteral("pdf"),
        QStringLiteral("docx"),
        QStringLiteral("doc"),
        QStringLiteral("xlsx"),
        QStringLiteral("xls"),
        QStringLiteral("pptx"),
        QStringLiteral("ppt"),
        QStringLiteral("txt"),
        QStringLiteral("md"),
        QStringLiteral("csv"),
        QStringLiteral("json"),
        QStringLiteral("xml"),
        QStringLiteral("yaml"),
        QStringLiteral("yml"),
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("gif"),
        QStringLiteral("svg"),
        QStringLiteral("mp3"),
        QStringLiteral("mp4"),
        QStringLiteral("wav"),
        QStringLiteral("avi"),
        QStringLiteral("mov"),
        QStringLiteral("zip"),
        QStringLiteral("tar"),
        QStringLiteral("gz"),
        QStringLiteral("py"),
        QStringLiteral("js"),
        QStringLiteral("ts"),
        QStringLiteral("cpp"),
        QStringLiteral("h"),
        QStringLiteral("java"),
        QStringLiteral("rb"),
        QStringLiteral("go"),
        QStringLiteral("rs"),
        QStringLiteral("swift"),
        QStringLiteral("el"),
    };
    return kKnownTypes;
}

} // namespace

ParsedQuery QueryParser::parse(const QString& normalizedQuery)
{
    ParsedQuery parsed;
    parsed.cleanedQuery = normalizedQuery.trimmed();
    if (parsed.cleanedQuery.isEmpty()) {
        return parsed;
    }

    QStringList tokens = parsed.cleanedQuery.split(QChar(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        return parsed;
    }

    const QString lastToken = tokens.last().toLower();
    if (!knownTypeTokens().contains(lastToken)) {
        return parsed;
    }

    parsed.extractedTypes.append(lastToken);
    parsed.filters.fileTypes.push_back(lastToken);
    parsed.hasTypeHint = true;

    tokens.removeLast();
    parsed.cleanedQuery = tokens.join(QChar(' ')).trimmed();
    return parsed;
}

} // namespace bs
