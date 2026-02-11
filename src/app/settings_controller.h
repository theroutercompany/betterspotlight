#pragma once

#include "platform_integration.h"

#include <QObject>
#include <QJsonObject>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <memory>

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
    Q_PROPERTY(bool queryRouterEnabled READ queryRouterEnabled WRITE setQueryRouterEnabled NOTIFY queryRouterEnabledChanged)
    Q_PROPERTY(bool fastEmbeddingEnabled READ fastEmbeddingEnabled WRITE setFastEmbeddingEnabled NOTIFY fastEmbeddingEnabledChanged)
    Q_PROPERTY(bool dualEmbeddingFusionEnabled READ dualEmbeddingFusionEnabled WRITE setDualEmbeddingFusionEnabled NOTIFY dualEmbeddingFusionEnabledChanged)
    Q_PROPERTY(bool rerankerCascadeEnabled READ rerankerCascadeEnabled WRITE setRerankerCascadeEnabled NOTIFY rerankerCascadeEnabledChanged)
    Q_PROPERTY(bool personalizedLtrEnabled READ personalizedLtrEnabled WRITE setPersonalizedLtrEnabled NOTIFY personalizedLtrEnabledChanged)
    Q_PROPERTY(int semanticBudgetMs READ semanticBudgetMs WRITE setSemanticBudgetMs NOTIFY semanticBudgetMsChanged)
    Q_PROPERTY(int rerankBudgetMs READ rerankBudgetMs WRITE setRerankBudgetMs NOTIFY rerankBudgetMsChanged)
    Q_PROPERTY(int maxFileSizeMB READ maxFileSizeMB WRITE setMaxFileSizeMB NOTIFY maxFileSizeMBChanged)
    Q_PROPERTY(QStringList userPatterns READ userPatterns WRITE setUserPatterns NOTIFY userPatternsChanged)
    Q_PROPERTY(bool enableFeedbackLogging READ enableFeedbackLogging WRITE setEnableFeedbackLogging NOTIFY enableFeedbackLoggingChanged)
    Q_PROPERTY(bool enableInteractionTracking READ enableInteractionTracking WRITE setEnableInteractionTracking NOTIFY enableInteractionTrackingChanged)
    Q_PROPERTY(bool clipboardSignalEnabled READ clipboardSignalEnabled WRITE setClipboardSignalEnabled NOTIFY clipboardSignalEnabledChanged)
    Q_PROPERTY(int feedbackRetentionDays READ feedbackRetentionDays WRITE setFeedbackRetentionDays NOTIFY feedbackRetentionDaysChanged)
    Q_PROPERTY(QStringList sensitivePaths READ sensitivePaths WRITE setSensitivePaths NOTIFY sensitivePathsChanged)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QString platformStatusMessage READ platformStatusMessage NOTIFY platformStatusChanged)
    Q_PROPERTY(QString platformStatusKey READ platformStatusKey NOTIFY platformStatusChanged)
    Q_PROPERTY(bool platformStatusSuccess READ platformStatusSuccess NOTIFY platformStatusChanged)

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
    bool queryRouterEnabled() const;
    bool fastEmbeddingEnabled() const;
    bool dualEmbeddingFusionEnabled() const;
    bool rerankerCascadeEnabled() const;
    bool personalizedLtrEnabled() const;
    int semanticBudgetMs() const;
    int rerankBudgetMs() const;
    int maxFileSizeMB() const;
    QStringList userPatterns() const;
    bool enableFeedbackLogging() const;
    bool enableInteractionTracking() const;
    bool clipboardSignalEnabled() const;
    int feedbackRetentionDays() const;
    QStringList sensitivePaths() const;
    QString theme() const;
    QString language() const;
    QString platformStatusMessage() const;
    QString platformStatusKey() const;
    bool platformStatusSuccess() const;

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
    void setQueryRouterEnabled(bool enabled);
    void setFastEmbeddingEnabled(bool enabled);
    void setDualEmbeddingFusionEnabled(bool enabled);
    void setRerankerCascadeEnabled(bool enabled);
    void setPersonalizedLtrEnabled(bool enabled);
    void setSemanticBudgetMs(int ms);
    void setRerankBudgetMs(int ms);
    void setMaxFileSizeMB(int mb);
    void setUserPatterns(const QStringList& patterns);
    void setEnableFeedbackLogging(bool enabled);
    void setEnableInteractionTracking(bool enabled);
    void setClipboardSignalEnabled(bool enabled);
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
    void queryRouterEnabledChanged();
    void fastEmbeddingEnabledChanged();
    void dualEmbeddingFusionEnabledChanged();
    void rerankerCascadeEnabledChanged();
    void personalizedLtrEnabledChanged();
    void semanticBudgetMsChanged();
    void rerankBudgetMsChanged();
    void maxFileSizeMBChanged();
    void userPatternsChanged();
    void enableFeedbackLoggingChanged();
    void enableInteractionTrackingChanged();
    void clipboardSignalEnabledChanged();
    void feedbackRetentionDaysChanged();
    void sensitivePathsChanged();
    void themeChanged();
    void languageChanged();
    void platformStatusChanged();
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
    void setPlatformStatus(const QString& key, bool success, const QString& message);

    QJsonObject m_settings;
    std::unique_ptr<PlatformIntegration> m_platformIntegration;
    QString m_platformStatusMessage;
    QString m_platformStatusKey;
    bool m_platformStatusSuccess = true;
};

} // namespace bs
