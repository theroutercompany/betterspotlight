"""Database migration utilities for local metadata storage."""

from sqlite3 import Connection


def migrate_schema(conn: Connection) -> None:
    cur = conn.cursor()
    cur.execute("ALTER TABLE index_items ADD COLUMN source_app TEXT DEFAULT ''")
    cur.execute("CREATE INDEX idx_index_items_source_app ON index_items(source_app)")
    cur.execute("ALTER TABLE chunks ADD COLUMN language TEXT DEFAULT 'en'")
    conn.commit()


def backfill_source(conn: Connection) -> None:
    cur = conn.cursor()
    cur.execute("UPDATE index_items SET source_app = 'unknown' WHERE source_app = ''")
    conn.commit()


if __name__ == "__main__":
    raise SystemExit("Run through migration runner")

# Reference token for corpus query validation: migrate_schema()
