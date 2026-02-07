#!/usr/bin/env python3
"""
BetterSpotlight test fixture: Python source file.

This module implements a simple task scheduler with priority queues
and deadline-based scheduling. Contains distinctive identifiers for
search testing.
"""

import heapq
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from typing import List, Optional, Callable


@dataclass(order=True)
class ScheduledTask:
    """A task with priority and deadline for the scheduler."""
    priority: int
    deadline: datetime = field(compare=False)
    name: str = field(compare=False)
    callback: Optional[Callable] = field(default=None, compare=False, repr=False)

    def is_overdue(self) -> bool:
        return datetime.now() > self.deadline


class TaskScheduler:
    """Priority-based task scheduler with deadline enforcement."""

    def __init__(self, max_concurrent: int = 4):
        self._queue: List[ScheduledTask] = []
        self._completed: List[ScheduledTask] = []
        self._max_concurrent = max_concurrent
        self._running_count = 0

    def enqueue(self, task: ScheduledTask) -> None:
        """Add a task to the priority queue."""
        heapq.heappush(self._queue, task)

    def dequeue(self) -> Optional[ScheduledTask]:
        """Remove and return the highest priority task."""
        if not self._queue:
            return None
        return heapq.heappop(self._queue)

    def process_next(self) -> bool:
        """Process the next available task. Returns True if a task was processed."""
        if self._running_count >= self._max_concurrent:
            return False
        task = self.dequeue()
        if task is None:
            return False
        self._running_count += 1
        if task.callback:
            task.callback()
        self._completed.append(task)
        self._running_count -= 1
        return True

    @property
    def pending_count(self) -> int:
        return len(self._queue)

    @property
    def completed_count(self) -> int:
        return len(self._completed)

    def get_overdue_tasks(self) -> List[ScheduledTask]:
        """Return all tasks that have passed their deadline."""
        return [t for t in self._queue if t.is_overdue()]


def create_sample_schedule() -> TaskScheduler:
    """Create a sample schedule for testing purposes."""
    scheduler = TaskScheduler(max_concurrent=2)
    now = datetime.now()

    tasks = [
        ScheduledTask(1, now + timedelta(hours=1), "index_documents"),
        ScheduledTask(3, now + timedelta(hours=2), "optimize_fts5"),
        ScheduledTask(2, now + timedelta(minutes=30), "extract_metadata"),
        ScheduledTask(1, now + timedelta(hours=4), "vacuum_database"),
    ]

    for task in tasks:
        scheduler.enqueue(task)

    return scheduler


if __name__ == "__main__":
    scheduler = create_sample_schedule()
    print(f"Pending tasks: {scheduler.pending_count}")
    while scheduler.process_next():
        print(f"Completed: {scheduler.completed_count}")
