#include "hotkey_manager.h"
#include "core/shared/logging.h"

#include <Carbon/Carbon.h>

namespace bs {

HotkeyManager::HotkeyManager(QObject* parent)
    : QObject(parent)
    , m_hotkeyString(QStringLiteral("Cmd+Space"))
{
}

HotkeyManager::~HotkeyManager()
{
    unregisterHotkey();
}

QString HotkeyManager::hotkey() const
{
    return m_hotkeyString;
}

void HotkeyManager::setHotkey(const QString& hotkey)
{
    if (m_hotkeyString == hotkey) {
        return;
    }

    bool wasRegistered = m_registered;
    if (wasRegistered) {
        unregisterHotkey();
    }

    m_hotkeyString = hotkey;
    emit hotkeyChanged();

    if (wasRegistered) {
        registerHotkey();
    }
}

bool HotkeyManager::registerHotkey()
{
    if (m_registered) {
        unregisterHotkey();
    }

    UInt32 modifiers = 0;
    UInt32 keyCode = 0;

    if (!parseHotkeyString(m_hotkeyString, modifiers, keyCode)) {
        LOG_ERROR(bsCore, "HotkeyManager: failed to parse hotkey string '%s'",
                  qPrintable(m_hotkeyString));
        return false;
    }

    // Install the Carbon event handler for hotkey events
    EventTypeSpec eventSpec;
    eventSpec.eventClass = kEventClassKeyboard;
    eventSpec.eventKind = kEventHotKeyPressed;

    OSStatus status = InstallApplicationEventHandler(
        &HotkeyManager::carbonEventHandler,
        1,
        &eventSpec,
        this,
        &m_eventHandlerRef);

    if (status != noErr) {
        LOG_ERROR(bsCore, "HotkeyManager: InstallApplicationEventHandler failed (%d)",
                  static_cast<int>(status));
        return false;
    }

    // Register the actual hotkey
    EventHotKeyID hotKeyID;
    hotKeyID.signature = 'BSpt';
    hotKeyID.id = 1;

    status = RegisterEventHotKey(
        keyCode,
        modifiers,
        hotKeyID,
        GetApplicationEventTarget(),
        0,
        &m_hotKeyRef);

    if (status != noErr) {
        LOG_ERROR(bsCore, "HotkeyManager: RegisterEventHotKey failed (%d)",
                  static_cast<int>(status));
        RemoveEventHandler(m_eventHandlerRef);
        m_eventHandlerRef = nullptr;
        return false;
    }

    m_registered = true;
    LOG_INFO(bsCore, "HotkeyManager: registered global hotkey '%s'",
             qPrintable(m_hotkeyString));
    return true;
}

void HotkeyManager::unregisterHotkey()
{
    if (m_hotKeyRef) {
        UnregisterEventHotKey(m_hotKeyRef);
        m_hotKeyRef = nullptr;
    }

    if (m_eventHandlerRef) {
        RemoveEventHandler(m_eventHandlerRef);
        m_eventHandlerRef = nullptr;
    }

    m_registered = false;
    LOG_INFO(bsCore, "HotkeyManager: unregistered global hotkey");
}

bool HotkeyManager::parseHotkeyString(const QString& str, UInt32& outModifiers, UInt32& outKeyCode)
{
    outModifiers = 0;
    outKeyCode = 0;

    QStringList parts = str.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return false;
    }

    // The last part is the key; preceding parts are modifiers
    for (int i = 0; i < parts.size() - 1; ++i) {
        QString mod = parts[i].trimmed().toLower();
        if (mod == QLatin1String("cmd") || mod == QLatin1String("command")) {
            outModifiers |= cmdKey;
        } else if (mod == QLatin1String("ctrl") || mod == QLatin1String("control")) {
            outModifiers |= controlKey;
        } else if (mod == QLatin1String("alt") || mod == QLatin1String("option") || mod == QLatin1String("opt")) {
            outModifiers |= optionKey;
        } else if (mod == QLatin1String("shift")) {
            outModifiers |= shiftKey;
        } else {
            LOG_WARN(bsCore, "HotkeyManager: unknown modifier '%s'", qPrintable(mod));
            return false;
        }
    }

