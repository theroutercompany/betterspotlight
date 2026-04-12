#pragma once

#include "service_process_harness.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QString>
#include <QStringList>

#include <vector>

namespace bs::test {

struct RelevanceCase {
    QString id;
    QString category;
    QString query;
    QString mode = QStringLiteral("auto");
    QString expectedFileName;
    int topN = 3;
    bool semanticRequired = false;
    bool requiresVectors = false;
    QString notes;
};

struct CorpusDocumentSpec {
    QString relativePath;
    QString content;
};

QString resolveJsonFixturePath(const QString& envVar,
                               const QString& compiledPath,
                               const QString& relativeFallback);
QString resolveModelsDirForTests();
std::vector<RelevanceCase> parseRelevanceCases(const QJsonArray& caseArray);
QString syntheticContentForFile(const QString& sourcePath);
bool envFlagEnabled(const QString& raw);
bool containsExpectedFileInTopN(const QJsonArray& ranked,
                                const QString& expectedFileName,
                                int topN,
                                QStringList* inspectedNames);

class HermeticQueryFixture {
public:
    HermeticQueryFixture();
    ~HermeticQueryFixture() = default;

    HermeticQueryFixture(const HermeticQueryFixture&) = delete;
    HermeticQueryFixture& operator=(const HermeticQueryFixture&) = delete;

    bool isValid() const;
    QString homeDir() const;
    QString dataDir() const;
    QString dbPath() const;
    QString documentsRoot() const;
    QString modelsDir() const;

    bool seedFixtureTreeUnderDocuments(const QString& fixtureRoot, QString* errorOut = nullptr);
    bool seedGeneratedCorpus(const std::vector<CorpusDocumentSpec>& documents,
                             QString* errorOut = nullptr);
    bool startQueryService(const QHash<QString, QString>& extraEnv = {},
                           QString* errorOut = nullptr);
    bool ensureSemanticReady(QString* errorOut = nullptr, int timeoutMs = 120000);
    QJsonObject request(const QString& method,
                        const QJsonObject& params = {},
                        int timeoutMs = -1);

private:
    bool upsertDocument(const QString& absolutePath,
                        const QString& content,
                        QString* errorOut);
    bool rebuildVectors(QString* errorOut, int timeoutMs);

    QTemporaryDir m_tempHome;
    QString m_dataDir;
    QString m_dbPath;
    QString m_documentsRoot;
    QString m_modelsDir;
    ServiceProcessHarness m_queryHarness;
    bool m_queryStarted = false;
};

} // namespace bs::test
