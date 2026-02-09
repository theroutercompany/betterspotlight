#!/bin/bash
# stress_1h.sh - short PR-friendly wrapper around stress_48h harness.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$SCRIPT_DIR/stress_48h.sh" "${1:-3600}"
