#include "relevance_harness.h"

#include "core/index/sqlite_store.h"
#include "core/models/model_manifest.h"
#include "core/models/model_registry.h"
#include "core/shared/chunk.h"
#include "ipc_test_utils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTest>

#include <algorithm>
#include <optional>

namespace {

QStringList splitContentIntoChunks(const QString& content)
{
    const QStringList rawChunks = content.split(
        QRegularExpression(QStringLiteral(R"(\n\s*\n+)")), Qt::SkipEmptyParts);
    QStringList chunks;
    for (const QString& rawChunk : rawChunks) {
        const QString simplified = rawChunk.simplified();
        if (!simplified.isEmpty()) {
            chunks.append(simplified);
        }
    }
    if (chunks.isEmpty() && !content.simplified().isEmpty()) {
        chunks.append(content.simplified());
    }
    return chunks;
}

bs::ItemKind classifyKind(const QString& extensionLower)
{
    if (extensionLower == QLatin1String("pdf")) {
        return bs::ItemKind::Pdf;
    }
    if (extensionLower == QLatin1String("md") || extensionLower == QLatin1String("markdown")) {
        return bs::ItemKind::Markdown;
    }
    if (extensionLower == QLatin1String("png") || extensionLower == QLatin1String("jpg")
        || extensionLower == QLatin1String("jpeg") || extensionLower == QLatin1String("webp")) {
        return bs::ItemKind::Image;
    }
    if (extensionLower == QLatin1String("mp3") || extensionLower == QLatin1String("mp4")
        || extensionLower == QLatin1String("mov")) {
        return bs::ItemKind::Binary;
    }
    if (extensionLower == QLatin1String("cpp") || extensionLower == QLatin1String("h")
        || extensionLower == QLatin1String("py") || extensionLower == QLatin1String("ts")
        || extensionLower == QLatin1String("js") || extensionLower == QLatin1String("go")
        || extensionLower == QLatin1String("rs")) {
        return bs::ItemKind::Code;
    }
    return bs::ItemKind::Text;
}

QString tokenizedName(const QString& fileName)
{
    QString out = fileName;
    out.replace(QLatin1Char('-'), QLatin1Char(' '));
    out.replace(QLatin1Char('_'), QLatin1Char(' '));
    out.replace(QLatin1Char('.'), QLatin1Char(' '));
    return out.simplified().toLower();
}

} // namespace

namespace bs::test {

QString resolveJsonFixturePath(const QString& envVar,
                               const QString& compiledPath,
                               const QString& relativeFallback)
{
    const QString fromEnv = qEnvironmentVariable(envVar.toUtf8().constData());
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv)) {
        return QDir::cleanPath(fromEnv);
    }

    if (!compiledPath.isEmpty() && QFileInfo::exists(compiledPath)) {
        return QDir::cleanPath(compiledPath);
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(QStringLiteral("../Tests/") + relativeFallback),
        QDir(appDir).filePath(QStringLiteral("../../Tests/") + relativeFallback),
        QDir(appDir).filePath(QStringLiteral("../../../Tests/") + relativeFallback),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QDir::cleanPath(candidate);
        }
    }

    return QString();
}

QString resolveModelsDirForTests()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString envPath = qEnvironmentVariable("BETTERSPOTLIGHT_MODELS_DIR");
    const QStringList candidates = {
        envPath,
        QDir(appDir).filePath(QStringLiteral("../../data/models")),
        QDir(appDir).filePath(QStringLiteral("../../../data/models")),
        QDir(appDir).filePath(QStringLiteral("../Resources/models")),
        QDir(appDir).filePath(QStringLiteral("../../app/betterspotlight.app/Contents/Resources/models")),
    };

    for (const QString& candidate : candidates) {
        if (candidate.trimmed().isEmpty()) {
            continue;
        }
        const QString manifestPath = QDir(candidate).filePath(QStringLiteral("manifest.json"));
        const auto manifest = bs::ModelManifest::loadFromFile(manifestPath);
        if (manifest.has_value() && !manifest->models.empty()) {
            return QDir::cleanPath(candidate);
        }
    }

    const QString resolved = bs::ModelRegistry::resolveModelsDir();
    const auto manifest = bs::ModelManifest::loadFromFile(
        QDir(resolved).filePath(QStringLiteral("manifest.json")));
    if (manifest.has_value() && !manifest->models.empty()) {
        return QDir::cleanPath(resolved);
    }

    return QString();
}

