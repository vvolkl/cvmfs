package backend

import (
	"context"
	"fmt"
	"time"
)

func (s *Services) NewRepo(ctx context.Context, name string, enabled bool) error {
	t0 := time.Now()

	outcome := "success"
	defer logAction(ctx, "new_repo", &outcome, t0)

	repo := Repository{
		Name:     name,
		Manifest: "",
		Enabled:  true,
	}

	if err := s.Store.CreateRepository(ctx, repo); err != nil {
		return fmt.Errorf("could not create repository: %w", err)
	}

	return nil
}

// GetRepo returns the access configuration of a repository
func (s *Services) GetRepo(ctx context.Context, repoName string) (*RepositoryConfig, error) {
	t0 := time.Now()

	outcome := "success"
	defer logAction(ctx, "get_repo", &outcome, t0)

	repo, err := s.Store.FindRepositoryByName(ctx, repoName)
	if err != nil {
		return nil, err
	}

	repoConfig := s.Access.GetRepo(repoName)
	if repo != nil && repoConfig != nil {
		repoConfig.Enabled = repo.Enabled
	}

	return repoConfig, nil
}

// GetRepos returns a map with repository access configurations
func (s *Services) GetRepos(ctx context.Context) (map[string]RepositoryConfig, error) {
	t0 := time.Now()

	outcome := "success"
	defer logAction(ctx, "get_repos", &outcome, t0)

	repos, err := s.Store.FindAllRepositories(ctx)
	if err != nil {
		return nil, err
	}

	repoConfig := s.Access.GetRepos()
	for _, repo := range repos {
		cfg := repoConfig[repo.Name]
		cfg.Enabled = repo.Enabled
		repoConfig[repo.Name] = cfg
	}

	return repoConfig, nil
}

// SetRepoEnabled enables or disables a repository. The change does not persist
// across applications restarts
func (s *Services) SetRepoEnabled(ctx context.Context, repoName string, enable bool) error {
	t0 := time.Now()

	outcome := "success"
	defer logAction(ctx, "set_repo_enabled", &outcome, t0)

	repo, err := s.Store.FindRepositoryByName(ctx, repoName)
	if err != nil {
		return err
	}

	repo.Enabled = enable

	if err := s.Store.UpdateRepository(ctx, *repo); err != nil {
		return err
	}

	return nil
}

func (s *Services) DeleteAllRepositories(ctx context.Context) error {
	t0 := time.Now()

	outcome := "success"
	defer logAction(ctx, "delete_all", &outcome, t0)

	if err := s.Store.DeleteAllRepositories(ctx); err != nil {
		return err
	}

	return nil
}
