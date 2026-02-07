# Sample Makefile fixture for text extraction testing.
# Build system for BetterSpotlight development tasks.

.PHONY: all build test clean lint format docs release install

SWIFT := swift
XCODEBUILD := xcodebuild
SCHEME := BetterSpotlight
CONFIGURATION := release
BUILD_DIR := .build
DOCS_DIR := docs

# Version from git tag
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")

all: build test

build:
	$(SWIFT) build -c $(CONFIGURATION)

build-debug:
	$(SWIFT) build -c debug

test:
	$(SWIFT) test

test-unit:
	$(SWIFT) test --filter CoreTests

test-integration:
	$(SWIFT) test --filter IntegrationTests

test-scoring:
	$(SWIFT) test --filter ScoringTests

clean:
	$(SWIFT) package clean
	rm -rf $(BUILD_DIR)
	rm -rf .swiftpm

lint:
	swiftlint lint --strict Sources/ Tests/

format:
	swiftformat Sources/ Tests/ --config .swiftformat

docs:
	swift package generate-documentation \
		--target Core \
		--output-path $(DOCS_DIR)/api

release: clean build test
	@echo "Building release $(VERSION)"
	$(XCODEBUILD) -scheme $(SCHEME) \
		-configuration Release \
		-archivePath $(BUILD_DIR)/$(SCHEME).xcarchive \
		archive
	@echo "Release $(VERSION) built successfully"

install: build
	cp $(BUILD_DIR)/$(CONFIGURATION)/BetterSpotlight /usr/local/bin/

# Database management
db-reset:
	rm -f ~/Library/Application\ Support/BetterSpotlight/index.db*
	@echo "Database reset complete"

db-vacuum:
	sqlite3 ~/Library/Application\ Support/BetterSpotlight/index.db "VACUUM;"

# Development helpers
run-debug: build-debug
	$(BUILD_DIR)/debug/BetterSpotlight --verbose

count-lines:
	@find Sources Tests -name '*.swift' | xargs wc -l | tail -1

help:
	@echo "BetterSpotlight Build System"
	@echo ""
	@echo "Targets:"
	@echo "  build            Build release binary"
	@echo "  test             Run all tests"
	@echo "  test-unit        Run unit tests only"
	@echo "  clean            Remove build artifacts"
	@echo "  lint             Run SwiftLint"
	@echo "  format           Run SwiftFormat"
	@echo "  release          Build release archive"
	@echo "  install          Install to /usr/local/bin"
