#!/usr/bin/env bash
# BetterSpotlight test fixture: Shell script
#
# Implements a deployment helper script with environment validation,
# backup creation, and rollback support.

set -euo pipefail

# Configuration
readonly APP_NAME="betterspotlight"
readonly DEPLOY_DIR="/opt/${APP_NAME}"
readonly BACKUP_DIR="/opt/${APP_NAME}/backups"
readonly LOG_FILE="/var/log/${APP_NAME}/deploy.log"
readonly MAX_BACKUPS=5

# Color output helpers
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC} $(date '+%Y-%m-%d %H:%M:%S') $*" | tee -a "$LOG_FILE"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $(date '+%Y-%m-%d %H:%M:%S') $*" | tee -a "$LOG_FILE"; }
log_error() { echo -e "${RED}[ERROR]${NC} $(date '+%Y-%m-%d %H:%M:%S') $*" | tee -a "$LOG_FILE"; }

validate_environment() {
    log_info "Validating deployment environment..."

    if [[ ! -d "$DEPLOY_DIR" ]]; then
        log_error "Deploy directory does not exist: $DEPLOY_DIR"
        return 1
    fi

    if ! command -v sqlite3 &> /dev/null; then
        log_error "sqlite3 is required but not installed"
        return 1
    fi

    local disk_space
    disk_space=$(df -P "$DEPLOY_DIR" | tail -1 | awk '{print $4}')
    if [[ "$disk_space" -lt 1048576 ]]; then
        log_warn "Less than 1GB of free disk space"
    fi

    log_info "Environment validation passed"
    return 0
}

create_backup() {
    local timestamp
    timestamp=$(date '+%Y%m%d_%H%M%S')
    local backup_path="${BACKUP_DIR}/${APP_NAME}_${timestamp}.tar.gz"

    log_info "Creating backup: $backup_path"
    mkdir -p "$BACKUP_DIR"

    tar -czf "$backup_path" -C "$DEPLOY_DIR" . \
        --exclude='backups' \
        --exclude='*.log'

    # Rotate old backups
    local backup_count
    backup_count=$(find "$BACKUP_DIR" -name "*.tar.gz" | wc -l)
    if [[ "$backup_count" -gt "$MAX_BACKUPS" ]]; then
        log_info "Rotating old backups (keeping $MAX_BACKUPS)"
        find "$BACKUP_DIR" -name "*.tar.gz" -type f \
            | sort | head -n -"$MAX_BACKUPS" \
            | xargs rm -f
    fi

    echo "$backup_path"
}

rollback() {
    local backup_file="$1"
    log_warn "Rolling back to: $backup_file"

    if [[ ! -f "$backup_file" ]]; then
        log_error "Backup file not found: $backup_file"
        return 1
    fi

    tar -xzf "$backup_file" -C "$DEPLOY_DIR"
    log_info "Rollback completed successfully"
}

main() {
    log_info "Starting deployment of ${APP_NAME}..."

    validate_environment || exit 1

    local backup_path
    backup_path=$(create_backup)
    log_info "Backup created at: $backup_path"

    log_info "Deployment completed successfully"
}

main "$@"
