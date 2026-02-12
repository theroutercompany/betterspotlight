#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QVariantList>

#include <memory>
#include <mutex>
#include <thread>

namespace bs {

class ControlPlaneActor;
class HealthAggregatorActor;
class Supervisor;

class ServiceManager : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool isReady READ isReady NOTIFY serviceStatusChanged)
    Q_PROPERTY(QString indexerStatus READ indexerStatus NOTIFY serviceStatusChanged)
    Q_PROPERTY(QString extractorStatus READ extractorStatus NOTIFY serviceStatusChanged)
    Q_PROPERTY(QString queryStatus READ queryStatus NOTIFY serviceStatusChanged)
    Q_PROPERTY(QString inferenceStatus READ inferenceStatus NOTIFY serviceStatusChanged)
    Q_PROPERTY(QString trayState READ trayState NOTIFY trayStateChanged)
    Q_PROPERTY(bool modelDownloadRunning READ modelDownloadRunning NOTIFY modelDownloadStateChanged)
    Q_PROPERTY(QString modelDownloadStatus READ modelDownloadStatus NOTIFY modelDownloadStateChanged)
    Q_PROPERTY(bool modelDownloadHasError READ modelDownloadHasError NOTIFY modelDownloadStateChanged)
    Q_PROPERTY(QVariantMap healthSnapshot READ healthSnapshot NOTIFY healthSnapshotChanged)

public:
    enum class TrayState {
        Idle,
        Indexing,
        Error,
    };
    Q_ENUM(TrayState)

    explicit ServiceManager(QObject* parent = nullptr);
    ~ServiceManager() override;

    bool isReady() const;
    QString indexerStatus() const;
    QString extractorStatus() const;
    QString queryStatus() const;
    QString inferenceStatus() const;
    QString trayState() const;
    bool modelDownloadRunning() const;
    QString modelDownloadStatus() const;
    bool modelDownloadHasError() const;
    QVariantMap healthSnapshot() const;

    // Legacy accessor retained for compatibility.
    Supervisor* supervisor() const;

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE bool pauseIndexing();
    Q_INVOKABLE bool resumeIndexing();
    Q_INVOKABLE void setIndexingUserActive(bool active);
    Q_INVOKABLE bool rebuildAll();
    Q_INVOKABLE bool rebuildVectorIndex();
    Q_INVOKABLE bool clearExtractionCache();
    Q_INVOKABLE bool reindexPath(const QString& path);
    Q_INVOKABLE bool downloadModels(const QStringList& roles, bool includeExisting = false);
    Q_INVOKABLE QVariantList serviceDiagnostics() const;
    Q_INVOKABLE void triggerInitialIndexing();
    Q_INVOKABLE QVariantMap latestHealthSnapshot() const;
    Q_INVOKABLE void requestHealthRefresh();

signals:
    void serviceStatusChanged();
    void allServicesReady();
    void serviceError(const QString& serviceName, const QString& error);
    void trayStateChanged();
    void modelDownloadStateChanged();
    void healthSnapshotChanged();
    void healthSnapshotUpdated(const QJsonObject& snapshot);

private slots:
    void onServiceStarted(const QString& name);
    void onServiceStopped(const QString& name);
    void onServiceCrashed(const QString& name, int crashCount);
    void onAllServicesReady();
    void onControlPlaneServicesUpdated(const QJsonArray& services);
    void onHealthSnapshotUpdated(const QJsonObject& snapshot);

private:
    struct ServiceRequestResult {
        bool ok = false;
        QString error;
        QJsonObject response;
    };

    static bool envFlagEnabled(const char* key, bool fallback = false);

    QString findServiceBinary(const QString& name) const;
    void updateServiceStatus(const QString& name, const QString& status);
    void updateTrayState();
    void refreshIndexerQueueStatus();
    void runModelDownloadWorker(const QStringList& roles, bool includeExisting);
    void joinModelDownloadThreadIfNeeded();
    void setModelDownloadState(bool running,
                               const QString& status,
                               bool hasError);
    static QString trayStateToString(TrayState state);
    void startIndexing();
    bool sendServiceRequest(const QString& serviceName,
                            const QString& method,
                            const QJsonObject& params = {});
    ServiceRequestResult sendServiceRequestSync(const QString& serviceName,
                                                const QString& method,
                                                const QJsonObject& params = {},
                                                int timeoutMs = 10000);
    bool sendIndexerRequest(const QString& method, const QJsonObject& params = {});
    QJsonArray loadIndexRoots() const;
    QJsonArray loadEmbeddingRoots() const;

    void startControlPlaneThread();
    void stopControlPlaneThread();
    void startHealthThread();
    void stopHealthThread();

    QThread m_controlPlaneThread;
    QThread m_healthThread;
    ControlPlaneActor* m_controlPlaneActor = nullptr;
    HealthAggregatorActor* m_healthAggregatorActor = nullptr;

    QJsonArray m_cachedServiceSnapshot;
    QJsonObject m_latestHealthSnapshot;
    bool m_controlPlaneModeLegacy = false;
    bool m_healthModeLegacy = false;

    QString m_indexerStatus;
    QString m_extractorStatus;
    QString m_queryStatus;
    QString m_inferenceStatus;
    bool m_allReady = false;
    bool m_initialIndexingStarted = false;
    bool m_indexingActive = false;
    TrayState m_trayState = TrayState::Indexing;
    QTimer m_indexingStatusTimer;
    bool m_lastQueueRebuildRunning = false;
    qint64 m_lastQueueRebuildFinishedAtMs = 0;
    bool m_pendingPostRebuildVectorRefresh = false;
    int m_pendingPostRebuildVectorRefreshAttempts = 0;
    bool m_started = false;
    bool m_stopping = false;
    std::thread m_modelDownloadThread;
    mutable std::mutex m_modelDownloadMutex;
    bool m_modelDownloadRunning = false;
    QString m_modelDownloadStatus;
    bool m_modelDownloadHasError = false;
};

} // namespace bs