    // Parse the key name
    QString key = parts.last().trimmed().toLower();

    // Map common key names to Carbon virtual key codes
    static const struct {
        const char* name;
        UInt32 keyCode;
    } keyMap[] = {
        {"space",      kVK_Space},
        {"return",     kVK_Return},
        {"enter",      kVK_Return},
        {"tab",        kVK_Tab},
        {"escape",     kVK_Escape},
        {"esc",        kVK_Escape},
        {"delete",     kVK_Delete},
        {"backspace",  kVK_Delete},
        {"up",         kVK_UpArrow},
        {"down",       kVK_DownArrow},
        {"left",       kVK_LeftArrow},
        {"right",      kVK_RightArrow},
        {"f1",         kVK_F1},
        {"f2",         kVK_F2},
        {"f3",         kVK_F3},
        {"f4",         kVK_F4},
        {"f5",         kVK_F5},
        {"f6",         kVK_F6},
        {"f7",         kVK_F7},
        {"f8",         kVK_F8},
        {"f9",         kVK_F9},
        {"f10",        kVK_F10},
        {"f11",        kVK_F11},
        {"f12",        kVK_F12},
        // Letters (ANSI US keyboard layout)
        {"a", kVK_ANSI_A}, {"b", kVK_ANSI_B}, {"c", kVK_ANSI_C},
        {"d", kVK_ANSI_D}, {"e", kVK_ANSI_E}, {"f", kVK_ANSI_F},
        {"g", kVK_ANSI_G}, {"h", kVK_ANSI_H}, {"i", kVK_ANSI_I},
        {"j", kVK_ANSI_J}, {"k", kVK_ANSI_K}, {"l", kVK_ANSI_L},
        {"m", kVK_ANSI_M}, {"n", kVK_ANSI_N}, {"o", kVK_ANSI_O},
        {"p", kVK_ANSI_P}, {"q", kVK_ANSI_Q}, {"r", kVK_ANSI_R},
        {"s", kVK_ANSI_S}, {"t", kVK_ANSI_T}, {"u", kVK_ANSI_U},
        {"v", kVK_ANSI_V}, {"w", kVK_ANSI_W}, {"x", kVK_ANSI_X},
        {"y", kVK_ANSI_Y}, {"z", kVK_ANSI_Z},
        // Numbers
        {"0", kVK_ANSI_0}, {"1", kVK_ANSI_1}, {"2", kVK_ANSI_2},
        {"3", kVK_ANSI_3}, {"4", kVK_ANSI_4}, {"5", kVK_ANSI_5},
        {"6", kVK_ANSI_6}, {"7", kVK_ANSI_7}, {"8", kVK_ANSI_8},
        {"9", kVK_ANSI_9},
    };

    QByteArray keyBytes = key.toLatin1();
    for (const auto& entry : keyMap) {
        if (keyBytes == entry.name) {
            outKeyCode = entry.keyCode;
            return true;
        }
    }

    LOG_WARN(bsCore, "HotkeyManager: unknown key '%s'", qPrintable(key));
    return false;
}

OSStatus HotkeyManager::carbonEventHandler(EventHandlerCallRef /*nextHandler*/,
                                            EventRef event,
                                            void* userData)
{
    auto* self = static_cast<HotkeyManager*>(userData);

    if (GetEventClass(event) == kEventClassKeyboard &&
        GetEventKind(event) == kEventHotKeyPressed) {

        EventHotKeyID hotKeyID;
        OSStatus status = GetEventParameter(event, kEventParamDirectObject,
                                            typeEventHotKeyID, nullptr,
                                            sizeof(hotKeyID), nullptr,
                                            &hotKeyID);

        if (status == noErr && hotKeyID.signature == 'BSpt' && hotKeyID.id == 1) {
            LOG_DEBUG(bsCore, "HotkeyManager: hotkey triggered");
            // Use QMetaObject::invokeMethod for thread safety (Carbon callbacks
            // may come from the main run loop, but invokeMethod is safe regardless)
            QMetaObject::invokeMethod(self, "hotkeyTriggered", Qt::QueuedConnection);
            return noErr;
        }
    }

    return eventNotHandledErr;
}

} // namespace bs
