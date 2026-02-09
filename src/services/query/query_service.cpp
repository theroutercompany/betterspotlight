#include "query_service.h"
#include "core/ipc/message.h"
#include "core/ipc/socket_client.h"
#include "core/query/query_normalizer.h"
#include "core/query/query_parser.h"
#include "core/shared/search_result.h"
#include "core/shared/search_options.h"
#include "core/shared/logging.h"
#include "core/ranking/match_classifier.h"

#include <sqlite3.h>

#include "core/embedding/embedding_manager.h"
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

enum class QueryClass {
    NaturalLanguage,
    PathOrCode,
    ShortAmbiguous,
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

    if (correction.docCount >= 25) {
        confidence += 0.22;
    } else if (correction.docCount >= 12) {
        confidence += 0.18;
    } else if (correction.docCount >= 6) {
        confidence += 0.13;
    } else if (correction.docCount >= 3) {
        confidence += 0.08;
    }

    if (!sourceToken.isEmpty() && !correction.corrected.isEmpty()
        && sourceToken.front().toLower() == correction.corrected.front().toLower()) {
        confidence += 0.06;
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

QString queryClassToString(QueryClass queryClass)
{
    switch (queryClass) {
    case QueryClass::NaturalLanguage:
        return QStringLiteral("natural_language");
    case QueryClass::PathOrCode:
        return QStringLiteral("path_or_code");
    case QueryClass::ShortAmbiguous:
        return QStringLiteral("short_ambiguous");
    }
    return QStringLiteral("unknown");
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

const QSet<QString>& queryStopwords()
{
    static const QSet<QString> stopwords = {
        QStringLiteral("a"),
        QStringLiteral("an"),
        QStringLiteral("any"),
        QStringLiteral("and"),
        QStringLiteral("are"),
        QStringLiteral("at"),
        QStringLiteral("for"),
        QStringLiteral("from"),
        QStringLiteral("how"),
        QStringLiteral("in"),
        QStringLiteral("is"),
        QStringLiteral("it"),
        QStringLiteral("my"),
        QStringLiteral("of"),
        QStringLiteral("on"),
        QStringLiteral("or"),
        QStringLiteral("that"),
        QStringLiteral("there"),
        QStringLiteral("the"),
        QStringLiteral("to"),
        QStringLiteral("what"),
        QStringLiteral("when"),
        QStringLiteral("where"),
        QStringLiteral("which"),
        QStringLiteral("who"),
        QStringLiteral("why"),
        QStringLiteral("with"),
    };
    return stopwords;
}

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

QJsonObject QueryService::handleRequest(const QJsonObject& request)
{
    QString method = request.value(QStringLiteral("method")).toString();
    uint64_t id = static_cast<uint64_t>(request.value(QStringLiteral("id")).toInteger());
    QJsonObject params = request.value(QStringLiteral("params")).toObject();

    if (method == QLatin1String("search"))          return handleSearch(id, params);
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
    m_vectorIndexPath = m_dataDir + QStringLiteral("/vectors.hnsw");
    m_vectorMetaPath = m_dataDir + QStringLiteral("/vectors.meta");

    auto store = SQLiteStore::open(m_dbPath);
    if (!store.has_value()) {
        LOG_ERROR(bsIpc, "Failed to open database at: %s", qPrintable(m_dbPath));
        return false;
    }

    m_store.emplace(std::move(store.value()));
    LOG_INFO(bsIpc, "Database opened at: %s", qPrintable(m_dbPath));

    initM2Modules();
    initBsignoreWatch();
    return true;
}

void QueryService::initM2Modules()
{
    if (m_m2Initialized) {
        return;
    }
    m_m2Initialized = true;

    ::sqlite3* db = m_store->rawDb();

    if (!m_typoLexicon.build(db)) {
        LOG_WARN(bsIpc, "TypoLexicon build failed; typo correction lexicon unavailable");
    } else {
        LOG_INFO(bsIpc, "TypoLexicon built with %d terms", m_typoLexicon.termCount());
    }

    m_interactionTracker = std::make_unique<InteractionTracker>(db);
    m_feedbackAggregator = std::make_unique<FeedbackAggregator>(db);
    m_pathPreferences = std::make_unique<PathPreferences>(db);
    m_typeAffinity = std::make_unique<TypeAffinity>(db);

    m_vectorStore = std::make_unique<VectorStore>(db);

    auto loadedVectorIndex = std::make_unique<VectorIndex>();
    if (QFile::exists(m_vectorIndexPath) && QFile::exists(m_vectorMetaPath)) {
        if (!loadedVectorIndex->load(m_vectorIndexPath.toStdString(), m_vectorMetaPath.toStdString())) {
            LOG_WARN(bsIpc, "Failed to load vector index, semantic search disabled");
            loadedVectorIndex.reset();
        } else {
            LOG_INFO(bsIpc, "Vector index loaded: %d vectors", loadedVectorIndex->totalElements());
        }
    } else {
        if (!loadedVectorIndex->create()) {
            LOG_WARN(bsIpc, "Failed to create vector index, semantic search disabled");
            loadedVectorIndex.reset();
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(m_vectorIndexMutex);
        m_vectorIndex = std::move(loadedVectorIndex);
    }

    const QString appDir = QCoreApplication::applicationDirPath();

    QStringList modelDirs;
    const QString envModelDir =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("BETTERSPOTLIGHT_MODELS_DIR"));
    if (!envModelDir.isEmpty()) {
        modelDirs << QDir::cleanPath(envModelDir);
    }

    modelDirs << QDir::cleanPath(appDir + QStringLiteral("/../Resources/models"));
    modelDirs << QDir::cleanPath(appDir + QStringLiteral("/../../app/betterspotlight.app/Contents/Resources/models"));
    modelDirs << QDir::cleanPath(appDir + QStringLiteral("/../../../app/betterspotlight.app/Contents/Resources/models"));
    modelDirs << QDir::cleanPath(appDir + QStringLiteral("/../../../../data/models"));
#ifdef BETTERSPOTLIGHT_SOURCE_DIR
    modelDirs << QDir::cleanPath(QString::fromUtf8(BETTERSPOTLIGHT_SOURCE_DIR)
                                 + QStringLiteral("/data/models"));
#endif
    modelDirs.removeDuplicates();

    QString modelPath;
    QString vocabPath;
    for (const QString& dir : modelDirs) {
        const QString onnx = dir + QStringLiteral("/bge-small-en-v1.5-int8.onnx");
        const QString vocab = dir + QStringLiteral("/vocab.txt");
        if (QFile::exists(onnx) && QFile::exists(vocab)) {
            modelPath = onnx;
            vocabPath = vocab;
            break;
        }
    }

    if (modelPath.isEmpty()) {
        LOG_WARN(bsIpc, "Embedding assets missing. Searched model dirs: %s",
                 qPrintable(modelDirs.join(QStringLiteral(", "))));
        const QString fallbackDir = modelDirs.isEmpty()
                                        ? QDir::cleanPath(appDir + QStringLiteral("/../Resources/models"))
                                        : modelDirs.first();
        modelPath = fallbackDir + QStringLiteral("/bge-small-en-v1.5-int8.onnx");
        vocabPath = fallbackDir + QStringLiteral("/vocab.txt");
    }

    m_embeddingModelPath = modelPath;
    m_embeddingVocabPath = vocabPath;

    m_embeddingManager = std::make_unique<EmbeddingManager>(modelPath, vocabPath);
    if (!m_embeddingManager->initialize()) {
        LOG_WARN(bsIpc, "EmbeddingManager unavailable, semantic search disabled");
    } else {
        LOG_INFO(bsIpc, "EmbeddingManager initialized");
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
    const QFileInfo bsignoreInfo(m_bsignorePath);
    if (bsignoreInfo.exists()) {
        m_bsignoreLoaded = m_bsignoreParser.loadFromFile(m_bsignorePath.toStdString());
    } else {
        m_bsignoreParser.clear();
        m_bsignoreLoaded = true;
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
    if (!ensureStoreOpen()) {
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
        if (ctxObj.contains(QStringLiteral("recentPaths"))) {
            QJsonArray recentArr = ctxObj.value(QStringLiteral("recentPaths")).toArray();
            context.recentPaths.reserve(static_cast<size_t>(recentArr.size()));
            for (const auto& val : recentArr) {
                context.recentPaths.push_back(val.toString());
            }
        }
    }

    const QString queryLower = query.toLower();
    const QueryHints queryHints = parseQueryHints(queryLower);
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
            if (hasAlpha && allUpper) {
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
    const auto runFtsSearch = [&](const QString& q, int localLimit, bool relaxedMode) {
        return hasSearchFilters
            ? m_store->searchFts5(q, localLimit, relaxedMode, searchOptions)
            : m_store->searchFts5(q, localLimit, relaxedMode);
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
            if (candidateConfidence < kRewriteCandidateThreshold) {
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

        const bool strictWeakOrEmpty = strictHits.empty() || bestLexicalStrength(strictHits) < 2.0;
        rewriteDecision = buildTypoRewriteDecision(strictWeakOrEmpty ? 2 : 1, strictWeakOrEmpty);

        if (strictWeakOrEmpty) {
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
                    rewriteDecision.reason = QStringLiteral("strict_weak_or_empty");
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
    std::vector<SearchResult> results;
    results.reserve(hits.size());
    std::unordered_map<int64_t, size_t> bestHitByItem;
    bestHitByItem.reserve(hits.size());
    QString classifyMatchQuery = classifyQuery;
    classifyMatchQuery.replace(QLatin1Char('-'), QLatin1Char(' '));

    for (const auto& hit : hits) {
        auto itemOpt = m_store->getItemById(hit.fileId);
        if (!itemOpt.has_value()) {
            continue;
        }

        const auto& item = itemOpt.value();
        if (isExcludedByBsignore(item.path)) {
            continue;
        }

        SearchResult sr;
        sr.itemId = item.id;
        sr.path = item.path;
        sr.name = item.name;
        sr.kind = item.kind;
        sr.bm25RawScore = hit.bm25Score;
        sr.snippet = hit.snippet;
        sr.highlights = parseHighlights(sr.snippet);
        sr.fileSize = item.size;

        // Format modification date as ISO 8601
        if (item.modifiedAt > 0.0) {
            sr.modificationDate = QDateTime::fromMSecsSinceEpoch(
                static_cast<qint64>(item.modifiedAt * 1000.0)).toUTC().toString(Qt::ISODate);
        }

        sr.isPinned = item.isPinned;

        // Query frequency data for this item
        auto freqOpt = m_store->getFrequency(item.id);
        if (freqOpt.has_value()) {
            sr.openCount = freqOpt->openCount;
            if (freqOpt->lastOpenedAt > 0.0) {
                sr.lastOpenDate = QDateTime::fromMSecsSinceEpoch(
                    static_cast<qint64>(freqOpt->lastOpenedAt * 1000.0))
                    .toUTC().toString(Qt::ISODate);
            }
        }

        // Classify match type for name/path matches
        sr.matchType = MatchClassifier::classify(classifyMatchQuery, item.name, item.path);
        if (sr.matchType == MatchType::Fuzzy) {
            if (hit.bm25Score == -1.0) {
                // Fuzzy filename fallback does not expose exact edit distance yet.
                sr.fuzzyDistance = 1;
            } else {
                const QString baseName = QFileInfo(item.name).completeBaseName();
                sr.fuzzyDistance = MatchClassifier::editDistance(classifyMatchQuery, baseName);
            }
        }

        const double lexicalStrength = std::max(0.0, -hit.bm25Score);
        auto existingIt = bestHitByItem.find(item.id);
        if (existingIt == bestHitByItem.end()) {
            bestHitByItem[item.id] = results.size();
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

    const QueryClass queryClass = classifyQueryShape(queryLower, querySignalTokens, queryTokensRaw);
    const bool naturalLanguageQuery = queryClass == QueryClass::NaturalLanguage;
    const bool shortAmbiguousQuery = queryClass == QueryClass::ShortAmbiguous;
    const float semanticThreshold = naturalLanguageQuery ? 0.62f
        : (shortAmbiguousQuery ? 0.66f : 0.70f);
    const float semanticOnlyFloor = naturalLanguageQuery ? 0.08f
        : (shortAmbiguousQuery ? 0.10f : 0.15f);
    const int semanticOnlyCap = naturalLanguageQuery
        ? std::min(6, limit)
        : (shortAmbiguousQuery ? std::min(4, limit) : std::min(3, limit / 2));
    const float mergeLexicalWeight = naturalLanguageQuery ? 0.55f
        : (shortAmbiguousQuery ? 0.65f : 0.70f);
    const float mergeSemanticWeight = naturalLanguageQuery ? 0.45f
        : (shortAmbiguousQuery ? 0.35f : 0.30f);
    const bool strictLexicalWeakOrEmpty =
        strictHits.empty() || bestLexicalStrength(strictHits) < 2.0;
    constexpr float kSemanticOnlySafetySimilarity = 0.78f;
    const float relaxedSemanticOnlySimilarity =
        std::max(semanticThreshold + 0.03f, 0.66f);

    std::vector<SemanticResult> semanticResults;
    std::unordered_map<int64_t, float> semanticSimilarityByItemId;
    std::unordered_map<int64_t, float> semanticNormalizedByItemId;
    int semanticOnlySuppressedCount = 0;
    int semanticOnlyAdmittedCount = 0;
    QHash<QString, int> semanticOnlyAdmitReasons;
    if (m_embeddingManager && m_embeddingManager->isAvailable()) {
        std::vector<float> queryVec = m_embeddingManager->embedQuery(query);
        if (!queryVec.empty()) {
            std::shared_lock<std::shared_mutex> lock(m_vectorIndexMutex);
            if (m_vectorIndex && m_vectorIndex->isAvailable() && m_vectorStore) {
                auto knnHits = m_vectorIndex->search(queryVec.data());
                semanticResults.reserve(knnHits.size());

                for (const auto& hit : knnHits) {
                    float cosineSim = 1.0f - hit.distance;
                    if (cosineSim < semanticThreshold) {
                        continue;
                    }
                    const float normalizedSemantic = SearchMerger::normalizeSemanticScore(
                        cosineSim, semanticThreshold);
                    if (normalizedSemantic <= semanticOnlyFloor) {
                        continue;
                    }
                    auto itemIdOpt = m_vectorStore->getItemId(hit.label);
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

                    SemanticResult sr;
                    sr.itemId = itemIdOpt.value();
                    sr.cosineSimilarity = cosineSim;
                    semanticResults.push_back(sr);
                    semanticSimilarityByItemId[sr.itemId] = cosineSim;
                    semanticNormalizedByItemId[sr.itemId] = normalizedSemantic;
                }
            }
        }
    }

    if (!semanticResults.empty()) {
        MergeConfig mergeConfig;
        mergeConfig.similarityThreshold = semanticThreshold;
        mergeConfig.lexicalWeight = mergeLexicalWeight;
        mergeConfig.semanticWeight = mergeSemanticWeight;
        mergeConfig.maxResults = std::max(limit * 2, limit);
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

    // M2: Apply interaction, path preference, and type affinity boosts
    QString normalizedQuery = InteractionTracker::normalizeQuery(query);

    auto isNoteLikeTextExtension = [](const QString& ext) {
        return ext == QLatin1String("md")
            || ext == QLatin1String("txt")
            || ext == QLatin1String("log");
    };

    for (auto& sr : results) {
        double m2Boost = 0.0;
        const QString ext = QFileInfo(sr.path).suffix().toLower();
        const uint8_t originBits = candidateOrigins.count(sr.itemId) > 0
            ? candidateOrigins.at(sr.itemId)
            : static_cast<uint8_t>(CandidateOriginStrict);
        const bool hasStrictOrigin = (originBits & CandidateOriginStrict) != 0;
        const bool hasRelaxedOrigin = (originBits & CandidateOriginRelaxed) != 0;
        const bool hasNameFallbackOrigin = (originBits & CandidateOriginNameFallback) != 0;
        const bool fallbackOnlyOrigin = hasNameFallbackOrigin && !hasStrictOrigin && !hasRelaxedOrigin;

        if (m_interactionTracker) {
            m2Boost += m_interactionTracker->getInteractionBoost(normalizedQuery, sr.itemId);
        }
        if (m_pathPreferences) {
            m2Boost += m_pathPreferences->getBoost(sr.path);
        }
        if (m_typeAffinity) {
            m2Boost += m_typeAffinity->getBoost(sr.path);
        }

        if (naturalLanguageQuery && sr.semanticNormalized > 0.0) {
            const bool semanticOnly = lexicalItemIds.find(sr.itemId) == lexicalItemIds.end();
            const double normalizedSemantic = std::clamp(sr.semanticNormalized, 0.0, 1.0);
            const double semanticBoost = semanticOnly
                ? std::min(14.0, 5.0 + (normalizedSemantic * 14.0))
                : std::min(8.0, normalizedSemantic * 8.0);
            m2Boost += semanticBoost;
            sr.scoreBreakdown.semanticBoost += semanticBoost;
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
                m2Boost += std::min(42.0, static_cast<double>(overlapCount) * 12.0);
                if (queryTokenCount >= 3 && overlapRatio >= 0.60) {
                    m2Boost += 8.0;
                }
            } else if (sr.matchType == MatchType::Content && querySignalTokens.size() >= 3) {
                // Long natural-language queries should not be dominated by unrelated
                // "query list" notes with zero filename signal.
                m2Boost -= 22.0;
                if (querySignalTokens.size() >= 4 && isNoteLikeTextExtension(ext)) {
                    m2Boost -= 8.0;
                }
            }

            if (looksLikeNaturalLanguageQuery(querySignalTokens) && overlapCount == 0
                && !hasStrictOrigin) {
                // Prevent low-signal relaxed/fallback candidates from dominating noisy corpora.
                m2Boost -= fallbackOnlyOrigin ? 24.0 : 14.0;
                if (fallbackOnlyOrigin && sr.matchType == MatchType::Fuzzy) {
                    m2Boost -= 6.0;
                }
            }
        }

        if (queryHints.downloadsHint && sr.path.startsWith(downloadsPath)) {
            m2Boost += 18.0;
        }
        if (queryHints.documentsHint && sr.path.startsWith(documentsPath)) {
            m2Boost += 18.0;
        }
        if (queryHints.desktopHint && sr.path.startsWith(desktopPath)) {
            m2Boost += 18.0;
        }

        if (!queryHints.extensionHint.isEmpty()) {
            if (queryHints.extensionHint == QLatin1String("__image__")) {
                if (ext == QLatin1String("png") || ext == QLatin1String("jpg")
                    || ext == QLatin1String("jpeg") || ext == QLatin1String("webp")
                    || ext == QLatin1String("bmp") || ext == QLatin1String("tiff")) {
                    m2Boost += 10.0;
                }
            } else if (ext == queryHints.extensionHint) {
                m2Boost += 10.0;
            }
        }

        if (!sr.modificationDate.isEmpty()
            && (queryHints.monthHint > 0 || queryHints.yearHint > 0)) {
            const QDateTime modified = QDateTime::fromString(sr.modificationDate, Qt::ISODate);
            if (modified.isValid()) {
                if (queryHints.monthHint > 0 && modified.date().month() == queryHints.monthHint) {
                    m2Boost += 6.0;
                }
                if (queryHints.yearHint > 0 && modified.date().year() == queryHints.yearHint) {
                    m2Boost += 4.0;
                }
            }
        }

        sr.score = std::max(0.0, sr.score + m2Boost);
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
        debugInfo[QStringLiteral("fusionMode")] = QStringLiteral("weighted_rrf");
        debugInfo[QStringLiteral("queryClass")] = queryClassToString(queryClass);
        debugInfo[QStringLiteral("semanticThresholdApplied")] = semanticThreshold;
        debugInfo[QStringLiteral("semanticOnlyFloorApplied")] = semanticOnlyFloor;
        debugInfo[QStringLiteral("semanticOnlyCapApplied")] = semanticOnlyCap;
        debugInfo[QStringLiteral("mergeLexicalWeightApplied")] = mergeLexicalWeight;
        debugInfo[QStringLiteral("mergeSemanticWeightApplied")] = mergeSemanticWeight;
        debugInfo[QStringLiteral("semanticOnlySuppressedCount")] = semanticOnlySuppressedCount;
        debugInfo[QStringLiteral("semanticOnlyAdmittedCount")] = semanticOnlyAdmittedCount;
        QJsonObject semanticReasonSummary;
        for (auto it = semanticOnlyAdmitReasons.constBegin();
             it != semanticOnlyAdmitReasons.constEnd(); ++it) {
            semanticReasonSummary[it.key()] = it.value();
        }
        debugInfo[QStringLiteral("semanticOnlyAdmitReasonSummary")] = semanticReasonSummary;
        debugInfo[QStringLiteral("queryAfterParse")] = query;
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
        if (!rewrittenRelaxedQuery.isEmpty() && rewrittenRelaxedQuery != query) {
            debugInfo[QStringLiteral("rewrittenQuery")] = rewrittenRelaxedQuery;
        }
        result[QStringLiteral("debugInfo")] = debugInfo;
    }
    return IpcMessage::makeResponse(id, result);
}

QJsonObject QueryService::handleGetHealth(uint64_t id)
{
    if (!ensureStoreOpen()) {
        return IpcMessage::makeError(id, IpcErrorCode::ServiceUnavailable,
                                     QStringLiteral("Database is not available"));
    }

    IndexHealth health = m_store->getHealth();
    const QString dataDir = m_dataDir.isEmpty()
                                ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                                      + QStringLiteral("/betterspotlight")
                                : m_dataDir;
    const int totalEmbeddedVectors = m_vectorStore ? m_vectorStore->countMappings() : 0;
    const qint64 vectorIndexSize = QFileInfo(dataDir + QStringLiteral("/vectors.hnsw")).size();
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

    VectorRebuildState rebuildStateCopy;
    {
        std::lock_guard<std::mutex> lock(m_vectorRebuildMutex);
        rebuildStateCopy = m_vectorRebuildState;
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

    if (!m_indexerClient) {
        m_indexerClient = std::make_unique<SocketClient>(this);
    }

    if (!m_indexerClient->isConnected()) {
        const QString indexerSocketPath = ServiceBase::socketPath(QStringLiteral("indexer"));
        m_indexerClient->connectToServer(indexerSocketPath, 150);
    }

    if (m_indexerClient->isConnected()) {
        auto queueResponse = m_indexerClient->sendRequest(
            QStringLiteral("getQueueStatus"), {}, 500);
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
        } else {
            queueSource = QStringLiteral("unavailable");
            m_indexerClient->disconnect();
        }
    }

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
    indexHealth[QStringLiteral("contentCoveragePct")] = contentCoveragePct;
    indexHealth[QStringLiteral("semanticCoveragePct")] = semanticCoveragePct;
    indexHealth[QStringLiteral("multiChunkEmbeddingEnabled")] = true;
    indexHealth[QStringLiteral("queryRewriteEnabled")] = true;
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
    const QJsonObject bsignoreStatus = bsignoreStatusJson();
    indexHealth[QStringLiteral("bsignorePath")] = bsignoreStatus.value(QStringLiteral("path")).toString();
    indexHealth[QStringLiteral("bsignoreLoaded")] = bsignoreStatus.value(QStringLiteral("loaded")).toBool(false);
    indexHealth[QStringLiteral("bsignorePatternCount")] =
        bsignoreStatus.value(QStringLiteral("patternCount")).toInt(0);
    indexHealth[QStringLiteral("bsignoreLastLoadedAtMs")] =
        bsignoreStatus.value(QStringLiteral("lastLoadedAtMs")).toInteger();
    indexHealth[QStringLiteral("bsignoreLastLoadedAt")] =
        bsignoreStatus.value(QStringLiteral("lastLoadedAt")).toString();
    const QJsonObject queryStats = queryStatsSnapshot();
    indexHealth[QStringLiteral("searchCount")] = queryStats.value(QStringLiteral("searchCount")).toInteger();
    indexHealth[QStringLiteral("rewriteAppliedCount")] =
        queryStats.value(QStringLiteral("rewriteAppliedCount")).toInteger();
    indexHealth[QStringLiteral("semanticOnlyAdmittedCount")] =
        queryStats.value(QStringLiteral("semanticOnlyAdmittedCount")).toInteger();
    indexHealth[QStringLiteral("semanticOnlySuppressedCount")] =
        queryStats.value(QStringLiteral("semanticOnlySuppressedCount")).toInteger();

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