std::vector<RelevanceCase> parseRelevanceCases(const QJsonArray& caseArray)
{
    std::vector<RelevanceCase> cases;
    cases.reserve(static_cast<size_t>(caseArray.size()));

    for (const QJsonValue& value : caseArray) {
        const QJsonObject obj = value.toObject();
        RelevanceCase c;
        c.id = obj.value(QStringLiteral("id")).toString();
        c.category = obj.value(QStringLiteral("category")).toString();
        c.query = obj.value(QStringLiteral("query")).toString();
        c.mode = obj.value(QStringLiteral("mode")).toString(QStringLiteral("auto"));
        c.expectedFileName = obj.value(QStringLiteral("expectedFileName")).toString();
        c.topN = std::max(1, obj.value(QStringLiteral("topN")).toInt(3));
        c.semanticRequired = obj.value(QStringLiteral("semanticRequired"))
                                 .toBool(c.category == QLatin1String("semantic_probe"));
        c.requiresVectors = obj.value(QStringLiteral("requiresVectors"))
                                .toBool(c.semanticRequired);
        c.notes = obj.value(QStringLiteral("notes")).toString();
        if (!c.id.isEmpty() && !c.query.isEmpty() && !c.expectedFileName.isEmpty()) {
            cases.push_back(c);
        }
    }

    return cases;
}

QString syntheticContentForFile(const QString& sourcePath)
{
    QFileInfo info(sourcePath);
    QString content = tokenizedName(info.fileName());
    content += QStringLiteral(" ");
    content += tokenizedName(info.completeBaseName());

    QFile file(sourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return content;
    }
    const QByteArray bytes = file.read(8192);
    file.close();
    if (bytes.contains('\0')) {
        return content;
    }

    QString decoded = QString::fromUtf8(bytes.constData(), bytes.size());
    if (decoded.isEmpty() && !bytes.isEmpty()) {
        decoded = QString::fromLatin1(bytes.constData(), bytes.size());
    }
    if (!decoded.isEmpty()) {
        content += QStringLiteral("\n");
        content += decoded.simplified();
    }
    return content;
}

bool envFlagEnabled(const QString& raw)
{
    const QString normalized = raw.trimmed().toLower();
    return normalized == QLatin1String("1")
        || normalized == QLatin1String("true")
        || normalized == QLatin1String("yes")
        || normalized == QLatin1String("on");
}

