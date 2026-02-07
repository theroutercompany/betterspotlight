<?php
declare(strict_types=1);

namespace BetterSpotlight\Extraction;

/**
 * Sample PHP fixture for text extraction testing.
 * Demonstrates modern PHP: typed properties, enums, match expressions.
 */

enum FileType: string {
    case Text = 'text';
    case Pdf = 'pdf';
    case Image = 'image';
    case Code = 'code';
    case Unknown = 'unknown';
}

class TextExtractor
{
    private const MAX_FILE_SIZE = 10 * 1024 * 1024; // 10 MB
    private const SUPPORTED_EXTENSIONS = ['txt', 'md', 'rst', 'csv', 'log'];

    public function __construct(
        private readonly string $basePath,
        private int $timeout = 30,
    ) {}

    public function extract(string $filePath): ?string
    {
        $fullPath = $this->basePath . DIRECTORY_SEPARATOR . $filePath;

        if (!file_exists($fullPath)) {
            throw new \RuntimeException("File not found: {$fullPath}");
        }

        if (filesize($fullPath) > self::MAX_FILE_SIZE) {
            return null;
        }

        $ext = pathinfo($fullPath, PATHINFO_EXTENSION);
        $type = $this->classifyExtension($ext);

        return match ($type) {
            FileType::Text, FileType::Code => file_get_contents($fullPath),
            FileType::Pdf => $this->extractPdf($fullPath),
            default => null,
        };
    }

    private function classifyExtension(string $ext): FileType
    {
        return match (true) {
            in_array($ext, self::SUPPORTED_EXTENSIONS) => FileType::Text,
            in_array($ext, ['py', 'js', 'ts', 'rb', 'php']) => FileType::Code,
            $ext === 'pdf' => FileType::Pdf,
            in_array($ext, ['png', 'jpg', 'gif']) => FileType::Image,
            default => FileType::Unknown,
        };
    }

    private function extractPdf(string $path): ?string
    {
        // Placeholder for PDF extraction
        return null;
    }
}
