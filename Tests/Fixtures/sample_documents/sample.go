// BetterSpotlight test fixture: Go source file.
//
// Implements a concurrent-safe rate limiter using the token bucket
// algorithm with configurable burst capacity and refill rate.

package ratelimiter

import (
	"context"
	"sync"
	"time"
)

// RateLimiter implements the token bucket algorithm for rate limiting.
// It is safe for concurrent use from multiple goroutines.
type RateLimiter struct {
	mu            sync.Mutex
	tokens        float64
	maxTokens     float64
	refillRate    float64 // tokens per second
	lastRefill    time.Time
	waiters       int
	totalAllowed  int64
	totalRejected int64
}

// NewRateLimiter creates a new rate limiter with the specified
// maximum burst capacity and refill rate (tokens per second).
func NewRateLimiter(maxTokens float64, refillRate float64) *RateLimiter {
	return &RateLimiter{
		tokens:     maxTokens,
		maxTokens:  maxTokens,
		refillRate: refillRate,
		lastRefill: time.Now(),
	}
}

// Allow checks if a request should be allowed and consumes one token
// if available. Returns true if the request is allowed.
func (rl *RateLimiter) Allow() bool {
	rl.mu.Lock()
	defer rl.mu.Unlock()

	rl.refill()

	if rl.tokens >= 1.0 {
		rl.tokens--
		rl.totalAllowed++
		return true
	}

	rl.totalRejected++
	return false
}

// Wait blocks until a token is available or the context is cancelled.
// Returns nil if a token was acquired, or the context error otherwise.
func (rl *RateLimiter) Wait(ctx context.Context) error {
	for {
		rl.mu.Lock()
		rl.refill()

		if rl.tokens >= 1.0 {
			rl.tokens--
			rl.totalAllowed++
			rl.mu.Unlock()
			return nil
		}

		// Calculate time until next token
		deficit := 1.0 - rl.tokens
		waitDuration := time.Duration(deficit / rl.refillRate * float64(time.Second))
		rl.waiters++
		rl.mu.Unlock()

		select {
		case <-ctx.Done():
			rl.mu.Lock()
			rl.waiters--
			rl.mu.Unlock()
			return ctx.Err()
		case <-time.After(waitDuration):
			rl.mu.Lock()
			rl.waiters--
			rl.mu.Unlock()
			continue
		}
	}
}

// Stats returns the current rate limiter statistics.
type Stats struct {
	AvailableTokens float64
	TotalAllowed    int64
	TotalRejected   int64
	ActiveWaiters   int
}

// GetStats returns a snapshot of the rate limiter statistics.
func (rl *RateLimiter) GetStats() Stats {
	rl.mu.Lock()
	defer rl.mu.Unlock()
	rl.refill()

	return Stats{
		AvailableTokens: rl.tokens,
		TotalAllowed:    rl.totalAllowed,
		TotalRejected:   rl.totalRejected,
		ActiveWaiters:   rl.waiters,
	}
}

func (rl *RateLimiter) refill() {
	now := time.Now()
	elapsed := now.Sub(rl.lastRefill).Seconds()
	rl.tokens += elapsed * rl.refillRate
	if rl.tokens > rl.maxTokens {
		rl.tokens = rl.maxTokens
	}
	rl.lastRefill = now
}
