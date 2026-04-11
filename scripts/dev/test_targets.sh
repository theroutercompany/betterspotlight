#!/usr/bin/env bash

bs_dev_app_targets() {
    printf '%s\n' \
        betterspotlight \
        betterspotlight-indexer \
        betterspotlight-extractor \
        betterspotlight-query \
        betterspotlight-inference
}

bs_dev_stabilization_test_targets() {
    printf '%s\n' \
        test-pipeline \
        test-indexer \
        test-indexer-service-ipc \
        test-full-pipeline \
        test-index-backpressure \
        test-index-persistence \
        test-incremental-update \
        test-embedding \
        test-cross-encoder-reranker \
        test-qa-extractive-model \
        test-health-aggregator-actor
}

bs_dev_extended_reliability_test_targets() {
    bs_dev_stabilization_test_targets
    printf '%s\n' \
        test-supervisor \
        test-inference-service-ipc \
        test-query-service-m2-ipc \
        test-app-lifecycle-states \
        test-health-consistency-v2 \
        test-orphan-reconciliation
}

bs_dev_targets_regex() {
    local kind="${1:-stabilization}"
    local names
    case "${kind}" in
        stabilization)
            names="$(bs_dev_stabilization_test_targets | paste -sd'|' -)"
            ;;
        extended)
            names="$(bs_dev_extended_reliability_test_targets | paste -sd'|' -)"
            ;;
        *)
            return 1
            ;;
    esac
    printf '^(%s)$\n' "${names}"
}
