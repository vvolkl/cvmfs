package backend

import (
	"context"
	"os"
	"testing"
	"time"

	"github.com/cvmfs/gateway/internal/gateway"
)

// TestPostgresStore exercises the LeaseStore implementation against a real
// PostgreSQL instance. It is skipped unless CVMFS_TEST_PG_URL points at a
// reachable database, e.g.
//
//	CVMFS_TEST_PG_URL="postgres://cvmfs:cvmfs@localhost:5432/cvmfs_gateway?sslmode=disable" \
//	    go test ./internal/gateway/backend/ -run TestPostgresStore
//
// The container stack in test/common/container/gateway-postgres exercises the
// same path end-to-end; this test gives fast feedback on the SQL/dialect layer.
func TestPostgresStore(t *testing.T) {
	url := os.Getenv("CVMFS_TEST_PG_URL")
	if url == "" {
		t.Skip("set CVMFS_TEST_PG_URL to run the postgres lease store test")
	}

	cfg := gateway.Config{DBType: "postgres", DBURL: url}
	var store LeaseStore
	store, err := OpenLeaseStore(cfg)
	if err != nil {
		t.Fatalf("could not open postgres store: %v", err)
	}
	defer store.Close()

	ctx := context.Background()

	// Start from a clean slate so the test is repeatable.
	if err := store.DeleteAllRepositories(ctx); err != nil {
		t.Fatalf("delete all repositories: %v", err)
	}

	const repo = "pgtest.repo.org"

	t.Run("repository CRUD", func(t *testing.T) {
		if err := store.CreateRepository(ctx, Repository{Name: repo, Manifest: "", Enabled: true}); err != nil {
			t.Fatalf("create repository: %v", err)
		}
		got, err := store.FindRepositoryByName(ctx, repo)
		if err != nil || got == nil {
			t.Fatalf("find repository: got=%v err=%v", got, err)
		}
		if !got.Enabled {
			t.Fatalf("expected repository to be enabled")
		}
		got.Enabled = false
		if err := store.UpdateRepository(ctx, *got); err != nil {
			t.Fatalf("update repository: %v", err)
		}
		got, _ = store.FindRepositoryByName(ctx, repo)
		if got.Enabled {
			t.Fatalf("expected repository to be disabled after update")
		}
	})

	t.Run("lease CRUD and overlap", func(t *testing.T) {
		token := NewLeaseToken()
		lease := Lease{
			Token:           token,
			Repository:      repo,
			Path:            "/foo/bar",
			KeyID:           "key1",
			Expiration:      time.Now().Add(time.Hour),
			ProtocolVersion: 3,
			Hostname:        "host1",
		}
		if err := store.CreateLease(ctx, lease); err != nil {
			t.Fatalf("create lease: %v", err)
		}

		found, err := store.FindLeaseByToken(ctx, token)
		if err != nil || found == nil {
			t.Fatalf("find lease by token: got=%v err=%v", found, err)
		}
		if found.Path != "/foo/bar" || found.Hostname != "host1" {
			t.Fatalf("unexpected lease round-trip: %+v", found)
		}

		// Overlap: a parent path must match the existing child lease.
		overlaps, err := store.FindLeasesByRepoAndOverlappingPath(ctx, repo, "/foo")
		if err != nil {
			t.Fatalf("overlap query: %v", err)
		}
		if len(overlaps) != 1 {
			t.Fatalf("expected 1 overlapping lease, got %d", len(overlaps))
		}

		active, err := store.FindActiveLeases(ctx)
		if err != nil || len(active) != 1 {
			t.Fatalf("active leases: n=%d err=%v", len(active), err)
		}

		if err := store.DeleteLeaseByToken(ctx, token); err != nil {
			t.Fatalf("delete lease: %v", err)
		}
		found, _ = store.FindLeaseByToken(ctx, token)
		if found != nil {
			t.Fatalf("expected lease to be deleted")
		}
	})

	t.Run("expired leases are swept", func(t *testing.T) {
		token := NewLeaseToken()
		if err := store.CreateLease(ctx, Lease{
			Token:           token,
			Repository:      repo,
			Path:            "/expired",
			KeyID:           "key1",
			Expiration:      time.Now().Add(-time.Hour),
			ProtocolVersion: 3,
			Hostname:        "host1",
		}); err != nil {
			t.Fatalf("create expired lease: %v", err)
		}
		if err := store.DeleteExpiredLeases(ctx); err != nil {
			t.Fatalf("delete expired: %v", err)
		}
		active, _ := store.FindActiveLeases(ctx)
		if len(active) != 0 {
			t.Fatalf("expected no active leases after sweep, got %d", len(active))
		}
	})

	// cleanup
	_ = store.DeleteLeasesByRepoAndPathPrefix(ctx, repo, "/")
	_ = store.DeleteAllRepositories(ctx)
}
