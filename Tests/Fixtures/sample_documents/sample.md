# BetterSpotlight Test Markdown Document

## Overview

This markdown file serves as a **test fixture** for verifying that the
BetterSpotlight indexing pipeline correctly handles markdown content,
including headers, code blocks, and inline formatting.

## Code Examples

### Python Fibonacci

```python
def fibonacci(n):
    if n <= 1:
        return n
    return fibonacci(n - 1) + fibonacci(n - 2)
```

### Rust Pattern Matching

```rust
fn classify_temperature(temp: f64) -> &'static str {
    match temp {
        t if t < 0.0 => "freezing",
        t if t < 20.0 => "cold",
        t if t < 30.0 => "comfortable",
        _ => "hot",
    }
}
```

## Key Concepts

- **Indexing**: The process of building a searchable data structure
- **Tokenization**: Breaking text into individual searchable terms
- **BM25 Ranking**: A probabilistic relevance scoring function
- **FTS5**: SQLite full-text search extension version 5

## Search Keywords

Distinctive terms for testing: ephemeral, kaleidoscope, metamorphosis,
parallax, synesthesia, tesseract, xenophobia, juxtaposition.

## Tables

| Feature       | Status    | Priority |
|---------------|-----------|----------|
| FTS5 Search   | Complete  | High     |
| PDF Extraction| Partial   | Medium   |
| OCR Support   | Planned   | Low      |
