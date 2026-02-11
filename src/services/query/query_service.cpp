#include "query_service.h"
#include "core/ipc/message.h"
#include "core/ipc/socket_client.h"
#include "core/query/doctype_classifier.h"
#include "core/query/query_normalizer.h"
#include "core/query/query_parser.h"
#include "core/query/query_router.h"
#include "core/query/rules_engine.h"
#include "core/query/stopwords.h"
#include "core/query/structured_query.h"
#include "core/shared/search_result.h"
#include "core/shared/search_options.h"
#include "core/shared/logging.h"
#include "core/ranking/match_classifier.h"
#include "core/ranking/cross_encoder_reranker.h"
#include "core/ranking/personalized_ltr.h"
#include "core/ranking/qa_extractive_model.h"
#include "core/ranking/reranker_cascade.h"

#include <sqlite3.h>

#include "core/embedding/embedding_manager.h"
#include "core/models/model_registry.h"
#include "core/vector/vector_index.h"
#include "core/vector/vector_store.h"
#include "core/vector/search_merger.h"
#include "core/feedback/interaction_tracker.h"
#include "core/feedback/feedback_aggregator.h"
#include "core/feedback/path_preferences.h"
#include "core/feedback/type_affinity.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace bs {

static std::vector<Highlight> parseHighlights(const QString& snippet)
{
    std::vector<Highlight> highlights;
    const QString openTag = QStringLiteral("<b>");
    const QString closeTag = QStringLiteral("</b>");

    int plainOffset = 0;
    int pos = 0;

    while (pos < snippet.size()) {
        int openPos = snippet.indexOf(openTag, pos);
        if (openPos == -1) {
            plainOffset += snippet.size() - pos;
            break;
        }

        plainOffset += openPos - pos;

        int contentStart = openPos + openTag.size();
        int closePos = snippet.indexOf(closeTag, contentStart);
        if (closePos == -1) {
            plainOffset += snippet.size() - openPos;
            break;
        }

        int highlightLen = closePos - contentStart;
        Highlight h;
        h.offset = plainOffset;
        h.length = highlightLen;
        highlights.push_back(h);

        plainOffset += highlightLen;
        pos = closePos + closeTag.size();
    }

    return highlights;
}

enum class SearchQueryMode {
    Auto,
    Strict,
    Relaxed,
};

enum CandidateOrigin : uint8_t {
    CandidateOriginStrict = 1 << 0,
    CandidateOriginRelaxed = 1 << 1,
    CandidateOriginNameFallback = 1 << 2,
};

struct RewriteDecision {
    QString rewrittenQuery;
    bool hasCandidate = false;
    bool applied = false;
    double confidence = 0.0;
    double minCandidateConfidence = 0.0;
    int candidatesConsidered = 0;
    QString reason = QStringLiteral("not_attempted");
    QJsonArray correctedTokens;
};

double bestLexicalStrength(const std::vector<SQLiteStore::FtsHit>& hits)
{
    double best = 0.0;
    for (const auto& hit : hits) {
        best = std::max(best, std::max(0.0, -hit.bm25Score));
    }
    return best;
}

double typoCandidateConfidence(const QString& sourceToken,
                               const TypoLexicon::Correction& correction)
{
    const auto looksLikeSingleTransposition = [](const QString& source,
                                                 const QString& corrected) {
        if (source.size() != corrected.size() || source.size() < 2) {
            return false;
        }
        int firstDiff = -1;
        int secondDiff = -1;
        for (int i = 0; i < source.size(); ++i) {
            if (source.at(i).toLower() != corrected.at(i).toLower()) {
                if (firstDiff == -1) {
                    firstDiff = i;
                } else if (secondDiff == -1) {
                    secondDiff = i;
                } else {
                    return false;
                }
            }
        }
        if (firstDiff == -1 || secondDiff == -1 || secondDiff != firstDiff + 1) {
            return false;
        }
        return source.at(firstDiff).toLower() == corrected.at(secondDiff).toLower()
            && source.at(secondDiff).toLower() == corrected.at(firstDiff).toLower();
    };

    double confidence = 0.48;
    if (correction.editDistance == 1) {
        confidence += 0.22;
    } else if (correction.editDistance == 2) {
        confidence += 0.10;
    }

    if (correction.docCount >= 50) {
        confidence += 0.26;
    } else if (correction.docCount >= 25) {
        confidence += 0.22;
    } else if (correction.docCount >= 12) {
        confidence += 0.18;
    } else if (correction.docCount >= 6) {
        confidence += 0.13;
    } else if (correction.docCount >= 3) {
        confidence += 0.08;
    }

    if (!sourceToken.isEmpty() && !correction.corrected.isEmpty()) {
        if (sourceToken.front().toLower() == correction.corrected.front().toLower()) {
            confidence += 0.06;
        } else {
            confidence -= 0.08;  // prefix-change penalty
        }
    }

    if (sourceToken.size() == correction.corrected.size()) {
        confidence += 0.05;
    }

    if (sourceToken.size() >= 2 && correction.corrected.size() >= 2
        && sourceToken.left(2).toLower() == correction.corrected.left(2).toLower()) {
        confidence += 0.04;
    }

    if (looksLikeSingleTransposition(sourceToken, correction.corrected)) {
        confidence += 0.06;
    }

    if (sourceToken.size() >= 8 && correction.editDistance == 2) {
        confidence += 0.05;
    }

    return std::clamp(confidence, 0.0, 1.0);
}

bool looksLikeNaturalLanguageQuery(const QSet<QString>& signalTokens)
{
    return signalTokens.size() >= 3;
}

bool looksLikePathOrCodeQuery(const QString& query)
{
    const QString queryLower = query.toLower();
    if (queryLower.contains(QLatin1Char('/'))
        || queryLower.contains(QLatin1Char('\\'))
        || queryLower.startsWith(QLatin1Char('.'))
        || queryLower.startsWith(QLatin1Char('~'))
        || queryLower.contains(QStringLiteral("::"))) {
        return true;
    }

    static const QRegularExpression extensionLikeToken(
        QStringLiteral(R"(\b[a-z0-9_\-]+\.[a-z0-9]{1,8}\b)"));
    if (extensionLikeToken.match(queryLower).hasMatch()) {
        return true;
    }

    static const QRegularExpression codePunctuation(
        QStringLiteral(R"([<>{}\[\]();=#])"));
    return codePunctuation.match(query).hasMatch();
}

bool shouldApplyConsumerPrefilter(const QString& queryLower,
                                  const QStringList& queryTokensRaw,
                                  const QSet<QString>& querySignalTokens)
{
    if (looksLikePathOrCodeQuery(queryLower) || queryTokensRaw.isEmpty()) {
        return false;
    }

    // Consumer-first default for phrase-like lookups while still avoiding
    // obvious code/path-style queries.
    return querySignalTokens.size() >= 2 || queryTokensRaw.size() >= 3;
}

bool envFlagEnabled(const QString& raw)
{
    const QString normalized = raw.trimmed().toLower();
    return normalized == QLatin1String("1")
        || normalized == QLatin1String("true")
        || normalized == QLatin1String("yes")
        || normalized == QLatin1String("on");
}

QueryClass classifyQueryShape(const QString& queryLower,
                              const QSet<QString>& querySignalTokens,
                              const QStringList& queryTokensRaw)
{
    if (looksLikePathOrCodeQuery(queryLower)) {
        return QueryClass::PathOrCode;
    }
    if (looksLikeNaturalLanguageQuery(querySignalTokens)) {
        return QueryClass::NaturalLanguage;
    }
    if (queryTokensRaw.size() <= 2) {
        return QueryClass::ShortAmbiguous;
    }
    return QueryClass::NaturalLanguage;
}

QString normalizeFileTypeToken(const QString& token)
{
    QString normalized = token.trimmed().toLower();
    if (normalized.startsWith(QLatin1Char('.'))) {
        normalized.remove(0, 1);
    }
    return normalized;
}

SearchQueryMode parseSearchQueryMode(const QJsonObject& params)
{
    const QString mode = params.value(QStringLiteral("queryMode"))
                             .toString(QStringLiteral("auto"))
                             .trimmed()
                             .toLower();
    if (mode == QLatin1String("strict")) {
        return SearchQueryMode::Strict;
    }
    if (mode == QLatin1String("relaxed")) {
        return SearchQueryMode::Relaxed;
    }
    return SearchQueryMode::Auto;
}

// queryStopwords() is now shared via core/query/stopwords.h

QStringList tokenizeWords(const QString& text)
{
    static const QRegularExpression tokenRegex(QStringLiteral(R"([A-Za-z0-9_]+)"));
    QStringList tokens;
    auto it = tokenRegex.globalMatch(text.toLower());
    while (it.hasNext()) {
        const QString token = it.next().captured(0);
        if (!token.isEmpty()) {
            tokens.append(token);
        }
    }
    return tokens;
}

QStringList splitAnswerSentences(const QString& text)
{
    const QString normalized = text.simplified();
    if (normalized.isEmpty()) {
        return {};
    }

    const QStringList rawParts = normalized.split(
        QRegularExpression(QStringLiteral("[\\n\\r\\.!\\?;]+")),
        Qt::SkipEmptyParts);
    QStringList parts;
    parts.reserve(rawParts.size());
    for (const QString& part : rawParts) {
        const QString simplified = part.simplified();
        if (!simplified.isEmpty()) {
            parts.append(simplified);
        }
    }
    return parts;
}

QString clipAnswerText(const QString& rawText, int maxChars, const QStringList& queryTokens)
{
    QString text = rawText.simplified();
    if (text.size() <= maxChars) {
        return text;
    }

    int hitPos = -1;
    for (const QString& token : queryTokens) {
        if (token.size() < 2) {
            continue;
        }
        const int pos = text.indexOf(token, 0, Qt::CaseInsensitive);
        if (pos >= 0 && (hitPos < 0 || pos < hitPos)) {
            hitPos = pos;
        }
    }

    int start = 0;
    if (hitPos >= 0) {
        start = std::max(0, hitPos - (maxChars / 3));
    }
    if (start + maxChars > text.size()) {
        start = std::max(0, static_cast<int>(text.size()) - maxChars);
    }

    QString clipped = text.mid(start, maxChars).trimmed();
    if (start > 0) {
        clipped.prepend(QStringLiteral("..."));
    }
    if (start + maxChars < text.size()) {
        clipped.append(QStringLiteral("..."));
    }
    return clipped;
}

bool isExpectedGapFailureMessage(const QString& errorMessage)
{
    const QString lowered = errorMessage.toLower();
    return lowered.contains(QStringLiteral("pdf extraction unavailable ("))
        || lowered.contains(QStringLiteral("ocr extraction unavailable ("))
        || lowered.contains(QStringLiteral("leptonica failed to read image"))
        || lowered.contains(QStringLiteral("is not supported by extractor"))
        || lowered.contains(QStringLiteral("exceeds configured limit"))
        || lowered == QLatin1String("file does not exist or is not a regular file")
        || lowered == QLatin1String("file is not readable")
        || lowered == QLatin1String("failed to load pdf document")
        || lowered == QLatin1String("pdf is encrypted or password-protected")
        || lowered == QLatin1String("file appears to be a cloud placeholder (size reported but no content readable)");
}

struct QueryHints {
    bool downloadsHint = false;
    bool documentsHint = false;
    bool desktopHint = false;
    QString extensionHint;
    int monthHint = 0;
    int yearHint = 0;
};

QueryHints parseQueryHints(const QString& queryLower)
{
    QueryHints hints;
    hints.downloadsHint = queryLower.contains(QStringLiteral(" downloads"))
        || queryLower.endsWith(QStringLiteral("downloads"));
    hints.documentsHint = queryLower.contains(QStringLiteral(" documents"))
        || queryLower.endsWith(QStringLiteral("documents"));
    hints.desktopHint = queryLower.contains(QStringLiteral(" desktop"))
        || queryLower.endsWith(QStringLiteral("desktop"));

    if (queryLower.contains(QStringLiteral(" pdf"))) {
        hints.extensionHint = QStringLiteral("pdf");
    } else if (queryLower.contains(QStringLiteral(" docx"))) {
        hints.extensionHint = QStringLiteral("docx");
    } else if (queryLower.contains(QStringLiteral(" markdown"))
               || queryLower.contains(QStringLiteral(" md "))) {
        hints.extensionHint = QStringLiteral("md");
    } else if (queryLower.contains(QStringLiteral(" image"))
               || queryLower.contains(QStringLiteral(" jpg"))
               || queryLower.contains(QStringLiteral(" jpeg"))
               || queryLower.contains(QStringLiteral(" png"))) {
        hints.extensionHint = QStringLiteral("__image__");
    }

    struct MonthToken {
        const char* token;
        int month;
    };
    static constexpr MonthToken kMonths[] = {
        {"january", 1}, {"february", 2}, {"march", 3}, {"april", 4},
        {"may", 5}, {"june", 6}, {"july", 7}, {"august", 8},
        {"september", 9}, {"october", 10}, {"november", 11}, {"december", 12},
    };
    for (const auto& month : kMonths) {
        if (queryLower.contains(QString::fromLatin1(month.token))) {
            hints.monthHint = month.month;
            break;
        }
    }

    static const QRegularExpression yearRegex(QStringLiteral(R"((19|20)\d{2})"));
    QRegularExpressionMatch yearMatch = yearRegex.match(queryLower);
    if (yearMatch.hasMatch()) {
        hints.yearHint = yearMatch.captured(0).toInt();
    }

    return hints;
}

QueryService::QueryService(QObject* parent)
    : ServiceBase(QStringLiteral("query"), parent)
{
    LOG_INFO(bsIpc, "QueryService created");
    initBsignoreWatch();
}

QueryService::~QueryService()
{
    m_stopRebuildRequested.store(true);
    joinVectorRebuildThread();
}

QString QueryService::vectorRebuildStatusToString(VectorRebuildState::Status status)
{
    switch (status) {
    case VectorRebuildState::Status::Idle:
        return QStringLiteral("idle");
    case VectorRebuildState::Status::Running:
        return QStringLiteral("running");
    case VectorRebuildState::Status::Succeeded:
        return QStringLiteral("succeeded");
    case VectorRebuildState::Status::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("idle");
}

void QueryService::joinVectorRebuildThread()
{
    if (!m_vectorRebuildThread.joinable()) {
        return;
    }
    if (m_vectorRebuildThread.get_id() == std::this_thread::get_id()) {
        return;
    }
    m_vectorRebuildThread.join();
}

void QueryService::updateVectorRebuildProgress(uint64_t runId, int totalCandidates,
                                               int processed, int embedded,
                                               int skipped, int failed)
{
    std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
    if (m_vectorRebuildState.runId != runId
        || m_vectorRebuildState.status != VectorRebuildState::Status::Running) {
        return;
    }

    m_vectorRebuildState.totalCandidates = totalCandidates;
    m_vectorRebuildState.processed = processed;
    m_vectorRebuildState.embedded = embedded;
    m_vectorRebuildState.skipped = skipped;
    m_vectorRebuildState.failed = failed;
}

QString QueryService::vectorIndexPathForGeneration(const QString& generation) const
{
    if (m_dataDir.isEmpty()) {
        return QString();
    }
    const QString normalized = generation.trimmed().isEmpty()
        ? QStringLiteral("v1")
        : generation.trimmed();
    if (normalized == QLatin1String("v1")) {
        const QString legacyPath = m_dataDir + QStringLiteral("/vectors.hnsw");
        const QString versionedPath = m_dataDir + QStringLiteral("/vectors-v1.hnsw");
        if (QFile::exists(versionedPath) || !QFile::exists(legacyPath)) {
            return versionedPath;
        }
        return legacyPath;
    }
    return m_dataDir + QStringLiteral("/vectors-") + normalized + QStringLiteral(".hnsw");
}

QString QueryService::vectorMetaPathForGeneration(const QString& generation) const
{
    if (m_dataDir.isEmpty()) {
        return QString();
    }
    const QString normalized = generation.trimmed().isEmpty()
        ? QStringLiteral("v1")
        : generation.trimmed();
    if (normalized == QLatin1String("v1")) {
        const QString legacyPath = m_dataDir + QStringLiteral("/vectors.meta");
        const QString versionedPath = m_dataDir + QStringLiteral("/vectors-v1.meta");
        if (QFile::exists(versionedPath) || !QFile::exists(legacyPath)) {
            return versionedPath;
        }
        return legacyPath;
    }
    return m_dataDir + QStringLiteral("/vectors-") + normalized + QStringLiteral(".meta");
}

void QueryService::refreshVectorGenerationState()
{
    if (!m_vectorStore) {
        return;
    }

    bool vectorRebuildRunning = false;
    {
        std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
        vectorRebuildRunning =
            m_vectorRebuildState.status == VectorRebuildState::Status::Running;
    }

    if (auto activeState = m_vectorStore->activeGenerationState(); activeState.has_value()) {
        m_activeVectorGeneration = QString::fromStdString(activeState->generationId);
        m_activeVectorModelId = QString::fromStdString(activeState->modelId);
        m_activeVectorProvider = QString::fromStdString(activeState->provider);
        m_activeVectorDimensions = std::max(activeState->dimensions, 1);
        if (!vectorRebuildRunning) {
            m_vectorMigrationState = QString::fromStdString(activeState->state);
            m_vectorMigrationProgressPct = activeState->progressPct;
        }
    }
    if (auto setting = m_store->getSetting(QStringLiteral("activeVectorGeneration"));
        setting.has_value() && !setting->trimmed().isEmpty()) {
        m_activeVectorGeneration = setting->trimmed();
    }

    if (auto setting = m_store->getSetting(QStringLiteral("targetVectorGeneration"));
        setting.has_value() && !setting->trimmed().isEmpty()) {
        m_targetVectorGeneration = setting->trimmed();
    }
    if (!vectorRebuildRunning) {
        if (auto setting = m_store->getSetting(QStringLiteral("vectorMigrationState"));
            setting.has_value() && !setting->trimmed().isEmpty()) {
            m_vectorMigrationState = setting->trimmed();
        }
        if (auto setting = m_store->getSetting(QStringLiteral("vectorMigrationProgressPct"));
            setting.has_value()) {
            bool ok = false;
            const double parsed = setting->toDouble(&ok);
            if (ok) {
                m_vectorMigrationProgressPct = parsed;
            }
        }
    }
}

QJsonObject QueryService::handleRequest(const QJsonObject& request)
{
    QString method = request.value(QStringLiteral("method")).toString();
    uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
    QJsonObject params = request.value(QStringLiteral("params")).toObject();

    if (method == QLatin1String("search"))          return handleSearch(id, params);
    if (method == QLatin1String("getAnswerSnippet")
        || method == QLatin1String("get_answer_snippet")) return handleGetAnswerSnippet(id, params);
    if (method == QLatin1String("getHealth"))        return handleGetHealth(id);
    if (method == QLatin1String("getHealthDetails")) return handleGetHealthDetails(id, params);
    if (method == QLatin1String("recordFeedback"))   return handleRecordFeedback(id, params);
    if (method == QLatin1String("getFrequency"))     return handleGetFrequency(id, params);

    if (method == QLatin1String("record_interaction"))       return handleRecordInteraction(id, params);
    if (method == QLatin1String("get_path_preferences"))     return handleGetPathPreferences(id, params);
    if (method == QLatin1String("get_file_type_affinity"))   return handleGetFileTypeAffinity(id);
    if (method == QLatin1String("run_aggregation"))          return handleRunAggregation(id);
    if (method == QLatin1String("export_interaction_data"))  return handleExportInteractionData(id, params);
    if (method == QLatin1String("rebuildVectorIndex")
        || method == QLatin1String("rebuild_vector_index")) {
        return handleRebuildVectorIndex(id, params);
    }

    return ServiceBase::handleRequest(request);
}

bool QueryService::ensureStoreOpen()
{
    if (m_store.has_value()) {
        return true;
    }

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString envDataDir = env.value(QStringLiteral("BETTERSPOTLIGHT_DATA_DIR")).trimmed();
    if (!envDataDir.isEmpty()) {
        m_dataDir = QDir::cleanPath(envDataDir);
    } else {
        m_dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                    + QStringLiteral("/betterspotlight");
    }
    m_dbPath = m_dataDir + QStringLiteral("/index.db");
    m_vectorIndexPath = vectorIndexPathForGeneration(m_activeVectorGeneration);
    m_vectorMetaPath = vectorMetaPathForGeneration(m_activeVectorGeneration);

    auto store = SQLiteStore::open(m_dbPath);
    if (!store.has_value()) {
        LOG_ERROR(bsIpc, "Failed to open database at: %s", qPrintable(m_dbPath));
        return false;
    }

    m_store.emplace(std::move(store.value()));
    LOG_INFO(bsIpc, "Database opened at: %s", qPrintable(m_dbPath));

    initBsignoreWatch();
    return true;
}

bool QueryService::ensureM2ModulesInitialized()
{
    if (!ensureStoreOpen()) {
        return false;
    }

    if (!m_m2Initialized) {
        initM2Modules();
    }
    return true;
}

bool QueryService::ensureTypoLexiconReady()
{
    if (m_typoLexiconReady) {
        return true;
    }
    if (m_typoLexiconBuildAttempted || !m_store.has_value()) {
        return false;
    }

    m_typoLexiconBuildAttempted = true;
    if (!m_typoLexicon.build(m_store->rawDb())) {
        LOG_WARN(bsIpc, "TypoLexicon build failed; typo correction lexicon unavailable");
        return false;
    }

    m_typoLexiconReady = true;
    LOG_INFO(bsIpc, "TypoLexicon built with %d terms", m_typoLexicon.termCount());
    return true;
}

bool QueryService::ensureInferenceClientConnected()
{
    if (m_inferenceClient && m_inferenceClient->isConnected()) {
        recordInferenceConnected(true);
        return true;
    }

    if (!m_inferenceClient) {
        m_inferenceClient = std::make_unique<SocketClient>();
    }

    const QString inferenceSocketPath = ServiceBase::socketPath(QStringLiteral("inference"));
    const bool connected = m_inferenceClient->connectToServer(inferenceSocketPath, 200);
    recordInferenceConnected(connected);
    if (!connected) {
        LOG_WARN(bsIpc, "Inference client connect failed: %s",
                 qUtf8Printable(inferenceSocketPath));
    }
    return connected;
}

std::optional<QJsonObject> QueryService::sendInferenceRequest(const QString& method,
                                                              const QJsonObject& params,
                                                              int timeoutMs,
                                                              const QString& roleForMetrics,
                                                              const QString& fallbackReasonKey,
                                                              const QString& cancelToken)
{
    std::lock_guard<std::mutex> lock(m_inferenceRpcMutex);
    if (!ensureInferenceClientConnected() || !m_inferenceClient) {
        recordInferenceFallback(roleForMetrics);
        return std::nullopt;
    }

    QJsonObject requestParams = params;
    if (!requestParams.contains(QStringLiteral("requestId"))) {
        requestParams[QStringLiteral("requestId")] = QStringLiteral("%1-%2")
            .arg(method)
            .arg(QDateTime::currentMSecsSinceEpoch());
    }
    if (!cancelToken.trimmed().isEmpty()) {
        requestParams[QStringLiteral("cancelToken")] = cancelToken;
    }

    auto response = m_inferenceClient->sendRequest(method, requestParams, timeoutMs);
    if (!response.has_value()) {
        recordInferenceConnected(false);
        recordInferenceTimeout(roleForMetrics);
        recordInferenceFallback(roleForMetrics);
        m_inferenceClient->disconnect();
        return std::nullopt;
    }

    const QString responseType = response->value(QStringLiteral("type")).toString();
    if (responseType == QLatin1String("error")) {
        recordInferenceConnected(false);
        recordInferenceFallback(roleForMetrics);
        m_inferenceClient->disconnect();
        return std::nullopt;
    }

    QJsonObject payload = response->value(QStringLiteral("result")).toObject();
    if (payload.isEmpty()) {
        recordInferenceFallback(roleForMetrics);
        return std::nullopt;
    }

    const QString status = payload.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("timeout")) {
        recordInferenceTimeout(roleForMetrics);
        recordInferenceFallback(roleForMetrics);
        if (payload.value(QStringLiteral("fallbackReason")).toString().isEmpty()) {
            payload[QStringLiteral("fallbackReason")] = fallbackReasonKey;
        }
    } else if (status != QLatin1String("ok")) {
        recordInferenceFallback(roleForMetrics);
        if (payload.value(QStringLiteral("fallbackReason")).toString().isEmpty()) {
            payload[QStringLiteral("fallbackReason")] = fallbackReasonKey;
        }
    }

    recordInferenceConnected(true);
    return payload;
}

void QueryService::recordInferenceTimeout(const QString& role)
{
    if (role.trimmed().isEmpty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_inferenceStatsMutex);
    m_inferenceTimeoutCountByRole[role] = m_inferenceTimeoutCountByRole.value(role) + 1;
}

void QueryService::recordInferenceFallback(const QString& role)
{
    if (role.trimmed().isEmpty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_inferenceStatsMutex);
    m_inferenceFallbackCountByRole[role] = m_inferenceFallbackCountByRole.value(role) + 1;
}

void QueryService::recordInferenceConnected(bool connected)
{
    std::lock_guard<std::mutex> lock(m_inferenceStatsMutex);
    m_inferenceServiceConnected = connected;
}

QJsonObject QueryService::inferenceHealthSnapshot()
{
    QJsonObject snapshot;
    snapshot[QStringLiteral("inferenceServiceConnected")] = false;
    snapshot[QStringLiteral("inferenceRoleStatusByModel")] = QJsonObject{};
    snapshot[QStringLiteral("inferenceQueueDepthByRole")] = QJsonObject{};
    snapshot[QStringLiteral("inferenceServiceTimeoutCountByRole")] = QJsonObject{};
    snapshot[QStringLiteral("inferenceServiceFailureCountByRole")] = QJsonObject{};
    snapshot[QStringLiteral("inferenceServiceRestartCountByRole")] = QJsonObject{};

    QJsonObject timeoutCounts;
    QJsonObject fallbackCounts;
    {
        std::lock_guard<std::mutex> lock(m_inferenceStatsMutex);
        snapshot[QStringLiteral("inferenceServiceConnected")] = m_inferenceServiceConnected;
        for (auto it = m_inferenceTimeoutCountByRole.constBegin();
             it != m_inferenceTimeoutCountByRole.constEnd(); ++it) {
            timeoutCounts[it.key()] = it.value();
        }
        for (auto it = m_inferenceFallbackCountByRole.constBegin();
             it != m_inferenceFallbackCountByRole.constEnd(); ++it) {
            fallbackCounts[it.key()] = it.value();
        }
    }
    snapshot[QStringLiteral("inferenceTimeoutCountByRole")] = timeoutCounts;
    snapshot[QStringLiteral("inferenceFallbackCountByRole")] = fallbackCounts;

    std::lock_guard<std::mutex> lock(m_inferenceRpcMutex);
    if (!ensureInferenceClientConnected() || !m_inferenceClient) {
        return snapshot;
    }

    auto response = m_inferenceClient->sendRequest(QStringLiteral("get_inference_health"), {}, 250);
    if (!response.has_value() || response->value(QStringLiteral("type")).toString() == QLatin1String("error")) {
        recordInferenceConnected(false);
        return snapshot;
    }

    const QJsonObject payload = response->value(QStringLiteral("result")).toObject();
    if (payload.isEmpty()) {
        return snapshot;
    }

    snapshot[QStringLiteral("inferenceServiceConnected")] =
        payload.value(QStringLiteral("connected")).toBool(true);
    snapshot[QStringLiteral("inferenceRoleStatusByModel")] =
        payload.value(QStringLiteral("roleStatusByModel")).toObject();
    snapshot[QStringLiteral("inferenceQueueDepthByRole")] =
        payload.value(QStringLiteral("queueDepthByRole")).toObject();
    snapshot[QStringLiteral("inferenceServiceTimeoutCountByRole")] =
        payload.value(QStringLiteral("timeoutCountByRole")).toObject();
    snapshot[QStringLiteral("inferenceServiceFailureCountByRole")] =
        payload.value(QStringLiteral("failureCountByRole")).toObject();
    snapshot[QStringLiteral("inferenceServiceRestartCountByRole")] =
        payload.value(QStringLiteral("restartCountByRole")).toObject();
    recordInferenceConnected(snapshot.value(QStringLiteral("inferenceServiceConnected")).toBool(false));
    return snapshot;
}

