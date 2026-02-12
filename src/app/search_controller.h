#pragma once

#include "core/ipc/socket_client.h"
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <optional>

namespace bs {

class Supervisor;

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

    // Set the supervisor to obtain the query service client
    void setSupervisor(Supervisor* supervisor);
    void setClipboardSignalsEnabled(bool enabled);

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

    // Request index health from the query service
    Q_INVOKABLE QVariantMap getHealthSync();

signals:
    void queryChanged();
    void resultsChanged();
    void resultRowsChanged();
    void isSearchingChanged();
    void selectedIndexChanged();

private slots:
    void executeSearch();

private:
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

    QTimer m_debounceTimer;
    static constexpr int kDebounceMs = 100;
    static constexpr int kSearchTimeoutMs = 10000;
};

} // namespace bs
