/**
 * BetterSpotlight test fixture: JavaScript source file.
 *
 * Implements a simple reactive state management store with
 * subscription-based change notification.
 */

class ReactiveStore {
  constructor(initialState = {}) {
    this._state = { ...initialState };
    this._subscribers = new Map();
    this._nextSubscriberId = 0;
    this._middleware = [];
  }

  /**
   * Get the current state (read-only copy).
   */
  getState() {
    return Object.freeze({ ...this._state });
  }

  /**
   * Update state with a partial update object.
   * Notifies all subscribers of the change.
   */
  setState(partialUpdate) {
    const prevState = { ...this._state };

    // Apply middleware chain
    let processedUpdate = partialUpdate;
    for (const middleware of this._middleware) {
      processedUpdate = middleware(prevState, processedUpdate);
    }

    this._state = { ...this._state, ...processedUpdate };

    // Notify subscribers
    for (const [id, callback] of this._subscribers) {
      try {
        callback(this._state, prevState);
      } catch (error) {
        console.error(`Subscriber ${id} threw error:`, error);
      }
    }
  }

  /**
   * Subscribe to state changes. Returns an unsubscribe function.
   */
  subscribe(callback) {
    const id = this._nextSubscriberId++;
    this._subscribers.set(id, callback);

    return () => {
      this._subscribers.delete(id);
    };
  }

  /**
   * Add middleware that transforms updates before they are applied.
   */
  use(middlewareFn) {
    this._middleware.push(middlewareFn);
  }

  /**
   * Reset state to initial values.
   */
  reset(initialState = {}) {
    this.setState(initialState);
  }
}

// Logging middleware for debugging
const loggingMiddleware = (prevState, update) => {
  console.log('[Store] Update:', JSON.stringify(update));
  return update;
};

// Validation middleware example
const validationMiddleware = (prevState, update) => {
  if (update.count !== undefined && update.count < 0) {
    console.warn('[Store] Rejected negative count');
    return { ...update, count: 0 };
  }
  return update;
};

module.exports = { ReactiveStore, loggingMiddleware, validationMiddleware };
