package backend

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"strings"
	"time"

	gw "github.com/cvmfs/gateway/internal/gateway"
	_ "github.com/jackc/pgx/v5/stdlib" // "pgx" driver for the postgres backend
	_ "github.com/mattn/go-sqlite3"
)

const (
	// latestSchemaVersion represents the most recent lease DB schema version
	// known to the application
	latestSchemaVersion = 3
)

// sqlDialect is the SQL dialect of the currently opened lease store. It is set
// by OpenDB and read by rebind() and the schema helpers. A package-level value
// (rather than a field on DB) keeps the entity functions, which only receive a
// *sql.Tx, dialect-aware without changing their signatures.
var sqlDialect = "sqlite"

// rebind rewrites the '?' positional placeholders used throughout the entity
// functions into the dialect-specific form. sqlite accepts '?' as-is; postgres
// requires '$1', '$2', ... (port of sqlx's Rebind).
func rebind(query string) string {
	if sqlDialect != "postgres" {
		return query
	}
	var sb strings.Builder
	n := 1
	for i := 0; i < len(query); i++ {
		if query[i] == '?' {
			sb.WriteByte('$')
			sb.WriteString(fmt.Sprintf("%d", n))
			n++
			continue
		}
		sb.WriteByte(query[i])
	}
	return sb.String()
}

// nowFunc returns the dialect-specific expression for the current timestamp.
func nowFunc() string {
	if sqlDialect == "postgres" {
		return "now()"
	}
	return "datetime('now')"
}

// DB stores active leases
type DB struct {
	SQL   *sql.DB
	Locks NamedLocks // Per-repository commit locks
}

// OpenDB opens or creates the gateway SQL lease store. The backend is selected
// by config.DBType: "sqlite" (default, a local file in config.WorkDir) or
// "postgres" (an external database addressed by config.DBURL).
func OpenDB(config gw.Config) (*DB, error) {
	switch config.DBType {
	case "", "sqlite":
		return openSQLite(config)
	case "postgres":
		return openPostgres(config)
	default:
		return nil, fmt.Errorf("unsupported SQL db_type %q", config.DBType)
	}
}

func openSQLite(config gw.Config) (*DB, error) {
	sqlDialect = "sqlite"

	if err := os.MkdirAll(config.WorkDir, 0777); err != nil {
		return nil, fmt.Errorf("could not create working directory: %w", err)
	}
	dbFile := config.WorkDir + "/gw.db"
	createDB := false
	if _, err := os.Stat(dbFile); os.IsNotExist(err) {
		createDB = true
	}

	sqlDB, err := sql.Open("sqlite3", "file:"+dbFile+"?mode=rwc")
	if err != nil {
		return nil, fmt.Errorf("could not open DB: %w", err)
	}

	if createDB {
		if err := createSchema(sqlDB); err != nil {
			return nil, fmt.Errorf("could not initialise DB: %w", err)
		}
	}

	if _, err := checkSchemaVersion(sqlDB); err != nil {
		return nil, fmt.Errorf("invalid schema version: %w", err)
	}

	gw.Log("leasedb", gw.LogInfo).
		Msgf("sqlite lease store opened (work dir: %v)", config.WorkDir)

	return &DB{SQL: sqlDB, Locks: NamedLocks{}}, nil
}

