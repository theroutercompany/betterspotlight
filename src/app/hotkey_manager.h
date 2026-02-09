#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <Carbon/Carbon.h>

namespace bs {

class HotkeyManager : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString hotkey READ hotkey WRITE setHotkey NOTIFY hotkeyChanged)
    Q_PROPERTY(bool hotkeyHealthy READ hotkeyHealthy NOTIFY hotkeyStatusChanged)
    Q_PROPERTY(QString registrationError READ registrationError NOTIFY hotkeyStatusChanged)
    Q_PROPERTY(QStringList suggestedAlternatives READ suggestedAlternatives NOTIFY hotkeyStatusChanged)

public:
    explicit HotkeyManager(QObject* parent = nullptr);
    ~HotkeyManager() override;

    QString hotkey() const;
    void setHotkey(const QString& hotkey);
    bool hotkeyHealthy() const;
    QString registrationError() const;
    QStringList suggestedAlternatives() const;

    // Register/unregister the global hotkey with the system
    bool registerHotkey();
    void unregisterHotkey();
    Q_INVOKABLE bool applyHotkey(const QString& hotkey);

signals:
    void hotkeyTriggered();
    void hotkeyChanged();
    void hotkeyStatusChanged();
    void hotkeyConflictDetected(const QString& attemptedHotkey,
                                const QString& error,
                                const QStringList& suggestions);

private:
    // Parse a hotkey string like "Cmd+Space" into Carbon modifier+keyCode
    static bool parseHotkeyString(const QString& str, UInt32& outModifiers, UInt32& outKeyCode);

    // Carbon event handler callback
    static OSStatus carbonEventHandler(EventHandlerCallRef nextHandler,
                                       EventRef event,
                                       void* userData);
    static QString statusToMessage(OSStatus status);
    static QStringList fallbackSuggestions();
    void setRegistrationState(bool healthy, const QString& error, const QStringList& suggestions = {});

    QString m_hotkeyString;
    EventHotKeyRef m_hotKeyRef = nullptr;
    EventHandlerRef m_eventHandlerRef = nullptr;
    bool m_registered = false;
    bool m_hotkeyHealthy = true;
    QString m_registrationError;
    QStringList m_suggestedAlternatives;
};

} // namespace bs
