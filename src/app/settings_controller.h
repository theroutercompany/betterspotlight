#pragma once

#include <QObject>
#include <QJsonObject>
#include <QVariantList>
#include <QString>
#include <QStringList>

namespace bs {

class SettingsController : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString hotkey READ hotkey WRITE setHotkey NOTIFY hotkeyChanged)
    Q_PROPERTY(bool launchAtLogin READ launchAtLogin WRITE setLaunchAtLogin NOTIFY launchAtLoginChanged)
    Q_PROPERTY(bool showInDock READ showInDock WRITE setShowInDock NOTIFY showInDockChanged)
    Q_PROPERTY(bool checkForUpdates READ checkForUpdates WRITE setCheckForUpdates NOTIFY checkForUpdatesChanged)
    Q_PROPERTY(int maxResults READ maxResults WRITE setMaxResults NOTIFY maxResultsChanged)
    Q_PROPERTY(QVariantList indexRoots READ indexRoots WRITE setIndexRoots NOTIFY indexRootsChanged)
    Q_PROPERTY(bool enablePdf READ enablePdf WRITE setEnablePdf NOTIFY enablePdfChanged)
    Q_PROPERTY(bool enableOcr READ enableOcr WRITE setEnableOcr NOTIFY enableOcrChanged)
    Q_PROPERTY(bool embeddingEnabled READ embeddingEnabled WRITE setEmbeddingEnabled NOTIFY embeddingEnabledChanged)
    Q_PROPERTY(int maxFileSizeMB READ maxFileSizeMB WRITE setMaxFileSizeMB NOTIFY maxFileSizeMBChanged)
    Q_PROPERTY(QStringList userPatterns READ userPatterns WRITE setUserPatterns NOTIFY userPatternsChanged)
    Q_PROPERTY(bool enableFeedbackLogging READ enableFeedbackLogging WRITE setEnableFeedbackLogging NOTIFY enableFeedbackLoggingChanged)
    Q_PROPERTY(bool enableInteractionTracking READ enableInteractionTracking WRITE setEnableInteractionTracking NOTIFY enableInteractionTrackingChanged)
    Q_PROPERTY(int feedbackRetentionDays READ feedbackRetentionDays WRITE setFeedbackRetentionDays NOTIFY feedbackRetentionDaysChanged)
    Q_PROPERTY(QStringList sensitivePaths READ sensitivePaths WRITE setSensitivePaths NOTIFY sensitivePathsChanged)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)

public:
    explicit SettingsController(QObject* parent = nullptr);

    // Getters
    QString hotkey() const;
    bool launchAtLogin() const;
    bool showInDock() const;
    bool checkForUpdates() const;
    int maxResults() const;
    QVariantList indexRoots() const;
    bool enablePdf() const;
    bool enableOcr() const;
    bool embeddingEnabled() const;
    int maxFileSizeMB() const;
    QStringList userPatterns() const;
    bool enableFeedbackLogging() const;
    bool enableInteractionTracking() const;
    int feedbackRetentionDays() const;
    QStringList sensitivePaths() const;
    QString theme() const;
    QString language() const;

    // Setters
    void setHotkey(const QString& hotkey);
    void setLaunchAtLogin(bool enabled);
    void setShowInDock(bool enabled);
    void setCheckForUpdates(bool enabled);
    void setMaxResults(int max);
    void setIndexRoots(const QVariantList& roots);
    void setEnablePdf(bool enabled);
    void setEnableOcr(bool enabled);
    void setEmbeddingEnabled(bool enabled);
    void setMaxFileSizeMB(int mb);
    void setUserPatterns(const QStringList& patterns);
    void setEnableFeedbackLogging(bool enabled);
    void setEnableInteractionTracking(bool enabled);
    void setFeedbackRetentionDays(int days);
    void setSensitivePaths(const QStringList& paths);
    void setTheme(const QString& theme);
    void setLanguage(const QString& language);

    Q_INVOKABLE void clearFeedbackData();
    Q_INVOKABLE void exportData();
    Q_INVOKABLE void pauseIndexing();
    Q_INVOKABLE void resumeIndexing();
    Q_INVOKABLE void rebuildIndex();
    Q_INVOKABLE void rebuildVectorIndex();
    Q_INVOKABLE void clearExtractionCache();
    Q_INVOKABLE void reindexFolder(const QString& folderPath);

signals:
    void hotkeyChanged();
    void launchAtLoginChanged();
    void showInDockChanged();
    void checkForUpdatesChanged();
    void maxResultsChanged();
    void indexRootsChanged();
    void enablePdfChanged();
    void enableOcrChanged();
    void embeddingEnabledChanged();
    void maxFileSizeMBChanged();
    void userPatternsChanged();
    void enableFeedbackLoggingChanged();
    void enableInteractionTrackingChanged();
    void feedbackRetentionDaysChanged();
    void sensitivePathsChanged();
    void themeChanged();
    void languageChanged();
    void settingsChanged(const QString& key);
    void feedbackDataCleared();
    void indexingPaused();
    void indexingResumed();
    void rebuildIndexRequested();
    void rebuildVectorIndexRequested();
    void clearExtractionCacheRequested();
    void reindexFolderRequested(const QString& folderPath);

private:
    void loadSettings();
    void saveSettings();
    QString settingsFilePath() const;

    QJsonObject m_settings;
};

} // namespace bs