bool containsExpectedFileInTopN(const QJsonArray& ranked,
                                const QString& expectedFileName,
                                int topN,
                                QStringList* inspectedNames)
{
    const int inspected = std::min<int>(topN, ranked.size());
    for (int i = 0; i < inspected; ++i) {
        const QString candidateName =
            QFileInfo(ranked.at(i).toObject().value(QStringLiteral("path")).toString()).fileName();
        inspectedNames->append(candidateName);
        if (candidateName.compare(expectedFileName, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

HermeticQueryFixture::HermeticQueryFixture()
    : m_queryHarness(QStringLiteral("query"), QStringLiteral("betterspotlight-query"))
{
    if (!m_tempHome.isValid()) {
        return;
    }

    m_dataDir =
        QDir(m_tempHome.path()).filePath(QStringLiteral("Library/Application Support/betterspotlight"));
    m_dbPath = QDir(m_dataDir).filePath(QStringLiteral("index.db"));
    m_documentsRoot = QDir(m_tempHome.path()).filePath(QStringLiteral("Documents"));
    m_modelsDir = resolveModelsDirForTests();

    QDir().mkpath(m_dataDir);
    QDir().mkpath(m_documentsRoot);
}

bool HermeticQueryFixture::isValid() const
{
    return m_tempHome.isValid()
        && !m_dataDir.isEmpty()
        && !m_dbPath.isEmpty()
        && !m_documentsRoot.isEmpty();
}

QString HermeticQueryFixture::homeDir() const
{
    return m_tempHome.path();
}

QString HermeticQueryFixture::dataDir() const
{
    return m_dataDir;
}

QString HermeticQueryFixture::dbPath() const
{
    return m_dbPath;
}

QString HermeticQueryFixture::documentsRoot() const
{
    return m_documentsRoot;
}

QString HermeticQueryFixture::modelsDir() const
{
    return m_modelsDir;
}

bool HermeticQueryFixture::seedFixtureTreeUnderDocuments(const QString& fixtureRoot,
                                                         QString* errorOut)
{
    if (errorOut) {
        *errorOut = QString();
    }

    if (!isValid()) {
        if (errorOut) {
            *errorOut = QStringLiteral("HermeticQueryFixture is not initialized");
        }
        return false;
    }

    QDirIterator it(fixtureRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString sourcePath = it.next();
        const QString relPath = QDir(fixtureRoot).relativeFilePath(sourcePath);
        const QString targetPath = QDir(m_documentsRoot).filePath(relPath);
        const QFileInfo targetInfo(targetPath);
        if (!QDir().mkpath(targetInfo.path())) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to create fixture directory: %1")
                                .arg(targetInfo.path());
            }
            return false;
        }
        QFile::remove(targetPath);
        if (!QFile::copy(sourcePath, targetPath)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to copy fixture file: %1").arg(sourcePath);
            }
            return false;
        }
        if (!upsertDocument(targetPath, syntheticContentForFile(sourcePath), errorOut)) {
            return false;
        }
    }

    return true;
}

bool HermeticQueryFixture::seedGeneratedCorpus(const std::vector<CorpusDocumentSpec>& documents,
                                               QString* errorOut)
{
    if (errorOut) {
        *errorOut = QString();
    }

    if (!isValid()) {
        if (errorOut) {
            *errorOut = QStringLiteral("HermeticQueryFixture is not initialized");
        }
        return false;
    }

    for (const CorpusDocumentSpec& doc : documents) {
        if (doc.relativePath.trimmed().isEmpty()) {
            continue;
        }
        const QString absolutePath = QDir(m_documentsRoot).filePath(doc.relativePath);
        const QFileInfo fileInfo(absolutePath);
        if (!QDir().mkpath(fileInfo.path())) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to create corpus directory: %1")
                                .arg(fileInfo.path());
            }
            return false;
        }

        QFile file(absolutePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to write corpus file: %1").arg(absolutePath);
            }
            return false;
        }
        file.write(doc.content.toUtf8());
        file.close();

        if (!upsertDocument(absolutePath, doc.content, errorOut)) {
            return false;
        }
    }

    return true;
}

bool HermeticQueryFixture::startQueryService(const QHash<QString, QString>& extraEnv,
                                             QString* errorOut)
{
    if (errorOut) {
        *errorOut = QString();
    }

    if (!isValid()) {
        if (errorOut) {
            *errorOut = QStringLiteral("HermeticQueryFixture is not initialized");
        }
        return false;
    }

    if (m_modelsDir.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("BETTERSPOTLIGHT_MODELS_DIR could not be resolved for tests");
        }
        return false;
    }

    ServiceLaunchConfig launch;
    launch.homeDir = m_tempHome.path();
    launch.dataDir = m_dataDir;
    launch.startTimeoutMs = 15000;
    launch.connectTimeoutMs = 15000;
    launch.readyTimeoutMs = 30000;
    launch.requestDefaultTimeoutMs = 8000;
    launch.env.insert(QStringLiteral("BETTERSPOTLIGHT_MODELS_DIR"), m_modelsDir);
    for (auto it = extraEnv.constBegin(); it != extraEnv.constEnd(); ++it) {
        launch.env.insert(it.key(), it.value());
    }

    if (!m_queryHarness.start(launch)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to start betterspotlight-query with hermetic test env");
        }
        return false;
    }

    m_queryStarted = true;
    return true;
}

bool HermeticQueryFixture::ensureSemanticReady(QString* errorOut, int timeoutMs)
{
    if (errorOut) {
        *errorOut = QString();
    }
    return rebuildVectors(errorOut, timeoutMs);
}

