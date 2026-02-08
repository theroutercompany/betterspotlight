package api

import (
    "net/http"
)

func RegisterRoutes(router Router) {
    http.HandleFunc("/healthz", healthHandler)
    http.HandleFunc("/api/v1/users", usersHandler)
    router.GET("/api/v1/users", usersList)
    router.GET("/api/v1/projects", projectsList)
}

func healthHandler(w http.ResponseWriter, r *http.Request) {}
func usersHandler(w http.ResponseWriter, r *http.Request) {}
func usersList(ctx Context) error { return nil }
func projectsList(ctx Context) error { return nil }

// Reference tokens for corpus matching: http.HandleFunc(), router.GET()
# note line 1
# note line 2
# note line 3