void QueryService::initM2Modules()
{
    if (m_m2Initialized) {
        return;
    }
    m_m2Initialized = true;

    ::sqlite3* db = m_store->rawDb();

    m_interactionTracker = std::make_unique<InteractionTracker>(db);
    m_feedbackAggregator = std::make_unique<FeedbackAggregator>(db);
    m_pathPreferences = std::make_unique<PathPreferences>(db);
    m_typeAffinity = std::make_unique<TypeAffinity>(db);

    m_vectorStore = std::make_unique<VectorStore>(db);
    refreshVectorGenerationState();

    const QString modelsDir = ModelRegistry::resolveModelsDir();
    m_modelRegistry = std::make_unique<ModelRegistry>(modelsDir);

    m_embeddingManager = std::make_unique<EmbeddingManager>(m_modelRegistry.get(), "bi-encoder");
    m_fastEmbeddingManager = std::make_unique<EmbeddingManager>(m_modelRegistry.get(), "bi-encoder-fast");
    bool embeddingAvailable = false;
    bool fastEmbeddingAvailable = false;
    if (!m_embeddingManager->initialize()) {
        LOG_WARN(bsIpc, "EmbeddingManager unavailable, semantic search disabled");
    } else {
        embeddingAvailable = true;
        m_targetVectorGeneration = m_embeddingManager->activeGenerationId().isEmpty()
            ? QStringLiteral("v2")
            : m_embeddingManager->activeGenerationId();
        m_activeVectorModelId = m_embeddingManager->activeModelId();
        m_activeVectorProvider = m_embeddingManager->providerName();
        m_activeVectorDimensions = std::max(m_embeddingManager->embeddingDimensions(), 1);
        LOG_INFO(bsIpc, "EmbeddingManager initialized");
    }
    if (!m_fastEmbeddingManager->initialize()) {
        LOG_WARN(bsIpc, "Fast EmbeddingManager unavailable, dual-index retrieval disabled");
    } else {
        fastEmbeddingAvailable = true;
        if (!m_fastEmbeddingManager->activeGenerationId().isEmpty()) {
            m_fastVectorGeneration = m_fastEmbeddingManager->activeGenerationId();
        }
        LOG_INFO(bsIpc, "Fast EmbeddingManager initialized (generation=%s)",
                 qUtf8Printable(m_fastVectorGeneration));
    }

    if (embeddingAvailable) {
        VectorStore::GenerationState targetState;
        targetState.generationId = m_targetVectorGeneration.toStdString();
        targetState.modelId = m_embeddingManager->activeModelId().toStdString();
        targetState.dimensions = std::max(m_embeddingManager->embeddingDimensions(), 1);
        targetState.provider = m_embeddingManager->providerName().toStdString();
        targetState.state = (m_activeVectorGeneration == m_targetVectorGeneration)
            ? "active"
            : "building";
        targetState.progressPct = (m_activeVectorGeneration == m_targetVectorGeneration) ? 100.0 : 0.0;
        targetState.active = (m_activeVectorGeneration == m_targetVectorGeneration);
        m_vectorStore->upsertGenerationState(targetState);
        m_store->setSetting(QStringLiteral("targetVectorGeneration"), m_targetVectorGeneration);

        const bool hasActiveMappings =
            m_vectorStore->countMappingsForGeneration(m_activeVectorGeneration.toStdString()) > 0;
        const bool hasActiveIndexFiles =
            QFile::exists(vectorIndexPathForGeneration(m_activeVectorGeneration))
            && QFile::exists(vectorMetaPathForGeneration(m_activeVectorGeneration));
        if (!hasActiveMappings && !hasActiveIndexFiles
            && m_activeVectorGeneration != m_targetVectorGeneration) {
            m_vectorStore->setActiveGeneration(m_targetVectorGeneration.toStdString());
            m_store->setSetting(QStringLiteral("activeVectorGeneration"), m_targetVectorGeneration);
        }
    }
    if (fastEmbeddingAvailable) {
        VectorStore::GenerationState fastState;
        fastState.generationId = m_fastVectorGeneration.toStdString();
        fastState.modelId = m_fastEmbeddingManager->activeModelId().toStdString();
        fastState.dimensions = std::max(m_fastEmbeddingManager->embeddingDimensions(), 1);
        fastState.provider = m_fastEmbeddingManager->providerName().toStdString();
        fastState.state = "building";
        fastState.progressPct = 0.0;
        fastState.active = false;
        m_vectorStore->upsertGenerationState(fastState);
    }

    refreshVectorGenerationState();
    m_vectorIndexPath = vectorIndexPathForGeneration(m_activeVectorGeneration);
    m_vectorMetaPath = vectorMetaPathForGeneration(m_activeVectorGeneration);
    m_fastVectorIndexPath = vectorIndexPathForGeneration(m_fastVectorGeneration);
    m_fastVectorMetaPath = vectorMetaPathForGeneration(m_fastVectorGeneration);

    VectorIndex::IndexMetadata indexMeta;
    indexMeta.dimensions = std::max(m_activeVectorDimensions, 1);
    indexMeta.modelId = m_activeVectorModelId.toStdString();
    indexMeta.generationId = m_activeVectorGeneration.toStdString();
    indexMeta.provider = m_activeVectorProvider.toStdString();

    auto loadedVectorIndex = std::make_unique<VectorIndex>(indexMeta);
    if (QFile::exists(m_vectorIndexPath) && QFile::exists(m_vectorMetaPath)) {
        if (!loadedVectorIndex->load(m_vectorIndexPath.toStdString(), m_vectorMetaPath.toStdString())) {
            LOG_WARN(bsIpc, "Failed to load vector index generation '%s' from %s",
                     qUtf8Printable(m_activeVectorGeneration),
                     qUtf8Printable(m_vectorIndexPath));
            loadedVectorIndex.reset();
        } else {
            LOG_INFO(bsIpc, "Vector index loaded: generation=%s vectors=%d",
                     qUtf8Printable(m_activeVectorGeneration),
                     loadedVectorIndex->totalElements());
        }
    } else {
        if (!loadedVectorIndex->create()) {
            LOG_WARN(bsIpc, "Failed to create vector index generation '%s' with dimensions=%d",
                     qUtf8Printable(m_activeVectorGeneration), indexMeta.dimensions);
            loadedVectorIndex.reset();
        }
    }

    auto loadedFastVectorIndex = std::unique_ptr<VectorIndex>{};
    if (fastEmbeddingAvailable) {
        VectorIndex::IndexMetadata fastMeta;
        fastMeta.dimensions = std::max(m_fastEmbeddingManager->embeddingDimensions(), 1);
        fastMeta.modelId = m_fastEmbeddingManager->activeModelId().toStdString();
        fastMeta.generationId = m_fastVectorGeneration.toStdString();
        fastMeta.provider = m_fastEmbeddingManager->providerName().toStdString();

        loadedFastVectorIndex = std::make_unique<VectorIndex>(fastMeta);
        if (QFile::exists(m_fastVectorIndexPath) && QFile::exists(m_fastVectorMetaPath)) {
            if (!loadedFastVectorIndex->load(m_fastVectorIndexPath.toStdString(),
                                             m_fastVectorMetaPath.toStdString())) {
                LOG_WARN(bsIpc, "Failed to load fast vector index generation '%s' from %s",
                         qUtf8Printable(m_fastVectorGeneration),
                         qUtf8Printable(m_fastVectorIndexPath));
                loadedFastVectorIndex.reset();
            } else {
                LOG_INFO(bsIpc, "Fast vector index loaded: generation=%s vectors=%d",
                         qUtf8Printable(m_fastVectorGeneration),
                         loadedFastVectorIndex->totalElements());
            }
        } else if (!loadedFastVectorIndex->create()) {
            LOG_WARN(bsIpc, "Failed to create fast vector index generation '%s' with dimensions=%d",
                     qUtf8Printable(m_fastVectorGeneration), fastMeta.dimensions);
            loadedFastVectorIndex.reset();
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(m_vectorIndexMutex);
        m_vectorIndex = std::move(loadedVectorIndex);
        m_fastVectorIndex = std::move(loadedFastVectorIndex);
    }

    m_fastCrossEncoderReranker =
        std::make_unique<CrossEncoderReranker>(m_modelRegistry.get(), "cross-encoder-fast");
    if (m_fastCrossEncoderReranker->initialize()) {
        LOG_INFO(bsIpc, "Fast cross-encoder reranker initialized");
    } else {
        LOG_WARN(bsIpc, "Fast cross-encoder reranker unavailable");
    }

    m_crossEncoderReranker =
        std::make_unique<CrossEncoderReranker>(m_modelRegistry.get(), "cross-encoder");
    if (m_crossEncoderReranker->initialize()) {
        LOG_INFO(bsIpc, "Cross-encoder reranker initialized");
    } else {
        LOG_WARN(bsIpc, "Cross-encoder reranker not available — skipping reranking");
    }

    m_personalizedLtr = std::make_unique<PersonalizedLtr>(
        m_dataDir + QStringLiteral("/ltr_model.json"));
    if (m_personalizedLtr->initialize(db)) {
        LOG_INFO(bsIpc, "Personalized LTR initialized: %s",
                 qUtf8Printable(m_personalizedLtr->modelVersion()));
    } else {
        LOG_WARN(bsIpc, "Personalized LTR unavailable (cold start)");
    }

    m_qaExtractiveModel =
        std::make_unique<QaExtractiveModel>(m_modelRegistry.get(), "qa-extractive");
    if (m_qaExtractiveModel->initialize()) {
        LOG_INFO(bsIpc, "QA extractive model initialized");
    } else {
        LOG_WARN(bsIpc, "QA extractive model unavailable (fallback preview mode)");
    }

    maybeStartBackgroundVectorMigration();
}

void QueryService::maybeStartBackgroundVectorMigration()
{
    if (!m_embeddingManager || !m_embeddingManager->isAvailable() || !m_store.has_value()) {
        return;
    }

    if (m_targetVectorGeneration.isEmpty()
        || m_activeVectorGeneration == m_targetVectorGeneration) {
        return;
    }

    const QString autoMigrationSetting = m_store->getSetting(
        QStringLiteral("autoVectorMigration")).value_or(QStringLiteral("true"));
    if (autoMigrationSetting.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0
        || autoMigrationSetting.compare(QStringLiteral("0"), Qt::CaseInsensitive) == 0
        || autoMigrationSetting.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0) {
        LOG_INFO(bsIpc, "Automatic vector migration disabled via autoVectorMigration setting");
        return;
    }

    QJsonObject params;
    params[QStringLiteral("targetGeneration")] = m_targetVectorGeneration;
    const QJsonObject response = handleRebuildVectorIndex(/*id=*/0, params);
    const QString type = response.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("error")) {
        const QString errorMsg = response.value(QStringLiteral("error"))
                                     .toObject()
                                     .value(QStringLiteral("message"))
                                     .toString();
        LOG_WARN(bsIpc, "Automatic vector migration start failed: %s", qPrintable(errorMsg));
        return;
    }

    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    if (result.value(QStringLiteral("started")).toBool(false)) {
        const qint64 runId = result.value(QStringLiteral("runId")).toInteger();
        LOG_INFO(bsIpc, "Automatic vector migration started (runId=%lld target=%s)",
                 static_cast<long long>(runId),
                 qUtf8Printable(m_targetVectorGeneration));
    } else if (result.value(QStringLiteral("alreadyRunning")).toBool(false)) {
        LOG_INFO(bsIpc, "Automatic vector migration already running");
    }
}

void QueryService::initBsignoreWatch()
{
    if (m_bsignorePath.isEmpty()) {
        m_bsignorePath = QDir::homePath() + QStringLiteral("/.bsignore");
    }

    if (!m_bsignoreWatcher) {
        m_bsignoreWatcher = std::make_unique<QFileSystemWatcher>(this);
        connect(m_bsignoreWatcher.get(), &QFileSystemWatcher::fileChanged,
                this, [this](const QString&) { reloadBsignore(); });
        connect(m_bsignoreWatcher.get(), &QFileSystemWatcher::directoryChanged,
                this, [this](const QString&) { reloadBsignore(); });
    }

    reloadBsignore();
}

void QueryService::reloadBsignore()
{
    if (m_bsignorePath.isEmpty()) {
        return;
    }

    m_bsignoreLastLoadedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_queryCache.clear();
    const QFileInfo bsignoreInfo(m_bsignorePath);
    if (bsignoreInfo.exists()) {
        m_bsignoreLoaded = m_bsignoreParser.loadFromFile(m_bsignorePath.toStdString());
    } else {
        m_bsignoreParser.clear();
        m_bsignoreLoaded = false;
    }
    m_bsignorePatternCount = static_cast<int>(m_bsignoreParser.patterns().size());

    if (m_bsignoreWatcher) {
        if (!m_bsignoreWatcher->files().isEmpty()) {
            m_bsignoreWatcher->removePaths(m_bsignoreWatcher->files());
        }
        if (!m_bsignoreWatcher->directories().isEmpty()) {
            m_bsignoreWatcher->removePaths(m_bsignoreWatcher->directories());
        }

        const QString parentDir = bsignoreInfo.absoluteDir().absolutePath();
        if (QFileInfo::exists(parentDir)) {
            m_bsignoreWatcher->addPath(parentDir);
        }
        if (bsignoreInfo.exists()) {
            m_bsignoreWatcher->addPath(m_bsignorePath);
        }
    }
}

bool QueryService::isExcludedByBsignore(const QString& absolutePath) const
{
    if (!m_bsignoreLoaded || m_bsignorePatternCount <= 0) {
        return false;
    }
    return m_bsignoreParser.matches(absolutePath.toStdString());
}

QJsonObject QueryService::bsignoreStatusJson() const
{
    QJsonObject status;
    status[QStringLiteral("path")] = m_bsignorePath;
    status[QStringLiteral("fileExists")] = QFileInfo::exists(m_bsignorePath);
    status[QStringLiteral("loaded")] = m_bsignoreLoaded;
    status[QStringLiteral("patternCount")] = m_bsignorePatternCount;
    status[QStringLiteral("lastLoadedAtMs")] = m_bsignoreLastLoadedAtMs;
    status[QStringLiteral("lastLoadedAt")] = m_bsignoreLastLoadedAtMs > 0
        ? QDateTime::fromMSecsSinceEpoch(m_bsignoreLastLoadedAtMs).toUTC().toString(Qt::ISODate)
        : QString();
    return status;
}

QJsonObject QueryService::processStatsForService(const QString& serviceName) const
{
    QJsonObject stats;
    stats[QStringLiteral("service")] = serviceName;
    stats[QStringLiteral("available")] = false;

    const QString pidPath = QStringLiteral("/tmp/betterspotlight-%1/%2.pid")
        .arg(getuid()).arg(serviceName);
    QFile pidFile(pidPath);
    if (!pidFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return stats;
    }

    bool ok = false;
    const qint64 pid = pidFile.readAll().trimmed().toLongLong(&ok);
    if (!ok || pid <= 0) {
        return stats;
    }

    QProcess ps;
    ps.start(QStringLiteral("ps"), {
        QStringLiteral("-o"), QStringLiteral("rss="),
        QStringLiteral("-o"), QStringLiteral("%cpu="),
        QStringLiteral("-p"), QString::number(pid),
    });
    if (!ps.waitForFinished(1000) || ps.exitStatus() != QProcess::NormalExit || ps.exitCode() != 0) {
        return stats;
    }

    const QString out = QString::fromUtf8(ps.readAllStandardOutput()).trimmed();
    if (out.isEmpty()) {
        return stats;
    }
    const QStringList fields = out.split(QRegularExpression(QStringLiteral("\\s+")),
                                         Qt::SkipEmptyParts);
    if (fields.size() < 2) {
        return stats;
    }

    bool rssOk = false;
    bool cpuOk = false;
    const qint64 rssKb = fields.at(0).toLongLong(&rssOk);
    const double cpuPct = fields.at(1).toDouble(&cpuOk);
    if (!rssOk || !cpuOk) {
        return stats;
    }

    stats[QStringLiteral("available")] = true;
    stats[QStringLiteral("pid")] = pid;
    stats[QStringLiteral("rssKb")] = rssKb;
    stats[QStringLiteral("cpuPct")] = cpuPct;
    return stats;
}

QJsonObject QueryService::queryStatsSnapshot() const
{
    QJsonObject stats;
    stats[QStringLiteral("searchCount")] = static_cast<qint64>(m_searchCount.load());
    stats[QStringLiteral("rewriteAppliedCount")] = static_cast<qint64>(m_rewriteAppliedCount.load());
    stats[QStringLiteral("semanticOnlyAdmittedCount")] =
        static_cast<qint64>(m_semanticOnlyAdmittedCount.load());
    stats[QStringLiteral("semanticOnlySuppressedCount")] =
        static_cast<qint64>(m_semanticOnlySuppressedCount.load());
    return stats;
}

