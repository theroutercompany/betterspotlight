/**
 * Sample TypeScript fixture for text extraction testing.
 * Demonstrates interfaces, generics, and async patterns.
 */

interface SearchResult {
  path: string;
  title: string;
  score: number;
  snippet?: string;
  matchType: "exact" | "prefix" | "substring" | "fuzzy";
}

interface SearchOptions {
  limit: number;
  offset: number;
  includeContent: boolean;
}

class SearchEngine<T extends SearchResult> {
  private results: Map<string, T> = new Map();

  async search(query: string, options: SearchOptions): Promise<T[]> {
    const matches = Array.from(this.results.values())
      .filter((r) => r.title.toLowerCase().includes(query.toLowerCase()))
      .sort((a, b) => b.score - a.score)
      .slice(options.offset, options.offset + options.limit);

    return matches;
  }

  addResult(result: T): void {
    this.results.set(result.path, result);
  }

  get size(): number {
    return this.results.size;
  }
}

// Usage
const engine = new SearchEngine<SearchResult>();
engine.addResult({
  path: "/docs/readme.md",
  title: "Project README",
  score: 0.92,
  matchType: "exact",
});

export { SearchEngine, SearchResult, SearchOptions };
