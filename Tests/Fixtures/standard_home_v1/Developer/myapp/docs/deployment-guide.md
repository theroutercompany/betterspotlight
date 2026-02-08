# Deployment Guide

## Build
Run docker build -t betterspotlight/service:latest .
Build artifacts include the indexer and query service binaries.

## Push
Push image to the internal registry after CI checks pass.

## Deploy to Kubernetes
kubectl apply -f k8s/namespace.yaml
kubectl apply -f k8s/deployment.yaml
kubectl apply -f k8s/service.yaml

## CI/CD pipeline
The CI/CD pipeline runs unit tests, relevance tests, and smoke checks.
Deployment only proceeds when all required checks are green.

## Verification
- Confirm pods are healthy
- Confirm relevance endpoint returns expected JSON
- Confirm index lag remains below SLA
