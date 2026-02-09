# Reindex Existing Emacs Lisp Files (`.el`)

When `.el` support is added or extraction behavior changes, historical rows may still carry old unsupported extraction failures.

## Goal
Refresh indexed metadata/content for Emacs Lisp files so old unsupported-format failures are replaced by current text extraction behavior.

## Steps
1. Launch BetterSpotlight.
2. Open **Settings → Indexing**.
3. Click **Reindex Folder...**.
4. Select the parent directory containing Emacs Lisp files (for example `~/.config/emacs`).
5. Wait for queue backlog to drain.
6. Open **Index Health** and confirm recent errors no longer include legacy `.el` unsupported-format rows.

## Optional verification query
Use the main search UI with a known symbol from one `.el` file to confirm content search hits.
