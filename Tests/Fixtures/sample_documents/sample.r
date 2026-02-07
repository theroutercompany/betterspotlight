# Sample R fixture for text extraction testing.
# Demonstrates data frames, functions, and tidyverse-style operations.

library(dplyr)
library(ggplot2)

# Simulated search result data
search_results <- data.frame(
  path = c("/docs/readme.md", "/src/main.py", "/tests/test_search.py",
           "/config/settings.yaml", "/data/results.csv"),
  match_type = c("exact_name", "prefix_name", "substring_name",
                 "content_exact", "content_fuzzy"),
  boost = c(200, 150, 100, 50, 25),
  recency_days = c(1, 3, 7, 30, 90),
  frequency = c(50, 30, 20, 5, 2),
  stringsAsFactors = FALSE
)

# Scoring function matching BetterSpotlight's ranking formula
compute_score <- function(boost, recency_days, frequency,
                          match_weight = 1.0,
                          recency_weight = 0.3,
                          frequency_weight = 0.2) {
  recency_decay <- exp(-recency_days / 30)
  freq_boost <- log1p(frequency)

  score <- (boost * match_weight) +
           (recency_decay * 100 * recency_weight) +
           (freq_boost * 10 * frequency_weight)

  return(round(score, 2))
}

# Apply scoring
results <- search_results %>%
  mutate(
    score = compute_score(boost, recency_days, frequency)
  ) %>%
  arrange(desc(score))

print("Ranked search results:")
print(results)

# Summary statistics
cat("\nScoring summary:\n")
cat(sprintf("  Mean score: %.2f\n", mean(results$score)))
cat(sprintf("  Max score:  %.2f\n", max(results$score)))
cat(sprintf("  Min score:  %.2f\n", min(results$score)))