func openPostgres(config gw.Config) (*DB, error) {
	sqlDialect = "postgres"

	if config.DBURL == "" {
		return nil, fmt.Errorf("db_url must be set when db_type is postgres")
	}

	sqlDB, err := sql.Open("pgx", config.DBURL)
	if err != nil {
		return nil, fmt.Errorf("could not open postgres DB: %w", err)
	}

	// Keep a small, recyclable pool so that a database failover is recovered
	// by establishing fresh connections rather than reusing stale ones.
	sqlDB.SetMaxOpenConns(16)
	sqlDB.SetMaxIdleConns(4)
	sqlDB.SetConnMaxLifetime(5 * time.Minute)

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := sqlDB.PingContext(ctx); err != nil {
		return nil, fmt.Errorf("could not reach postgres DB: %w", err)
	}

	exists, err := schemaExists(ctx, sqlDB)
	if err != nil {
		return nil, fmt.Errorf("could not inspect postgres schema: %w", err)
	}
	if !exists {
		if err := createSchema(sqlDB); err != nil {
			return nil, fmt.Errorf("could not initialise postgres schema: %w", err)
		}
	}

	if _, err := checkSchemaVersion(sqlDB); err != nil {
		return nil, fmt.Errorf("invalid schema version: %w", err)
	}

	gw.Log("leasedb", gw.LogInfo).
		Msg("postgres lease store opened")

	return &DB{SQL: sqlDB, Locks: NamedLocks{}}, nil
}

// Close the lease database
func (db *DB) Close() error {
	return db.SQL.Close()
}

// WithLock runs the given task while holding a commit lock for the repository
func (db *DB) WithLock(ctx context.Context, repository string, task func() error) error {
	return db.Locks.WithLock(repository, task)
}

// withTx runs fn inside a transaction, committing on success and rolling back
// on error. It is the single transaction boundary used by the LeaseStore methods.
func (db *DB) withTx(ctx context.Context, fn func(tx *sql.Tx) error) error {
	tx, err := db.SQL.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("could not begin transaction: %w", err)
	}
	defer tx.Rollback()

	if err := fn(tx); err != nil {
		return err
	}

	if err := tx.Commit(); err != nil {
		return fmt.Errorf("could not commit transaction: %w", err)
	}
	return nil
}

// --- LeaseStore interface implementation (lease operations) ---

func (db *DB) CreateLease(ctx context.Context, lease Lease) error {
	return db.withTx(ctx, func(tx *sql.Tx) error { return CreateLease(ctx, tx, lease) })
}

func (db *DB) FindLeaseByToken(ctx context.Context, token string) (*Lease, error) {
	var lease *Lease
	err := db.withTx(ctx, func(tx *sql.Tx) error {
		var e error
		lease, e = FindLeaseByToken(ctx, tx, token)
		return e
	})
	return lease, err
}

func (db *DB) FindActiveLeases(ctx context.Context) ([]Lease, error) {
	var leases []Lease
	err := db.withTx(ctx, func(tx *sql.Tx) error {
		var e error
		leases, e = FindAllActiveLeases(ctx, tx)
		return e
	})
	return leases, err
}

func (db *DB) FindLeasesByRepoAndOverlappingPath(ctx context.Context, repo, path string) ([]Lease, error) {
	var leases []Lease
	err := db.withTx(ctx, func(tx *sql.Tx) error {
		var e error
		leases, e = FindAllLeasesByRepositoryAndOverlappingPath(ctx, tx, repo, path)
		return e
	})
	return leases, err
}

func (db *DB) DeleteExpiredLeases(ctx context.Context) error {
	return db.withTx(ctx, func(tx *sql.Tx) error { return DeleteAllExpiredLeases(ctx, tx) })
}

func (db *DB) DeleteLeaseByToken(ctx context.Context, token string) error {
	return db.withTx(ctx, func(tx *sql.Tx) error { return DeleteLeaseByToken(ctx, tx, token) })
}

func (db *DB) DeleteLeasesByRepoAndPathPrefix(ctx context.Context, repo, path string) error {
	return db.withTx(ctx, func(tx *sql.Tx) error {
		return DeleteAllLeasesByRepositoryAndPathPrefix(ctx, tx, repo, path)
	})
}

// --- LeaseStore interface implementation (repository operations) ---

func (db *DB) CreateRepository(ctx context.Context, repo Repository) error {
	return db.withTx(ctx, func(tx *sql.Tx) error { return CreateRepository(ctx, tx, repo) })
}