QJsonObject HermeticQueryFixture::request(const QString& method,
                                          const QJsonObject& params,
                                          int timeoutMs)
{
    return m_queryHarness.request(method, params, timeoutMs);
}

bool HermeticQueryFixture::upsertDocument(const QString& absolutePath,
                                          const QString& content,
                                          QString* errorOut)
{
    auto storeOpt = bs::SQLiteStore::open(m_dbPath);
    if (!storeOpt.has_value()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open fixture SQLite store: %1").arg(m_dbPath);
        }
        return false;
    }
    bs::SQLiteStore store = std::move(storeOpt.value());

    const QFileInfo fileInfo(absolutePath);
    const QString extension = fileInfo.suffix().toLower();
    const double now = static_cast<double>(QDateTime::currentSecsSinceEpoch());

    auto itemId = store.upsertItem(
        absolutePath,
        fileInfo.fileName(),
        extension.isEmpty() ? QString() : QStringLiteral(".") + extension,
        classifyKind(extension),
        std::max<int64_t>(1, static_cast<int64_t>(fileInfo.size())),
        now,
        now,
        QString(),
        QStringLiteral("normal"),
        fileInfo.path());
    if (!itemId.has_value()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to upsert fixture item: %1").arg(absolutePath);
        }
        return false;
    }

    const QStringList chunkTexts = splitContentIntoChunks(content);
    std::vector<bs::Chunk> chunks;
    chunks.reserve(static_cast<size_t>(chunkTexts.size()));
    int byteOffset = 0;
    for (int i = 0; i < chunkTexts.size(); ++i) {
        const QString text = chunkTexts.at(i);
        if (text.isEmpty()) {
            continue;
        }
        bs::Chunk chunk;
        chunk.chunkId = bs::computeChunkId(absolutePath, i);
        chunk.filePath = absolutePath;
        chunk.chunkIndex = i;
        chunk.content = text;
        chunk.byteOffset = byteOffset;
        byteOffset += text.toUtf8().size() + 1;
        chunks.push_back(std::move(chunk));
    }
    if (chunks.empty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("No chunkable content generated for %1").arg(absolutePath);
        }
        return false;
    }
    if (!store.insertChunks(itemId.value(), fileInfo.fileName(), absolutePath, chunks)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to insert chunks for %1").arg(absolutePath);
        }
        return false;
    }

    return true;
}

bool HermeticQueryFixture::rebuildVectors(QString* errorOut, int timeoutMs)
{
    if (errorOut) {
        *errorOut = QString();
    }

    if (!m_queryStarted) {
        if (errorOut) {
            *errorOut = QStringLiteral("Query service has not been started");
        }
        return false;
    }

    QJsonObject rebuildParams;
    QJsonArray includePaths;
    includePaths.append(m_documentsRoot);
    rebuildParams[QStringLiteral("includePaths")] = includePaths;

    const QJsonObject rebuildResponse =
        request(QStringLiteral("rebuildVectorIndex"), rebuildParams, 15000);
    if (!isResponse(rebuildResponse)) {
        if (errorOut) {
            *errorOut = QStringLiteral("rebuildVectorIndex request failed");
        }
        return false;
    }

    QElapsedTimer rebuildTimer;
    rebuildTimer.start();
    while (rebuildTimer.elapsed() < timeoutMs) {
        const QJsonObject healthResponse = request(QStringLiteral("getHealth"));
        if (!isResponse(healthResponse)) {
            QTest::qWait(150);
            continue;
        }

        const QJsonObject indexHealth = resultPayload(healthResponse)
                                            .value(QStringLiteral("indexHealth"))
                                            .toObject();
        const QString status = indexHealth.value(QStringLiteral("vectorRebuildStatus")).toString();
        if (status == QLatin1String("succeeded")) {
            return true;
        }
        if (status == QLatin1String("failed")) {
            if (errorOut) {
                *errorOut = indexHealth.value(QStringLiteral("vectorRebuildLastError"))
                                .toString(QStringLiteral("vector_rebuild_failed"));
            }
            return false;
        }
        QTest::qWait(150);
    }

    if (errorOut) {
        *errorOut = QStringLiteral("vector_rebuild_timeout");
    }
    return false;
}

} // namespace bs::test
