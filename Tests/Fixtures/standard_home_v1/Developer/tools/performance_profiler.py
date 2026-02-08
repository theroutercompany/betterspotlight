"""Simple performance profiler wrapper."""

import time
from contextlib import contextmanager


@contextmanager
def profile_block(name: str):
    start = time.perf_counter()
    try:
        yield
    finally:
        duration = (time.perf_counter() - start) * 1000
        print(f"{name}: {duration:.2f} ms")


def main() -> None:
    with profile_block("index_scan"):
        time.sleep(0.01)
    with profile_block("query_merge"):
        time.sleep(0.02)


if __name__ == "__main__":
    main()