func (db *DB) UpdateRepository(ctx context.Context, repo Repository) error {
	return db.withTx(ctx, func(tx *sql.Tx) error { return UpdateRepository(ctx, tx, repo) })
}

func (db *DB) FindRepositoryByName(ctx context.Context, name string) (*Repository, error) {
	var repo *Repository
	err := db.withTx(ctx, func(tx *sql.Tx) error {
		var e error
		repo, e = FindRepositoryByName(ctx, tx, name)
		return e
	})
	return repo, err
}

func (db *DB) FindAllRepositories(ctx context.Context) ([]Repository, error) {
	var repos []Repository
	err := db.withTx(ctx, func(tx *sql.Tx) error {
		var e error
		repos, e = FindAllRepositories(ctx, tx)
		return e
	})
	return repos, err
}

func (db *DB) DeleteAllRepositories(ctx context.Context) error {
	return db.withTx(ctx, func(tx *sql.Tx) error { return DeleteAllRepositories(ctx, tx) })
}

// --- schema management ---

// schemaExists reports whether the lease schema has already been created
// (used for the postgres backend, where there is no file to stat).
func schemaExists(ctx context.Context, db *sql.DB) (bool, error) {
	var exists bool
	err := db.QueryRowContext(
		ctx,
		"select exists (select 1 from information_schema.tables where table_name = 'schemaversion');",
	).Scan(&exists)
	if err != nil {
		return false, err
	}
	return exists, nil
}

func createSchema(db *sql.DB) error {
	// Column types differ between dialects: sqlite is permissive ("string"/
	// "bool") while postgres needs concrete types. The column *order* must match
	// the positional scans in scanLease / scanRepository.
	var leaseCols, repoCols string
	if sqlDialect == "postgres" {
		leaseCols = `
	Token text not null unique primary key,
	Repository text not null,
	Path text not null,
	KeyID text not null,
	Expiration bigint not null,
	ProtocolVersion integer not null,
	Hostname text`
		repoCols = `
	Name text not null unique primary key,
	Manifest text,
	Enabled boolean not null`
	} else {
		leaseCols = `
	Token string not null unique primary key,
	Repository string not null,
	Path string not null,
	KeyID string not null,
	Expiration integer not null,
	ProtocolVersion integer not null,
	Hostname string`
		repoCols = `
	Name string not null unique primary key,
	Manifest string,
	Enabled bool not null`
	}

	statement := fmt.Sprintf(`
create table SchemaVersion (
    VersionNumber integer not null unique primary key,
    ValidFrom timestamp not null,
    ValidTo timestamp
);
insert into SchemaVersion (VersionNumber, ValidFrom) values (%v, %s);
create table if not exists Lease (%s
);
create index lease_repository_path_idx ON Lease(Repository,Path);
create table if not exists Repository (%s
);
`,
		latestSchemaVersion, nowFunc(), leaseCols, repoCols)
	if _, err := db.Exec(statement); err != nil {
		return fmt.Errorf("could not create table 'SchemaVersion': %w", err)
	}
	return nil
}

func checkSchemaVersion(db *sql.DB) (int, error) {
	var version int
	if err := db.QueryRow(
		"select VersionNumber from SchemaVersion;").Scan(&version); err != nil {
		return 0, fmt.Errorf("could not retrieve schema version: %w", err)
	}

	if version > latestSchemaVersion {
		return 0, fmt.Errorf(
			"unknown schema version: %v, latest known %v",
			version, latestSchemaVersion)
	}

	if version == 2 {
		statement := fmt.Sprintf(`
alter table lease add column hostname string;
update SchemaVersion set VersionNumber=3, ValidFrom=%s;
`, nowFunc())
		if _, err := db.Exec(statement); err != nil {
			return 2, fmt.Errorf("could not migrate table schema (2->3): %w", err)
		}

		version = 3
	}

	return version, nil
}
