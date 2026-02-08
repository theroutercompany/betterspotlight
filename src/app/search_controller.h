#pragma once

#include "core/ipc/socket_client.h"
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace bs {

class Supervisor;

class SearchController : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(bool isSearching READ isSearching NOTIFY isSearchingChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex WRITE setSelectedIndex NOTIFY selectedIndexChanged)

public:
    explicit SearchController(QObject* parent = nullptr);
    ~SearchController() override;

    // Set the supervisor to obtain the query service client
    void setSupervisor(Supervisor* supervisor);

    QString query() const;
    void setQuery(const QString& query);

    QVariantList results() const;
    bool isSearching() const;

    int selectedIndex() const;
    void setSelectedIndex(int index);

    Q_INVOKABLE void openResult(int index);
    Q_INVOKABLE void revealInFinder(int index);
    Q_INVOKABLE void copyPath(int index);
    Q_INVOKABLE void clearResults();

    // Request index health from the query service
    Q_INVOKABLE QVariantMap getHealthSync();

signals:
    void queryChanged();
    void resultsChanged();
    void isSearchingChanged();
    void selectedIndexChanged();

private slots:
    void executeSearch();

private:
    void parseSearchResponse(const QJsonObject& response);
    QString pathForResult(int index) const;

    Supervisor* m_supervisor = nullptr;
    QString m_query;
    QVariantList m_results;
    bool m_isSearching = false;
    int m_selectedIndex = -1;
    QVariantMap m_lastHealthSnapshot;

    QTimer m_debounceTimer;
    static constexpr int kDebounceMs = 100;
    static constexpr int kSearchTimeoutMs = 10000;
};

} // namespace bs
