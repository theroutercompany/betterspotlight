package com.betterspotlight.config

/**
 * Sample Kotlin fixture for text extraction testing.
 * Demonstrates data classes, sealed classes, coroutines patterns.
 */

import kotlinx.coroutines.*

sealed class ExtractionResult {
    data class Success(val text: String, val charCount: Int) : ExtractionResult()
    data class Partial(val text: String, val reason: String) : ExtractionResult()
    data class Failure(val error: String, val path: String) : ExtractionResult()
}

data class ExtractionConfig(
    val maxFileSizeMB: Int = 10,
    val timeoutSeconds: Int = 30,
    val concurrentWorkers: Int = 4,
    val supportedExtensions: Set<String> = setOf("txt", "md", "pdf", "html", "xml")
)

class ExtractionManager(private val config: ExtractionConfig) {
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    suspend fun extract(path: String): ExtractionResult {
        return withTimeout(config.timeoutSeconds * 1000L) {
            try {
                val content = readFileContent(path)
                ExtractionResult.Success(content, content.length)
            } catch (e: Exception) {
                ExtractionResult.Failure(e.message ?: "Unknown error", path)
            }
        }
    }

    suspend fun extractBatch(paths: List<String>): List<ExtractionResult> {
        return paths.map { path ->
            scope.async { extract(path) }
        }.awaitAll()
    }

    private suspend fun readFileContent(path: String): String {
        // Simulated file reading
        return "Extracted content from $path"
    }

    fun shutdown() {
        scope.cancel()
    }
}

fun main() {
    val manager = ExtractionManager(ExtractionConfig())
    runBlocking {
        val result = manager.extract("/Users/test/document.txt")
        println(result)
    }
}
