import Foundation

/// XPC Service entry point for the Indexer service
let delegate = IndexerServiceDelegate()
let listener = NSXPCListener.service()
listener.delegate = delegate
listener.resume()

// Keep the service running
RunLoop.current.run()
