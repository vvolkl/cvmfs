package backend

import (
	"context"
	"fmt"

	gw "github.com/cvmfs/gateway/internal/gateway"
)

// LeaseStore is the persistence seam for the gateway's lease and repository
// state (the "lease DB").
//
// It abstracts the lease database so the backend can be selected at runtime
// (see OpenLeaseStore). The SQL implementation (*DB) covers both the local sqlite
// file (default) and an external, highly-available PostgreSQL, which is the
// supported way to run the gateway in an active-passive HA setup: several
// gateway instances, only one active at a time behind a VIP/load balancer, all
// sharing the same external database so lease state survives a failover.
//
// A future etcd backend is a natural fit for this interface and would unlock
// active-active operation: etcd leases (TTL keys) map onto our expiring leases
// (replacing DeleteExpiredLeases), etcd concurrency.Mutex gives a *distributed*
// per-repository lock for WithLock (today's NamedLocks is in-process only), and
// etcd Txn/STM provides multi-key atomicity. A sketch of the intended key
// layout, for whoever implements it:
//
//	<prefix>/leases/<repo>/<path>      -> JSON-encoded Lease, attached to an
//	                                      etcd lease with the remaining TTL
//	<prefix>/leases/by-token/<token>   -> <repo>/<path> index (written in the
//	                                      same Txn as the lease key)
//	<prefix>/repos/<name>              -> JSON-encoded Repository
//
// Note that active-active additionally requires replacing the in-process
// leaseMutex (lease_service.go) with the distributed lock; that is out of scope
// until the etcd backend lands.
type LeaseStore interface {
	// Lease operations
	CreateLease(ctx context.Context, lease Lease) error
	FindLeaseByToken(ctx context.Context, token string) (*Lease, error)
	FindActiveLeases(ctx context.Context) ([]Lease, error)
	FindLeasesByRepoAndOverlappingPath(ctx context.Context, repo, path string) ([]Lease, error)
	DeleteExpiredLeases(ctx context.Context) error
	DeleteLeaseByToken(ctx context.Context, token string) error
	DeleteLeasesByRepoAndPathPrefix(ctx context.Context, repo, path string) error

	// Repository operations
	CreateRepository(ctx context.Context, repo Repository) error
	UpdateRepository(ctx context.Context, repo Repository) error
	FindRepositoryByName(ctx context.Context, name string) (*Repository, error)
	FindAllRepositories(ctx context.Context) ([]Repository, error)
	DeleteAllRepositories(ctx context.Context) error

	// WithLock runs task while holding a per-repository commit lock
	WithLock(ctx context.Context, name string, task func() error) error

	// Close releases the underlying resources
	Close() error
}

// compile-time assertion that the SQL backend satisfies the LeaseStore interface
var _ LeaseStore = (*DB)(nil)

// OpenLeaseStore opens the lease store backend selected by config.DBType.
//
//   - "sqlite" (default): local file in config.WorkDir (single-node).
//   - "postgres": external HA database addressed by config.DBURL.
//   - "etcd": reserved; not implemented yet.
func OpenLeaseStore(config gw.Config) (LeaseStore, error) {
	switch config.DBType {
	case "", "sqlite", "postgres":
		return OpenDB(config)
	case "etcd":
		return nil, fmt.Errorf("etcd lease store is not yet implemented")
	default:
		return nil, fmt.Errorf("unknown db_type %q (expected sqlite|postgres)", config.DBType)
	}
}
