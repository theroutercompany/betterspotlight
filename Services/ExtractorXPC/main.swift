import Foundation

/// XPC Service entry point for the Extractor service
let delegate = ExtractorServiceDelegate()
let listener = NSXPCListener.service()
listener.delegate = delegate
listener.resume()

// Keep the service running
RunLoop.current.run()
