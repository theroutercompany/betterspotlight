#pragma once

#include <QObject>
#include <QVariantList>
#include <QString>

namespace bs {

class OnboardingController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool needsOnboarding READ needsOnboarding NOTIFY needsOnboardingChanged)
    Q_PROPERTY(bool fdaGranted READ fdaGranted NOTIFY fdaGrantedChanged)
    Q_PROPERTY(QVariantList homeDirectories READ homeDirectories NOTIFY homeDirectoriesChanged)

public:
    explicit OnboardingController(QObject* parent = nullptr);

    bool needsOnboarding() const;
    bool fdaGranted() const;
    QVariantList homeDirectories() const;

    Q_INVOKABLE void checkFda();
    Q_INVOKABLE void saveHomeMap(const QVariantList& directories);
    Q_INVOKABLE void completeOnboarding();

signals:
    void needsOnboardingChanged();
    void fdaGrantedChanged();
    void homeDirectoriesChanged();

private:
    void scanHomeDirectories();
    QString suggestMode(const QString& dirName) const;

    bool m_needsOnboarding = true;
    bool m_fdaGranted = false;
    QVariantList m_homeDirectories;
};

} // namespace bs
