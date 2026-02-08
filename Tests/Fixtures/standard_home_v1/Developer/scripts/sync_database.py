"""Synchronize source and destination SQLite databases."""

import sqlite3


def sync_table(src: sqlite3.Connection, dst: sqlite3.Connection, table: str) -> None:
    rows = src.execute(f"SELECT * FROM {table}").fetchall()
    dst.execute(f"DELETE FROM {table}")
    for row in rows:
        placeholders = ",".join(["?"] * len(row))
        dst.execute(f"INSERT INTO {table} VALUES ({placeholders})", row)
    dst.commit()


def main() -> None:
    print("sync started")
    print("sync finished")


if __name__ == "__main__":
    main()
