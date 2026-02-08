#pragma once

#include "core/ipc/supervisor.h"
#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <memory>

namespace bs {

class ServiceManager : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool isReady READ isReady NOTIFY serviceStatusChanged)
    Q_PROPERTY(QString indexerStatus READ indexerStatus NOTIFY serviceStatusChanged)
    Q_PROPERTY(QString extractorStatus READ extractorStatus NOTIFY serviceStatusChanged)
    Q_PROPERTY(QString queryStatus READ queryStatus NOTIFY serviceStatusChanged)

public:
    explicit ServiceManager(QObject* parent = nullptr);
    ~ServiceManager() override;

    bool isReady() const;
    QString indexerStatus() const;
    QString extractorStatus() const;
    QString queryStatus() const;

    // Access to the underlying supervisor (e.g., for SearchController to get clients)
    Supervisor* supervisor() const;

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void pauseIndexing();
    Q_INVOKABLE void resumeIndexing();
    Q_INVOKABLE void setIndexingUserActive(bool active);
    Q_INVOKABLE void rebuildAll();
    Q_INVOKABLE void rebuildVectorIndex();
    Q_INVOKABLE void clearExtractionCache();
    Q_INVOKABLE void reindexPath(const QString& path);

signals:
    void serviceStatusChanged();
    void allServicesReady();
    void serviceError(const QString& serviceName, const QString& error);

private slots:
    void onServiceStarted(const QString& name);
    void onServiceStopped(const QString& name);
    void onServiceCrashed(const QString& name, int crashCount);
    void onAllServicesReady();

private:
    QString findServiceBinary(const QString& name) const;
    void updateServiceStatus(const QString& name, const QString& status);
    void startIndexing();
    bool sendServiceRequest(const QString& serviceName,
                            const QString& method,
                            const QJsonObject& params = {});
    bool sendIndexerRequest(const QString& method, const QJsonObject& params = {});
    QJsonArray loadIndexRoots() const;
    QJsonArray loadEmbeddingRoots() const;

    std::unique_ptr<Supervisor> m_supervisor;

    QString m_indexerStatus;
    QString m_extractorStatus;
    QString m_queryStatus;
    bool m_allReady = false;
};

} // namespace bs
