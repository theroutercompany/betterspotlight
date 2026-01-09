import Foundation
import Shared
import Carbon
import AppKit

/// Manages global hotkey registration and handling
public final class HotkeyManager {
    private var hotkeyRef: EventHotKeyRef?
    private var eventHandler: EventHandlerRef?

    /// Callback when hotkey is pressed
    public var onHotkey: (() -> Void)?

    public init() {}

    deinit {
        unregisterHotkey()
    }

    /// Register a global hotkey
    /// - Parameters:
    ///   - keyCode: The key code (e.g., kVK_Space = 49)
    ///   - modifiers: Modifier flags (e.g., cmdKey = 256)
    public func registerHotkey(keyCode: UInt16, modifiers: UInt32) {
        // Unregister existing hotkey first
        unregisterHotkey()

        // Convert to Carbon modifier flags
        var carbonModifiers: UInt32 = 0

        if modifiers & UInt32(NSEvent.ModifierFlags.command.rawValue) != 0 {
            carbonModifiers |= UInt32(cmdKey)
        }
        if modifiers & UInt32(NSEvent.ModifierFlags.option.rawValue) != 0 {
            carbonModifiers |= UInt32(optionKey)
        }
        if modifiers & UInt32(NSEvent.ModifierFlags.control.rawValue) != 0 {
            carbonModifiers |= UInt32(controlKey)
        }
        if modifiers & UInt32(NSEvent.ModifierFlags.shift.rawValue) != 0 {
            carbonModifiers |= UInt32(shiftKey)
        }

        // Create hotkey ID
        var hotkeyID = EventHotKeyID(signature: OSType("BSPT".fourCharCode), id: 1)

        // Install event handler
        var eventSpec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))

        let handlerCallback: EventHandlerUPP = { (_, event, userData) -> OSStatus in
            guard let userData = userData else { return noErr }
            let manager = Unmanaged<HotkeyManager>.fromOpaque(userData).takeUnretainedValue()

            // Verify it's our hotkey
            var hotkeyID = EventHotKeyID()
            GetEventParameter(
                event,
                EventParamName(kEventParamDirectObject),
                EventParamType(typeEventHotKeyID),
                nil,
                MemoryLayout<EventHotKeyID>.size,
                nil,
                &hotkeyID
            )

            if hotkeyID.signature == OSType("BSPT".fourCharCode) {
                DispatchQueue.main.async {
                    manager.onHotkey?()
                }
            }

            return noErr
        }

        let userData = Unmanaged.passUnretained(self).toOpaque()

        InstallEventHandler(
            GetApplicationEventTarget(),
            handlerCallback,
            1,
            &eventSpec,
            userData,
            &eventHandler
        )

        // Register the hotkey
        let status = RegisterEventHotKey(
            UInt32(keyCode),
            carbonModifiers,
            hotkeyID,
            GetApplicationEventTarget(),
            0,
            &hotkeyRef
        )

        if status != noErr {
            print("Failed to register hotkey: \(status)")
        }
    }

    /// Unregister the current hotkey
    public func unregisterHotkey() {
        if let hotkeyRef = hotkeyRef {
            UnregisterEventHotKey(hotkeyRef)
            self.hotkeyRef = nil
        }

        if let eventHandler = eventHandler {
            RemoveEventHandler(eventHandler)
            self.eventHandler = nil
        }
    }

    /// Check if a hotkey combination is available
    public func isHotkeyAvailable(keyCode: UInt16, modifiers: UInt32) -> Bool {
        // Would need to try registering and immediately unregistering
        // For now, assume available
        return true
    }
}

// MARK: - String Extension for FourCharCode

private extension String {
    var fourCharCode: FourCharCode {
        guard count == 4 else { return 0 }
        var result: FourCharCode = 0
        for char in utf8 {
            result = result << 8 + FourCharCode(char)
        }
        return result
    }
}

// MARK: - Common Key Codes

public enum KeyCode: UInt16 {
    case space = 49
    case returnKey = 36
    case tab = 48
    case escape = 53
    case delete = 51
    case forwardDelete = 117

    case a = 0
    case b = 11
    case c = 8
    case d = 2
    case e = 14
    case f = 3
    case g = 5
    case h = 4
    case i = 34
    case j = 38
    case k = 40
    case l = 37
    case m = 46
    case n = 45
    case o = 31
    case p = 35
    case q = 12
    case r = 15
    case s = 1
    case t = 17
    case u = 32
    case v = 9
    case w = 13
    case x = 7
    case y = 16
    case z = 6
}
