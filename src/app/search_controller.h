#pragma once

#include "core/ipc/socket_client.h"

#include <QObject>
#include <QJsonObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>

namespace bs {

class Supervisor;
class ServiceManager;

class SearchController : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(QVariantList resultRows READ resultRows NOTIFY resultRowsChanged)
    Q_PROPERTY(bool isSearching READ isSearching NOTIFY isSearchingChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex WRITE setSelectedIndex NOTIFY selectedIndexChanged)

public:
    explicit SearchController(QObject* parent = nullptr);
    ~SearchController() override;

    // Legacy wiring retained for compatibility.
    void setSupervisor(Supervisor* supervisor);
    void setServiceManager(ServiceManager* serviceManager);
    void setClipboardSignalsEnabled(bool enabled);
    void recordBehaviorEvent(const QJsonObject& event);

    QString query() const;
    void setQuery(const QString& query);

    QVariantList results() const;
    QVariantList resultRows() const;
    bool isSearching() const;

    int selectedIndex() const;
    void setSelectedIndex(int index);

    Q_INVOKABLE void openResult(int index);
    Q_INVOKABLE void revealInFinder(int index);
    Q_INVOKABLE void copyPath(int index);
    Q_INVOKABLE QVariantMap requestAnswerSnippet(int index);
    Q_INVOKABLE void clearResults();
    Q_INVOKABLE void moveSelection(int delta);

    Q_INVOKABLE QVariantMap getHealthSync();

signals:
    void queryChanged();
    void resultsChanged();
    void resultRowsChanged();
    void isSearchingChanged();
    void selectedIndexChanged();

private slots:
    void executeSearch();
    void onHealthSnapshotUpdated(const QJsonObject& snapshot);

private:
    static bool envFlagEnabled(const char* key, bool fallback = false);
    SocketClient* ensureQueryClient(int timeoutMs = 400);
    SocketClient* ensureIndexerClient(int timeoutMs = 250);

    void parseSearchResponse(const QJsonObject& response);
    void rebuildResultRows();
    int resultIndexForRow(int rowIndex) const;
    int firstSelectableRow() const;
    int nextSelectableRow(int fromIndex, int delta) const;
    QString pathForResult(int index) const;
    void handleClipboardChanged();
    void clearClipboardSignals();
    void updateClipboardSignalsFromText(const QString& text);

    Supervisor* m_supervisor = nullptr;
    ServiceManager* m_serviceManager = nullptr;
    std::unique_ptr<SocketClient> m_queryClient;
    std::unique_ptr<SocketClient> m_indexerClient;

    QString m_query;
    QVariantList m_results;
    QVariantList m_resultRows;
    bool m_isSearching = false;
    int m_selectedIndex = -1;
    QVariantMap m_lastHealthSnapshot;
    qint64 m_lastHealthSnapshotTimeMs = 0;
    bool m_clipboardSignalsEnabled = false;
    std::optional<QString> m_clipboardBasenameSignal;
    std::optional<QString> m_clipboardDirnameSignal;
    std::optional<QString> m_clipboardExtensionSignal;
    QString m_lastContextEventId;
    QString m_lastActivityDigest;

    QTimer m_debounceTimer;
    static constexpr int kDebounceMs = 100;
    static constexpr int kSearchTimeoutMs = 10000;
};

} // namespace bs
