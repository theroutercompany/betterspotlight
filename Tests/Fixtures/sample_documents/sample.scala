package com.betterspotlight.ranking

/**
 * Sample Scala fixture for text extraction testing.
 * Demonstrates case classes, pattern matching, and traits.
 */

sealed trait MatchType {
  def boost: Double
}

object MatchType {
  case object ExactName extends MatchType { val boost = 200.0 }
  case object PrefixName extends MatchType { val boost = 150.0 }
  case object SubstringName extends MatchType { val boost = 100.0 }
  case object ContentExact extends MatchType { val boost = 50.0 }
  case object ContentFuzzy extends MatchType { val boost = 25.0 }
}

case class ScoringWeights(
  matchWeight: Double = 1.0,
  recencyWeight: Double = 0.3,
  frequencyWeight: Double = 0.2,
  contextWeight: Double = 0.15
)

case class SearchResult(
  path: String,
  title: String,
  matchType: MatchType,
  rawScore: Double
) {
  def computeScore(weights: ScoringWeights): Double = {
    matchType.boost * weights.matchWeight + rawScore
  }
}

object RankingEngine {
  def rank(results: Seq[SearchResult], weights: ScoringWeights = ScoringWeights()): Seq[SearchResult] = {
    results.sortBy(r => -r.computeScore(weights))
  }

  def main(args: Array[String]): Unit = {
    val results = Seq(
      SearchResult("/docs/readme.md", "README", MatchType.ExactName, 0.9),
      SearchResult("/src/main.scala", "Main", MatchType.PrefixName, 0.7)
    )
    rank(results).foreach(r => println(s"${r.title}: ${r.computeScore(ScoringWeights())}"))
  }
}