QJsonObject QueryService::handleSearch(uint64_t id, const QJsonObject& params)
{
    if (!ensureM2ModulesInitialized()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    // Parse query
    const QString originalRawQuery = params.value(QStringLiteral("query")).toString();
    QString query = originalRawQuery;
    {
        const auto nq = QueryNormalizer::normalize(query);
        query = nq.normalized;
    }
    const QString normalizedQueryBeforeParse = query;
    const auto parsed = QueryParser::parse(query);
    if (parsed.hasTypeHint) {
        LOG_INFO(bsIpc, "QueryParser: extracted types=[%s] from query='%s'",
                 qUtf8Printable(parsed.extractedTypes.join(QStringLiteral(","))),
                 qUtf8Printable(query));
    }
    if (!parsed.cleanedQuery.isEmpty()) {
        query = parsed.cleanedQuery;
    } else if (parsed.hasTypeHint) {
        // Preserve query text for type-only inputs (e.g. "pdf") so search still runs.
        query = normalizedQueryBeforeParse;
    }
    if (query.isEmpty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'query' parameter"));
    }
    m_searchCount.fetch_add(1);

    // Parse limit (default 20)
    int limit = 20;
    if (params.contains(QStringLiteral("limit"))) {
        limit = params.value(QStringLiteral("limit")).toInt(20);
        if (limit < 1) {
            limit = 1;
        } else if (limit > 200) {
            limit = 200;
        }
    }
    const bool debugRequested = params.value(QStringLiteral("debug")).toBool(false);
    const SearchQueryMode queryMode = parseSearchQueryMode(params);

    SearchOptions searchOptions;
    const bool hasUserProvidedFilters = params.contains(QStringLiteral("filters"));
    const auto addFileTypeFilter = [&](const QString& rawType) {
        const QString normalized = normalizeFileTypeToken(rawType);
        if (normalized.isEmpty()) {
            return;
        }
        const bool alreadyPresent = std::any_of(
            searchOptions.fileTypes.begin(),
            searchOptions.fileTypes.end(),
            [&](const QString& existing) {
                return normalizeFileTypeToken(existing) == normalized;
            });
        if (!alreadyPresent) {
            searchOptions.fileTypes.push_back(normalized);
        }
    };
    const auto addPathFilterUnique = [](std::vector<QString>& container,
                                        const QString& rawPath) {
        const QString normalized = QDir::cleanPath(rawPath.trimmed());
        if (normalized.isEmpty()) {
            return;
        }
        const bool alreadyPresent = std::any_of(
            container.begin(), container.end(),
            [&](const QString& existing) {
                return QDir::cleanPath(existing) == normalized;
            });
        if (!alreadyPresent) {
            container.push_back(normalized);
        }
    };
    if (params.contains(QStringLiteral("filters"))) {
        const QJsonObject filters = params.value(QStringLiteral("filters")).toObject();

        if (filters.contains(QStringLiteral("fileTypes"))) {
            const QJsonArray types = filters.value(QStringLiteral("fileTypes")).toArray();
            for (const auto& t : types) {
                addFileTypeFilter(t.toString());
            }
        }
        if (filters.contains(QStringLiteral("excludePaths"))) {
            const QJsonArray paths = filters.value(QStringLiteral("excludePaths")).toArray();
            searchOptions.excludePaths.reserve(static_cast<size_t>(paths.size()));
            for (const auto& p : paths) {
                addPathFilterUnique(searchOptions.excludePaths, p.toString());
            }
        }
        if (filters.contains(QStringLiteral("includePaths"))) {
            const QJsonArray paths = filters.value(QStringLiteral("includePaths")).toArray();
            searchOptions.includePaths.reserve(static_cast<size_t>(paths.size()));
            for (const auto& p : paths) {
                addPathFilterUnique(searchOptions.includePaths, p.toString());
            }
        }
        if (filters.contains(QStringLiteral("modifiedAfter"))) {
            searchOptions.modifiedAfter = filters.value(QStringLiteral("modifiedAfter")).toDouble();
        }
        if (filters.contains(QStringLiteral("modifiedBefore"))) {
            searchOptions.modifiedBefore = filters.value(QStringLiteral("modifiedBefore")).toDouble();
        }
        if (filters.contains(QStringLiteral("minSize"))) {
            searchOptions.minSizeBytes = static_cast<int64_t>(
                filters.value(QStringLiteral("minSize")).toDouble());
        }
        if (filters.contains(QStringLiteral("maxSize"))) {
            searchOptions.maxSizeBytes = static_cast<int64_t>(
                filters.value(QStringLiteral("maxSize")).toDouble());
        }
    }

    for (const QString& parsedType : parsed.filters.fileTypes) {
        addFileTypeFilter(parsedType);
    }

    // Parse context
    QueryContext context;
    if (params.contains(QStringLiteral("context"))) {
        QJsonObject ctxObj = params.value(QStringLiteral("context")).toObject();
        if (ctxObj.contains(QStringLiteral("cwdPath"))) {
            context.cwdPath = ctxObj.value(QStringLiteral("cwdPath")).toString();
        }
        if (ctxObj.contains(QStringLiteral("frontmostAppBundleId"))) {
            context.frontmostAppBundleId = ctxObj.value(
                QStringLiteral("frontmostAppBundleId")).toString();
        }
        if (ctxObj.contains(QStringLiteral("clipboardBasename"))) {
            const QString basename = ctxObj.value(QStringLiteral("clipboardBasename"))
                .toString().trimmed().toLower();
            if (!basename.isEmpty()) {
                context.clipboardBasename = basename;
            }
        }
        if (ctxObj.contains(QStringLiteral("clipboardDirname"))) {
            const QString dirname = ctxObj.value(QStringLiteral("clipboardDirname"))
                .toString().trimmed().toLower();
            if (!dirname.isEmpty()) {
                context.clipboardDirname = dirname;
            }
        }
        if (ctxObj.contains(QStringLiteral("clipboardExtension"))) {
            const QString extension = normalizeFileTypeToken(
                ctxObj.value(QStringLiteral("clipboardExtension")).toString());
            if (!extension.isEmpty()) {
                context.clipboardExtension = extension;
            }
        }
        if (ctxObj.contains(QStringLiteral("recentPaths"))) {
            QJsonArray recentArr = ctxObj.value(QStringLiteral("recentPaths")).toArray();
            context.recentPaths.reserve(static_cast<size_t>(recentArr.size()));
            for (const auto& val : recentArr) {
                context.recentPaths.push_back(val.toString());
            }
        }
    }

    const auto readBoolSetting = [&](const QString& key, bool defaultValue) {
        if (!m_store.has_value()) {
            return defaultValue;
        }
        const auto raw = m_store->getSetting(key);
        if (!raw.has_value()) {
            return defaultValue;
        }
        const QString normalized = raw->trimmed().toLower();
        if (normalized.isEmpty()) {
            return defaultValue;
        }
        return normalized == QLatin1String("1")
            || normalized == QLatin1String("true")
            || normalized == QLatin1String("yes")
            || normalized == QLatin1String("on");
    };
    const auto readIntSetting = [&](const QString& key, int defaultValue) {
        if (!m_store.has_value()) {
            return defaultValue;
        }
        const auto raw = m_store->getSetting(key);
        if (!raw.has_value()) {
            return defaultValue;
        }
        bool ok = false;
        const int parsed = raw->toInt(&ok);
        return ok ? parsed : defaultValue;
    };
    const auto readDoubleSetting = [&](const QString& key, double defaultValue) {
        if (!m_store.has_value()) {
            return defaultValue;
        }
        const auto raw = m_store->getSetting(key);
        if (!raw.has_value()) {
            return defaultValue;
        }
        bool ok = false;
        const double parsed = raw->toDouble(&ok);
        return ok ? parsed : defaultValue;
    };

    const bool embeddingEnabled = readBoolSetting(QStringLiteral("embeddingEnabled"), true);
    const bool inferenceServiceEnabled =
        readBoolSetting(QStringLiteral("inferenceServiceEnabled"), true);
    const bool inferenceEmbedOffloadEnabled =
        readBoolSetting(QStringLiteral("inferenceEmbedOffloadEnabled"), true);
    const bool inferenceRerankOffloadEnabled =
        readBoolSetting(QStringLiteral("inferenceRerankOffloadEnabled"), true);
    const bool inferenceQaOffloadEnabled =
        readBoolSetting(QStringLiteral("inferenceQaOffloadEnabled"), true);
    const bool inferenceShadowModeEnabled =
        readBoolSetting(QStringLiteral("inferenceShadowModeEnabled"), false);
    const bool queryRouterEnabled = readBoolSetting(QStringLiteral("queryRouterEnabled"), true);
    const double queryRouterMinConfidence =
        std::clamp(readDoubleSetting(QStringLiteral("queryRouterMinConfidence"), 0.45), 0.0, 1.0);
    const bool fastEmbeddingEnabled = readBoolSetting(QStringLiteral("fastEmbeddingEnabled"), true);
    const bool dualEmbeddingFusionEnabled =
        readBoolSetting(QStringLiteral("dualEmbeddingFusionEnabled"), true);
    const int strongEmbeddingTopK = std::max(1, readIntSetting(QStringLiteral("strongEmbeddingTopK"), 40));
    const int fastEmbeddingTopK = std::max(1, readIntSetting(QStringLiteral("fastEmbeddingTopK"), 60));
    const int semanticBudgetMs = std::max(20, readIntSetting(QStringLiteral("semanticBudgetMs"), 70));
    const bool rerankerCascadeEnabled = readBoolSetting(QStringLiteral("rerankerCascadeEnabled"), true);
    const int rerankBudgetMs = std::max(40, readIntSetting(QStringLiteral("rerankBudgetMs"), 120));
    const int rerankerStage1Max = std::max(4, readIntSetting(QStringLiteral("rerankerStage1Max"), 40));
    const int rerankerStage2Max = std::max(4, readIntSetting(QStringLiteral("rerankerStage2Max"), 12));
    const bool personalizedLtrEnabled = readBoolSetting(QStringLiteral("personalizedLtrEnabled"), true);
    const double semanticThresholdNaturalLanguageBase = std::clamp(
        readDoubleSetting(QStringLiteral("semanticThresholdNaturalLanguageBase"), 0.62), 0.0, 1.0);
    const double semanticThresholdShortAmbiguousBase = std::clamp(
        readDoubleSetting(QStringLiteral("semanticThresholdShortAmbiguousBase"), 0.66), 0.0, 1.0);
    const double semanticThresholdPathOrCodeBase = std::clamp(
        readDoubleSetting(QStringLiteral("semanticThresholdPathOrCodeBase"), 0.70), 0.0, 1.0);
    const double semanticThresholdNeedScale = std::clamp(
        readDoubleSetting(QStringLiteral("semanticThresholdNeedScale"), 0.06), 0.0, 1.0);
    const double semanticThresholdMin = std::clamp(
        readDoubleSetting(QStringLiteral("semanticThresholdMin"), 0.55), 0.0, 1.0);
    const double semanticThresholdMax = std::clamp(
        readDoubleSetting(QStringLiteral("semanticThresholdMax"), 0.80),
        semanticThresholdMin,
        1.0);
    const double semanticOnlyFloorNaturalLanguage = std::clamp(
        readDoubleSetting(QStringLiteral("semanticOnlyFloorNaturalLanguage"), 0.08), 0.0, 1.0);
    const double semanticOnlyFloorShortAmbiguous = std::clamp(
        readDoubleSetting(QStringLiteral("semanticOnlyFloorShortAmbiguous"), 0.10), 0.0, 1.0);
    const double semanticOnlyFloorPathOrCode = std::clamp(
        readDoubleSetting(QStringLiteral("semanticOnlyFloorPathOrCode"), 0.15), 0.0, 1.0);
    const double strictLexicalWeakCutoff = std::max(
        0.0, readDoubleSetting(QStringLiteral("strictLexicalWeakCutoff"), 2.0));
    const int semanticOnlyCapNaturalLanguageWeak = std::max(
        1, readIntSetting(QStringLiteral("semanticOnlyCapNaturalLanguageWeak"), 8));
    const int semanticOnlyCapNaturalLanguageStrong = std::max(
        1, readIntSetting(QStringLiteral("semanticOnlyCapNaturalLanguageStrong"), 6));
    const int semanticOnlyCapShortAmbiguous = std::max(
        1, readIntSetting(QStringLiteral("semanticOnlyCapShortAmbiguous"), 4));
    const int semanticOnlyCapPathOrCode = std::max(
        1, readIntSetting(QStringLiteral("semanticOnlyCapPathOrCode"), 3));
    const int semanticOnlyCapPathOrCodeDivisor = std::max(
        1, readIntSetting(QStringLiteral("semanticOnlyCapPathOrCodeDivisor"), 2));
    const double mergeLexicalWeightNaturalLanguageWeak = std::clamp(
        readDoubleSetting(QStringLiteral("mergeLexicalWeightNaturalLanguageWeak"), 0.45), 0.0, 1.0);
    const double mergeSemanticWeightNaturalLanguageWeak = std::clamp(
        readDoubleSetting(QStringLiteral("mergeSemanticWeightNaturalLanguageWeak"), 0.55), 0.0, 1.0);
    const double mergeLexicalWeightNaturalLanguageStrong = std::clamp(
        readDoubleSetting(QStringLiteral("mergeLexicalWeightNaturalLanguageStrong"), 0.55), 0.0, 1.0);
    const double mergeSemanticWeightNaturalLanguageStrong = std::clamp(
        readDoubleSetting(QStringLiteral("mergeSemanticWeightNaturalLanguageStrong"), 0.45), 0.0, 1.0);
    const double mergeLexicalWeightPathOrCode = std::clamp(
        readDoubleSetting(QStringLiteral("mergeLexicalWeightPathOrCode"), 0.75), 0.0, 1.0);
    const double mergeSemanticWeightPathOrCode = std::clamp(
        readDoubleSetting(QStringLiteral("mergeSemanticWeightPathOrCode"), 0.25), 0.0, 1.0);
    const double mergeLexicalWeightShortAmbiguous = std::clamp(
        readDoubleSetting(QStringLiteral("mergeLexicalWeightShortAmbiguous"), 0.65), 0.0, 1.0);
    const double mergeSemanticWeightShortAmbiguous = std::clamp(
        readDoubleSetting(QStringLiteral("mergeSemanticWeightShortAmbiguous"), 0.35), 0.0, 1.0);
    const double semanticOnlySafetySimilarityWeakNatural = std::clamp(
        readDoubleSetting(QStringLiteral("semanticOnlySafetySimilarityWeakNatural"), 0.74), 0.0, 1.0);
    const double semanticOnlySafetySimilarityDefault = std::clamp(
        readDoubleSetting(QStringLiteral("semanticOnlySafetySimilarityDefault"), 0.78), 0.0, 1.0);
    const double relaxedSemanticOnlyDeltaWeakNatural = std::max(
        0.0, readDoubleSetting(QStringLiteral("relaxedSemanticOnlyDeltaWeakNatural"), 0.02));
    const double relaxedSemanticOnlyDeltaDefault = std::max(
        0.0, readDoubleSetting(QStringLiteral("relaxedSemanticOnlyDeltaDefault"), 0.03));
    const double relaxedSemanticOnlyMinWeakNatural = std::clamp(
        readDoubleSetting(QStringLiteral("relaxedSemanticOnlyMinWeakNatural"), 0.64), 0.0, 1.0);
    const double relaxedSemanticOnlyMinDefault = std::clamp(
        readDoubleSetting(QStringLiteral("relaxedSemanticOnlyMinDefault"), 0.66), 0.0, 1.0);
    const int semanticPassageCapNaturalLanguage = std::max(
        1, readIntSetting(QStringLiteral("semanticPassageCapNaturalLanguage"), 3));
    const int semanticPassageCapOther = std::max(
        1, readIntSetting(QStringLiteral("semanticPassageCapOther"), 2));
    const double semanticSoftmaxTemperatureNaturalLanguage = std::max(
        0.1, readDoubleSetting(QStringLiteral("semanticSoftmaxTemperatureNaturalLanguage"), 8.0));
    const double semanticSoftmaxTemperatureOther = std::max(
        0.1, readDoubleSetting(QStringLiteral("semanticSoftmaxTemperatureOther"), 6.0));
    const double rerankerStage1WeightScale = std::clamp(
        readDoubleSetting(QStringLiteral("rerankerStage1WeightScale"), 0.55), 0.0, 4.0);
    const double rerankerStage1MinWeight = std::max(
        0.0, readDoubleSetting(QStringLiteral("rerankerStage1MinWeight"), 8.0));
    const double rerankerStage2WeightScale = std::clamp(
        readDoubleSetting(QStringLiteral("rerankerStage2WeightScale"), 1.0), 0.0, 4.0);
    const double rerankerAmbiguityMarginThreshold = std::clamp(
        readDoubleSetting(QStringLiteral("rerankerAmbiguityMarginThreshold"), 0.08), 0.0, 1.0);
    const int rerankerFallbackElapsed80Ms = std::max(
        1, readIntSetting(QStringLiteral("rerankerFallbackElapsed80Ms"), 80));
    const int rerankerFallbackElapsed130Ms = std::max(
        rerankerFallbackElapsed80Ms,
        readIntSetting(QStringLiteral("rerankerFallbackElapsed130Ms"), 130));
    const int rerankerFallbackElapsed180Ms = std::max(
        rerankerFallbackElapsed130Ms,
        readIntSetting(QStringLiteral("rerankerFallbackElapsed180Ms"), 180));
    const int rerankerFallbackCapDefault = std::max(
        1, readIntSetting(QStringLiteral("rerankerFallbackCapDefault"), 40));
    const int rerankerFallbackCapElapsed80 = std::max(
        1, readIntSetting(QStringLiteral("rerankerFallbackCapElapsed80"), 32));
    const int rerankerFallbackCapElapsed130 = std::max(
        1, readIntSetting(QStringLiteral("rerankerFallbackCapElapsed130"), 24));
    const int rerankerFallbackCapElapsed180 = std::max(
        1, readIntSetting(QStringLiteral("rerankerFallbackCapElapsed180"), 12));
    const int rerankerFallbackBudgetCap = std::max(
        1, readIntSetting(QStringLiteral("rerankerFallbackBudgetCap"), 8));

    const QString queryLower = query.toLower();
    const QueryHints queryHints = parseQueryHints(queryLower);

    // Stage 0: Query understanding (rules engine)
    const StructuredQuery structured = RulesEngine::analyze(originalRawQuery);

    const QStringList queryTokensRaw = tokenizeWords(queryLower);
    QSet<QString> highSignalShortTokens;
    {
        static const QRegularExpression rawTokenRegex(QStringLiteral(R"([A-Za-z0-9_]+)"));
        auto tokenMatch = rawTokenRegex.globalMatch(originalRawQuery);
        while (tokenMatch.hasNext()) {
            const QString token = tokenMatch.next().captured(0);
            if (token.size() != 3) {
                continue;
            }
            bool hasAlpha = false;
            bool allUpper = true;
            for (const QChar ch : token) {
                if (ch.isLetter()) {
                    hasAlpha = true;
                    if (!ch.isUpper()) {
                        allUpper = false;
                        break;
                    }
                }
            }
            const bool hasDigit = std::any_of(token.begin(), token.end(),
                [](QChar c) { return c.isDigit(); });
            if ((hasAlpha && allUpper) || (hasAlpha && hasDigit)) {
                highSignalShortTokens.insert(token.toLower());
            }
        }
    }
    QSet<QString> querySignalTokens;
    for (const QString& token : queryTokensRaw) {
        if (token.size() >= 3 && !queryStopwords().contains(token)) {
            querySignalTokens.insert(token);
        }
    }

    QString plannerReason = QStringLiteral("none");
    bool plannerApplied = false;
    const QString homePath = QDir::homePath();
    const QString documentsPath = homePath + QStringLiteral("/Documents");
    const QString desktopPath = homePath + QStringLiteral("/Desktop");
    const QString downloadsPath = homePath + QStringLiteral("/Downloads");

    if (!hasUserProvidedFilters) {
        if (queryHints.documentsHint || queryHints.desktopHint || queryHints.downloadsHint) {
            plannerReason = QStringLiteral("query_location_hint");
            if (queryHints.documentsHint) {
                addPathFilterUnique(searchOptions.includePaths, documentsPath);
            }
            if (queryHints.desktopHint) {
                addPathFilterUnique(searchOptions.includePaths, desktopPath);
            }
            if (queryHints.downloadsHint) {
                addPathFilterUnique(searchOptions.includePaths, downloadsPath);
            }
            plannerApplied = !searchOptions.includePaths.empty();
        } else if (shouldApplyConsumerPrefilter(queryLower, queryTokensRaw, querySignalTokens)) {
            // Consumer-first default: constrain natural-language lookups to
            // high-signal user roots unless callers opt into explicit filters.
            plannerReason = QStringLiteral("consumer_curated_prefilter");
            addPathFilterUnique(searchOptions.includePaths, documentsPath);
            addPathFilterUnique(searchOptions.includePaths, desktopPath);
            addPathFilterUnique(searchOptions.includePaths, downloadsPath);
            plannerApplied = true;
        }
    }

    const bool hasSearchFilters = searchOptions.hasFilters();

    LOG_INFO(bsIpc, "Search: query='%s' limit=%d mode=%d",
             qPrintable(query), limit, static_cast<int>(queryMode));

    // Build cache key from normalized query + mode + filters
    QString cacheKey = query + QStringLiteral("|")
        + QString::number(static_cast<int>(queryMode));
    if (!searchOptions.fileTypes.empty()) {
        QStringList sortedTypes;
        sortedTypes.reserve(static_cast<int>(searchOptions.fileTypes.size()));
        for (const auto& ft : searchOptions.fileTypes) { sortedTypes.append(ft); }
        sortedTypes.sort();
        cacheKey += QStringLiteral("|ft:") + sortedTypes.join(QStringLiteral(","));
    }
    if (!searchOptions.includePaths.empty()) {
        QStringList sortedPaths;
        sortedPaths.reserve(static_cast<int>(searchOptions.includePaths.size()));
        for (const auto& p : searchOptions.includePaths) { sortedPaths.append(p); }
        sortedPaths.sort();
        cacheKey += QStringLiteral("|ip:") + sortedPaths.join(QStringLiteral(","));
    }

    // Check cache (skip for debug requests — callers expect fresh data)
    if (!debugRequested) {
        auto cached = m_queryCache.get(cacheKey);
        if (cached.has_value()) {
            QJsonObject cachedResult = cached.value();
            cachedResult[QStringLiteral("cached")] = true;
            return IpcMessage::makeResponse(id, cachedResult);
        }
    }

    QElapsedTimer timer;
    timer.start();

    // Overquery for ranking: fetch limit * 2 from strict FTS5
    int ftsLimit = limit * 2;
    std::vector<SQLiteStore::FtsHit> hits;
    std::vector<SQLiteStore::FtsHit> strictHits;
    std::vector<SQLiteStore::FtsHit> relaxedHits;
    std::unordered_map<int64_t, uint8_t> candidateOrigins;
    candidateOrigins.reserve(static_cast<size_t>(limit * 6));
    RewriteDecision rewriteDecision;
    QJsonArray correctedTokensDebug;
    QString rewrittenRelaxedQuery;
    QString classifyQuery = query;
    QString nameFuzzyQuery = query;
    nameFuzzyQuery.replace(QLatin1Char('-'), QLatin1Char(' '));
    // Hydrated item cache: populated by searchFts5Joined, avoids N+1 getItemById calls.
    std::unordered_map<int64_t, SQLiteStore::FtsJoinedHit> hydratedItemCache;
    hydratedItemCache.reserve(static_cast<size_t>(limit * 6));

    const auto runFtsSearch = [&](const QString& q, int localLimit, bool relaxedMode) {
        auto joinedHits = m_store->searchFts5Joined(q, localLimit, relaxedMode, searchOptions);
        std::vector<SQLiteStore::FtsHit> ftsHits;
        ftsHits.reserve(joinedHits.size());
        for (auto& jh : joinedHits) {
            if (hydratedItemCache.find(jh.fileId) == hydratedItemCache.end()) {
                hydratedItemCache.emplace(jh.fileId, jh);
            }
            SQLiteStore::FtsHit fh;
            fh.fileId = jh.fileId;
            fh.chunkId = jh.chunkId;
            fh.bm25Score = jh.bm25Score;
            fh.snippet = jh.snippet;
            ftsHits.push_back(std::move(fh));
        }
        return ftsHits;
    };
    const auto runNameSearch = [&](const QString& q, int localLimit) {
        return hasSearchFilters
            ? m_store->searchByNameFuzzy(q, localLimit, searchOptions)
            : m_store->searchByNameFuzzy(q, localLimit);
    };

    const auto markOrigins = [&](const std::vector<SQLiteStore::FtsHit>& sourceHits,
                                 uint8_t originFlag) {
        for (const auto& hit : sourceHits) {
            candidateOrigins[hit.fileId] |= originFlag;
        }
    };

    constexpr double kRewriteAggregateThreshold = 0.72;
    constexpr double kRewriteCandidateThreshold = 0.66;

    auto buildTypoRewriteDecision = [&](int maxReplacements,
                                        bool allowDistanceTwo) -> RewriteDecision {
        RewriteDecision decision;
        QStringList queryTokens = tokenizeWords(query);
        if (queryTokens.isEmpty()) {
            decision.reason = QStringLiteral("empty_query_tokens");
            return decision;
        }
        if (!ensureTypoLexiconReady()) {
            decision.reason = QStringLiteral("typo_lexicon_unavailable");
            decision.rewrittenQuery = query;
            return decision;
        }

        const QSet<QString>& stopwords = queryStopwords();
        QVector<double> appliedCandidateConfidences;
        appliedCandidateConfidences.reserve(std::max(1, maxReplacements));
        int appliedReplacements = 0;
        for (int i = 0; i < queryTokens.size(); ++i) {
            const QString token = queryTokens.at(i);
            const bool eligibleShortToken =
                token.size() == 3 && highSignalShortTokens.contains(token.toLower());
            if ((!eligibleShortToken && token.size() < 4) || stopwords.contains(token)) {
                continue;
            }

            if (m_typoLexicon.contains(token)) {
                continue;
            }

            ++decision.candidatesConsidered;
            std::optional<TypoLexicon::Correction> correction = m_typoLexicon.correct(token, 1);
            if (!correction.has_value() && allowDistanceTwo && token.size() >= 8) {
                const auto distTwo = m_typoLexicon.correct(token, 2);
                if (distTwo.has_value() && distTwo->editDistance <= 2 && distTwo->docCount >= 5) {
                    correction = distTwo;
                }
            }
            if (!correction.has_value() || correction->corrected == token) {
                continue;
            }

            const double candidateConfidence = typoCandidateConfidence(token, correction.value());
            const double effectiveThreshold = (correction->docCount >= 25)
                ? 0.60 : kRewriteCandidateThreshold;
            if (candidateConfidence < effectiveThreshold) {
                continue;
            }

            QJsonObject replacement;
            replacement[QStringLiteral("from")] = token;
            replacement[QStringLiteral("to")] = correction->corrected;
            replacement[QStringLiteral("editDistance")] = correction->editDistance;
            replacement[QStringLiteral("docCount")] =
                static_cast<qint64>(correction->docCount);
            replacement[QStringLiteral("candidateConfidence")] = candidateConfidence;
            decision.correctedTokens.append(replacement);
            queryTokens[i] = correction->corrected;
            decision.hasCandidate = true;
            appliedCandidateConfidences.append(candidateConfidence);
            ++appliedReplacements;

            if (appliedReplacements >= std::max(1, maxReplacements)) {
                break;
            }
        }

        if (!decision.hasCandidate) {
            decision.reason = QStringLiteral("no_corrections");
            decision.rewrittenQuery = query;
            return decision;
        }

        double aggregate = 0.0;
        double minCandidate = 1.0;
        for (double value : appliedCandidateConfidences) {
            aggregate += value;
            minCandidate = std::min(minCandidate, value);
        }
        aggregate /= static_cast<double>(appliedCandidateConfidences.size());
        decision.confidence = aggregate;
        decision.minCandidateConfidence = minCandidate;
        decision.rewrittenQuery = queryTokens.join(QLatin1Char(' '));
        return decision;
    };

    const int relaxedSearchLimit = std::max(ftsLimit * 2, limit * 4);
    switch (queryMode) {
    case SearchQueryMode::Strict:
        strictHits = runFtsSearch(query, ftsLimit, false);
        hits = strictHits;
        markOrigins(strictHits, CandidateOriginStrict);
        rewriteDecision.reason = QStringLiteral("strict_mode");
        break;
    case SearchQueryMode::Relaxed: {
        rewriteDecision = buildTypoRewriteDecision(2, true);
        const auto relaxedOriginalHits = runFtsSearch(query, relaxedSearchLimit, true);
        relaxedHits = relaxedOriginalHits;
        rewrittenRelaxedQuery = query;

        if (rewriteDecision.hasCandidate
            && rewriteDecision.confidence >= kRewriteAggregateThreshold
            && rewriteDecision.rewrittenQuery != query) {
            auto rewrittenHits = runFtsSearch(rewriteDecision.rewrittenQuery, relaxedSearchLimit, true);
            if (bestLexicalStrength(rewrittenHits) >= bestLexicalStrength(relaxedOriginalHits)) {
                rewrittenRelaxedQuery = rewriteDecision.rewrittenQuery;
                rewriteDecision.applied = true;
                rewriteDecision.reason = QStringLiteral("relaxed_mode_high_confidence");
                relaxedHits = std::move(rewrittenHits);
            } else {
                rewriteDecision.reason = QStringLiteral("rewritten_weaker_than_original");
            }
        } else if (rewriteDecision.hasCandidate) {
            rewriteDecision.reason = QStringLiteral("low_confidence");
        } else {
            rewriteDecision.reason = QStringLiteral("no_corrections");
        }

        classifyQuery = rewrittenRelaxedQuery;
        hits = relaxedHits;
        markOrigins(relaxedHits, CandidateOriginRelaxed);
        break;
    }
    case SearchQueryMode::Auto:
    default: {
        strictHits = runFtsSearch(query, ftsLimit, false);
        hits = strictHits;
        markOrigins(strictHits, CandidateOriginStrict);

        const bool strictWeakOrEmpty = strictHits.empty();
        const int signalTokenCount = static_cast<int>(querySignalTokens.size());
        const int rewriteBudget = strictWeakOrEmpty
            ? std::clamp(signalTokenCount / 2, 2, 3)
            : std::clamp(signalTokenCount / 3, 1, 2);
        if (strictWeakOrEmpty) {
            rewriteDecision = buildTypoRewriteDecision(rewriteBudget, true);
        } else {
            rewriteDecision.reason = QStringLiteral("strict_hits_present");
            rewriteDecision.rewrittenQuery = query;
        }

        if (strictWeakOrEmpty) {
            const auto relaxedOriginalHits = runFtsSearch(query, relaxedSearchLimit, true);
            relaxedHits = relaxedOriginalHits;
            rewrittenRelaxedQuery = query;

            if (rewriteDecision.hasCandidate
                && rewriteDecision.confidence >= kRewriteAggregateThreshold
                && rewriteDecision.rewrittenQuery != query) {
                auto rewrittenHits = runFtsSearch(rewriteDecision.rewrittenQuery, relaxedSearchLimit, true);
                const bool rewrittenStronger = bestLexicalStrength(rewrittenHits)
                                               >= bestLexicalStrength(relaxedOriginalHits);
                const auto rewrittenNameHits = runNameSearch(rewriteDecision.rewrittenQuery, 5);
                const auto originalNameHits = runNameSearch(query, 5);
                const bool rewrittenHasNameHit = !rewrittenNameHits.empty();
                const bool originalHasNameHit = !originalNameHits.empty();
                if (rewrittenStronger || (rewrittenHasNameHit && !originalHasNameHit)) {
                    rewrittenRelaxedQuery = rewriteDecision.rewrittenQuery;
                    rewriteDecision.applied = true;
                    rewriteDecision.reason = rewrittenStronger
                        ? QStringLiteral("strict_weak_or_empty")
                        : QStringLiteral("rewritten_has_name_match");
                    relaxedHits = std::move(rewrittenHits);
                } else {
                    rewriteDecision.reason = QStringLiteral("rewritten_weaker_than_original");
                }
            } else if (rewriteDecision.hasCandidate) {
                rewriteDecision.reason = QStringLiteral("low_confidence");
            } else {
                rewriteDecision.reason = QStringLiteral("strict_empty_relaxed_original");
            }

            classifyQuery = rewrittenRelaxedQuery;
            hits.insert(hits.end(), relaxedHits.begin(), relaxedHits.end());
            markOrigins(relaxedHits, CandidateOriginRelaxed);
        } else {
            if (!rewriteDecision.hasCandidate) {
                rewriteDecision.reason = QStringLiteral("no_corrections");
            } else if (rewriteDecision.confidence < kRewriteAggregateThreshold) {
                rewriteDecision.reason = QStringLiteral("low_confidence");
            } else {
                rewriteDecision.reason = QStringLiteral("strict_hits_present");
            }
        }
        break;
    }
    }
    correctedTokensDebug = rewriteDecision.correctedTokens;

    const int maxNameFallbackAdds = std::max(3, std::min(6, limit / 2));
    int nameFallbackAdded = 0;
    const auto appendNameFallbackHits = [&](const QString& q, int localLimit) {
        if (nameFallbackAdded >= maxNameFallbackAdds) {
            return;
        }
        auto nameHits = runNameSearch(q, localLimit);
        for (const auto& nh : nameHits) {
            if (nameFallbackAdded >= maxNameFallbackAdds) {
                break;
            }
            bool alreadyPresent = std::any_of(hits.begin(), hits.end(),
                [&](const SQLiteStore::FtsHit& h) { return h.fileId == nh.fileId; });
            if (alreadyPresent) {
                continue;
            }
            SQLiteStore::FtsHit fakeHit;
            fakeHit.fileId = nh.fileId;
            fakeHit.bm25Score = -50.0;
            fakeHit.snippet = QString();
            hits.push_back(fakeHit);
            candidateOrigins[nh.fileId] |= CandidateOriginNameFallback;
            ++nameFallbackAdded;
        }
    };

    // Fuzzy filename fallback when all FTS paths return empty.
    if (hits.empty()) {
        appendNameFallbackHits(nameFuzzyQuery, ftsLimit);
    }

    // Always merge fuzzy name matches so files with matching names but no
    // extracted content (0 FTS5 rows) still appear in results.
    // Use both original and typo-rewritten query to cover misspellings.
    {
        QString rewrittenNameQuery = rewrittenRelaxedQuery.isEmpty() ? query : rewrittenRelaxedQuery;
        rewrittenNameQuery.replace(QLatin1Char('-'), QLatin1Char(' '));
        for (const QString& q : {nameFuzzyQuery, rewrittenNameQuery}) {
            appendNameFallbackHits(q, std::max(3, limit));
        }
    }

    const int strictHitsCount = static_cast<int>(strictHits.size());
    const int relaxedHitsCount = static_cast<int>(relaxedHits.size());
    const int totalMatches = static_cast<int>(hits.size());

    // Build SearchResult list from FTS hits.
    // Deduplicate by itemId and keep the strongest lexical chunk per file.
    // Uses hydratedItemCache from searchFts5Joined to avoid N+1 getItemById calls.
    std::vector<SearchResult> results;
    results.reserve(hits.size());
    std::unordered_map<int64_t, size_t> bestHitByItem;
    bestHitByItem.reserve(hits.size());
    QString classifyMatchQuery = classifyQuery;
    classifyMatchQuery.replace(QLatin1Char('-'), QLatin1Char(' '));

    // Batch-fetch frequencies for all candidate items (replaces per-item getFrequency)
    std::vector<int64_t> candidateItemIds;
    candidateItemIds.reserve(hits.size());
    {
        std::unordered_set<int64_t> seen;
        seen.reserve(hits.size());
        for (const auto& hit : hits) {
            if (seen.insert(hit.fileId).second) {
                candidateItemIds.push_back(hit.fileId);
            }
        }
    }
    const auto freqMap = m_store->getFrequenciesBatch(candidateItemIds);

    for (const auto& hit : hits) {
        // Try hydrated cache first (populated by searchFts5Joined)
        auto cachedIt = hydratedItemCache.find(hit.fileId);
        if (cachedIt == hydratedItemCache.end()) {
            // Fallback for items not in cache (e.g., name fallback hits)
            auto itemOpt = m_store->getItemById(hit.fileId);
            if (!itemOpt.has_value()) {
                continue;
            }
            SQLiteStore::FtsJoinedHit jh;
            jh.fileId = itemOpt->id;
            jh.path = itemOpt->path;
            jh.name = itemOpt->name;
            jh.kind = itemOpt->kind;
            jh.size = itemOpt->size;
            jh.modifiedAt = itemOpt->modifiedAt;
            jh.isPinned = itemOpt->isPinned;
            cachedIt = hydratedItemCache.emplace(hit.fileId, std::move(jh)).first;
        }
        const auto& cachedItem = cachedIt->second;

        if (isExcludedByBsignore(cachedItem.path)) {
            continue;
        }

        SearchResult sr;
        sr.itemId = cachedItem.fileId;
        sr.path = cachedItem.path;
        sr.name = cachedItem.name;
        sr.kind = cachedItem.kind;
        sr.bm25RawScore = hit.bm25Score;
        sr.snippet = hit.snippet;
        sr.highlights = parseHighlights(sr.snippet);
        sr.fileSize = cachedItem.size;

        // Format modification date as ISO 8601
        if (cachedItem.modifiedAt > 0.0) {
            sr.modificationDate = QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>(cachedItem.modifiedAt * 1000.0)).toUTC().toString(Qt::ISODate);
        }

        sr.isPinned = cachedItem.isPinned;

        // Look up frequency from batch result
        auto freqIt = freqMap.find(cachedItem.fileId);
        if (freqIt != freqMap.end()) {
            sr.openCount = freqIt->second.openCount;
            if (freqIt->second.lastOpenedAt > 0.0) {
                sr.lastOpenDate = QDateTime::fromMSecsSinceEpoch(
                    static_cast<qint64>(freqIt->second.lastOpenedAt * 1000.0))
                    .toUTC().toString(Qt::ISODate);
            }
        }

        // Classify match type for name/path matches
        sr.matchType = MatchClassifier::classify(classifyMatchQuery, cachedItem.name, cachedItem.path);
        if (sr.matchType == MatchType::Fuzzy) {
            if (hit.bm25Score == -1.0) {
                // Fuzzy filename fallback does not expose exact edit distance yet.
                sr.fuzzyDistance = 1;
            } else {
                const QString baseName = QFileInfo(cachedItem.name).completeBaseName();
                sr.fuzzyDistance = MatchClassifier::editDistance(classifyMatchQuery, baseName);
            }
        }

        const double lexicalStrength = std::max(0.0, -hit.bm25Score);
        auto existingIt = bestHitByItem.find(cachedItem.fileId);
        if (existingIt == bestHitByItem.end()) {
            bestHitByItem[cachedItem.fileId] = results.size();
            results.push_back(std::move(sr));
            continue;
        }

        SearchResult& existing = results[existingIt->second];
        const double existingStrength = std::max(0.0, -existing.bm25RawScore);
        if (lexicalStrength > existingStrength) {
            existing = std::move(sr);
        }
    }

    // Apply multi-signal ranking (M1 base scoring)
    m_scorer.rankResults(results, context);

    std::unordered_set<int64_t> lexicalItemIds;
    lexicalItemIds.reserve(results.size());
    for (const auto& result : results) {
        lexicalItemIds.insert(result.itemId);
    }

    // M2: Semantic search + merge
    const auto itemPassesSearchOptions = [&](const SQLiteStore::ItemRow& item) {
        if (!searchOptions.includePaths.empty()) {
            bool insideIncludedRoot = false;
            for (const QString& includePrefix : searchOptions.includePaths) {
                if (item.path.startsWith(includePrefix)) {
                    insideIncludedRoot = true;
                    break;
                }
            }
            if (!insideIncludedRoot) {
                return false;
            }
        }

        for (const QString& excludePrefix : searchOptions.excludePaths) {
            if (item.path.startsWith(excludePrefix)) {
                return false;
            }
        }

        if (!searchOptions.fileTypes.empty()) {
            const QString ext = QFileInfo(item.path).suffix().toLower();
            bool matchedType = false;
            for (const QString& rawType : searchOptions.fileTypes) {
                if (normalizeFileTypeToken(rawType) == ext) {
                    matchedType = true;
                    break;
                }
            }
            if (!matchedType) {
                return false;
            }
        }

        if (searchOptions.modifiedAfter.has_value()
            && item.modifiedAt < searchOptions.modifiedAfter.value()) {
            return false;
        }
        if (searchOptions.modifiedBefore.has_value()
            && item.modifiedAt > searchOptions.modifiedBefore.value()) {
            return false;
        }
        if (searchOptions.minSizeBytes.has_value()
            && item.size < searchOptions.minSizeBytes.value()) {
            return false;
        }
        if (searchOptions.maxSizeBytes.has_value()
            && item.size > searchOptions.maxSizeBytes.value()) {
            return false;
        }

        return true;
    };

    QueryClass queryClass = classifyQueryShape(queryLower, querySignalTokens, queryTokensRaw);
    bool routerApplied = false;
    float routerConfidence = 0.0f;
    QueryDomain queryDomain = QueryDomain::Unknown;
    if (queryRouterEnabled
        && structured.queryClass != QueryClass::Unknown
        && structured.queryClassConfidence >= queryRouterMinConfidence) {
        queryClass = structured.queryClass;
        queryDomain = structured.queryDomain;
        routerConfidence = structured.queryClassConfidence;
        routerApplied = true;
    }

    const bool naturalLanguageQuery = queryClass == QueryClass::NaturalLanguage;
    const bool shortAmbiguousQuery = queryClass == QueryClass::ShortAmbiguous;
    const float routerSemanticNeed = std::clamp(structured.semanticNeedScore, 0.0f, 1.0f);
    const float semanticThresholdBase = naturalLanguageQuery
        ? static_cast<float>(semanticThresholdNaturalLanguageBase)
        : (shortAmbiguousQuery
            ? static_cast<float>(semanticThresholdShortAmbiguousBase)
            : static_cast<float>(semanticThresholdPathOrCodeBase));
    const float semanticThreshold = std::clamp(
        semanticThresholdBase
            - ((routerApplied ? routerSemanticNeed : 0.0f)
               * static_cast<float>(semanticThresholdNeedScale)),
        static_cast<float>(semanticThresholdMin),
        static_cast<float>(semanticThresholdMax));
    const float semanticOnlyFloor = naturalLanguageQuery
        ? static_cast<float>(semanticOnlyFloorNaturalLanguage)
        : (shortAmbiguousQuery
            ? static_cast<float>(semanticOnlyFloorShortAmbiguous)
            : static_cast<float>(semanticOnlyFloorPathOrCode));
    const bool strictLexicalWeakOrEmpty =
        strictHits.empty() || bestLexicalStrength(strictHits) < strictLexicalWeakCutoff;
    const int semanticOnlyCap = naturalLanguageQuery
        ? (strictLexicalWeakOrEmpty
               ? std::min(semanticOnlyCapNaturalLanguageWeak, limit)
               : std::min(semanticOnlyCapNaturalLanguageStrong, limit))
        : (shortAmbiguousQuery
               ? std::min(semanticOnlyCapShortAmbiguous, limit)
               : std::min(semanticOnlyCapPathOrCode,
                          std::max(1, limit / semanticOnlyCapPathOrCodeDivisor)));

    float mergeLexicalWeight, mergeSemanticWeight;
    const auto normalizeBlendWeights = [](double lexicalWeight,
                                          double semanticWeight,
                                          double defaultLexical,
                                          double defaultSemantic) {
        lexicalWeight = std::clamp(lexicalWeight, 0.0, 1.0);
        semanticWeight = std::clamp(semanticWeight, 0.0, 1.0);
        const double sum = lexicalWeight + semanticWeight;
        if (sum > 0.000001) {
            lexicalWeight /= sum;
            semanticWeight /= sum;
        } else {
            lexicalWeight = defaultLexical;
            semanticWeight = defaultSemantic;
        }
        return std::pair<float, float>{
            static_cast<float>(lexicalWeight),
            static_cast<float>(semanticWeight),
        };
    };
    if (naturalLanguageQuery) {
        const auto normalized = strictLexicalWeakOrEmpty
            ? normalizeBlendWeights(
                mergeLexicalWeightNaturalLanguageWeak,
                mergeSemanticWeightNaturalLanguageWeak,
                0.45,
                0.55)
            : normalizeBlendWeights(
                mergeLexicalWeightNaturalLanguageStrong,
                mergeSemanticWeightNaturalLanguageStrong,
                0.55,
                0.45);
        mergeLexicalWeight = normalized.first;
        mergeSemanticWeight = normalized.second;
    } else if (queryClass == QueryClass::PathOrCode) {
        const auto normalized = normalizeBlendWeights(
            mergeLexicalWeightPathOrCode,
            mergeSemanticWeightPathOrCode,
            0.75,
            0.25);
        mergeLexicalWeight = normalized.first;
        mergeSemanticWeight = normalized.second;
    } else { // ShortAmbiguous
        const auto normalized = normalizeBlendWeights(
            mergeLexicalWeightShortAmbiguous,
            mergeSemanticWeightShortAmbiguous,
            0.65,
            0.35);
        mergeLexicalWeight = normalized.first;
        mergeSemanticWeight = normalized.second;
    }

    const float kSemanticOnlySafetySimilarity =
        (strictLexicalWeakOrEmpty && naturalLanguageQuery)
            ? static_cast<float>(semanticOnlySafetySimilarityWeakNatural)
            : static_cast<float>(semanticOnlySafetySimilarityDefault);
    const float relaxedSemanticOnlySimilarity =
        (strictLexicalWeakOrEmpty && naturalLanguageQuery)
            ? std::max(
                semanticThreshold + static_cast<float>(relaxedSemanticOnlyDeltaWeakNatural),
                static_cast<float>(relaxedSemanticOnlyMinWeakNatural))
            : std::max(
                semanticThreshold + static_cast<float>(relaxedSemanticOnlyDeltaDefault),
                static_cast<float>(relaxedSemanticOnlyMinDefault));

    std::vector<SemanticResult> semanticResults;
    std::unordered_map<int64_t, float> semanticSimilarityByItemId;
    std::unordered_map<int64_t, float> semanticNormalizedByItemId;
    int semanticOnlySuppressedCount = 0;
    int semanticOnlyAdmittedCount = 0;
    QHash<QString, int> semanticOnlyAdmitReasons;
    int strongSemanticCandidates = 0;
    int fastSemanticCandidates = 0;
    bool dualIndexUsed = false;
    const bool inferenceEmbedOffloadActive =
        embeddingEnabled && inferenceServiceEnabled && inferenceEmbedOffloadEnabled;
    if (embeddingEnabled && m_vectorStore) {
        QElapsedTimer semanticTimer;
        semanticTimer.start();
        std::unordered_map<int64_t, double> combinedSemanticByItemId;
        combinedSemanticByItemId.reserve(128);

        const auto parseEmbeddingVector = [](const QJsonArray& values) {
            std::vector<float> out;
            out.reserve(static_cast<size_t>(values.size()));
            for (const QJsonValue& value : values) {
                out.push_back(static_cast<float>(value.toDouble(0.0)));
            }
            return out;
        };

        auto accumulateSemantic = [&](const QString& role,
                                      EmbeddingManager* manager,
                                      VectorIndex* index,
                                      const QString& generation,
                                      int topK,
                                      double generationWeight,
                                      int& candidateCounter) {
            if (!index || !index->isAvailable() || generationWeight <= 0.0) {
                return;
            }
            std::vector<float> queryVec;
            if (inferenceEmbedOffloadActive) {
                const qint64 remainingBudget = std::max(
                    1, semanticBudgetMs - static_cast<int>(semanticTimer.elapsed()));
                const QString cancelToken = QStringLiteral("search-%1-embed-%2")
                    .arg(id)
                    .arg(role);
                QJsonObject embedParams;
                embedParams[QStringLiteral("query")] = query;
                embedParams[QStringLiteral("role")] = role;
                embedParams[QStringLiteral("priority")] = QStringLiteral("live");
                embedParams[QStringLiteral("deadlineMs")] =
                    QDateTime::currentMSecsSinceEpoch() + remainingBudget;
                embedParams[QStringLiteral("requestId")] = QStringLiteral("search-%1-%2")
                    .arg(id)
                    .arg(role);
                auto payload = sendInferenceRequest(
                    QStringLiteral("embed_query"),
                    embedParams,
                    static_cast<int>(std::min<qint64>(remainingBudget + 25, 2000)),
                    role,
                    QStringLiteral("embed_query_failed"),
                    cancelToken);
                if (payload.has_value()
                    && payload->value(QStringLiteral("status")).toString() == QLatin1String("ok")) {
                    const QJsonArray embeddingArray = payload->value(QStringLiteral("result"))
                        .toObject()
                        .value(QStringLiteral("embedding"))
                        .toArray();
                    queryVec = parseEmbeddingVector(embeddingArray);
                }
            } else if (manager && manager->isAvailable()) {
                queryVec = manager->embedQuery(query);
            }
            if (queryVec.empty()) {
                return;
            }

            const auto knnHits = index->search(queryVec.data(), std::max(1, topK));
            const std::string generationId = generation.toStdString();
            for (const auto& hit : knnHits) {
                if (semanticTimer.elapsed() > semanticBudgetMs) {
                    break;
                }
                const float cosineSim = 1.0f - hit.distance;
                if (cosineSim < semanticThreshold) {
                    continue;
                }
                const float normalizedSemantic =
                    SearchMerger::normalizeSemanticScore(cosineSim, semanticThreshold);
                if (normalizedSemantic <= semanticOnlyFloor) {
                    continue;
                }
                auto itemIdOpt = m_vectorStore->getItemId(hit.label, generationId);
                if (!itemIdOpt.has_value()) {
                    continue;
                }
                auto semanticItemOpt = m_store->getItemById(itemIdOpt.value());
                if (!semanticItemOpt.has_value()) {
                    continue;
                }
                if (isExcludedByBsignore(semanticItemOpt->path)) {
                    continue;
                }
                if (hasSearchFilters && !itemPassesSearchOptions(semanticItemOpt.value())) {
                    continue;
                }

                ++candidateCounter;
                const double weightedNorm = static_cast<double>(normalizedSemantic) * generationWeight;
                combinedSemanticByItemId[itemIdOpt.value()] = std::min(
                    1.0,
                    combinedSemanticByItemId[itemIdOpt.value()] + weightedNorm);

                auto existingSimilarity = semanticSimilarityByItemId.find(itemIdOpt.value());
                if (existingSimilarity == semanticSimilarityByItemId.end()) {
                    semanticSimilarityByItemId[itemIdOpt.value()] = cosineSim;
                } else {
                    existingSimilarity->second =
                        std::max(existingSimilarity->second, cosineSim);
                }
            }
        };

        std::shared_lock<std::shared_mutex> lock(m_vectorIndexMutex);
        if (m_vectorIndex && m_vectorIndex->isAvailable()
            && (inferenceEmbedOffloadActive
                || (m_embeddingManager && m_embeddingManager->isAvailable()))) {
            const double strongWeight = dualEmbeddingFusionEnabled ? 0.60 : 1.0;
            accumulateSemantic(QStringLiteral("bi-encoder"),
                               m_embeddingManager.get(),
                               m_vectorIndex.get(),
                               m_activeVectorGeneration,
                               strongEmbeddingTopK,
                               strongWeight,
                               strongSemanticCandidates);
        }
        if (dualEmbeddingFusionEnabled
            && fastEmbeddingEnabled
            && m_fastVectorIndex
            && m_fastVectorIndex->isAvailable()
            && (inferenceEmbedOffloadActive
                || (m_fastEmbeddingManager && m_fastEmbeddingManager->isAvailable()))
            && semanticTimer.elapsed() <= semanticBudgetMs) {
            dualIndexUsed = true;
            accumulateSemantic(QStringLiteral("bi-encoder-fast"),
                               m_fastEmbeddingManager.get(),
                               m_fastVectorIndex.get(),
                               m_fastVectorGeneration,
                               fastEmbeddingTopK,
                               0.40,
                               fastSemanticCandidates);
        }

        semanticResults.reserve(combinedSemanticByItemId.size());
        for (const auto& [itemId, combinedNorm] : combinedSemanticByItemId) {
            if (combinedNorm <= static_cast<double>(semanticOnlyFloor)) {
                continue;
            }
            const double cosine = static_cast<double>(semanticThreshold)
                + ((1.0 - static_cast<double>(semanticThreshold)) * combinedNorm);
            SemanticResult sr;
            sr.itemId = itemId;
            sr.cosineSimilarity = static_cast<float>(std::clamp(cosine, 0.0, 1.0));
            semanticResults.push_back(sr);
            semanticNormalizedByItemId[itemId] = static_cast<float>(combinedNorm);
        }
    }

    if (!semanticResults.empty()) {
        MergeConfig mergeConfig;
        mergeConfig.similarityThreshold = semanticThreshold;
        mergeConfig.lexicalWeight = mergeLexicalWeight;
        mergeConfig.semanticWeight = mergeSemanticWeight;
        mergeConfig.maxResults = std::max(limit * 2, limit);
        mergeConfig.semanticPassageCap = naturalLanguageQuery
            ? semanticPassageCapNaturalLanguage
            : semanticPassageCapOther;
        mergeConfig.semanticSoftmaxTemperature = naturalLanguageQuery
            ? static_cast<float>(semanticSoftmaxTemperatureNaturalLanguage)
            : static_cast<float>(semanticSoftmaxTemperatureOther);
        results = SearchMerger::merge(results, semanticResults, mergeConfig);

        int semanticOnlyAdded = 0;
        std::vector<SearchResult> cappedResults;
        cappedResults.reserve(results.size());
        for (const auto& raw : results) {
            SearchResult sr = raw;
            const bool semanticOnly = lexicalItemIds.find(sr.itemId) == lexicalItemIds.end();
            const float semanticSimilarity = semanticSimilarityByItemId.count(sr.itemId) > 0
                ? semanticSimilarityByItemId.at(sr.itemId)
                : 0.0f;
            const float semanticNormalized = semanticNormalizedByItemId.count(sr.itemId) > 0
                ? semanticNormalizedByItemId.at(sr.itemId)
                : 0.0f;
            sr.semanticSimilarity = semanticSimilarity;
            sr.semanticNormalized = semanticNormalized;
            if (semanticOnly) {
                bool allowSemanticOnly = semanticSimilarity >= kSemanticOnlySafetySimilarity;
                QString admitReason = allowSemanticOnly
                    ? QStringLiteral("high_similarity")
                    : QStringLiteral("suppressed");
                if (!allowSemanticOnly) {
                    if (sr.path.isEmpty() || sr.name.isEmpty()) {
                        auto itemOpt = m_store->getItemById(sr.itemId);
                        if (itemOpt.has_value()) {
                            sr.path = itemOpt->path;
                            sr.name = itemOpt->name;
                            sr.kind = itemOpt->kind;
                            sr.fileSize = itemOpt->size;
                            sr.isPinned = itemOpt->isPinned;
                        }
                    }

                    if (!querySignalTokens.isEmpty()) {
                        const QStringList overlapTokens = tokenizeWords(
                            (sr.name + QLatin1Char(' ') + QFileInfo(sr.path).absolutePath()).toLower());
                        for (const QString& token : overlapTokens) {
                            if (querySignalTokens.contains(token)) {
                                allowSemanticOnly = true;
                                admitReason = QStringLiteral("lexical_overlap");
                                break;
                            }
                        }
                    }

                    if (!allowSemanticOnly && strictLexicalWeakOrEmpty
                        && naturalLanguageQuery
                        && semanticSimilarity >= relaxedSemanticOnlySimilarity) {
                        allowSemanticOnly = true;
                        admitReason = QStringLiteral("weak_lexical_semantic");
                    }
                }

                if (!allowSemanticOnly) {
                    ++semanticOnlySuppressedCount;
                    continue;
                }
                if (semanticOnlyAdded >= semanticOnlyCap) {
                    ++semanticOnlySuppressedCount;
                    continue;
                }
                ++semanticOnlyAdded;
                ++semanticOnlyAdmittedCount;
                semanticOnlyAdmitReasons[admitReason] =
                    semanticOnlyAdmitReasons.value(admitReason) + 1;
            } else {
                if (semanticSimilarity > 0.0f) {
                    ++semanticOnlyAdmitReasons[QStringLiteral("blended_result")];
                }
            }
            cappedResults.push_back(sr);
        }
        results.swap(cappedResults);

        for (auto& sr : results) {
            if (sr.path.isEmpty()) {
                auto itemOpt = m_store->getItemById(sr.itemId);
                if (itemOpt.has_value()) {
                    const auto& item = itemOpt.value();
                    sr.path = item.path;
                    sr.name = item.name;
                    sr.kind = item.kind;
                    sr.fileSize = item.size;
                    sr.isPinned = item.isPinned;
                    if (item.modifiedAt > 0.0) {
                        sr.modificationDate = QDateTime::fromMSecsSinceEpoch(
                            static_cast<qint64>(item.modifiedAt * 1000.0))
                            .toUTC().toString(Qt::ISODate);
                    }
                }
            }
        }
    }

    int rerankDepthApplied = 0;
    int rerankerStage1Depth = 0;
    int rerankerStage2Depth = 0;
    bool rerankerStage1Applied = false;
    bool rerankerStage2Applied = false;
    bool rerankerAmbiguous = false;
    const bool inferenceRerankOffloadActive =
        embeddingEnabled && inferenceServiceEnabled && inferenceRerankOffloadEnabled;
    const bool coremlProviderUsed = (m_embeddingManager
            && m_embeddingManager->providerName().compare(QStringLiteral("coreml"), Qt::CaseInsensitive) == 0)
        || (m_fastEmbeddingManager
            && m_fastEmbeddingManager->providerName().compare(QStringLiteral("coreml"), Qt::CaseInsensitive) == 0);

    auto applyRerankScoresFromInference = [&](const QString& method,
                                              const QString& roleForMetrics,
                                              int maxCandidates,
                                              float weight,
                                              float minScoreThreshold,
                                              int budgetRemainingMs,
                                              const QString& cancelToken,
                                              bool& stageAppliedOut,
                                              int& stageDepthOut) {
        stageAppliedOut = false;
        stageDepthOut = std::min(maxCandidates, static_cast<int>(results.size()));
        if (stageDepthOut <= 0 || budgetRemainingMs <= 0) {
            return;
        }

        QJsonArray candidates;
        for (int i = 0; i < stageDepthOut; ++i) {
            const SearchResult& result = results[static_cast<size_t>(i)];
            QJsonObject candidate;
            candidate[QStringLiteral("itemId")] = static_cast<qint64>(result.itemId);
            candidate[QStringLiteral("path")] = result.path;
            candidate[QStringLiteral("name")] = result.name;
            candidate[QStringLiteral("snippet")] = result.snippet;
            candidate[QStringLiteral("score")] = result.score;
            candidates.append(candidate);
        }

        QJsonObject rerankParams;
        rerankParams[QStringLiteral("query")] = originalRawQuery;
        rerankParams[QStringLiteral("candidates")] = candidates;
        rerankParams[QStringLiteral("priority")] = QStringLiteral("live");
        rerankParams[QStringLiteral("deadlineMs")] =
            QDateTime::currentMSecsSinceEpoch() + budgetRemainingMs;
        rerankParams[QStringLiteral("requestId")] = QStringLiteral("search-%1-%2")
            .arg(id)
            .arg(method);

        auto payload = sendInferenceRequest(
            method,
            rerankParams,
            std::min(budgetRemainingMs + 25, 2000),
            roleForMetrics,
            QStringLiteral("rerank_offload_failed"),
            cancelToken);
        if (!payload.has_value()
            || payload->value(QStringLiteral("status")).toString() != QLatin1String("ok")) {
            return;
        }

        const QJsonArray scores = payload->value(QStringLiteral("result"))
            .toObject()
            .value(QStringLiteral("scores"))
            .toArray();
        QHash<qint64, float> scoreByItemId;
        for (const QJsonValue& scoreValue : scores) {
            const QJsonObject scoreObject = scoreValue.toObject();
            scoreByItemId.insert(
                scoreObject.value(QStringLiteral("itemId")).toInteger(),
                static_cast<float>(scoreObject.value(QStringLiteral("score")).toDouble(0.0)));
        }

        int boosted = 0;
        for (int i = 0; i < stageDepthOut; ++i) {
            SearchResult& result = results[static_cast<size_t>(i)];
            if (!scoreByItemId.contains(result.itemId)) {
                continue;
            }

            const float score = scoreByItemId.value(result.itemId);
            result.crossEncoderScore = score;
            if (score >= minScoreThreshold) {
                const double boost = static_cast<double>(weight) * static_cast<double>(score);
                result.score += boost;
                result.scoreBreakdown.crossEncoderBoost += boost;
                ++boosted;
            }
        }
        stageAppliedOut = boosted > 0;
    };

    const auto isRerankerTopKAmbiguous = [&](const std::vector<SearchResult>& ranked) {
        if (ranked.size() < 2) {
            return false;
        }
        const double margin = ranked[0].score - ranked[1].score;
        if (margin < rerankerAmbiguityMarginThreshold) {
            return true;
        }
        const int topK = std::min(static_cast<int>(ranked.size()), 10);
        int highSemantic = 0;
        int lowSemantic = 0;
        for (int i = 0; i < topK; ++i) {
            const double semantic = ranked[static_cast<size_t>(i)].semanticNormalized;
            if (semantic >= 0.55) {
                ++highSemantic;
            } else if (semantic <= 0.12) {
                ++lowSemantic;
            }
        }
        return highSemantic >= 3 && lowSemantic >= 3;
    };

    // Cross-encoder reranking (soft boost, before M2 boosts)
    const int elapsedBeforeRerankMs = static_cast<int>(timer.elapsed());
    if (inferenceRerankOffloadActive && rerankerCascadeEnabled) {
        const float stage1Weight = static_cast<float>(std::max(
            rerankerStage1MinWeight,
            static_cast<double>(m_scorer.weights().crossEncoderWeight) * rerankerStage1WeightScale));
        const float stage2Weight = static_cast<float>(
            static_cast<double>(m_scorer.weights().crossEncoderWeight) * rerankerStage2WeightScale);
        QElapsedTimer rerankTimer;
        rerankTimer.start();

        if (elapsedBeforeRerankMs < rerankBudgetMs) {
            applyRerankScoresFromInference(
                QStringLiteral("rerank_fast"),
                QStringLiteral("cross-encoder-fast"),
                rerankerStage1Max,
                stage1Weight,
                0.05f,
                rerankBudgetMs - elapsedBeforeRerankMs,
                QStringLiteral("search-%1-rerank-fast").arg(id),
                rerankerStage1Applied,
                rerankerStage1Depth);
        }

        rerankerAmbiguous = isRerankerTopKAmbiguous(results);
        const int elapsedAfterStage1Ms =
            elapsedBeforeRerankMs + static_cast<int>(rerankTimer.elapsed());
        if (rerankerAmbiguous && elapsedAfterStage1Ms < rerankBudgetMs) {
            applyRerankScoresFromInference(
                QStringLiteral("rerank_strong"),
                QStringLiteral("cross-encoder"),
                rerankerStage2Max,
                stage2Weight,
                0.10f,
                rerankBudgetMs - elapsedAfterStage1Ms,
                QStringLiteral("search-%1-rerank-strong").arg(id),
                rerankerStage2Applied,
                rerankerStage2Depth);
        }
        rerankDepthApplied = std::max(rerankerStage1Depth, rerankerStage2Depth);
    } else if (inferenceRerankOffloadActive && embeddingEnabled) {
        int rerankCap = rerankerFallbackCapDefault;
        if (elapsedBeforeRerankMs >= rerankerFallbackElapsed180Ms) {
            rerankCap = rerankerFallbackCapElapsed180;
        } else if (elapsedBeforeRerankMs >= rerankerFallbackElapsed130Ms) {
            rerankCap = rerankerFallbackCapElapsed130;
        } else if (elapsedBeforeRerankMs >= rerankerFallbackElapsed80Ms) {
            rerankCap = rerankerFallbackCapElapsed80;
        }
        rerankDepthApplied = std::min(static_cast<int>(results.size()), rerankCap);
        if (elapsedBeforeRerankMs >= rerankBudgetMs) {
            rerankDepthApplied = std::min(rerankDepthApplied, rerankerFallbackBudgetCap);
        }
        applyRerankScoresFromInference(
            QStringLiteral("rerank_strong"),
            QStringLiteral("cross-encoder"),
            rerankDepthApplied,
            m_scorer.weights().crossEncoderWeight,
            0.10f,
            std::max(1, rerankBudgetMs - elapsedBeforeRerankMs),
            QStringLiteral("search-%1-rerank-fallback").arg(id),
            rerankerStage2Applied,
            rerankerStage2Depth);
    } else if (embeddingEnabled
        && rerankerCascadeEnabled
        && ((m_fastCrossEncoderReranker && m_fastCrossEncoderReranker->isAvailable())
            || (m_crossEncoderReranker && m_crossEncoderReranker->isAvailable()))) {
        RerankerCascadeConfig cascadeConfig;
        cascadeConfig.enabled = true;
        cascadeConfig.stage1MaxCandidates = rerankerStage1Max;
        cascadeConfig.stage2MaxCandidates = rerankerStage2Max;
        cascadeConfig.rerankBudgetMs = rerankBudgetMs;
        cascadeConfig.stage1Weight = static_cast<float>(std::max(
            rerankerStage1MinWeight,
            static_cast<double>(m_scorer.weights().crossEncoderWeight) * rerankerStage1WeightScale));
        cascadeConfig.stage2Weight = static_cast<float>(
            static_cast<double>(m_scorer.weights().crossEncoderWeight) * rerankerStage2WeightScale);
        cascadeConfig.ambiguityMarginThreshold =
            static_cast<float>(rerankerAmbiguityMarginThreshold);
        const RerankerCascadeStats cascadeStats = RerankerCascade::run(
            originalRawQuery,
            results,
            m_fastCrossEncoderReranker.get(),
            m_crossEncoderReranker.get(),
            cascadeConfig,
            elapsedBeforeRerankMs);
        rerankerStage1Applied = cascadeStats.stage1Applied;
        rerankerStage2Applied = cascadeStats.stage2Applied;
        rerankerStage1Depth = cascadeStats.stage1Depth;
        rerankerStage2Depth = cascadeStats.stage2Depth;
        rerankerAmbiguous = cascadeStats.ambiguous;
        rerankDepthApplied = std::max(rerankerStage1Depth, rerankerStage2Depth);
    } else if (embeddingEnabled
               && m_crossEncoderReranker && m_crossEncoderReranker->isAvailable()) {
        RerankerConfig rerankerConfig;
        rerankerConfig.weight = m_scorer.weights().crossEncoderWeight;
        int rerankCap = rerankerFallbackCapDefault;
        if (elapsedBeforeRerankMs >= rerankerFallbackElapsed180Ms) {
            rerankCap = rerankerFallbackCapElapsed180;
        } else if (elapsedBeforeRerankMs >= rerankerFallbackElapsed130Ms) {
            rerankCap = rerankerFallbackCapElapsed130;
        } else if (elapsedBeforeRerankMs >= rerankerFallbackElapsed80Ms) {
            rerankCap = rerankerFallbackCapElapsed80;
        }
        rerankerConfig.maxCandidates = std::min(static_cast<int>(results.size()), rerankCap);
        rerankDepthApplied = rerankerConfig.maxCandidates;
        if (elapsedBeforeRerankMs >= rerankBudgetMs) {
            rerankDepthApplied = std::min(rerankDepthApplied, rerankerFallbackBudgetCap);
            rerankerConfig.maxCandidates = rerankDepthApplied;
        }
        m_crossEncoderReranker->rerank(originalRawQuery, results, rerankerConfig);
    }

    // StructuredQuery signal boosts (soft — rules engine only, nluConfidence=0.0)
    {
        const auto& weights = m_scorer.weights();
        for (auto& candidate : results) {
            double sqBoost = 0.0;

            // Temporal: boost items whose modifiedAt falls within the temporal range
            if (structured.temporal.has_value() && !candidate.modificationDate.isEmpty()) {
                bool ok = false;
                double modAt = candidate.modificationDate.toDouble(&ok);
                if (!ok) {
                    const QDateTime dt = QDateTime::fromString(candidate.modificationDate, Qt::ISODate);
                    if (dt.isValid()) {
                        modAt = static_cast<double>(dt.toSecsSinceEpoch());
                        ok = true;
                    }
                }
                if (ok) {
                    if (modAt >= structured.temporal->startEpoch &&
                        modAt <= structured.temporal->endEpoch) {
                        sqBoost += static_cast<double>(weights.temporalBoostWeight);
                    } else {
                        const double rangeSize = structured.temporal->endEpoch - structured.temporal->startEpoch;
                        if (modAt >= structured.temporal->startEpoch - rangeSize &&
                            modAt <= structured.temporal->endEpoch + rangeSize) {
                            sqBoost += static_cast<double>(weights.temporalNearWeight);
                        }
                    }
                }
            }

            // DocType: boost items whose extension matches the intent
            if (structured.docTypeIntent.has_value()) {
                const auto exts = DoctypeClassifier::extensionsForIntent(*structured.docTypeIntent);
                const QString ext = QFileInfo(candidate.path).suffix().toLower();
                if (std::find(exts.begin(), exts.end(), ext) != exts.end()) {
                    sqBoost += static_cast<double>(weights.docTypeIntentWeight);
                }
            }

            // Entity: boost items whose name or path contains extracted entities
            double entityBoost = 0.0;
            for (const auto& entity : structured.entities) {
                if (candidate.name.contains(entity.text, Qt::CaseInsensitive) ||
                    candidate.path.contains(entity.text, Qt::CaseInsensitive)) {
                    entityBoost += static_cast<double>(weights.entityMatchWeight);
                }
            }
            sqBoost += std::min(entityBoost, static_cast<double>(weights.entityMatchCap));

            if (sqBoost > 0.0) {
                candidate.score += sqBoost;
                candidate.scoreBreakdown.structuredQueryBoost = sqBoost;
            }
        }
    }

    // M2: Apply interaction, path preference, and type affinity boosts
    QString normalizedQuery = InteractionTracker::normalizeQuery(query);

    auto isNoteLikeTextExtension = [](const QString& ext) {
        return ext == QLatin1String("md")
            || ext == QLatin1String("txt")
            || ext == QLatin1String("log");
    };
    int clipboardSignalBoostedResults = 0;
    bool ltrApplied = false;
    double ltrDeltaTop10 = 0.0;
    QString ltrModelVersion = QStringLiteral("unavailable");

    for (auto& sr : results) {
        double feedbackBoost = 0.0;
        double m2SignalBoost = 0.0;
        const QString ext = QFileInfo(sr.path).suffix().toLower();
        const uint8_t originBits = candidateOrigins.count(sr.itemId) > 0
            ? candidateOrigins.at(sr.itemId)
            : static_cast<uint8_t>(CandidateOriginStrict);
        const bool hasStrictOrigin = (originBits & CandidateOriginStrict) != 0;
        const bool hasRelaxedOrigin = (originBits & CandidateOriginRelaxed) != 0;
        const bool hasNameFallbackOrigin = (originBits & CandidateOriginNameFallback) != 0;
        const bool fallbackOnlyOrigin = hasNameFallbackOrigin && !hasStrictOrigin && !hasRelaxedOrigin;

        if (m_interactionTracker) {
            feedbackBoost += m_interactionTracker->getInteractionBoost(normalizedQuery, sr.itemId);
        }
        if (m_pathPreferences) {
            feedbackBoost += m_pathPreferences->getBoost(sr.path);
        }
        if (m_typeAffinity) {
            feedbackBoost += m_typeAffinity->getBoost(sr.path);
        }
        sr.scoreBreakdown.feedbackBoost = feedbackBoost;

        if (naturalLanguageQuery && sr.semanticNormalized > 0.0) {
            const bool semanticOnly = lexicalItemIds.find(sr.itemId) == lexicalItemIds.end();
            const double normalizedSemantic = std::clamp(sr.semanticNormalized, 0.0, 1.0);
            const double semanticBoost = semanticOnly
                ? std::min(18.0, 5.0 + (normalizedSemantic * 18.0))
                : std::min(naturalLanguageQuery ? 18.0 : 8.0, normalizedSemantic * (naturalLanguageQuery ? 18.0 : 8.0));
            m2SignalBoost += semanticBoost;
            sr.scoreBreakdown.semanticBoost += semanticBoost;
        }

        double clipboardSignalBoost = 0.0;
        const QFileInfo pathInfo(sr.path);
        const QString fileNameLower = pathInfo.fileName().toLower();
        const QString parentNameLower = pathInfo.dir().dirName().toLower();
        if (context.clipboardBasename.has_value()) {
            if (fileNameLower == *context.clipboardBasename) {
                clipboardSignalBoost += 16.0;
            } else if (!fileNameLower.isEmpty()
                       && fileNameLower.contains(*context.clipboardBasename)) {
                clipboardSignalBoost += 8.0;
            }
        }
        if (context.clipboardDirname.has_value()
            && !parentNameLower.isEmpty()
            && parentNameLower == *context.clipboardDirname) {
            clipboardSignalBoost += 7.0;
        }
        if (context.clipboardExtension.has_value()
            && ext == *context.clipboardExtension) {
            clipboardSignalBoost += 3.0;
        }
        if (clipboardSignalBoost > 0.0) {
            m2SignalBoost += std::min(24.0, clipboardSignalBoost);
            ++clipboardSignalBoostedResults;
        }

        if (!querySignalTokens.isEmpty()) {
            const QStringList nameTokens = tokenizeWords(sr.name.toLower());
            QSet<QString> matchedQueryTokens;
            for (const QString& token : nameTokens) {
                if (querySignalTokens.contains(token)) {
                    matchedQueryTokens.insert(token);
                }
            }

            // Parent-directory tokens provide additional deterministic signal
            // without requiring new ranking models.
            const QStringList parentTokens =
                tokenizeWords(QFileInfo(sr.path).absolutePath().toLower());
            for (const QString& token : parentTokens) {
                if (querySignalTokens.contains(token)) {
                    matchedQueryTokens.insert(token);
                }
            }

            const int overlapCount = matchedQueryTokens.size();
            const int queryTokenCount = querySignalTokens.size();
            const double overlapRatio = queryTokenCount > 0
                ? static_cast<double>(overlapCount) / static_cast<double>(queryTokenCount)
                : 0.0;

            if (overlapCount > 0) {
                m2SignalBoost += std::min(42.0, static_cast<double>(overlapCount) * 12.0);
                if (queryTokenCount >= 3 && overlapRatio >= 0.60) {
                    m2SignalBoost += 8.0;
                }
            } else if (sr.matchType == MatchType::Content && querySignalTokens.size() >= 3) {
                m2SignalBoost -= 22.0;
                if (querySignalTokens.size() >= 4 && isNoteLikeTextExtension(ext)) {
                    m2SignalBoost -= 8.0;
                }
            }

            if (looksLikeNaturalLanguageQuery(querySignalTokens) && overlapCount == 0
                && !hasStrictOrigin) {
                m2SignalBoost -= fallbackOnlyOrigin ? 24.0 : 14.0;
                if (fallbackOnlyOrigin && sr.matchType == MatchType::Fuzzy) {
                    m2SignalBoost -= 6.0;
                }
            }
        }

        if (queryHints.downloadsHint && sr.path.startsWith(downloadsPath)) {
            m2SignalBoost += 18.0;
        }
        if (queryHints.documentsHint && sr.path.startsWith(documentsPath)) {
            m2SignalBoost += 18.0;
        }
        if (queryHints.desktopHint && sr.path.startsWith(desktopPath)) {
            m2SignalBoost += 18.0;
        }

        if (!queryHints.extensionHint.isEmpty()) {
            if (queryHints.extensionHint == QLatin1String("__image__")) {
                if (ext == QLatin1String("png") || ext == QLatin1String("jpg")
                    || ext == QLatin1String("jpeg") || ext == QLatin1String("webp")
                    || ext == QLatin1String("bmp") || ext == QLatin1String("tiff")) {
                    m2SignalBoost += 10.0;
                }
            } else if (ext == queryHints.extensionHint) {
                m2SignalBoost += 10.0;
            }
        }

        if (!sr.modificationDate.isEmpty()
            && (queryHints.monthHint > 0 || queryHints.yearHint > 0)) {
            const QDateTime modified = QDateTime::fromString(sr.modificationDate, Qt::ISODate);
            if (modified.isValid()) {
                if (queryHints.monthHint > 0 && modified.date().month() == queryHints.monthHint) {
                    m2SignalBoost += 6.0;
                }
                if (queryHints.yearHint > 0 && modified.date().year() == queryHints.yearHint) {
                    m2SignalBoost += 4.0;
                }
            }
        }

        sr.scoreBreakdown.m2SignalBoost = m2SignalBoost;
        sr.score = std::max(0.0, sr.score + feedbackBoost + m2SignalBoost);
    }

    if (personalizedLtrEnabled && m_personalizedLtr && m_personalizedLtr->isAvailable()) {
        LtrContext ltrContext;
        ltrContext.queryClass = queryClass;
        ltrContext.routerConfidence = routerConfidence;
        ltrContext.semanticNeedScore = std::clamp(structured.semanticNeedScore, 0.0f, 1.0f);
        ltrDeltaTop10 = m_personalizedLtr->apply(results, ltrContext, 100);
        ltrModelVersion = m_personalizedLtr->modelVersion();
        ltrApplied = true;
    }

    // Re-sort after M2 boosts
    std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.itemId < b.itemId;
    });

    // Truncate to the requested limit
    if (static_cast<int>(results.size()) > limit) {
        results.resize(static_cast<size_t>(limit));
    }

    // Serialize results to JSON array
    QJsonArray resultsArray;
    for (const auto& sr : results) {
        QJsonObject metadata;
        metadata[QStringLiteral("fileSize")] = static_cast<qint64>(sr.fileSize);
        metadata[QStringLiteral("modificationDate")] = sr.modificationDate;

        QJsonObject frequency;
        frequency[QStringLiteral("openCount")] = sr.openCount;
        frequency[QStringLiteral("lastOpenDate")] = sr.lastOpenDate;

        QJsonObject obj;
        obj[QStringLiteral("itemId")] = static_cast<qint64>(sr.itemId);
        obj[QStringLiteral("path")] = sr.path;
        obj[QStringLiteral("name")] = sr.name;
        obj[QStringLiteral("kind")] = sr.kind;
        obj[QStringLiteral("matchType")] = matchTypeToString(sr.matchType);
        obj[QStringLiteral("score")] = sr.score;
        obj[QStringLiteral("bm25Raw")] = sr.bm25RawScore;

        QString plainSnippet = sr.snippet;
        plainSnippet.replace(QStringLiteral("<b>"), QString());
        plainSnippet.replace(QStringLiteral("</b>"), QString());
        obj[QStringLiteral("snippet")] = plainSnippet;

        QJsonArray highlightsArray;
        for (const auto& highlight : sr.highlights) {
            QJsonObject highlightObj;
            highlightObj[QStringLiteral("offset")] = highlight.offset;
            highlightObj[QStringLiteral("length")] = highlight.length;
            highlightsArray.append(highlightObj);
        }
        obj[QStringLiteral("highlights")] = highlightsArray;
        obj[QStringLiteral("metadata")] = metadata;
        obj[QStringLiteral("isPinned")] = sr.isPinned;
        obj[QStringLiteral("frequency")] = frequency;
        const auto availability = m_store->getItemAvailability(sr.itemId);
        if (availability.has_value()) {
            obj[QStringLiteral("contentAvailable")] = availability->contentAvailable;
            obj[QStringLiteral("availabilityStatus")] = availability->availabilityStatus;
        } else {
            obj[QStringLiteral("contentAvailable")] = true;
            obj[QStringLiteral("availabilityStatus")] = QStringLiteral("available");
        }
        resultsArray.append(obj);
    }

    QJsonObject result;
    result[QStringLiteral("results")] = resultsArray;
    result[QStringLiteral("queryTime")] = static_cast<int>(timer.elapsed());
    result[QStringLiteral("totalMatches")] = totalMatches;

    if (rewriteDecision.applied) {
        m_rewriteAppliedCount.fetch_add(1);
    }
    if (semanticOnlyAdmittedCount > 0) {
        m_semanticOnlyAdmittedCount.fetch_add(
            static_cast<uint64_t>(semanticOnlyAdmittedCount));
    }
    if (semanticOnlySuppressedCount > 0) {
        m_semanticOnlySuppressedCount.fetch_add(
            static_cast<uint64_t>(semanticOnlySuppressedCount));
    }
    if (debugRequested) {
        QJsonObject debugInfo;
        switch (queryMode) {
        case SearchQueryMode::Strict:
            debugInfo[QStringLiteral("queryMode")] = QStringLiteral("strict");
            break;
        case SearchQueryMode::Relaxed:
            debugInfo[QStringLiteral("queryMode")] = QStringLiteral("relaxed");
            break;
        case SearchQueryMode::Auto:
        default:
            debugInfo[QStringLiteral("queryMode")] = QStringLiteral("auto");
            break;
        }
        debugInfo[QStringLiteral("lexicalStrictHits")] = strictHitsCount;
        debugInfo[QStringLiteral("lexicalRelaxedHits")] = relaxedHitsCount;
        debugInfo[QStringLiteral("semanticCandidates")] =
            static_cast<int>(semanticResults.size());
        debugInfo[QStringLiteral("strongSemanticCandidates")] = strongSemanticCandidates;
        debugInfo[QStringLiteral("fastSemanticCandidates")] = fastSemanticCandidates;
        debugInfo[QStringLiteral("dualIndexUsed")] = dualIndexUsed;
        QJsonObject candidateCountsBySource;
        candidateCountsBySource[QStringLiteral("lexical")] = totalMatches;
        candidateCountsBySource[QStringLiteral("passageAnn")] = static_cast<int>(semanticResults.size());
        candidateCountsBySource[QStringLiteral("passageAnnStrong")] = strongSemanticCandidates;
        candidateCountsBySource[QStringLiteral("passageAnnFast")] = fastSemanticCandidates;
        candidateCountsBySource[QStringLiteral("rerankInput")] = rerankDepthApplied;
        debugInfo[QStringLiteral("candidateCountsBySource")] = candidateCountsBySource;
        debugInfo[QStringLiteral("activeVectorGeneration")] = m_activeVectorGeneration;
        debugInfo[QStringLiteral("fastVectorGeneration")] = m_fastVectorGeneration;
        debugInfo[QStringLiteral("coremlProviderUsed")] = coremlProviderUsed;
        debugInfo[QStringLiteral("rerankDepthApplied")] = rerankDepthApplied;
        QJsonObject rerankerStages;
        rerankerStages[QStringLiteral("stage1Applied")] = rerankerStage1Applied;
        rerankerStages[QStringLiteral("stage2Applied")] = rerankerStage2Applied;
        rerankerStages[QStringLiteral("stage1Depth")] = rerankerStage1Depth;
        rerankerStages[QStringLiteral("stage2Depth")] = rerankerStage2Depth;
        rerankerStages[QStringLiteral("ambiguous")] = rerankerAmbiguous;
        debugInfo[QStringLiteral("rerankerStagesApplied")] = rerankerStages;
        debugInfo[QStringLiteral("semanticAggregationMode")] =
            (m_embeddingManager ? m_embeddingManager->semanticAggregationMode()
                                : QStringLiteral("max_softmax_cap"));
        debugInfo[QStringLiteral("fusionMode")] = QStringLiteral("weighted_rrf");
        debugInfo[QStringLiteral("queryClass")] = queryClassToString(queryClass);
        debugInfo[QStringLiteral("routerApplied")] = routerApplied;
        debugInfo[QStringLiteral("routerConfidence")] = static_cast<double>(routerConfidence);
        debugInfo[QStringLiteral("routerClass")] = queryClassToString(structured.queryClass);
        debugInfo[QStringLiteral("routerDomain")] = queryDomainToString(queryDomain);
        debugInfo[QStringLiteral("inferenceServiceEnabled")] = inferenceServiceEnabled;
        debugInfo[QStringLiteral("inferenceEmbedOffloadEnabled")] = inferenceEmbedOffloadEnabled;
        debugInfo[QStringLiteral("inferenceRerankOffloadEnabled")] = inferenceRerankOffloadEnabled;
        debugInfo[QStringLiteral("inferenceQaOffloadEnabled")] = inferenceQaOffloadEnabled;
        debugInfo[QStringLiteral("inferenceShadowModeEnabled")] = inferenceShadowModeEnabled;
        debugInfo[QStringLiteral("semanticNeedScore")] =
            static_cast<double>(structured.semanticNeedScore);
        debugInfo[QStringLiteral("semanticThresholdApplied")] = semanticThreshold;
        debugInfo[QStringLiteral("semanticOnlyFloorApplied")] = semanticOnlyFloor;
        debugInfo[QStringLiteral("semanticOnlyCapApplied")] = semanticOnlyCap;
        debugInfo[QStringLiteral("mergeLexicalWeightApplied")] = mergeLexicalWeight;
        debugInfo[QStringLiteral("mergeSemanticWeightApplied")] = mergeSemanticWeight;
        debugInfo[QStringLiteral("semanticBudgetMs")] = semanticBudgetMs;
        debugInfo[QStringLiteral("rerankBudgetMs")] = rerankBudgetMs;
        debugInfo[QStringLiteral("embeddingEnabled")] = embeddingEnabled;
        debugInfo[QStringLiteral("queryRouterEnabled")] = queryRouterEnabled;
        debugInfo[QStringLiteral("queryRouterMinConfidence")] = queryRouterMinConfidence;
        debugInfo[QStringLiteral("fastEmbeddingEnabled")] = fastEmbeddingEnabled;
        debugInfo[QStringLiteral("dualEmbeddingFusionEnabled")] = dualEmbeddingFusionEnabled;
        debugInfo[QStringLiteral("strongEmbeddingTopK")] = strongEmbeddingTopK;
        debugInfo[QStringLiteral("fastEmbeddingTopK")] = fastEmbeddingTopK;
        debugInfo[QStringLiteral("rerankerCascadeEnabled")] = rerankerCascadeEnabled;
        debugInfo[QStringLiteral("rerankerStage1Max")] = rerankerStage1Max;
        debugInfo[QStringLiteral("rerankerStage2Max")] = rerankerStage2Max;
        debugInfo[QStringLiteral("personalizedLtrEnabled")] = personalizedLtrEnabled;
        debugInfo[QStringLiteral("semanticThresholdNaturalLanguageBase")] =
            semanticThresholdNaturalLanguageBase;
        debugInfo[QStringLiteral("semanticThresholdShortAmbiguousBase")] =
            semanticThresholdShortAmbiguousBase;
        debugInfo[QStringLiteral("semanticThresholdPathOrCodeBase")] =
            semanticThresholdPathOrCodeBase;
        debugInfo[QStringLiteral("semanticThresholdNeedScale")] =
            semanticThresholdNeedScale;
        debugInfo[QStringLiteral("semanticThresholdMin")] = semanticThresholdMin;
        debugInfo[QStringLiteral("semanticThresholdMax")] = semanticThresholdMax;
        debugInfo[QStringLiteral("semanticOnlyFloorNaturalLanguage")] =
            semanticOnlyFloorNaturalLanguage;
        debugInfo[QStringLiteral("semanticOnlyFloorShortAmbiguous")] =
            semanticOnlyFloorShortAmbiguous;
        debugInfo[QStringLiteral("semanticOnlyFloorPathOrCode")] =
            semanticOnlyFloorPathOrCode;
        debugInfo[QStringLiteral("strictLexicalWeakCutoff")] = strictLexicalWeakCutoff;
        debugInfo[QStringLiteral("semanticOnlyCapNaturalLanguageWeak")] =
            semanticOnlyCapNaturalLanguageWeak;
        debugInfo[QStringLiteral("semanticOnlyCapNaturalLanguageStrong")] =
            semanticOnlyCapNaturalLanguageStrong;
        debugInfo[QStringLiteral("semanticOnlyCapShortAmbiguous")] =
            semanticOnlyCapShortAmbiguous;
        debugInfo[QStringLiteral("semanticOnlyCapPathOrCode")] =
            semanticOnlyCapPathOrCode;
        debugInfo[QStringLiteral("semanticOnlyCapPathOrCodeDivisor")] =
            semanticOnlyCapPathOrCodeDivisor;
        debugInfo[QStringLiteral("mergeLexicalWeightNaturalLanguageWeak")] =
            mergeLexicalWeightNaturalLanguageWeak;
        debugInfo[QStringLiteral("mergeSemanticWeightNaturalLanguageWeak")] =
            mergeSemanticWeightNaturalLanguageWeak;
        debugInfo[QStringLiteral("mergeLexicalWeightNaturalLanguageStrong")] =
            mergeLexicalWeightNaturalLanguageStrong;
        debugInfo[QStringLiteral("mergeSemanticWeightNaturalLanguageStrong")] =
            mergeSemanticWeightNaturalLanguageStrong;
        debugInfo[QStringLiteral("mergeLexicalWeightPathOrCode")] =
            mergeLexicalWeightPathOrCode;
        debugInfo[QStringLiteral("mergeSemanticWeightPathOrCode")] =
            mergeSemanticWeightPathOrCode;
        debugInfo[QStringLiteral("mergeLexicalWeightShortAmbiguous")] =
            mergeLexicalWeightShortAmbiguous;
        debugInfo[QStringLiteral("mergeSemanticWeightShortAmbiguous")] =
            mergeSemanticWeightShortAmbiguous;
        debugInfo[QStringLiteral("semanticOnlySafetySimilarityWeakNatural")] =
            semanticOnlySafetySimilarityWeakNatural;
        debugInfo[QStringLiteral("semanticOnlySafetySimilarityDefault")] =
            semanticOnlySafetySimilarityDefault;
        debugInfo[QStringLiteral("relaxedSemanticOnlyDeltaWeakNatural")] =
            relaxedSemanticOnlyDeltaWeakNatural;
        debugInfo[QStringLiteral("relaxedSemanticOnlyDeltaDefault")] =
            relaxedSemanticOnlyDeltaDefault;
        debugInfo[QStringLiteral("relaxedSemanticOnlyMinWeakNatural")] =
            relaxedSemanticOnlyMinWeakNatural;
        debugInfo[QStringLiteral("relaxedSemanticOnlyMinDefault")] =
            relaxedSemanticOnlyMinDefault;
        debugInfo[QStringLiteral("semanticPassageCapNaturalLanguage")] =
            semanticPassageCapNaturalLanguage;
        debugInfo[QStringLiteral("semanticPassageCapOther")] =
            semanticPassageCapOther;
        debugInfo[QStringLiteral("semanticSoftmaxTemperatureNaturalLanguage")] =
            semanticSoftmaxTemperatureNaturalLanguage;
        debugInfo[QStringLiteral("semanticSoftmaxTemperatureOther")] =
            semanticSoftmaxTemperatureOther;
        debugInfo[QStringLiteral("rerankerStage1WeightScale")] =
            rerankerStage1WeightScale;
        debugInfo[QStringLiteral("rerankerStage1MinWeight")] =
            rerankerStage1MinWeight;
        debugInfo[QStringLiteral("rerankerStage2WeightScale")] =
            rerankerStage2WeightScale;
        debugInfo[QStringLiteral("rerankerAmbiguityMarginThreshold")] =
            rerankerAmbiguityMarginThreshold;
        debugInfo[QStringLiteral("rerankerFallbackElapsed80Ms")] =
            rerankerFallbackElapsed80Ms;
        debugInfo[QStringLiteral("rerankerFallbackElapsed130Ms")] =
            rerankerFallbackElapsed130Ms;
        debugInfo[QStringLiteral("rerankerFallbackElapsed180Ms")] =
            rerankerFallbackElapsed180Ms;
        debugInfo[QStringLiteral("rerankerFallbackCapDefault")] =
            rerankerFallbackCapDefault;
        debugInfo[QStringLiteral("rerankerFallbackCapElapsed80")] =
            rerankerFallbackCapElapsed80;
        debugInfo[QStringLiteral("rerankerFallbackCapElapsed130")] =
            rerankerFallbackCapElapsed130;
        debugInfo[QStringLiteral("rerankerFallbackCapElapsed180")] =
            rerankerFallbackCapElapsed180;
        debugInfo[QStringLiteral("rerankerFallbackBudgetCap")] =
            rerankerFallbackBudgetCap;
        debugInfo[QStringLiteral("semanticOnlySuppressedCount")] = semanticOnlySuppressedCount;
        debugInfo[QStringLiteral("semanticOnlyAdmittedCount")] = semanticOnlyAdmittedCount;
        QJsonObject semanticReasonSummary;
        for (auto it = semanticOnlyAdmitReasons.constBegin();
             it != semanticOnlyAdmitReasons.constEnd(); ++it) {
            semanticReasonSummary[it.key()] = it.value();
        }
        debugInfo[QStringLiteral("semanticOnlyAdmitReasonSummary")] = semanticReasonSummary;

        // Per-result score breakdowns
        QJsonArray resultBreakdowns;
        for (const auto& sr : results) {
            QJsonObject rb;
            rb[QStringLiteral("itemId")] = static_cast<qint64>(sr.itemId);
            rb[QStringLiteral("name")] = sr.name;
            rb[QStringLiteral("finalScore")] = sr.score;
            QJsonObject bd;
            bd[QStringLiteral("baseMatchScore")] = sr.scoreBreakdown.baseMatchScore;
            bd[QStringLiteral("recencyBoost")] = sr.scoreBreakdown.recencyBoost;
            bd[QStringLiteral("frequencyBoost")] = sr.scoreBreakdown.frequencyBoost;
            bd[QStringLiteral("contextBoost")] = sr.scoreBreakdown.contextBoost;
            bd[QStringLiteral("pinnedBoost")] = sr.scoreBreakdown.pinnedBoost;
            bd[QStringLiteral("junkPenalty")] = sr.scoreBreakdown.junkPenalty;
            bd[QStringLiteral("semanticBoost")] = sr.scoreBreakdown.semanticBoost;
            bd[QStringLiteral("crossEncoderBoost")] = sr.scoreBreakdown.crossEncoderBoost;
            bd[QStringLiteral("structuredQueryBoost")] = sr.scoreBreakdown.structuredQueryBoost;
            bd[QStringLiteral("feedbackBoost")] = sr.scoreBreakdown.feedbackBoost;
            bd[QStringLiteral("m2SignalBoost")] = sr.scoreBreakdown.m2SignalBoost;
            rb[QStringLiteral("scoreBreakdown")] = bd;
            resultBreakdowns.append(rb);
        }
        debugInfo[QStringLiteral("resultBreakdowns")] = resultBreakdowns;
        debugInfo[QStringLiteral("adaptiveMergeWeightsApplied")] =
            (naturalLanguageQuery && strictLexicalWeakOrEmpty);
        debugInfo[QStringLiteral("effectiveSemanticOnlySafetySimilarity")] =
            static_cast<double>(kSemanticOnlySafetySimilarity);
        debugInfo[QStringLiteral("ltrApplied")] = ltrApplied;
        debugInfo[QStringLiteral("ltrModelVersion")] = ltrModelVersion;
        debugInfo[QStringLiteral("ltrDeltaTop10")] = ltrDeltaTop10;
        debugInfo[QStringLiteral("queryAfterParse")] = query;
        debugInfo[QStringLiteral("clipboardSignalsProvided")] =
            (context.clipboardBasename.has_value()
             || context.clipboardDirname.has_value()
             || context.clipboardExtension.has_value());
        debugInfo[QStringLiteral("clipboardSignalBoostedResults")] =
            clipboardSignalBoostedResults;
        QJsonArray parsedTypes;
        for (const QString& extractedType : parsed.extractedTypes) {
            parsedTypes.append(normalizeFileTypeToken(extractedType));
        }
        debugInfo[QStringLiteral("parsedTypes")] = parsedTypes;
        QJsonObject filtersDebug;
        filtersDebug[QStringLiteral("hasFilters")] = hasSearchFilters;
        QJsonArray fileTypesDebug;
        for (const QString& fileType : searchOptions.fileTypes) {
            fileTypesDebug.append(fileType);
        }
        filtersDebug[QStringLiteral("fileTypes")] = fileTypesDebug;
        QJsonArray includePathsDebug;
        for (const QString& includePath : searchOptions.includePaths) {
            includePathsDebug.append(includePath);
        }
        filtersDebug[QStringLiteral("includePaths")] = includePathsDebug;
        QJsonArray excludePathsDebug;
        for (const QString& excludePath : searchOptions.excludePaths) {
            excludePathsDebug.append(excludePath);
        }
        filtersDebug[QStringLiteral("excludePaths")] = excludePathsDebug;
        if (searchOptions.modifiedAfter.has_value()) {
            filtersDebug[QStringLiteral("modifiedAfter")] = searchOptions.modifiedAfter.value();
        }
        if (searchOptions.modifiedBefore.has_value()) {
            filtersDebug[QStringLiteral("modifiedBefore")] = searchOptions.modifiedBefore.value();
        }
        if (searchOptions.minSizeBytes.has_value()) {
            filtersDebug[QStringLiteral("minSize")] =
                static_cast<double>(searchOptions.minSizeBytes.value());
        }
        if (searchOptions.maxSizeBytes.has_value()) {
            filtersDebug[QStringLiteral("maxSize")] =
                static_cast<double>(searchOptions.maxSizeBytes.value());
        }
        debugInfo[QStringLiteral("filters")] = filtersDebug;
        debugInfo[QStringLiteral("correctedTokens")] = correctedTokensDebug;
        debugInfo[QStringLiteral("rewriteApplied")] = rewriteDecision.applied;
        debugInfo[QStringLiteral("rewriteConfidence")] = rewriteDecision.confidence;
        debugInfo[QStringLiteral("rewriteMinCandidateConfidence")] =
            rewriteDecision.minCandidateConfidence;
        debugInfo[QStringLiteral("rewriteCandidatesConsidered")] =
            rewriteDecision.candidatesConsidered;
        debugInfo[QStringLiteral("rewriteReason")] = rewriteDecision.reason;
        debugInfo[QStringLiteral("plannerApplied")] = plannerApplied;
        debugInfo[QStringLiteral("plannerReason")] = plannerReason;

        // Stage 0 structured query diagnostics
        QJsonObject sqDebug;
        sqDebug[QStringLiteral("cleanedQuery")] = structured.cleanedQuery;
        sqDebug[QStringLiteral("nluConfidence")] = static_cast<double>(structured.nluConfidence);
        sqDebug[QStringLiteral("queryClass")] = queryClassToString(structured.queryClass);
        sqDebug[QStringLiteral("queryClassConfidence")] =
            static_cast<double>(structured.queryClassConfidence);
        sqDebug[QStringLiteral("queryDomain")] = queryDomainToString(structured.queryDomain);
        sqDebug[QStringLiteral("queryDomainConfidence")] =
            static_cast<double>(structured.queryDomainConfidence);
        sqDebug[QStringLiteral("semanticNeedScore")] =
            static_cast<double>(structured.semanticNeedScore);
        if (structured.temporal) {
            QJsonObject temporal;
            temporal[QStringLiteral("startEpoch")] = structured.temporal->startEpoch;
            temporal[QStringLiteral("endEpoch")] = structured.temporal->endEpoch;
            sqDebug[QStringLiteral("temporal")] = temporal;
        }
        QJsonArray entitiesDebug;
        for (const auto& e : structured.entities) {
            QJsonObject ent;
            ent[QStringLiteral("text")] = e.text;
            ent[QStringLiteral("type")] = static_cast<int>(e.type);
            entitiesDebug.append(ent);
        }
        sqDebug[QStringLiteral("entities")] = entitiesDebug;
        if (structured.docTypeIntent) {
            sqDebug[QStringLiteral("docTypeIntent")] = *structured.docTypeIntent;
        }
        QJsonArray locationHintsDebug;
        for (const auto& loc : structured.locationHints) {
            locationHintsDebug.append(loc);
        }
        sqDebug[QStringLiteral("locationHints")] = locationHintsDebug;
        QJsonArray keyTokensDebug;
        for (const auto& tok : structured.keyTokens) {
            keyTokensDebug.append(tok);
        }
        sqDebug[QStringLiteral("keyTokens")] = keyTokensDebug;
        debugInfo[QStringLiteral("structuredQuery")] = sqDebug;
        debugInfo[QStringLiteral("crossEncoderAvailable")] =
            (m_crossEncoderReranker && m_crossEncoderReranker->isAvailable());
        if (!rewrittenRelaxedQuery.isEmpty() && rewrittenRelaxedQuery != query) {
            debugInfo[QStringLiteral("rewrittenQuery")] = rewrittenRelaxedQuery;
        }
        result[QStringLiteral("debugInfo")] = debugInfo;
    }

    // Store in cache (skip debug requests)
    if (!debugRequested) {
        m_queryCache.put(cacheKey, result);
    }

    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetAnswerSnippet(uint64_t id, const QJsonObject& params)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    const QString query = params.value(QStringLiteral("query")).toString().trimmed();
    if (query.isEmpty()) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'query' parameter"));
    }

    const int timeoutMs =
        std::clamp(params.value(QStringLiteral("timeoutMs")).toInt(350), 50, 1500);
    const int maxChars =
        std::clamp(params.value(QStringLiteral("maxChars")).toInt(240), 80, 600);
    const int maxChunks =
        std::clamp(params.value(QStringLiteral("maxChunks")).toInt(24), 1, 80);

    bool qaSnippetEnabled = true;
    bool inferenceServiceEnabled = true;
    bool inferenceQaOffloadEnabled = true;
    if (m_store.has_value()) {
        if (const auto raw = m_store->getSetting(QStringLiteral("qaSnippetEnabled"));
            raw.has_value()) {
            qaSnippetEnabled = envFlagEnabled(raw.value());
        }
        if (const auto raw = m_store->getSetting(QStringLiteral("inferenceServiceEnabled"));
            raw.has_value()) {
            inferenceServiceEnabled = envFlagEnabled(raw.value());
        }
        if (const auto raw = m_store->getSetting(QStringLiteral("inferenceQaOffloadEnabled"));
            raw.has_value()) {
            inferenceQaOffloadEnabled = envFlagEnabled(raw.value());
        }
    }
    const bool inferenceQaOffloadActive = inferenceServiceEnabled && inferenceQaOffloadEnabled;

    int64_t itemId = params.value(QStringLiteral("itemId")).toInteger(0);
    QString path = params.value(QStringLiteral("path")).toString().trimmed();

    std::optional<SQLiteStore::ItemRow> item;
    if (itemId > 0) {
        item = m_store->getItemById(itemId);
    } else if (!path.isEmpty()) {
        item = m_store->getItemByPath(path);
        if (item.has_value()) {
            itemId = item->id;
        }
    } else {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'itemId' or 'path' parameter"));
    }

    if (!item.has_value()) {
        QJsonObject result;
        result[QStringLiteral("available")] = false;
        result[QStringLiteral("reason")] = QStringLiteral("item_not_found");
        result[QStringLiteral("answer")] = QString();
        return IpcMessage::makeResponse(id, result);
    }
    if (path.isEmpty()) {
        path = item->path;
    }

    QSet<QString> signalTokens;
    const QStringList queryTokens = tokenizeWords(query);
    const QSet<QString>& stopwords = queryStopwords();
    for (const QString& token : queryTokens) {
        if (token.size() >= 2 && !stopwords.contains(token)) {
            signalTokens.insert(token);
        }
    }

    QElapsedTimer timer;
    timer.start();
    const bool qaModelDeclared = m_modelRegistry && m_modelRegistry->hasModel("qa-extractive");
    const bool qaModelActive = inferenceQaOffloadActive
        ? m_inferenceServiceConnected
        : (m_qaExtractiveModel && m_qaExtractiveModel->isAvailable());

    if (!qaSnippetEnabled) {
        QJsonObject result;
        result[QStringLiteral("available")] = false;
        result[QStringLiteral("itemId")] = static_cast<qint64>(itemId);
        result[QStringLiteral("path")] = path;
        result[QStringLiteral("reason")] = QStringLiteral("feature_disabled");
        result[QStringLiteral("answer")] = QString();
        result[QStringLiteral("timedOut")] = false;
        result[QStringLiteral("elapsedMs")] = timer.elapsed();
        result[QStringLiteral("qaModelDeclared")] = qaModelDeclared;
        result[QStringLiteral("qaModelActive")] = qaModelActive;
        return IpcMessage::makeResponse(id, result);
    }

    if (signalTokens.isEmpty()) {
        QJsonObject result;
        result[QStringLiteral("available")] = false;
        result[QStringLiteral("itemId")] = static_cast<qint64>(itemId);
        result[QStringLiteral("path")] = path;
        result[QStringLiteral("reason")] = QStringLiteral("query_too_short");
        result[QStringLiteral("answer")] = QString();
        result[QStringLiteral("timedOut")] = false;
        result[QStringLiteral("elapsedMs")] = timer.elapsed();
        result[QStringLiteral("qaModelDeclared")] = qaModelDeclared;
        return IpcMessage::makeResponse(id, result);
    }

    sqlite3* db = m_store->rawDb();
    if (!db) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database handle is unavailable"));
    }

    const char* sql =
        "SELECT chunk_text FROM content WHERE item_id = ?1 ORDER BY chunk_index LIMIT ?2";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return IpcMessage::makeError(
            id, IpcErrorCode::InternalError,
            QStringLiteral("Failed to prepare snippet query: %1")
                .arg(QString::fromUtf8(sqlite3_errmsg(db))));
    }
    sqlite3_bind_int64(stmt, 1, itemId);
    sqlite3_bind_int(stmt, 2, maxChunks);

    std::vector<QString> chunks;
    chunks.reserve(static_cast<size_t>(maxChunks));
    bool timedOut = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (timer.elapsed() > timeoutMs) {
            timedOut = true;
            break;
        }
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!text) {
            continue;
        }
        const QString chunk = QString::fromUtf8(text).trimmed();
        if (!chunk.isEmpty()) {
            chunks.push_back(chunk);
        }
    }
    sqlite3_finalize(stmt);

    if (timedOut) {
        QJsonObject result;
        result[QStringLiteral("available")] = false;
        result[QStringLiteral("itemId")] = static_cast<qint64>(itemId);
        result[QStringLiteral("path")] = path;
        result[QStringLiteral("reason")] = QStringLiteral("timeout");
        result[QStringLiteral("answer")] = QString();
        result[QStringLiteral("timedOut")] = true;
        result[QStringLiteral("elapsedMs")] = timer.elapsed();
        result[QStringLiteral("qaModelDeclared")] = qaModelDeclared;
        return IpcMessage::makeResponse(id, result);
    }

    if (chunks.empty()) {
        QJsonObject result;
        result[QStringLiteral("available")] = false;
        result[QStringLiteral("itemId")] = static_cast<qint64>(itemId);
        result[QStringLiteral("path")] = path;
        result[QStringLiteral("reason")] = QStringLiteral("no_content");
        result[QStringLiteral("answer")] = QString();
        result[QStringLiteral("timedOut")] = false;
        result[QStringLiteral("elapsedMs")] = timer.elapsed();
        result[QStringLiteral("qaModelDeclared")] = qaModelDeclared;
        return IpcMessage::makeResponse(id, result);
    }

    QString bestSentence;
    int bestOverlap = 0;
    int bestChunkIndex = 0;
    double bestScore = -1.0;
    QaExtractiveModel::Answer bestModelAnswer;
    int bestModelChunkIndex = -1;
    const QString queryLower = query.toLower();

    if (inferenceQaOffloadActive) {
        QJsonArray contexts;
        for (const QString& chunk : chunks) {
            contexts.append(chunk);
        }
        QJsonObject qaParams;
        qaParams[QStringLiteral("query")] = query;
        qaParams[QStringLiteral("contexts")] = contexts;
        qaParams[QStringLiteral("maxAnswerChars")] = maxChars;
        qaParams[QStringLiteral("priority")] = QStringLiteral("live");
        qaParams[QStringLiteral("deadlineMs")] =
            QDateTime::currentMSecsSinceEpoch() + timeoutMs;
        qaParams[QStringLiteral("requestId")] = QStringLiteral("qa-%1-%2")
            .arg(itemId)
            .arg(QDateTime::currentMSecsSinceEpoch());

        auto payload = sendInferenceRequest(
            QStringLiteral("qa_extract"),
            qaParams,
            timeoutMs + 50,
            QStringLiteral("qa-extractive"),
            QStringLiteral("qa_extract_failed"),
            QStringLiteral("qa-%1-cancel").arg(itemId));
        if (payload.has_value()) {
            const QString status = payload->value(QStringLiteral("status")).toString();
            if (status == QLatin1String("timeout")) {
                timedOut = true;
            } else if (status == QLatin1String("ok")) {
                const QJsonObject qaResult = payload->value(QStringLiteral("result")).toObject();
                bestModelAnswer.available = qaResult.value(QStringLiteral("available")).toBool(false);
                bestModelAnswer.answer = qaResult.value(QStringLiteral("answer")).toString();
                bestModelAnswer.confidence = qaResult.value(QStringLiteral("confidence")).toDouble();
                bestModelAnswer.rawScore = qaResult.value(QStringLiteral("rawScore")).toDouble();
                bestModelAnswer.startToken = qaResult.value(QStringLiteral("startToken")).toInt(-1);
                bestModelAnswer.endToken = qaResult.value(QStringLiteral("endToken")).toInt(-1);
                bestModelChunkIndex = qaResult.value(QStringLiteral("contextIndex")).toInt(-1);
            }
        }
    }

    for (size_t chunkIdx = 0; chunkIdx < chunks.size(); ++chunkIdx) {
        if (timer.elapsed() > timeoutMs) {
            timedOut = true;
            break;
        }

        const QString& chunk = chunks[chunkIdx];
        if (!inferenceQaOffloadActive && qaModelActive) {
            const auto modelAnswer = m_qaExtractiveModel->extract(query, chunk, maxChars);
            if (modelAnswer.available && (!bestModelAnswer.available
                                          || modelAnswer.confidence > bestModelAnswer.confidence)) {
                bestModelAnswer = modelAnswer;
                bestModelChunkIndex = static_cast<int>(chunkIdx);
            }
        }
        QStringList candidates = splitAnswerSentences(chunk);
        if (candidates.empty()) {
            candidates.push_back(chunk.simplified());
        }

        for (const QString& sentenceRaw : candidates) {
            const QString sentence = sentenceRaw.simplified();
            if (sentence.size() < 18) {
                continue;
            }

            const QString lower = sentence.toLower();
            int overlap = 0;
            for (const QString& token : signalTokens) {
                if (lower.contains(token)) {
                    ++overlap;
                }
            }
            if (overlap == 0) {
                continue;
            }

            const double overlapRatio =
                static_cast<double>(overlap) / static_cast<double>(signalTokens.size());
            const bool exactPhrase = queryLower.size() >= 4 && lower.contains(queryLower);
            double score = (overlapRatio * 1.45) + (exactPhrase ? 0.35 : 0.0);
            score += std::max(0.0, 0.14 - (static_cast<double>(chunkIdx) * 0.01));

            if (sentence.size() > 340) {
                score -= 0.12;
            } else if (sentence.size() < 26) {
                score -= 0.14;
            }

            if (score > bestScore) {
                bestScore = score;
                bestSentence = sentence;
                bestOverlap = overlap;
                bestChunkIndex = static_cast<int>(chunkIdx);
            }
        }
    }

    if (timedOut) {
        QJsonObject result;
        result[QStringLiteral("available")] = false;
        result[QStringLiteral("itemId")] = static_cast<qint64>(itemId);
        result[QStringLiteral("path")] = path;
        result[QStringLiteral("reason")] = QStringLiteral("timeout");
        result[QStringLiteral("answer")] = QString();
        result[QStringLiteral("timedOut")] = true;
        result[QStringLiteral("elapsedMs")] = timer.elapsed();
        result[QStringLiteral("qaModelDeclared")] = qaModelDeclared;
        return IpcMessage::makeResponse(id, result);
    }

    if (bestSentence.isEmpty() || bestScore < 0.20) {
        if (bestModelAnswer.available) {
            QJsonObject result;
            result[QStringLiteral("available")] = true;
            result[QStringLiteral("itemId")] = static_cast<qint64>(itemId);
            result[QStringLiteral("path")] = path;
            result[QStringLiteral("answer")] = bestModelAnswer.answer;
            result[QStringLiteral("confidence")] = bestModelAnswer.confidence;
            result[QStringLiteral("reason")] = QStringLiteral("ok");
            result[QStringLiteral("source")] = QStringLiteral("qa_extractive_model");
            result[QStringLiteral("timedOut")] = false;
            result[QStringLiteral("elapsedMs")] = timer.elapsed();
            result[QStringLiteral("qaModelDeclared")] = qaModelDeclared;
            result[QStringLiteral("qaModelActive")] = qaModelActive;
            result[QStringLiteral("matchedTokens")] = 0;
            result[QStringLiteral("chunkOrdinal")] = bestModelChunkIndex;
            return IpcMessage::makeResponse(id, result);
        }

        QJsonObject result;
        result[QStringLiteral("available")] = false;
        result[QStringLiteral("itemId")] = static_cast<qint64>(itemId);
        result[QStringLiteral("path")] = path;
        result[QStringLiteral("reason")] = QStringLiteral("no_answer");
        result[QStringLiteral("answer")] = QString();
        result[QStringLiteral("timedOut")] = false;
        result[QStringLiteral("elapsedMs")] = timer.elapsed();
        result[QStringLiteral("qaModelDeclared")] = qaModelDeclared;
        return IpcMessage::makeResponse(id, result);
    }

    const QString clipped = clipAnswerText(bestSentence, maxChars, queryTokens);
    double confidence = std::clamp(bestScore / 1.8, 0.0, 1.0);
    QString source = QStringLiteral("extractive_preview");
    int chunkOrdinal = bestChunkIndex;
    if (bestModelAnswer.available && bestModelAnswer.confidence >= confidence) {
        confidence = bestModelAnswer.confidence;
        source = QStringLiteral("qa_extractive_model");
        chunkOrdinal = bestModelChunkIndex;
    }

    QJsonObject result;
    result[QStringLiteral("available")] = true;
    result[QStringLiteral("itemId")] = static_cast<qint64>(itemId);
    result[QStringLiteral("path")] = path;
    result[QStringLiteral("answer")] =
        (source == QLatin1String("qa_extractive_model")) ? bestModelAnswer.answer : clipped;
    result[QStringLiteral("confidence")] = confidence;
    result[QStringLiteral("reason")] = QStringLiteral("ok");
    result[QStringLiteral("source")] = source;
    result[QStringLiteral("timedOut")] = false;
    result[QStringLiteral("elapsedMs")] = timer.elapsed();
    result[QStringLiteral("qaModelDeclared")] = qaModelDeclared;
    result[QStringLiteral("qaModelActive")] = qaModelActive;
    result[QStringLiteral("matchedTokens")] = bestOverlap;
    result[QStringLiteral("chunkOrdinal")] = chunkOrdinal;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetHealth(uint64_t id)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    refreshVectorGenerationState();

    IndexHealth health = m_store->getHealth();
    const int totalEmbeddedVectors = m_vectorStore
        ? m_vectorStore->countMappingsForGeneration(m_activeVectorGeneration.toStdString())
        : 0;
    const qint64 vectorIndexSize =
        QFileInfo(vectorIndexPathForGeneration(m_activeVectorGeneration)).size();
    const double contentCoveragePct = health.totalIndexedItems > 0
                                          ? 100.0 * static_cast<double>(
                                                        health.totalIndexedItems - health.itemsWithoutContent)
                                                / static_cast<double>(health.totalIndexedItems)
                                          : 100.0;
    const double semanticCoveragePct = health.totalIndexedItems > 0
                                           ? 100.0 * static_cast<double>(totalEmbeddedVectors)
                                                 / static_cast<double>(health.totalIndexedItems)
                                           : 100.0;

    QString lastScanTimeIso;
    if (health.lastIndexTime > 0.0) {
        lastScanTimeIso = QDateTime::fromSecsSinceEpoch(
                              static_cast<qint64>(health.lastIndexTime))
                              .toUTC()
                              .toString(Qt::ISODate);
    }

    QJsonArray recentErrors;
    if (sqlite3* db = m_store->rawDb()) {
        const char* sql = R"(
            SELECT i.path, f.error_message
            FROM failures f
            JOIN items i ON i.id = f.item_id
            WHERE NOT (
                f.stage = 'extraction'
                AND (
                    f.error_message LIKE 'PDF extraction unavailable (%'
                    OR f.error_message LIKE 'OCR extraction unavailable (%'
                    OR f.error_message LIKE 'Leptonica failed to read image%'
                    OR f.error_message LIKE 'Extension % is not supported by extractor'
                    OR f.error_message LIKE 'File size % exceeds configured limit %'
                    OR f.error_message = 'File does not exist or is not a regular file'
                    OR f.error_message = 'File is not readable'
                    OR f.error_message = 'Failed to load PDF document'
                    OR f.error_message = 'PDF is encrypted or password-protected'
                    OR f.error_message = 'File appears to be a cloud placeholder (size reported but no content readable)'
                )
            )
            ORDER BY f.last_failed_at DESC
            LIMIT 25
        )";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* error = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                QJsonObject entry;
                entry[QStringLiteral("path")] = path ? QString::fromUtf8(path) : QString();
                entry[QStringLiteral("error")] = error ? QString::fromUtf8(error) : QString();
                recentErrors.append(entry);
            }
        }
        sqlite3_finalize(stmt);
    }

    QJsonObject memoryByService;
    qint64 aggregateRssKb = 0;
    for (const QString& serviceName : {QStringLiteral("query"),
                                       QStringLiteral("indexer"),
                                       QStringLiteral("extractor"),
                                       QStringLiteral("inference")}) {
        const QJsonObject serviceStats = processStatsForService(serviceName);
        memoryByService[serviceName] = serviceStats;
        if (serviceStats.value(QStringLiteral("available")).toBool(false)) {
            aggregateRssKb += serviceStats.value(QStringLiteral("rssKb")).toInteger();
        }
    }
    const double aggregateRssMb = static_cast<double>(aggregateRssKb) / 1024.0;

    VectorRebuildState rebuildStateCopy;
    QString migrationStateCopy;
    double migrationProgressCopy = 0.0;
    {
        std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
        rebuildStateCopy = m_vectorRebuildState;
        migrationStateCopy = m_vectorMigrationState;
        migrationProgressCopy = m_vectorMigrationProgressPct;
    }

    const double progressPct = rebuildStateCopy.totalCandidates > 0
                                   ? (100.0 * static_cast<double>(rebuildStateCopy.processed)
                                      / static_cast<double>(rebuildStateCopy.totalCandidates))
                                   : 0.0;

    int queuePending = 0;
    int queueInProgress = 0;
    int queueDropped = 0;
    int queuePreparing = 0;
    int queueWriting = 0;
    int queueCoalesced = 0;
    int queueStaleDropped = 0;
    int queuePrepWorkers = 0;
    int queueWriterBatchDepth = 0;
    QString queueSource = QStringLiteral("unavailable");
    QJsonArray queueRoots;

    {
        // Use a short-lived client per request to avoid reentrant contention with other
        // synchronous IPC paths and to keep health RPC bounded under load.
        SocketClient indexerClient;
        const QString indexerSocketPath = ServiceBase::socketPath(QStringLiteral("indexer"));
        if (indexerClient.connectToServer(indexerSocketPath, 75)) {
            auto queueResponse = indexerClient.sendRequest(
                QStringLiteral("getQueueStatus"), {}, 150);
            if (queueResponse.has_value()
                && queueResponse->value(QStringLiteral("type")).toString() != QLatin1String("error")) {
                const QJsonObject queueResult = queueResponse->value(QStringLiteral("result")).toObject();
                queuePending = queueResult.value(QStringLiteral("pending")).toInt();
                queueInProgress = queueResult.value(QStringLiteral("processing")).toInt();
                queueDropped = queueResult.value(QStringLiteral("dropped")).toInt();
                queuePreparing = queueResult.value(QStringLiteral("preparing")).toInt();
                queueWriting = queueResult.value(QStringLiteral("writing")).toInt();
                queueCoalesced = queueResult.value(QStringLiteral("coalesced")).toInt();
                queueStaleDropped = queueResult.value(QStringLiteral("staleDropped")).toInt();
                queuePrepWorkers = queueResult.value(QStringLiteral("prepWorkers")).toInt();
                queueWriterBatchDepth = queueResult.value(QStringLiteral("writerBatchDepth")).toInt();
                queueRoots = queueResult.value(QStringLiteral("roots")).toArray();
                queueSource = QStringLiteral("indexer_rpc");
            }
        }
    }

    const QJsonObject inferenceHealth = inferenceHealthSnapshot();
    QString overallStatus = QStringLiteral("healthy");
    QString healthStatusReason = QStringLiteral("healthy");
    if (rebuildStateCopy.status == VectorRebuildState::Status::Running
        || health.totalIndexedItems == 0) {
        overallStatus = QStringLiteral("rebuilding");
        healthStatusReason = QStringLiteral("rebuilding");
    } else if (queueSource != QLatin1String("indexer_rpc")) {
        overallStatus = QStringLiteral("degraded");
        healthStatusReason = QStringLiteral("indexer_unavailable");
    } else if (health.criticalFailures > 0) {
        overallStatus = QStringLiteral("degraded");
        healthStatusReason = QStringLiteral("degraded_critical_failures");
    }

    QJsonObject indexHealth;
    indexHealth[QStringLiteral("overallStatus")] = overallStatus;
    indexHealth[QStringLiteral("healthStatusReason")] = healthStatusReason;
    indexHealth[QStringLiteral("isHealthy")] = health.isHealthy;
    indexHealth[QStringLiteral("totalIndexedItems")] = static_cast<qint64>(health.totalIndexedItems);
    indexHealth[QStringLiteral("totalChunks")] = static_cast<qint64>(health.totalChunks);
    indexHealth[QStringLiteral("totalEmbeddedVectors")] = totalEmbeddedVectors;
    indexHealth[QStringLiteral("totalFailures")] = static_cast<qint64>(health.totalFailures);
    indexHealth[QStringLiteral("criticalFailures")] = static_cast<qint64>(health.criticalFailures);
    indexHealth[QStringLiteral("expectedGapFailures")] = static_cast<qint64>(health.expectedGapFailures);
    indexHealth[QStringLiteral("lastIndexTime")] = health.lastIndexTime;
    indexHealth[QStringLiteral("lastScanTime")] = lastScanTimeIso;
    indexHealth[QStringLiteral("indexAge")] = health.indexAge;
    indexHealth[QStringLiteral("ftsIndexSize")] = static_cast<qint64>(health.ftsIndexSize);
    indexHealth[QStringLiteral("vectorIndexSize")] = vectorIndexSize;
    indexHealth[QStringLiteral("itemsWithoutContent")] = static_cast<qint64>(health.itemsWithoutContent);
    indexHealth[QStringLiteral("queuePending")] = queuePending;
    indexHealth[QStringLiteral("queueInProgress")] = queueInProgress;
    indexHealth[QStringLiteral("queueEmbedding")] = 0;
    indexHealth[QStringLiteral("queueDropped")] = queueDropped;
    indexHealth[QStringLiteral("queuePreparing")] = queuePreparing;
    indexHealth[QStringLiteral("queueWriting")] = queueWriting;
    indexHealth[QStringLiteral("queueCoalesced")] = queueCoalesced;
    indexHealth[QStringLiteral("queueStaleDropped")] = queueStaleDropped;
    indexHealth[QStringLiteral("queuePrepWorkers")] = queuePrepWorkers;
    indexHealth[QStringLiteral("queueWriterBatchDepth")] = queueWriterBatchDepth;
    indexHealth[QStringLiteral("queueSource")] = queueSource;
    indexHealth[QStringLiteral("inferenceServiceConnected")] =
        inferenceHealth.value(QStringLiteral("inferenceServiceConnected")).toBool(false);
    indexHealth[QStringLiteral("inferenceRoleStatusByModel")] =
        inferenceHealth.value(QStringLiteral("inferenceRoleStatusByModel")).toObject();
    indexHealth[QStringLiteral("inferenceQueueDepthByRole")] =
        inferenceHealth.value(QStringLiteral("inferenceQueueDepthByRole")).toObject();
    indexHealth[QStringLiteral("inferenceTimeoutCountByRole")] =
        inferenceHealth.value(QStringLiteral("inferenceTimeoutCountByRole")).toObject();
    indexHealth[QStringLiteral("inferenceFallbackCountByRole")] =
        inferenceHealth.value(QStringLiteral("inferenceFallbackCountByRole")).toObject();
    indexHealth[QStringLiteral("inferenceServiceTimeoutCountByRole")] =
        inferenceHealth.value(QStringLiteral("inferenceServiceTimeoutCountByRole")).toObject();
    indexHealth[QStringLiteral("inferenceServiceFailureCountByRole")] =
        inferenceHealth.value(QStringLiteral("inferenceServiceFailureCountByRole")).toObject();
    indexHealth[QStringLiteral("inferenceServiceRestartCountByRole")] =
        inferenceHealth.value(QStringLiteral("inferenceServiceRestartCountByRole")).toObject();
    indexHealth[QStringLiteral("contentCoveragePct")] = contentCoveragePct;
    indexHealth[QStringLiteral("semanticCoveragePct")] = semanticCoveragePct;
    indexHealth[QStringLiteral("multiChunkEmbeddingEnabled")] = true;
    indexHealth[QStringLiteral("queryRewriteEnabled")] = true;
    indexHealth[QStringLiteral("m2ModulesInitialized")] = m_m2Initialized;
    indexHealth[QStringLiteral("memoryAggregateRssMb")] = aggregateRssMb;
    indexHealth[QStringLiteral("memoryByService")] = memoryByService;
    indexHealth[QStringLiteral("vectorMigrationState")] = migrationStateCopy;
    indexHealth[QStringLiteral("vectorMigrationProgressPct")] = migrationProgressCopy;
    indexHealth[QStringLiteral("vectorGenerationActive")] = m_activeVectorGeneration;
    indexHealth[QStringLiteral("activeVectorModelId")] = m_activeVectorModelId;
    indexHealth[QStringLiteral("activeVectorProvider")] = m_activeVectorProvider;
    indexHealth[QStringLiteral("activeVectorDimensions")] = m_activeVectorDimensions;
    indexHealth[QStringLiteral("recentErrors")] = recentErrors;
    indexHealth[QStringLiteral("indexRoots")] = queueRoots;
    indexHealth[QStringLiteral("vectorRebuildStatus")] =
        vectorRebuildStatusToString(rebuildStateCopy.status);
    indexHealth[QStringLiteral("vectorRebuildRunId")] =
        static_cast<qint64>(rebuildStateCopy.runId);
    indexHealth[QStringLiteral("vectorRebuildStartedAt")] = rebuildStateCopy.startedAt;
    indexHealth[QStringLiteral("vectorRebuildFinishedAt")] = rebuildStateCopy.finishedAt;
    indexHealth[QStringLiteral("vectorRebuildTotalCandidates")] = rebuildStateCopy.totalCandidates;
    indexHealth[QStringLiteral("vectorRebuildProcessed")] = rebuildStateCopy.processed;
    indexHealth[QStringLiteral("vectorRebuildEmbedded")] = rebuildStateCopy.embedded;
    indexHealth[QStringLiteral("vectorRebuildSkipped")] = rebuildStateCopy.skipped;
    indexHealth[QStringLiteral("vectorRebuildFailed")] = rebuildStateCopy.failed;
    indexHealth[QStringLiteral("vectorRebuildProgressPct")] = progressPct;
    indexHealth[QStringLiteral("vectorRebuildLastError")] = rebuildStateCopy.lastError;
    indexHealth[QStringLiteral("vectorRebuildScopeRoots")] =
        QJsonArray::fromStringList(rebuildStateCopy.scopeRoots);
    indexHealth[QStringLiteral("vectorRebuildScopeCandidates")] =
        rebuildStateCopy.scopeCandidates;
    QJsonObject memorySummary;
    memorySummary[QStringLiteral("aggregateRssMb")] = aggregateRssMb;
    memorySummary[QStringLiteral("byService")] = memoryByService;
    indexHealth[QStringLiteral("memory")] = memorySummary;
    QJsonObject vectorMigration;
    vectorMigration[QStringLiteral("state")] = migrationStateCopy;
    vectorMigration[QStringLiteral("progressPct")] = migrationProgressCopy;
    vectorMigration[QStringLiteral("activeGeneration")] = m_activeVectorGeneration;
    vectorMigration[QStringLiteral("targetGeneration")] = m_targetVectorGeneration;
    indexHealth[QStringLiteral("vectorMigration")] = vectorMigration;
    QJsonObject vectorGeneration;
    vectorGeneration[QStringLiteral("active")] = m_activeVectorGeneration;
    vectorGeneration[QStringLiteral("modelId")] = m_activeVectorModelId;
    vectorGeneration[QStringLiteral("provider")] = m_activeVectorProvider;
    vectorGeneration[QStringLiteral("dimensions")] = m_activeVectorDimensions;
    indexHealth[QStringLiteral("vectorGeneration")] = vectorGeneration;
    const QJsonObject bsignoreStatus = bsignoreStatusJson();
    indexHealth[QStringLiteral("bsignorePath")] = bsignoreStatus.value(QStringLiteral("path")).toString();
    indexHealth[QStringLiteral("bsignoreFileExists")] =
        bsignoreStatus.value(QStringLiteral("fileExists")).toBool(false);
    indexHealth[QStringLiteral("bsignoreLoaded")] = bsignoreStatus.value(QStringLiteral("loaded")).toBool(false);
    indexHealth[QStringLiteral("bsignorePatternCount")] =
        bsignoreStatus.value(QStringLiteral("patternCount")).toInt(0);
    indexHealth[QStringLiteral("bsignoreLastLoadedAtMs")] =
        bsignoreStatus.value(QStringLiteral("lastLoadedAtMs")).toInteger();
    indexHealth[QStringLiteral("bsignoreLastLoadedAt")] =
        bsignoreStatus.value(QStringLiteral("lastLoadedAt")).toString();
    const auto cacheStats = m_queryCache.stats();
    QJsonObject cacheStatsJson;
    cacheStatsJson[QStringLiteral("hits")] = static_cast<qint64>(cacheStats.hits);
    cacheStatsJson[QStringLiteral("misses")] = static_cast<qint64>(cacheStats.misses);
    cacheStatsJson[QStringLiteral("evictions")] = static_cast<qint64>(cacheStats.evictions);
    cacheStatsJson[QStringLiteral("currentSize")] = cacheStats.currentSize;
    indexHealth[QStringLiteral("queryCache")] = cacheStatsJson;

    const QJsonObject queryStats = queryStatsSnapshot();
    indexHealth[QStringLiteral("searchCount")] = queryStats.value(QStringLiteral("searchCount")).toInteger();
    indexHealth[QStringLiteral("rewriteAppliedCount")] =
        queryStats.value(QStringLiteral("rewriteAppliedCount")).toInteger();
    indexHealth[QStringLiteral("semanticOnlyAdmittedCount")] =
        queryStats.value(QStringLiteral("semanticOnlyAdmittedCount")).toInteger();
    indexHealth[QStringLiteral("semanticOnlySuppressedCount")] =
        queryStats.value(QStringLiteral("semanticOnlySuppressedCount")).toInteger();

    const auto readSettingValue = [&](const QString& key) -> std::optional<QString> {
        if (!m_store.has_value()) {
            return std::nullopt;
        }
        return m_store->getSetting(key);
    };
    const auto readBoolRuntimeSetting = [&](const QString& key, bool defaultValue) -> bool {
        const std::optional<QString> value = readSettingValue(key);
        if (!value.has_value()) {
            return defaultValue;
        }
        return envFlagEnabled(value.value());
    };
    const auto readIntRuntimeSetting = [&](const QString& key, int defaultValue) -> int {
        const std::optional<QString> value = readSettingValue(key);
        if (!value.has_value()) {
            return defaultValue;
        }
        bool ok = false;
        const int parsed = value.value().toInt(&ok);
        return ok ? parsed : defaultValue;
    };
    const auto readDoubleRuntimeSetting = [&](const QString& key, double defaultValue) -> double {
        const std::optional<QString> value = readSettingValue(key);
        if (!value.has_value()) {
            return defaultValue;
        }
        bool ok = false;
        const double parsed = value.value().toDouble(&ok);
        return ok ? parsed : defaultValue;
    };

    QJsonObject runtimeSettings;
    runtimeSettings[QStringLiteral("embeddingEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("embeddingEnabled"), true);
    runtimeSettings[QStringLiteral("inferenceServiceEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("inferenceServiceEnabled"), true);
    runtimeSettings[QStringLiteral("inferenceEmbedOffloadEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("inferenceEmbedOffloadEnabled"), true);
    runtimeSettings[QStringLiteral("inferenceRerankOffloadEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("inferenceRerankOffloadEnabled"), true);
    runtimeSettings[QStringLiteral("inferenceQaOffloadEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("inferenceQaOffloadEnabled"), true);
    runtimeSettings[QStringLiteral("inferenceShadowModeEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("inferenceShadowModeEnabled"), false);
    runtimeSettings[QStringLiteral("queryRouterEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("queryRouterEnabled"), true);
    runtimeSettings[QStringLiteral("queryRouterMinConfidence")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("queryRouterMinConfidence"), 0.45), 0.0, 1.0);
    runtimeSettings[QStringLiteral("fastEmbeddingEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("fastEmbeddingEnabled"), true);
    runtimeSettings[QStringLiteral("dualEmbeddingFusionEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("dualEmbeddingFusionEnabled"), true);
    runtimeSettings[QStringLiteral("strongEmbeddingTopK")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("strongEmbeddingTopK"), 40));
    runtimeSettings[QStringLiteral("fastEmbeddingTopK")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("fastEmbeddingTopK"), 60));
    runtimeSettings[QStringLiteral("rerankerCascadeEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("rerankerCascadeEnabled"), true);
    runtimeSettings[QStringLiteral("rerankerStage1Max")] = std::max(
        4, readIntRuntimeSetting(QStringLiteral("rerankerStage1Max"), 40));
    runtimeSettings[QStringLiteral("rerankerStage2Max")] = std::max(
        4, readIntRuntimeSetting(QStringLiteral("rerankerStage2Max"), 12));
    runtimeSettings[QStringLiteral("qaSnippetEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("qaSnippetEnabled"), true);
    runtimeSettings[QStringLiteral("personalizedLtrEnabled")] =
        readBoolRuntimeSetting(QStringLiteral("personalizedLtrEnabled"), true);
    runtimeSettings[QStringLiteral("semanticBudgetMs")] = std::max(
        20, readIntRuntimeSetting(QStringLiteral("semanticBudgetMs"), 70));
    runtimeSettings[QStringLiteral("rerankBudgetMs")] = std::max(
        40, readIntRuntimeSetting(QStringLiteral("rerankBudgetMs"), 120));
    const int maxFileSizeBytes = std::max(
        1, readIntRuntimeSetting(QStringLiteral("max_file_size"), 50 * 1024 * 1024));
    runtimeSettings[QStringLiteral("maxFileSizeBytes")] = maxFileSizeBytes;
    runtimeSettings[QStringLiteral("maxFileSizeMB")] =
        static_cast<double>(maxFileSizeBytes) / (1024.0 * 1024.0);
    runtimeSettings[QStringLiteral("extractionTimeoutMs")] = std::max(
        1000, readIntRuntimeSetting(QStringLiteral("extraction_timeout_ms"), 30000));
    runtimeSettings[QStringLiteral("bm25WeightName")] = std::max(
        0.0, readDoubleRuntimeSetting(QStringLiteral("bm25WeightName"), 10.0));
    runtimeSettings[QStringLiteral("bm25WeightPath")] = std::max(
        0.0, readDoubleRuntimeSetting(QStringLiteral("bm25WeightPath"), 5.0));
    runtimeSettings[QStringLiteral("bm25WeightContent")] = std::max(
        0.0, readDoubleRuntimeSetting(QStringLiteral("bm25WeightContent"), 1.0));
    runtimeSettings[QStringLiteral("autoVectorMigration")] =
        readBoolRuntimeSetting(QStringLiteral("autoVectorMigration"), true);
    runtimeSettings[QStringLiteral("semanticThresholdNaturalLanguageBase")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("semanticThresholdNaturalLanguageBase"), 0.62),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("semanticThresholdShortAmbiguousBase")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("semanticThresholdShortAmbiguousBase"), 0.66),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("semanticThresholdPathOrCodeBase")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("semanticThresholdPathOrCodeBase"), 0.70),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("semanticThresholdNeedScale")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("semanticThresholdNeedScale"), 0.06),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("semanticThresholdMin")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("semanticThresholdMin"), 0.55),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("semanticThresholdMax")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("semanticThresholdMax"), 0.80),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("semanticOnlyFloorNaturalLanguage")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("semanticOnlyFloorNaturalLanguage"), 0.08),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("semanticOnlyFloorShortAmbiguous")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("semanticOnlyFloorShortAmbiguous"), 0.10),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("semanticOnlyFloorPathOrCode")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("semanticOnlyFloorPathOrCode"), 0.15),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("strictLexicalWeakCutoff")] = std::max(
        0.0, readDoubleRuntimeSetting(QStringLiteral("strictLexicalWeakCutoff"), 2.0));
    runtimeSettings[QStringLiteral("semanticOnlyCapNaturalLanguageWeak")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("semanticOnlyCapNaturalLanguageWeak"), 8));
    runtimeSettings[QStringLiteral("semanticOnlyCapNaturalLanguageStrong")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("semanticOnlyCapNaturalLanguageStrong"), 6));
    runtimeSettings[QStringLiteral("semanticOnlyCapShortAmbiguous")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("semanticOnlyCapShortAmbiguous"), 4));
    runtimeSettings[QStringLiteral("semanticOnlyCapPathOrCode")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("semanticOnlyCapPathOrCode"), 3));
    runtimeSettings[QStringLiteral("semanticOnlyCapPathOrCodeDivisor")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("semanticOnlyCapPathOrCodeDivisor"), 2));
    runtimeSettings[QStringLiteral("mergeLexicalWeightNaturalLanguageWeak")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("mergeLexicalWeightNaturalLanguageWeak"), 0.45),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("mergeSemanticWeightNaturalLanguageWeak")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("mergeSemanticWeightNaturalLanguageWeak"), 0.55),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("mergeLexicalWeightNaturalLanguageStrong")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("mergeLexicalWeightNaturalLanguageStrong"), 0.55),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("mergeSemanticWeightNaturalLanguageStrong")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("mergeSemanticWeightNaturalLanguageStrong"), 0.45),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("mergeLexicalWeightPathOrCode")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("mergeLexicalWeightPathOrCode"), 0.75),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("mergeSemanticWeightPathOrCode")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("mergeSemanticWeightPathOrCode"), 0.25),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("mergeLexicalWeightShortAmbiguous")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("mergeLexicalWeightShortAmbiguous"), 0.65),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("mergeSemanticWeightShortAmbiguous")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("mergeSemanticWeightShortAmbiguous"), 0.35),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("semanticOnlySafetySimilarityWeakNatural")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("semanticOnlySafetySimilarityWeakNatural"), 0.74),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("semanticOnlySafetySimilarityDefault")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("semanticOnlySafetySimilarityDefault"), 0.78),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("relaxedSemanticOnlyDeltaWeakNatural")] = std::max(
        0.0, readDoubleRuntimeSetting(QStringLiteral("relaxedSemanticOnlyDeltaWeakNatural"), 0.02));
    runtimeSettings[QStringLiteral("relaxedSemanticOnlyDeltaDefault")] = std::max(
        0.0, readDoubleRuntimeSetting(QStringLiteral("relaxedSemanticOnlyDeltaDefault"), 0.03));
    runtimeSettings[QStringLiteral("relaxedSemanticOnlyMinWeakNatural")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("relaxedSemanticOnlyMinWeakNatural"), 0.64),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("relaxedSemanticOnlyMinDefault")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("relaxedSemanticOnlyMinDefault"), 0.66),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("semanticPassageCapNaturalLanguage")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("semanticPassageCapNaturalLanguage"), 3));
    runtimeSettings[QStringLiteral("semanticPassageCapOther")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("semanticPassageCapOther"), 2));
    runtimeSettings[QStringLiteral("semanticSoftmaxTemperatureNaturalLanguage")] = std::max(
        0.1, readDoubleRuntimeSetting(QStringLiteral("semanticSoftmaxTemperatureNaturalLanguage"), 8.0));
    runtimeSettings[QStringLiteral("semanticSoftmaxTemperatureOther")] = std::max(
        0.1, readDoubleRuntimeSetting(QStringLiteral("semanticSoftmaxTemperatureOther"), 6.0));
    runtimeSettings[QStringLiteral("rerankerStage1WeightScale")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("rerankerStage1WeightScale"), 0.55),
        0.0,
        4.0);
    runtimeSettings[QStringLiteral("rerankerStage1MinWeight")] = std::max(
        0.0, readDoubleRuntimeSetting(QStringLiteral("rerankerStage1MinWeight"), 8.0));
    runtimeSettings[QStringLiteral("rerankerStage2WeightScale")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("rerankerStage2WeightScale"), 1.0),
        0.0,
        4.0);
    runtimeSettings[QStringLiteral("rerankerAmbiguityMarginThreshold")] = std::clamp(
        readDoubleRuntimeSetting(QStringLiteral("rerankerAmbiguityMarginThreshold"), 0.08),
        0.0,
        1.0);
    runtimeSettings[QStringLiteral("rerankerFallbackElapsed80Ms")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("rerankerFallbackElapsed80Ms"), 80));
    runtimeSettings[QStringLiteral("rerankerFallbackElapsed130Ms")] = std::max(
        runtimeSettings.value(QStringLiteral("rerankerFallbackElapsed80Ms")).toInt(),
        readIntRuntimeSetting(QStringLiteral("rerankerFallbackElapsed130Ms"), 130));
    runtimeSettings[QStringLiteral("rerankerFallbackElapsed180Ms")] = std::max(
        runtimeSettings.value(QStringLiteral("rerankerFallbackElapsed130Ms")).toInt(),
        readIntRuntimeSetting(QStringLiteral("rerankerFallbackElapsed180Ms"), 180));
    runtimeSettings[QStringLiteral("rerankerFallbackCapDefault")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("rerankerFallbackCapDefault"), 40));
    runtimeSettings[QStringLiteral("rerankerFallbackCapElapsed80")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("rerankerFallbackCapElapsed80"), 32));
    runtimeSettings[QStringLiteral("rerankerFallbackCapElapsed130")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("rerankerFallbackCapElapsed130"), 24));
    runtimeSettings[QStringLiteral("rerankerFallbackCapElapsed180")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("rerankerFallbackCapElapsed180"), 12));
    runtimeSettings[QStringLiteral("rerankerFallbackBudgetCap")] = std::max(
        1, readIntRuntimeSetting(QStringLiteral("rerankerFallbackBudgetCap"), 8));
    indexHealth[QStringLiteral("runtimeSettings")] = runtimeSettings;

    QJsonObject runtimeSettingsRaw;
    if (sqlite3* db = m_store->rawDb()) {
        static constexpr const char* kRuntimeSettingsSql = R"(
            SELECT key, value
            FROM settings
            ORDER BY key ASC
        )";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, kRuntimeSettingsSql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (key && key[0] != '\0') {
                    runtimeSettingsRaw[QString::fromUtf8(key)] =
                        value ? QString::fromUtf8(value) : QString();
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    indexHealth[QStringLiteral("runtimeSettingsRaw")] = runtimeSettingsRaw;

    QJsonObject runtimeComponents;
    runtimeComponents[QStringLiteral("queryRouterRuntimeMode")] = QStringLiteral("heuristic_rules");
    runtimeComponents[QStringLiteral("queryRouterModelDeclared")] =
        m_modelRegistry && m_modelRegistry->hasModel("query-router");
    runtimeComponents[QStringLiteral("queryRouterModelActive")] = false;
    runtimeComponents[QStringLiteral("queryRouterInactiveReason")] =
        QStringLiteral("Query router currently uses heuristic implementation.");
    runtimeComponents[QStringLiteral("inferenceServiceConnected")] =
        inferenceHealth.value(QStringLiteral("inferenceServiceConnected")).toBool(false);
    runtimeComponents[QStringLiteral("inferenceRoleStatusByModel")] =
        inferenceHealth.value(QStringLiteral("inferenceRoleStatusByModel")).toObject();
    runtimeComponents[QStringLiteral("inferenceQueueDepthByRole")] =
        inferenceHealth.value(QStringLiteral("inferenceQueueDepthByRole")).toObject();
    runtimeComponents[QStringLiteral("embeddingStrongAvailable")] =
        (m_embeddingManager && m_embeddingManager->isAvailable());
    runtimeComponents[QStringLiteral("embeddingStrongModelId")] =
        m_embeddingManager ? m_embeddingManager->activeModelId() : QString();
    runtimeComponents[QStringLiteral("embeddingStrongProvider")] =
        m_embeddingManager ? m_embeddingManager->providerName() : QString();
    runtimeComponents[QStringLiteral("embeddingStrongGeneration")] =
        m_embeddingManager ? m_embeddingManager->activeGenerationId() : QString();
    runtimeComponents[QStringLiteral("embeddingFastAvailable")] =
        (m_fastEmbeddingManager && m_fastEmbeddingManager->isAvailable());
    runtimeComponents[QStringLiteral("embeddingFastModelId")] =
        m_fastEmbeddingManager ? m_fastEmbeddingManager->activeModelId() : QString();
    runtimeComponents[QStringLiteral("embeddingFastProvider")] =
        m_fastEmbeddingManager ? m_fastEmbeddingManager->providerName() : QString();
    runtimeComponents[QStringLiteral("embeddingFastGeneration")] =
        m_fastEmbeddingManager ? m_fastEmbeddingManager->activeGenerationId() : QString();
    const QJsonObject inferenceRoleStatusForComponents =
        inferenceHealth.value(QStringLiteral("inferenceRoleStatusByModel")).toObject();
    const bool useInferenceRerank = runtimeSettings.value(QStringLiteral("inferenceServiceEnabled")).toBool(true)
        && runtimeSettings.value(QStringLiteral("inferenceRerankOffloadEnabled")).toBool(true);
    const bool useInferenceQa = runtimeSettings.value(QStringLiteral("inferenceServiceEnabled")).toBool(true)
        && runtimeSettings.value(QStringLiteral("inferenceQaOffloadEnabled")).toBool(true);
    runtimeComponents[QStringLiteral("crossEncoderFastAvailable")] =
        useInferenceRerank
            ? (inferenceRoleStatusForComponents.value(QStringLiteral("cross-encoder-fast")).toString()
               == QLatin1String("ready"))
            : (m_fastCrossEncoderReranker && m_fastCrossEncoderReranker->isAvailable());
    runtimeComponents[QStringLiteral("crossEncoderStrongAvailable")] =
        useInferenceRerank
            ? (inferenceRoleStatusForComponents.value(QStringLiteral("cross-encoder")).toString()
               == QLatin1String("ready"))
            : (m_crossEncoderReranker && m_crossEncoderReranker->isAvailable());
    runtimeComponents[QStringLiteral("personalizedLtrAvailable")] =
        (m_personalizedLtr && m_personalizedLtr->isAvailable());
    runtimeComponents[QStringLiteral("personalizedLtrModelVersion")] =
        m_personalizedLtr ? m_personalizedLtr->modelVersion() : QString();
    runtimeComponents[QStringLiteral("qaExtractiveAvailable")] =
        useInferenceQa
            ? (inferenceRoleStatusForComponents.value(QStringLiteral("qa-extractive")).toString()
               == QLatin1String("ready"))
            : (m_qaExtractiveModel && m_qaExtractiveModel->isAvailable());
    runtimeComponents[QStringLiteral("qaSnippetEnabled")] =
        runtimeSettings.value(QStringLiteral("qaSnippetEnabled")).toBool(true);
    runtimeComponents[QStringLiteral("qaPreviewMode")] =
        useInferenceQa
            ? QStringLiteral("inference_service_plus_extractive_fallback")
            : ((m_qaExtractiveModel && m_qaExtractiveModel->isAvailable())
                ? QStringLiteral("model_plus_extractive_fallback")
                : QStringLiteral("extractive_fallback_only"));
    runtimeComponents[QStringLiteral("vectorStoreAvailable")] =
        (m_vectorStore != nullptr);
    runtimeComponents[QStringLiteral("vectorIndexStrongAvailable")] =
        (m_vectorIndex && m_vectorIndex->isAvailable());
    runtimeComponents[QStringLiteral("vectorIndexFastAvailable")] =
        (m_fastVectorIndex && m_fastVectorIndex->isAvailable());
    runtimeComponents[QStringLiteral("modelRegistryInitialized")] =
        (m_modelRegistry != nullptr);
    indexHealth[QStringLiteral("runtimeComponents")] = runtimeComponents;

    const QString modelsDirResolved = m_modelRegistry
        ? m_modelRegistry->modelsDir()
        : ModelRegistry::resolveModelsDir();
    const QString manifestPath = modelsDirResolved + QStringLiteral("/manifest.json");
    indexHealth[QStringLiteral("modelsDirResolved")] = modelsDirResolved;
    indexHealth[QStringLiteral("manifestPathResolved")] = manifestPath;
    indexHealth[QStringLiteral("manifestPresent")] = QFileInfo::exists(manifestPath);

    QJsonArray modelManifest;
    if (m_modelRegistry) {
        std::vector<QString> roles;
        roles.reserve(m_modelRegistry->manifest().models.size());
        for (const auto& pair : m_modelRegistry->manifest().models) {
            roles.push_back(QString::fromStdString(pair.first));
        }
        std::sort(roles.begin(), roles.end(),
                  [](const QString& a, const QString& b) {
                      return a.toLower() < b.toLower();
                  });

        const auto modelIdMatches = [](const QString& runtimeModelId,
                                       const QString& entryModelId,
                                       const QString& entryName) {
            return !runtimeModelId.isEmpty()
                && (runtimeModelId == entryModelId || runtimeModelId == entryName);
        };
        const QJsonObject inferenceRoleStatusByModel =
            inferenceHealth.value(QStringLiteral("inferenceRoleStatusByModel")).toObject();
        const bool inferenceEnabled =
            runtimeSettings.value(QStringLiteral("inferenceServiceEnabled")).toBool(true);
        const bool inferenceEmbedOffload =
            runtimeSettings.value(QStringLiteral("inferenceEmbedOffloadEnabled")).toBool(true);
        const bool inferenceRerankOffload =
            runtimeSettings.value(QStringLiteral("inferenceRerankOffloadEnabled")).toBool(true);
        const bool inferenceQaOffload =
            runtimeSettings.value(QStringLiteral("inferenceQaOffloadEnabled")).toBool(true);

        for (const QString& role : roles) {
            const auto it = m_modelRegistry->manifest().models.find(role.toStdString());
            if (it == m_modelRegistry->manifest().models.end()) {
                continue;
            }
            const ModelManifestEntry& entry = it->second;

            const QString modelPath = modelsDirResolved + QStringLiteral("/") + entry.file;
            const QFileInfo modelInfo(modelPath);
            const QString vocabPath = entry.vocab.isEmpty()
                ? QString()
                : (modelsDirResolved + QStringLiteral("/") + entry.vocab);
            const QFileInfo vocabInfo(vocabPath);

            bool runtimeActive = false;
            QString runtimeState = QStringLiteral("inactive");
            QString runtimeReason;
            const QString inferenceRoleState =
                inferenceRoleStatusByModel.value(role).toString();

            if (role == QLatin1String("bi-encoder")) {
                if (inferenceEnabled && inferenceEmbedOffload && !inferenceRoleState.isEmpty()) {
                    runtimeActive = inferenceRoleState == QLatin1String("ready");
                    runtimeState = runtimeActive ? QStringLiteral("active")
                                                 : inferenceRoleState;
                    if (!runtimeActive) {
                        runtimeReason = QStringLiteral("Served by inference process role state: %1.")
                            .arg(inferenceRoleState);
                    }
                } else {
                    runtimeActive = m_embeddingManager
                        && m_embeddingManager->isAvailable()
                        && modelIdMatches(m_embeddingManager->activeModelId(),
                                          entry.modelId, entry.name);
                    runtimeState = runtimeActive
                        ? QStringLiteral("active")
                        : ((m_embeddingManager && m_embeddingManager->isAvailable())
                               ? QStringLiteral("available_not_selected")
                               : QStringLiteral("unavailable"));
                    if (!runtimeActive && (m_embeddingManager && m_embeddingManager->isAvailable())) {
                        runtimeReason = QStringLiteral("Embedding manager loaded a fallback role/model.");
                    }
                }
            } else if (role == QLatin1String("bi-encoder-fast")) {
                if (inferenceEnabled && inferenceEmbedOffload && !inferenceRoleState.isEmpty()) {
                    runtimeActive = inferenceRoleState == QLatin1String("ready");
                    runtimeState = runtimeActive ? QStringLiteral("active")
                                                 : inferenceRoleState;
                } else {
                    runtimeActive = m_fastEmbeddingManager
                        && m_fastEmbeddingManager->isAvailable()
                        && modelIdMatches(m_fastEmbeddingManager->activeModelId(),
                                          entry.modelId, entry.name);
                    runtimeState = runtimeActive
                        ? QStringLiteral("active")
                        : ((m_fastEmbeddingManager && m_fastEmbeddingManager->isAvailable())
                               ? QStringLiteral("available_not_selected")
                               : QStringLiteral("unavailable"));
                }
            } else if (role == QLatin1String("cross-encoder-fast")) {
                if (inferenceEnabled && inferenceRerankOffload && !inferenceRoleState.isEmpty()) {
                    runtimeActive = inferenceRoleState == QLatin1String("ready");
                    runtimeState = runtimeActive ? QStringLiteral("active")
                                                 : inferenceRoleState;
                } else {
                    runtimeActive = m_fastCrossEncoderReranker
                        && m_fastCrossEncoderReranker->isAvailable();
                    runtimeState = runtimeActive ? QStringLiteral("active")
                                                 : QStringLiteral("unavailable");
                }
            } else if (role == QLatin1String("cross-encoder")) {
                if (inferenceEnabled && inferenceRerankOffload && !inferenceRoleState.isEmpty()) {
                    runtimeActive = inferenceRoleState == QLatin1String("ready");
                    runtimeState = runtimeActive ? QStringLiteral("active")
                                                 : inferenceRoleState;
                } else {
                    runtimeActive = m_crossEncoderReranker
                        && m_crossEncoderReranker->isAvailable();
                    runtimeState = runtimeActive ? QStringLiteral("active")
                                                 : QStringLiteral("unavailable");
                }
            } else if (role == QLatin1String("qa-extractive")) {
                if (inferenceEnabled && inferenceQaOffload && !inferenceRoleState.isEmpty()) {
                    runtimeActive = inferenceRoleState == QLatin1String("ready");
                    runtimeState = runtimeActive ? QStringLiteral("active")
                                                 : inferenceRoleState;
                } else {
                    runtimeActive = m_qaExtractiveModel
                        && m_qaExtractiveModel->isAvailable();
                    runtimeState = runtimeActive ? QStringLiteral("active")
                                                 : QStringLiteral("unavailable");
                }
            } else if (role == QLatin1String("query-router")) {
                runtimeActive = false;
                runtimeState = QStringLiteral("inactive");
                runtimeReason = QStringLiteral("Heuristic query router is active in current build.");
            } else {
                runtimeState = QStringLiteral("declared_only");
            }

            QJsonObject model;
            model[QStringLiteral("role")] = role;
            model[QStringLiteral("name")] = entry.name;
            model[QStringLiteral("task")] = entry.task;
            model[QStringLiteral("latencyTier")] = entry.latencyTier;
            model[QStringLiteral("modelId")] = entry.modelId;
            model[QStringLiteral("generationId")] = entry.generationId;
            model[QStringLiteral("fallbackRole")] = entry.fallbackRole;
            model[QStringLiteral("file")] = entry.file;
            model[QStringLiteral("vocab")] = entry.vocab;
            model[QStringLiteral("dimensions")] = entry.dimensions;
            model[QStringLiteral("maxSeqLength")] = entry.maxSeqLength;
            model[QStringLiteral("tokenizer")] = entry.tokenizer;
            model[QStringLiteral("queryPrefix")] = entry.queryPrefix;
            model[QStringLiteral("extractionStrategy")] = entry.extractionStrategy;
            model[QStringLiteral("poolingStrategy")] = entry.poolingStrategy;
            model[QStringLiteral("semanticAggregationMode")] = entry.semanticAggregationMode;
            model[QStringLiteral("outputTransform")] = entry.outputTransform;
            model[QStringLiteral("modelPath")] = modelPath;
            model[QStringLiteral("modelExists")] = modelInfo.exists();
            model[QStringLiteral("modelReadable")] = modelInfo.isReadable();
            model[QStringLiteral("modelSizeBytes")] =
                modelInfo.exists() ? modelInfo.size() : 0;
            model[QStringLiteral("vocabPath")] = vocabPath;
            model[QStringLiteral("vocabExists")] =
                entry.vocab.isEmpty() ? false : vocabInfo.exists();
            model[QStringLiteral("vocabReadable")] =
                entry.vocab.isEmpty() ? false : vocabInfo.isReadable();
            model[QStringLiteral("runtimeActive")] = runtimeActive;
            model[QStringLiteral("runtimeState")] = runtimeState;
            model[QStringLiteral("runtimeReason")] = runtimeReason;
            model[QStringLiteral("providerPreferred")] =
                entry.providerPolicy.preferredProvider;
            model[QStringLiteral("providerPreferCoreMl")] =
                entry.providerPolicy.preferCoreMl;
            model[QStringLiteral("providerAllowCpuFallback")] =
                entry.providerPolicy.allowCpuFallback;
            model[QStringLiteral("providerDisableCoreMlEnvVar")] =
                entry.providerPolicy.disableCoreMlEnvVar;

            QJsonArray inputs;
            for (const QString& input : entry.inputs) {
                inputs.append(input);
            }
            model[QStringLiteral("inputs")] = inputs;

            QJsonArray outputs;
            for (const QString& output : entry.outputs) {
                outputs.append(output);
            }
            model[QStringLiteral("outputs")] = outputs;

            modelManifest.append(model);
        }
    }
    indexHealth[QStringLiteral("modelManifest")] = modelManifest;

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QJsonArray environmentKnown;
    const auto appendKnownEnv = [&](const QString& key,
                                    const QString& description,
                                    const QString& fallbackValue,
                                    bool parseAsBool) {
        const bool isSet = env.contains(key);
        const QString rawValue = env.value(key);
        QJsonObject row;
        row[QStringLiteral("key")] = key;
        row[QStringLiteral("description")] = description;
        row[QStringLiteral("isSet")] = isSet;
        row[QStringLiteral("value")] = rawValue;
        row[QStringLiteral("fallback")] = fallbackValue;
        if (parseAsBool) {
            const bool effective = isSet ? envFlagEnabled(rawValue) : envFlagEnabled(fallbackValue);
            row[QStringLiteral("effectiveBool")] = effective;
            row[QStringLiteral("effectiveValue")] = effective
                ? QStringLiteral("true")
                : QStringLiteral("false");
        } else {
            row[QStringLiteral("effectiveValue")] = isSet ? rawValue : fallbackValue;
        }
        environmentKnown.append(row);
    };

    appendKnownEnv(QStringLiteral("BETTERSPOTLIGHT_DATA_DIR"),
                   QStringLiteral("Override BetterSpotlight data directory."),
                   m_dataDir,
                   false);
    appendKnownEnv(QStringLiteral("BETTERSPOTLIGHT_MODELS_DIR"),
                   QStringLiteral("Override models directory (manifest + model artifacts)."),
                   modelsDirResolved,
                   false);
    appendKnownEnv(QStringLiteral("BETTERSPOTLIGHT_DISABLE_COREML"),
                   QStringLiteral("Disable CoreML execution provider and force CPU path."),
                   QStringLiteral("0"),
                   true);
    appendKnownEnv(QStringLiteral("BETTERSPOTLIGHT_SOCKET_DIR"),
                   QStringLiteral("Override IPC socket directory."),
                   QString(),
                   false);
    appendKnownEnv(QStringLiteral("BETTERSPOTLIGHT_EMBED_BATCH_BASE"),
                   QStringLiteral("Base embedding batch size."),
                   QStringLiteral("24"),
                   false);
    appendKnownEnv(QStringLiteral("BETTERSPOTLIGHT_EMBED_BATCH_MIN"),
                   QStringLiteral("Minimum embedding batch size under pressure."),
                   QStringLiteral("8"),
                   false);
    appendKnownEnv(QStringLiteral("BETTERSPOTLIGHT_EMBED_RSS_SOFT_MB"),
                   QStringLiteral("Embedding pipeline soft RSS cap (MB)."),
                   QStringLiteral("900"),
                   false);
    appendKnownEnv(QStringLiteral("BETTERSPOTLIGHT_EMBED_RSS_HARD_MB"),
                   QStringLiteral("Embedding pipeline hard RSS cap (MB)."),
                   QStringLiteral("1200"),
                   false);
    appendKnownEnv(QStringLiteral("BETTERSPOTLIGHT_INDEXER_RSS_SOFT_MB"),
                   QStringLiteral("Indexer soft RSS cap (MB)."),
                   QStringLiteral("900"),
                   false);
    appendKnownEnv(QStringLiteral("BETTERSPOTLIGHT_INDEXER_RSS_HARD_MB"),
                   QStringLiteral("Indexer hard RSS cap (MB)."),
                   QStringLiteral("1200"),
                   false);
    appendKnownEnv(QStringLiteral("BETTERSPOTLIGHT_INDEXER_PREP_WORKERS_PRESSURE"),
                   QStringLiteral("Indexer prep worker backpressure threshold."),
                   QStringLiteral("4"),
                   false);

    QJsonArray environmentAll;
    QStringList envKeys = env.keys();
    envKeys.erase(std::remove_if(envKeys.begin(), envKeys.end(),
                                 [](const QString& key) {
                                     return !key.startsWith(QStringLiteral("BETTERSPOTLIGHT_"));
                                 }),
                  envKeys.end());
    envKeys.sort(Qt::CaseInsensitive);
    for (const QString& key : envKeys) {
        QJsonObject row;
        row[QStringLiteral("key")] = key;
        row[QStringLiteral("value")] = env.value(key);
        environmentAll.append(row);
    }
    indexHealth[QStringLiteral("environmentKnown")] = environmentKnown;
    indexHealth[QStringLiteral("environmentAll")] = environmentAll;

    bool includesHomeRoot = false;
    const QString homePath = QDir::homePath();
    for (const QJsonValue& rootValue : queueRoots) {
        const QString rootPath = rootValue.toString();
        if (rootPath == homePath) {
            includesHomeRoot = true;
            break;
        }
    }
    const bool lowCoverage = contentCoveragePct < 50.0;
    const bool highBacklog = queuePending > 2000;
    const bool highRootFanout = queueRoots.size() > 32;
    if (includesHomeRoot && (lowCoverage || highBacklog)) {
        QJsonObject advisory;
        advisory[QStringLiteral("code")] = QStringLiteral("curated_roots_recommended");
        advisory[QStringLiteral("severity")] = QStringLiteral("info");
        advisory[QStringLiteral("message")] =
            QStringLiteral("Index roots include the full home directory while coverage is low or backlog is high.");
        advisory[QStringLiteral("recommendation")] =
            QStringLiteral("Prefer curated roots (Documents/Projects/Downloads) to reduce lexical noise and improve extraction coverage.");
        advisory[QStringLiteral("contentCoveragePct")] = contentCoveragePct;
        advisory[QStringLiteral("queuePending")] = queuePending;
        indexHealth[QStringLiteral("retrievalAdvisory")] = advisory;
    } else if (highRootFanout && (lowCoverage || highBacklog)) {
        QJsonObject advisory;
        advisory[QStringLiteral("code")] = QStringLiteral("root_fanout_recommended");
        advisory[QStringLiteral("severity")] = QStringLiteral("info");
        advisory[QStringLiteral("message")] =
            QStringLiteral("Index roots fan out across many directories while backlog is high or coverage is low.");
        advisory[QStringLiteral("recommendation")] =
            QStringLiteral("Reduce roots to high-signal folders (for example Documents/Desktop/Downloads) to improve quality and indexing throughput.");
        advisory[QStringLiteral("rootCount")] = queueRoots.size();
        advisory[QStringLiteral("contentCoveragePct")] = contentCoveragePct;
        advisory[QStringLiteral("queuePending")] = queuePending;
        indexHealth[QStringLiteral("retrievalAdvisory")] = advisory;
    }

    QJsonObject serviceHealth;
    serviceHealth[QStringLiteral("indexerRunning")] = (queueSource == QLatin1String("indexer_rpc"));
    serviceHealth[QStringLiteral("extractorRunning")] = true;
    serviceHealth[QStringLiteral("queryServiceRunning")] = true;
    serviceHealth[QStringLiteral("inferenceServiceRunning")] =
        inferenceHealth.value(QStringLiteral("inferenceServiceConnected")).toBool(false);
    serviceHealth[QStringLiteral("uptime")] = 0;

    QJsonObject result;
    result[QStringLiteral("indexHealth")] = indexHealth;
    result[QStringLiteral("serviceHealth")] = serviceHealth;
    result[QStringLiteral("issues")] = QJsonArray();
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetHealthDetails(uint64_t id, const QJsonObject& params)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    int limit = params.value(QStringLiteral("limit")).toInt(50);
    int offset = params.value(QStringLiteral("offset")).toInt(0);
    if (limit < 1) {
        limit = 1;
    } else if (limit > 500) {
        limit = 500;
    }
    if (offset < 0) {
        offset = 0;
    }

    const QJsonObject summaryResponse = handleGetHealth(id);
    if (summaryResponse.value(QStringLiteral("type")).toString() == QLatin1String("error")) {
        return summaryResponse;
    }
    const QJsonObject summaryResult = summaryResponse.value(QStringLiteral("result")).toObject();

    QJsonArray failures;
    int expectedGapRows = 0;
    int criticalRows = 0;
    if (sqlite3* db = m_store->rawDb()) {
        const char* sql = R"(
            SELECT i.path, f.stage, f.error_message, f.failure_count, f.last_failed_at
            FROM failures f
            JOIN items i ON i.id = f.item_id
            ORDER BY f.last_failed_at DESC
            LIMIT ? OFFSET ?
        )";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, limit);
            sqlite3_bind_int(stmt, 2, offset);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* stage = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                const char* error = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                const int failureCount = sqlite3_column_int(stmt, 3);
                const double lastFailedAt = sqlite3_column_double(stmt, 4);

                const QString errorText = error ? QString::fromUtf8(error) : QString();
                const bool expectedGap = isExpectedGapFailureMessage(errorText);
                if (expectedGap) {
                    ++expectedGapRows;
                } else {
                    ++criticalRows;
                }

                QJsonObject entry;
                entry[QStringLiteral("path")] = path ? QString::fromUtf8(path) : QString();
                entry[QStringLiteral("stage")] = stage ? QString::fromUtf8(stage) : QString();
                entry[QStringLiteral("error")] = errorText;
                entry[QStringLiteral("failureCount")] = failureCount;
                entry[QStringLiteral("expectedGap")] = expectedGap;
                entry[QStringLiteral("severity")] =
                    expectedGap ? QStringLiteral("expected_gap") : QStringLiteral("critical");
                entry[QStringLiteral("lastFailedAt")] = lastFailedAt > 0.0
                    ? QDateTime::fromSecsSinceEpoch(static_cast<qint64>(lastFailedAt))
                          .toUTC()
                          .toString(Qt::ISODate)
                    : QString();
                failures.append(entry);
            }
        }
        sqlite3_finalize(stmt);
    }

    QJsonObject processStats;
    processStats[QStringLiteral("query")] = processStatsForService(QStringLiteral("query"));
    processStats[QStringLiteral("indexer")] = processStatsForService(QStringLiteral("indexer"));
    processStats[QStringLiteral("extractor")] = processStatsForService(QStringLiteral("extractor"));

    QJsonObject details;
    details[QStringLiteral("failures")] = failures;
    details[QStringLiteral("failuresLimit")] = limit;
    details[QStringLiteral("failuresOffset")] = offset;
    details[QStringLiteral("criticalFailureRows")] = criticalRows;
    details[QStringLiteral("expectedGapFailureRows")] = expectedGapRows;
    details[QStringLiteral("processStats")] = processStats;
    details[QStringLiteral("queryStats")] = queryStatsSnapshot();
    details[QStringLiteral("bsignore")] = bsignoreStatusJson();

    QJsonObject result;
    result[QStringLiteral("indexHealth")] = summaryResult.value(QStringLiteral("indexHealth")).toObject();
    result[QStringLiteral("serviceHealth")] = summaryResult.value(QStringLiteral("serviceHealth")).toObject();
    result[QStringLiteral("issues")] = summaryResult.value(QStringLiteral("issues")).toArray();
    result[QStringLiteral("details")] = details;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleRecordFeedback(uint64_t id, const QJsonObject& params)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    // Parse required fields
    if (!params.contains(QStringLiteral("itemId"))) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'itemId' parameter"));
    }

    int64_t itemId = static_cast<int64_t>(params.value(QStringLiteral("itemId")).toInteger());
    QString action = params.value(QStringLiteral("action")).toString();
    QString query = params.value(QStringLiteral("query")).toString();
    int position = params.value(QStringLiteral("position")).toInt(0);

    // Record feedback in the feedback table
    if (!m_store->recordFeedback(itemId, action, query, position)) {
        LOG_WARN(bsIpc, "Failed to insert feedback row for item %lld",
                 static_cast<long long>(itemId));
    }

    // Also update frequency counters
    if (!m_store->incrementFrequency(itemId)) {
        return IpcMessage::makeError(id, IpcErrorCode::InternalError,
                                     QStringLiteral("Failed to record feedback for item %1")
                                         .arg(itemId));
    }

    LOG_INFO(bsIpc, "Feedback recorded for item %lld", static_cast<long long>(itemId));

    // Feedback changes scores — invalidate cache
    m_queryCache.clear();

    QJsonObject result;
    result[QStringLiteral("recorded")] = true;
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetFrequency(uint64_t id, const QJsonObject& params)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    if (!params.contains(QStringLiteral("itemId"))) {
        return IpcMessage::makeError(id, IpcErrorCode::InvalidParams,
                                     QStringLiteral("Missing 'itemId' parameter"));
    }

    int64_t itemId = static_cast<int64_t>(params.value(QStringLiteral("itemId")).toInteger());

    auto freqOpt = m_store->getFrequency(itemId);

    int openCount = 0;
    QString lastOpenDate;
    if (freqOpt.has_value()) {
        openCount = freqOpt->openCount;
        if (freqOpt->lastOpenedAt > 0.0) {
            lastOpenDate = QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>(freqOpt->lastOpenedAt * 1000.0))
                .toUTC().toString(Qt::ISODate);
        }
    }

    // Compute frequency tier: 0 opens = tier 0, 1-5 = tier 1, 6-20 = tier 2, 21+ = tier 3
    int frequencyTier = 0;
    if (openCount >= 21) {
        frequencyTier = 3;
    } else if (openCount >= 6) {
        frequencyTier = 2;
    } else if (openCount >= 1) {
        frequencyTier = 1;
    }

    // Compute boost using scorer
    double boost = m_scorer.computeFrequencyBoost(
        openCount, freqOpt.has_value() ? freqOpt->lastOpenedAt : 0.0);

    QJsonObject result;
    result[QStringLiteral("openCount")] = openCount;
    result[QStringLiteral("lastOpenDate")] = lastOpenDate;
    result[QStringLiteral("frequencyTier")] = frequencyTier;
    result[QStringLiteral("boost")] = boost;
    return IpcMessage::makeResponse(id, result);
}

} // namespace bs
