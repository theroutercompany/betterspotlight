#pragma once

#include <QObject>
#include <QString>

namespace bs {

class UpdateManager : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool available READ available NOTIFY statusChanged)
    Q_PROPERTY(bool automaticallyChecks READ automaticallyChecks WRITE setAutomaticallyChecks NOTIFY statusChanged)
    Q_PROPERTY(QString lastStatus READ lastStatus NOTIFY statusChanged)

public:
    explicit UpdateManager(QObject* parent = nullptr);
    ~UpdateManager() override;

    bool available() const;
    bool automaticallyChecks() const;
    QString lastStatus() const;

    Q_INVOKABLE void initialize();
    Q_INVOKABLE void checkNow();
    void setAutomaticallyChecks(bool enabled);

signals:
    void statusChanged();

private:
    void setStatus(const QString& status);

    bool m_available = false;
    bool m_automaticallyChecks = true;
    QString m_lastStatus = QStringLiteral("idle");

    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace bs
