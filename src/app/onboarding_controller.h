#pragma once

#include <QObject>
#include <QVariantList>
#include <QString>

namespace bs {

class OnboardingController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool needsOnboarding READ needsOnboarding NOTIFY needsOnboardingChanged)
    Q_PROPERTY(bool fdaGranted READ fdaGranted NOTIFY fdaGrantedChanged)
    Q_PROPERTY(QString fdaStatusMessage READ fdaStatusMessage NOTIFY fdaStatusMessageChanged)
    Q_PROPERTY(QVariantList homeDirectories READ homeDirectories NOTIFY homeDirectoriesChanged)

public:
    explicit OnboardingController(QObject* parent = nullptr);

    bool needsOnboarding() const;
    bool fdaGranted() const;
    QString fdaStatusMessage() const;
    QVariantList homeDirectories() const;

    Q_INVOKABLE bool refreshFda();
    Q_INVOKABLE bool checkFda();
    Q_INVOKABLE void openFdaSystemSettings();
    Q_INVOKABLE void saveHomeMap(const QVariantList& directories);
    Q_INVOKABLE void completeOnboarding();

signals:
    void needsOnboardingChanged();
    void fdaGrantedChanged();
    void fdaStatusMessageChanged();
    void homeDirectoriesChanged();
    void onboardingCompleted();

private:
    bool updateFdaState();
    void setFdaStatusMessage(const QString& message);
    void scanHomeDirectories();
    QString suggestMode(const QString& dirName) const;

    bool m_needsOnboarding = true;
    bool m_fdaGranted = false;
    QString m_fdaStatusMessage;
    QVariantList m_homeDirectories;
};

} // namespace bs
