// BetterSpotlight test fixture: Rust source file.
//
// Implements a generic least-recently-used (LRU) cache with
// configurable capacity and eviction callbacks.

use std::collections::HashMap;
use std::hash::Hash;

/// A node in the doubly-linked list used by the LRU cache.
struct CacheNode<K, V> {
    key: K,
    value: V,
    prev: Option<usize>,
    next: Option<usize>,
}

/// A generic LRU cache with O(1) lookup, insertion, and eviction.
///
/// Uses a HashMap for key lookups combined with a doubly-linked list
/// for maintaining access order. The least recently used entry is
/// evicted when the cache exceeds its capacity.
pub struct LruCache<K, V> {
    capacity: usize,
    map: HashMap<K, usize>,
    nodes: Vec<CacheNode<K, V>>,
    head: Option<usize>,
    tail: Option<usize>,
    eviction_count: u64,
}

impl<K: Clone + Eq + Hash, V> LruCache<K, V> {
    /// Create a new LRU cache with the given capacity.
    pub fn new(capacity: usize) -> Self {
        assert!(capacity > 0, "LRU cache capacity must be positive");
        LruCache {
            capacity,
            map: HashMap::with_capacity(capacity),
            nodes: Vec::with_capacity(capacity),
            head: None,
            tail: None,
            eviction_count: 0,
        }
    }

    /// Get a reference to the value associated with the key.
    /// Marks the entry as most recently used.
    pub fn get(&mut self, key: &K) -> Option<&V> {
        if let Some(&idx) = self.map.get(key) {
            self.move_to_front(idx);
            Some(&self.nodes[idx].value)
        } else {
            None
        }
    }

    /// Insert a key-value pair into the cache.
    /// If the key already exists, updates the value.
    /// If the cache is full, evicts the least recently used entry.
    pub fn put(&mut self, key: K, value: V) {
        if let Some(&idx) = self.map.get(&key) {
            self.nodes[idx].value = value;
            self.move_to_front(idx);
            return;
        }

        if self.nodes.len() >= self.capacity {
            self.evict_lru();
        }

        let idx = self.nodes.len();
        self.nodes.push(CacheNode {
            key: key.clone(),
            value,
            prev: None,
            next: self.head,
        });

        if let Some(old_head) = self.head {
            self.nodes[old_head].prev = Some(idx);
        }
        self.head = Some(idx);
        if self.tail.is_none() {
            self.tail = Some(idx);
        }

        self.map.insert(key, idx);
    }

    /// Returns the number of entries in the cache.
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Returns true if the cache is empty.
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }

    /// Returns the total number of evictions since creation.
    pub fn eviction_count(&self) -> u64 {
        self.eviction_count
    }

    fn move_to_front(&mut self, idx: usize) {
        if self.head == Some(idx) {
            return;
        }
        self.detach(idx);
        self.nodes[idx].prev = None;
        self.nodes[idx].next = self.head;
        if let Some(old_head) = self.head {
            self.nodes[old_head].prev = Some(idx);
        }
        self.head = Some(idx);
    }

    fn detach(&mut self, idx: usize) {
        let prev = self.nodes[idx].prev;
        let next = self.nodes[idx].next;
        if let Some(p) = prev {
            self.nodes[p].next = next;
        }
        if let Some(n) = next {
            self.nodes[n].prev = prev;
        }
        if self.tail == Some(idx) {
            self.tail = prev;
        }
    }

    fn evict_lru(&mut self) {
        if let Some(tail_idx) = self.tail {
            let key = self.nodes[tail_idx].key.clone();
            self.detach(tail_idx);
            self.map.remove(&key);
            self.eviction_count += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_basic_operations() {
        let mut cache = LruCache::new(3);
        cache.put("alpha", 1);
        cache.put("beta", 2);
        cache.put("gamma", 3);
        assert_eq!(cache.get(&"alpha"), Some(&1));
        assert_eq!(cache.len(), 3);
    }
}
