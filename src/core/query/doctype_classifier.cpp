#include "core/query/doctype_classifier.h"

#include <QStringList>

namespace bs {

namespace {

struct MultiWordPattern {
    const char* phrase;
    const char* intent;
};

constexpr MultiWordPattern kMultiWordPatterns[] = {
    {"lease agreement",  "legal_document"},
    {"rental agreement", "legal_document"},
    {"credit card",      "financial_document"},
    {"bank statement",   "financial_document"},
    {"tax return",       "financial_document"},
    {"tax form",         "financial_document"},
    {"cover letter",     "job_document"},
    {"meeting notes",    "notes"},
    {"primary source",   "reference_material"},
};

struct SingleWordPattern {
    const char* keyword;
    const char* intent;
};

constexpr SingleWordPattern kSingleWordPatterns[] = {
    {"lease",         "legal_document"},
    {"contract",      "legal_document"},
    {"agreement",     "legal_document"},
    {"invoice",       "financial_document"},
    {"receipt",       "financial_document"},
    {"budget",        "financial_document"},
    {"resume",        "job_document"},
    {"cv",            "job_document"},
    {"application",   "application_form"},
    {"form",          "application_form"},
    {"report",        "report"},
    {"analysis",      "report"},
    {"presentation",  "presentation"},
    {"slides",        "presentation"},
    {"photo",         "image"},
    {"picture",       "image"},
    {"screenshot",    "image"},
    {"spreadsheet",   "spreadsheet"},
    {"notes",         "notes"},
    {"manual",        "documentation"},
    {"documentation", "documentation"},
    {"guide",         "documentation"},
};

} // namespace

std::optional<QString> DoctypeClassifier::classify(const QString& queryLower)
{
    if (queryLower.isEmpty()) {
        return std::nullopt;
    }

    // Multi-word patterns checked first (longest match wins)
    for (const auto& pattern : kMultiWordPatterns) {
        if (queryLower.contains(QString::fromLatin1(pattern.phrase))) {
            return QString::fromLatin1(pattern.intent);
        }
    }

    // Single-word patterns: check as whole-word boundary via contains
    // We split and check individual tokens for whole-word matching
    const QStringList tokens = queryLower.split(QChar(' '), Qt::SkipEmptyParts);
    for (const auto& pattern : kSingleWordPatterns) {
        const QString keyword = QString::fromLatin1(pattern.keyword);
        for (const QString& token : tokens) {
            if (token == keyword) {
                return QString::fromLatin1(pattern.intent);
            }
        }
    }

    return std::nullopt;
}

std::vector<QString> DoctypeClassifier::extensionsForIntent(const QString& intent)
{
    if (intent == QLatin1String("legal_document")) {
        return {QStringLiteral("pdf"), QStringLiteral("doc"), QStringLiteral("docx")};
    }
    if (intent == QLatin1String("financial_document")) {
        return {QStringLiteral("pdf"), QStringLiteral("xlsx"),
                QStringLiteral("csv"), QStringLiteral("xls")};
    }
    if (intent == QLatin1String("job_document")) {
        return {QStringLiteral("pdf"), QStringLiteral("doc"), QStringLiteral("docx")};
    }
    if (intent == QLatin1String("presentation")) {
        return {QStringLiteral("pptx"), QStringLiteral("ppt"),
                QStringLiteral("key"), QStringLiteral("pdf")};
    }
    if (intent == QLatin1String("image")) {
        return {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
                QStringLiteral("heic"), QStringLiteral("gif"), QStringLiteral("webp"),
                QStringLiteral("tiff")};
    }
    if (intent == QLatin1String("spreadsheet")) {
        return {QStringLiteral("xlsx"), QStringLiteral("xls"),
                QStringLiteral("csv"), QStringLiteral("numbers")};
    }
    if (intent == QLatin1String("notes")) {
        return {QStringLiteral("md"), QStringLiteral("txt"), QStringLiteral("rtf")};
    }
    if (intent == QLatin1String("documentation")) {
        return {QStringLiteral("pdf"), QStringLiteral("md"),
                QStringLiteral("html"), QStringLiteral("txt")};
    }
    if (intent == QLatin1String("report")) {
        return {QStringLiteral("pdf"), QStringLiteral("doc"),
                QStringLiteral("docx"), QStringLiteral("md")};
    }
    if (intent == QLatin1String("application_form")) {
        return {QStringLiteral("pdf"), QStringLiteral("doc"), QStringLiteral("docx")};
    }
    if (intent == QLatin1String("reference_material")) {
        return {QStringLiteral("pdf"), QStringLiteral("doc"),
                QStringLiteral("docx"), QStringLiteral("html")};
    }
    return {};
}

} // namespace bs
