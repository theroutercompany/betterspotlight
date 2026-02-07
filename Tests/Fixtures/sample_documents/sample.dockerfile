# Sample Dockerfile fixture for text extraction testing.
# Multi-stage build for a hypothetical BetterSpotlight indexer service.

FROM swift:5.9-jammy AS builder

WORKDIR /app

# Install system dependencies
RUN apt-get update && apt-get install -y \
    libsqlite3-dev \
    libpoppler-cpp-dev \
    libtesseract-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy package manifest first for dependency caching
COPY Package.swift Package.resolved ./
RUN swift package resolve

# Copy source and build
COPY Sources/ Sources/
COPY Tests/ Tests/
RUN swift build -c release --product IndexerService

# Runtime stage
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libsqlite3-0 \
    libpoppler-cpp0v5 \
    libtesseract4 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --create-home --shell /bin/bash indexer

WORKDIR /home/indexer

COPY --from=builder /app/.build/release/IndexerService /usr/local/bin/

USER indexer

ENV BS_DATA_DIR=/home/indexer/data
ENV BS_LOG_LEVEL=info
ENV BS_MAX_FILE_SIZE_MB=10

VOLUME ["/home/indexer/data"]

HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD /usr/local/bin/IndexerService --health-check

ENTRYPOINT ["/usr/local/bin/IndexerService"]
CMD ["--serve"]
