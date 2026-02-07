#pragma once

#include <QObject>
#include <QString>

#include <Carbon/Carbon.h>

namespace bs {

class HotkeyManager : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString hotkey READ hotkey WRITE setHotkey NOTIFY hotkeyChanged)

public:
    explicit HotkeyManager(QObject* parent = nullptr);
    ~HotkeyManager() override;

    QString hotkey() const;
    void setHotkey(const QString& hotkey);

    // Register/unregister the global hotkey with the system
    bool registerHotkey();
    void unregisterHotkey();

signals:
    void hotkeyTriggered();
    void hotkeyChanged();

private:
    // Parse a hotkey string like "Cmd+Space" into Carbon modifier+keyCode
    static bool parseHotkeyString(const QString& str, UInt32& outModifiers, UInt32& outKeyCode);

    // Carbon event handler callback
    static OSStatus carbonEventHandler(EventHandlerCallRef nextHandler,
                                       EventRef event,
                                       void* userData);

    QString m_hotkeyString;
    EventHotKeyRef m_hotKeyRef = nullptr;
    EventHandlerRef m_eventHandlerRef = nullptr;
    bool m_registered = false;
};

} // namespace bs
