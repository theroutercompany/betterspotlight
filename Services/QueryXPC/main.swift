import Foundation

/// XPC Service entry point for the Query service
let delegate = QueryServiceDelegate()
let listener = NSXPCListener.service()
listener.delegate = delegate
listener.resume()

// Keep the service running
RunLoop.current.run()
