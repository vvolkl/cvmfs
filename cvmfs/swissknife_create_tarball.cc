/**
 * This file is part of the CernVM File System.
 */

#include "swissknife_create_tarball.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "catalog_mgr_ro.h"
#include "compression/compression.h"
#include "crypto/hash.h"
#include "duplex_libarchive.h"  // IWYU pragma: keep
#include "network/sink_mem.h"
#include "object_fetcher.h"
#include "util/concurrency.h"
#include "util/logging.h"
#include "util/pointer.h"
#include "util/posix.h"
#include "util/smalloc.h"
#include "util/string.h"
#include "util/tube.h"

using namespace std;  // NOLINT

namespace {

const unsigned kMaxExportWorkers = 8;
const unsigned kWorkerQueueFactor = 8;
const size_t kIoBufferSize = 256 * 1024;
const size_t kSmallFileThreshold = 256 * 1024;

void LogCreateTarballError(const string &message) {
  LogCvmfs(kLogCvmfs, kLogStderr, "%s", message.c_str());
}


string StripLeadingSlash(const string &path) {
  if (path == "/")
    return "";

  size_t start = 0;
  while ((start < path.length()) && (path[start] == '/')) {
    ++start;
  }
  return path.substr(start);
}


string JoinRepoPath(const string &parent, const string &name) {
  return (parent == "/") ? "/" + name : parent + "/" + name;
}


string JoinTarPath(const string &parent, const string &name) {
  return parent.empty() ? name : parent + "/" + name;
}



PathString MakeCatalogPath(const string &repo_path) {
  PathString result;
  if ((repo_path != "/") && !repo_path.empty()) {
    result.Assign(repo_path.data(), repo_path.length());
  }
  return result;
}


bool CopyPathToFile(const string &source_path, FILE *destination,
                   string *error) {
  const int fd_source = open(source_path.c_str(), O_RDONLY);
  if (fd_source < 0) {
    *error = "failed to open temporary payload '" + source_path + "'";
    return false;
  }

  unsigned char buffer[kIoBufferSize];
  bool success = true;
  while (true) {
    const ssize_t bytes_read = SafeRead(fd_source, buffer, sizeof(buffer));
    if (bytes_read < 0) {
      *error = "failed to read temporary payload '" + source_path + "'";
      success = false;
      break;
    }
    if (bytes_read == 0) {
      break;
    }
    if (!SafeWrite(fileno(destination), buffer, bytes_read)) {
      *error = "failed to write assembled payload data";
      success = false;
      break;
    }
  }

  close(fd_source);
  return success;
}


bool WriteArchiveDataFromPath(struct archive *archive,
                              const string &payload_path,
                              string *error) {
  const int fd_payload = open(payload_path.c_str(), O_RDONLY);
  if (fd_payload < 0) {
    *error = "failed to open fetched payload '" + payload_path + "'";
    return false;
  }

  unsigned char buffer[kIoBufferSize];
  bool success = true;
  while (true) {
    const ssize_t bytes_read = SafeRead(fd_payload, buffer, sizeof(buffer));
    if (bytes_read < 0) {
      *error = "failed to read fetched payload '" + payload_path + "'";
      success = false;
      break;
    }
    if (bytes_read == 0) {
      break;
    }
    la_ssize_t bytes_written = archive_write_data(archive, buffer, bytes_read);
    if (bytes_written != bytes_read) {
      *error = string("failed to write tar payload: ")
               + archive_error_string(archive);
      success = false;
      break;
    }
  }

  close(fd_payload);
  return success;
}


bool CompareDirectoryEntriesByName(const catalog::DirectoryEntry &lhs,
                                   const catalog::DirectoryEntry &rhs) {
  return lhs.name() < rhs.name();
}


bool IsCatalogControlEntry(const string &name) {
  return (name == ".cvmfscatalog") || (name == ".cvmfsautocatalog");
}


struct TarEntry {
  TarEntry()
      : sequence(0)
      , requires_payload(false)
      , is_hardlink(false)
      , scheduled(false) { }

  size_t sequence;
  string repo_path;
  string tar_path;
  catalog::DirectoryEntry dirent;
  bool requires_payload;
  bool is_hardlink;
  string hardlink_target;
  bool scheduled;
};


bool AppendTarEntry(const string &repo_path,
                    const string &tar_path,
                    const catalog::DirectoryEntry &dirent,
                    map<uint32_t, string> *hardlink_targets,
                    vector<TarEntry> *entries,
                    string *error) {
  if (dirent.IsHidden() || IsCatalogControlEntry(dirent.name().ToString())) {
    return true;
  }

  if (dirent.IsSocket()) {
    *error = "socket entries are not supported in tar export: " + repo_path;
    return false;
  }

  TarEntry tar_entry;
  tar_entry.sequence = entries->size();
  tar_entry.repo_path = repo_path;
  tar_entry.tar_path = tar_path;
  tar_entry.dirent = dirent;

  if (dirent.IsRegular()) {
    const uint32_t hardlink_group = dirent.hardlink_group();
    if ((hardlink_group > 0) && (dirent.linkcount() > 1)) {
      map<uint32_t, string>::const_iterator it =
          hardlink_targets->find(hardlink_group);
      if (it == hardlink_targets->end()) {
        (*hardlink_targets)[hardlink_group] = tar_path;
        tar_entry.requires_payload = true;
      } else {
        tar_entry.is_hardlink = true;
        tar_entry.hardlink_target = it->second;
      }
    } else {
      tar_entry.requires_payload = true;
    }
  } else if (dirent.IsSpecial()
             && !(dirent.IsFifo() || dirent.IsCharDev() || dirent.IsBlockDev())) {
    *error = "unsupported special entry in tar export: " + repo_path;
    return false;
  }

  entries->push_back(tar_entry);
  return true;
}


bool EnumerateSubtree(catalog::SimpleCatalogManager *catalog_manager,
                      const string &repo_path,
                      const string &tar_path,
                      const catalog::DirectoryEntry &dirent,
                      const bool emit_current_entry,
                      map<uint32_t, string> *hardlink_targets,
                      vector<TarEntry> *entries,
                      string *error) {
  // Use an explicit stack to avoid deep recursion on large directory trees.
  // Each frame holds a sorted, filtered listing and the current iteration index.
  struct StackFrame {
    string repo_path;
    string tar_path;
    catalog::DirectoryEntryList children;
    size_t index;
    StackFrame(const string &rp, const string &tp)
        : repo_path(rp), tar_path(tp), index(0) { }
  };

  entries->reserve(4096);

  if (emit_current_entry) {
    if (!AppendTarEntry(repo_path, tar_path, dirent, hardlink_targets, entries,
                        error)) {
      return false;
    }
  }

  if (!dirent.IsDirectory()) {
    return true;
  }

  vector<StackFrame *> stack;
  StackFrame *initial = new StackFrame(repo_path, tar_path);
  if (!catalog_manager->Listing(MakeCatalogPath(repo_path),
                                &initial->children)) {
    *error = "failed to list catalog path '" + repo_path + "'";
    delete initial;
    return false;
  }
  sort(initial->children.begin(), initial->children.end(),
       CompareDirectoryEntriesByName);
  stack.push_back(initial);

  while (!stack.empty()) {
    StackFrame *frame = stack.back();
    if (frame->index >= frame->children.size()) {
      delete frame;
      stack.pop_back();
      continue;
    }

    const catalog::DirectoryEntry &child = frame->children[frame->index];
    ++frame->index;

    if (child.IsHidden() || IsCatalogControlEntry(child.name().ToString())) {
      continue;
    }

    const string child_name = child.name().ToString();
    const string child_repo_path = JoinRepoPath(frame->repo_path, child_name);
    const string child_tar_path = JoinTarPath(frame->tar_path, child_name);

    if (!AppendTarEntry(child_repo_path, child_tar_path, child,
                        hardlink_targets, entries, error)) {
      for (size_t i = 0; i < stack.size(); ++i) delete stack[i];
      return false;
    }

    if (child.IsDirectory()) {
      StackFrame *child_frame = new StackFrame(child_repo_path, child_tar_path);
      if (!catalog_manager->Listing(MakeCatalogPath(child_repo_path),
                                    &child_frame->children)) {
        *error = "failed to list catalog path '" + child_repo_path + "'";
        delete child_frame;
        for (size_t i = 0; i < stack.size(); ++i) delete stack[i];
        return false;
      }
      sort(child_frame->children.begin(), child_frame->children.end(),
           CompareDirectoryEntriesByName);
      stack.push_back(child_frame);
    }
  }

  return true;
}


struct FetchResult {
  FetchResult() : sequence(0), success(false), mem_data(NULL), mem_size(0) { }
  ~FetchResult() { free(mem_data); }
  size_t sequence;
  bool success;
  string payload_path;
  // In-memory payload for small files (avoids temp file round-trip)
  unsigned char *mem_data;
  size_t mem_size;
  string error;

 private:
  FetchResult(const FetchResult &);
  FetchResult &operator=(const FetchResult &);
};




struct FetcherConfig {
  enum Type {
    kLocal,
    kHttp,
  };

  FetcherConfig()
      : type(kLocal)
      , download_manager(NULL)
      , signature_manager(NULL) { }

  Type type;
  string local_repository;
  string repository_url;
  string repository_name;
  string tmp_dir;
  download::DownloadManager *download_manager;
  signature::SignatureManager *signature_manager;
};


class PayloadFetcher {
 public:
  virtual ~PayloadFetcher() { }
  virtual bool Fetch(const shash::Any &object_hash,
                     string *payload_path,
                     string *error) = 0;
  // Fetch object into memory. Returns true on success with data/size set.
  // Default falls back to Fetch() + read + unlink.
  virtual bool FetchMem(const shash::Any &object_hash,
                        unsigned char **data, size_t *size,
                        string *error) {
    string path;
    if (!Fetch(object_hash, &path, error))
      return false;
    int fd = open(path.c_str(), O_RDONLY);
    unlink(path.c_str());
    if (fd < 0) {
      *error = "failed to open fetched object for in-memory read";
      return false;
    }
    struct stat info;
    if (fstat(fd, &info) != 0) {
      close(fd);
      *error = "failed to stat fetched object";
      return false;
    }
    *size = info.st_size;
    *data = reinterpret_cast<unsigned char *>(smalloc(*size));
    ssize_t nbytes = SafeRead(fd, *data, *size);
    close(fd);
    if (nbytes < 0 || static_cast<size_t>(nbytes) != *size) {
      free(*data);
      *data = NULL;
      *size = 0;
      *error = "short read on fetched object";
      return false;
    }
    return true;
  }
};


class LocalPayloadFetcher : public PayloadFetcher {
 public:
  LocalPayloadFetcher(const string &repository_path, const string &tmp_dir)
      : fetcher_(repository_path, tmp_dir)
      , base_path_(repository_path) { }

  virtual bool Fetch(const shash::Any &object_hash,
                     string *payload_path,
                     string *error) {
    const LocalObjectFetcher<>::Failures retval =
        fetcher_.Fetch(object_hash, payload_path);
    if (retval != LocalObjectFetcher<>::kFailOk) {
      *error = "failed to fetch local object '" + object_hash.ToString() + "'";
      return false;
    }
    return true;
  }

  virtual bool FetchMem(const shash::Any &object_hash,
                        unsigned char **data, size_t *size,
                        string *error) {
    // Decompress directly from CAS to memory, bypassing temp files
    const string source = base_path_ + "/data/" + object_hash.MakePath();
    FILE *fsrc = fopen(source.c_str(), "r");
    if (fsrc == NULL) {
      *error = "failed to open local object '" + source + "'";
      return false;
    }

    cvmfs::MemSink memsink;
    z_stream strm;
    zlib::DecompressInit(&strm);

    unsigned char buf[32768];
    bool success = true;
    zlib::StreamStates stream_state = zlib::kStreamIOError;
    size_t have;
    while ((have = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
      stream_state = zlib::DecompressZStream2Sink(buf, have, &strm, &memsink);
      if ((stream_state == zlib::kStreamDataError)
          || (stream_state == zlib::kStreamIOError)) {
        success = false;
        break;
      }
    }
    if (success && ((stream_state != zlib::kStreamEnd) || ferror(fsrc))) {
      success = false;
    }

    zlib::DecompressFini(&strm);
    fclose(fsrc);

    if (!success) {
      *error = "failed to decompress local object '" + source + "'";
      return false;
    }

    *size = memsink.pos();
    // Transfer ownership of the buffer
    *data = memsink.data();
    memsink.Release();
    return true;
  }

 private:
  LocalObjectFetcher<> fetcher_;
  const string base_path_;
};


class HttpPayloadFetcher : public PayloadFetcher {
 public:
  HttpPayloadFetcher(const string &repository_name,
                     const string &repository_url,
                     const string &tmp_dir,
                     download::DownloadManager *download_manager,
                     signature::SignatureManager *signature_manager)
      : fetcher_(repository_name, repository_url, tmp_dir, download_manager,
                 signature_manager)
      , repo_url_(repository_url)
      , download_manager_(download_manager) { }

  virtual bool Fetch(const shash::Any &object_hash,
                     string *payload_path,
                     string *error) {
    const HttpObjectFetcher<>::Failures retval =
        fetcher_.Fetch(object_hash, payload_path);
    if (retval != HttpObjectFetcher<>::kFailOk) {
      *error = "failed to fetch remote object '" + object_hash.ToString() + "'";
      return false;
    }
    return true;
  }

  virtual bool FetchMem(const shash::Any &object_hash,
                        unsigned char **data, size_t *size,
                        string *error) {
    const string url = repo_url_ + "/data/" + object_hash.MakePath();
    const bool decompress = true;
    const bool probe_hosts = false;
    cvmfs::MemSink memsink;
    download::JobInfo download_job(&url, decompress, probe_hosts,
                                   &object_hash, &memsink);
    download::Failures retval = download_manager_->Fetch(&download_job);
    if (retval != download::kFailOk) {
      *error = "failed to download object '" + object_hash.ToString()
               + "' to memory";
      return false;
    }
    *size = memsink.pos();
    *data = memsink.data();
    memsink.Release();
    return true;
  }

 private:
  HttpObjectFetcher<> fetcher_;
  const string repo_url_;
  download::DownloadManager *download_manager_;
};


PayloadFetcher *CreatePayloadFetcher(const FetcherConfig &config) {
  if (config.type == FetcherConfig::kHttp) {
    return new HttpPayloadFetcher(config.repository_name, config.repository_url,
                                  config.tmp_dir, config.download_manager,
                                  config.signature_manager);
  }
  return new LocalPayloadFetcher(config.local_repository, config.tmp_dir);
}



bool PrepareChunkedPayload(catalog::SimpleCatalogManager *catalog_manager,
                           PayloadFetcher *fetcher,
                           const FetcherConfig &fetcher_config,
                           const TarEntry &entry,
                           const string &tmp_dir,
                           string *payload_path,
                           string *error) {
  FileChunkList chunks;
  if (!catalog_manager->ListFileChunks(MakeCatalogPath(entry.repo_path),
                                       entry.dirent.hash_algorithm(), &chunks)
      || (chunks.size() == 0)) {
    *error = "failed to enumerate chunks for '" + entry.repo_path + "'";
    return false;
  }

  // Validate chunk offsets before fetching
  uint64_t expected_offset = 0;
  for (size_t i = 0; i < chunks.size(); ++i) {
    const FileChunk *chunk = chunks.AtPtr(i);
    if (static_cast<uint64_t>(chunk->offset()) != expected_offset) {
      *error = "chunk offsets are inconsistent for '" + entry.repo_path + "'";
      return false;
    }
    expected_offset += chunk->size();
  }
  if (expected_offset != entry.dirent.size()) {
    *error = "chunk total size mismatch for '" + entry.repo_path + "'";
    return false;
  }

  // Fetch all chunks (in parallel for multi-chunk files)
  vector<string> chunk_paths(chunks.size());
  bool success = true;

  if (chunks.size() > 1) {
    // Parallel fetch: each thread gets its own fetcher instance
    struct ChunkFetchArg {
      const FetcherConfig *config;
      const FileChunk *chunk;
      string *result_path;
      string error;
      bool success;
    };

    vector<ChunkFetchArg> args(chunks.size());
    vector<pthread_t> threads(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
      args[i].config = &fetcher_config;
      args[i].chunk = chunks.AtPtr(i);
      args[i].result_path = &chunk_paths[i];
      args[i].success = false;
    }

    struct ChunkFetchHelper {
      static void *Run(void *data) {
        ChunkFetchArg *arg = reinterpret_cast<ChunkFetchArg *>(data);
        UniquePtr<PayloadFetcher> f(CreatePayloadFetcher(*arg->config));
        arg->success = f->Fetch(arg->chunk->content_hash(),
                                arg->result_path, &arg->error);
        return NULL;
      }
    };

    for (size_t i = 0; i < chunks.size(); ++i) {
      const int retval = pthread_create(&threads[i], NULL,
                                        &ChunkFetchHelper::Run, &args[i]);
      if (retval != 0) {
        // Fall back: join already-started threads and clean up
        for (size_t j = 0; j < i; ++j) {
          pthread_join(threads[j], NULL);
          if (!chunk_paths[j].empty()) unlink(chunk_paths[j].c_str());
        }
        *error = "failed to start chunk fetch thread for '" + entry.repo_path
                 + "'";
        return false;
      }
    }

    for (size_t i = 0; i < chunks.size(); ++i) {
      pthread_join(threads[i], NULL);
      if (!args[i].success && success) {
        *error = args[i].error;
        success = false;
      }
    }

    if (!success) {
      for (size_t i = 0; i < chunk_paths.size(); ++i) {
        if (!chunk_paths[i].empty()) unlink(chunk_paths[i].c_str());
      }
      return false;
    }
  } else {
    // Single chunk: fetch directly with the caller's fetcher
    if (!fetcher->Fetch(chunks.AtPtr(0)->content_hash(),
                        &chunk_paths[0], error)) {
      return false;
    }
  }

  // Assemble fetched chunks into a single temp file in order
  FILE *assembled = CreateTempFile(tmp_dir + "/create_tarball_chunked_"
                                   + StringifyInt(entry.sequence),
                                   0600, "w", payload_path);
  if (assembled == NULL) {
    for (size_t i = 0; i < chunk_paths.size(); ++i) {
      if (!chunk_paths[i].empty()) unlink(chunk_paths[i].c_str());
    }
    *error = "failed to create temporary payload for '" + entry.repo_path + "'";
    return false;
  }

  for (size_t i = 0; i < chunk_paths.size(); ++i) {
    if (!CopyPathToFile(chunk_paths[i], assembled, error)) {
      unlink(chunk_paths[i].c_str());
      for (size_t j = i + 1; j < chunk_paths.size(); ++j) {
        if (!chunk_paths[j].empty()) unlink(chunk_paths[j].c_str());
      }
      fclose(assembled);
      unlink(payload_path->c_str());
      payload_path->clear();
      return false;
    }
    unlink(chunk_paths[i].c_str());
  }

  fclose(assembled);
  return true;
}


bool PreparePayload(catalog::SimpleCatalogManager *catalog_manager,
                    PayloadFetcher *fetcher,
                    const FetcherConfig &fetcher_config,
                    const TarEntry &entry,
                    const string &tmp_dir,
                    string *payload_path,
                    unsigned char **mem_data,
                    size_t *mem_size,
                    string *error) {
  *mem_data = NULL;
  *mem_size = 0;
  if (!entry.dirent.IsRegular() || entry.is_hardlink) {
    payload_path->clear();
    return true;
  }
  if (entry.dirent.IsChunkedFile()) {
    return PrepareChunkedPayload(catalog_manager, fetcher, fetcher_config,
                                 entry, tmp_dir, payload_path, error);
  }
  if (entry.dirent.checksum().IsNull()) {
    *error = "regular file has no content hash: " + entry.repo_path;
    return false;
  }
  // Use in-memory path for small files to avoid temp file round-trip
  if (entry.dirent.size() <= kSmallFileThreshold) {
    return fetcher->FetchMem(entry.dirent.checksum(), mem_data, mem_size,
                             error);
  }
  return fetcher->Fetch(entry.dirent.checksum(), payload_path, error);
}


bool PopulateArchiveEntry(const TarEntry &entry,
                          struct archive_entry *archive_entry,
                          string *error) {
  archive_entry_clear(archive_entry);
  archive_entry_set_pathname(archive_entry, entry.tar_path.c_str());
  archive_entry_set_uid(archive_entry, entry.dirent.uid());
  archive_entry_set_gid(archive_entry, entry.dirent.gid());
  archive_entry_set_mode(archive_entry, entry.dirent.mode());
  archive_entry_set_mtime(archive_entry, entry.dirent.mtime(),
                          entry.dirent.HasMtimeNs()
                              ? entry.dirent.mtime_ns()
                              : 0);

  if (entry.is_hardlink) {
    archive_entry_set_size(archive_entry, 0);
    archive_entry_copy_hardlink(archive_entry, entry.hardlink_target.c_str());
    return true;
  }

  if (entry.dirent.IsRegular()) {
    archive_entry_set_size(archive_entry, entry.dirent.size());
    return true;
  }
  if (entry.dirent.IsDirectory()) {
    archive_entry_set_size(archive_entry, 0);
    return true;
  }
  if (entry.dirent.IsLink()) {
    archive_entry_set_size(archive_entry, 0);
    archive_entry_copy_symlink(archive_entry,
                               entry.dirent.symlink().ToString().c_str());
    return true;
  }
  if (entry.dirent.IsFifo() || entry.dirent.IsBlockDev() || entry.dirent.IsCharDev()) {
    archive_entry_set_size(archive_entry, 0);
    archive_entry_set_rdev(archive_entry, entry.dirent.rdev());
    return true;
  }

  *error = "unsupported entry type in tar export: " + entry.repo_path;
  return false;
}


class PayloadPipeline {
 public:
  PayloadPipeline(const FetcherConfig &config,
                  catalog::SimpleCatalogManager *catalog_manager,
                  const string &tmp_dir,
                  const unsigned num_workers,
                  const unsigned queue_limit)
      : config_(config)
      , catalog_manager_(catalog_manager)
      , tmp_dir_(tmp_dir)
      , queue_(queue_limit)
      , started_(false)
      , active_workers_(0)
      , cancelled_(false) {
    pthread_mutex_init(&lock_, NULL);
    pthread_cond_init(&results_ready_, NULL);
    workers_.resize(num_workers);
  }

  ~PayloadPipeline() {
    Stop();
    CleanupPendingResults();
    pthread_cond_destroy(&results_ready_);
    pthread_mutex_destroy(&lock_);
  }

  bool Start(string *error) {
    if (started_) {
      return true;
    }
    started_ = true;
    const unsigned num_workers = workers_.size();
    for (unsigned i = 0; i < num_workers; ++i) {
      const int retval = pthread_create(&workers_[i], NULL,
                                        &PayloadPipeline::MainWorker, this);
      if (retval != 0) {
        *error = "failed to start payload worker threads";
        Cancel();
        Stop();
        return false;
      }
      ++active_workers_;
    }
    return true;
  }

  void Schedule(TarEntry *entry) { queue_.EnqueueBack(new FetchTask(entry)); }

  FetchResult *WaitFor(const size_t sequence) {
    pthread_mutex_lock(&lock_);
    while (results_.find(sequence) == results_.end()) {
      pthread_cond_wait(&results_ready_, &lock_);
    }
    FetchResult *result = results_[sequence];
    results_.erase(sequence);
    pthread_mutex_unlock(&lock_);
    return result;
  }

  void Cancel() {
    pthread_mutex_lock(&lock_);
    cancelled_ = true;
    pthread_cond_broadcast(&results_ready_);
    pthread_mutex_unlock(&lock_);
  }

  void Stop() {
    if (!started_) {
      return;
    }
    for (unsigned i = 0; i < active_workers_; ++i) {
      queue_.EnqueueBack(new FetchTask());
    }
    for (unsigned i = 0; i < active_workers_; ++i) {
      pthread_join(workers_[i], NULL);
    }
    active_workers_ = 0;
    started_ = false;
  }

 private:
  struct FetchTask {
    FetchTask() : entry(NULL) { }
    explicit FetchTask(TarEntry *entry) : entry(entry) { }
    bool IsQuit() const { return entry == NULL; }
    TarEntry *entry;
  };

  static void *MainWorker(void *data) {
    PayloadPipeline *pipeline = reinterpret_cast<PayloadPipeline *>(data);
    pipeline->RunWorker();
    return NULL;
  }

  void RunWorker() {
    UniquePtr<PayloadFetcher> fetcher(CreatePayloadFetcher(config_));

    while (true) {
      FetchTask *task = queue_.PopFront();
      if (task->IsQuit()) {
        delete task;
        return;
      }

      FetchResult *result = new FetchResult();
      result->sequence = task->entry->sequence;
      if (!IsCancelled()) {
        result->success = PreparePayload(catalog_manager_, fetcher.weak_ref(),
                                         config_,
                                         *task->entry, tmp_dir_,
                                         &result->payload_path,
                                         &result->mem_data, &result->mem_size,
                                         &result->error);
      } else {
        result->success = false;
        result->error = "cancelled";
      }

      pthread_mutex_lock(&lock_);
      results_[result->sequence] = result;
      pthread_cond_broadcast(&results_ready_);
      pthread_mutex_unlock(&lock_);
      delete task;
    }
  }

  bool IsCancelled() {
    pthread_mutex_lock(&lock_);
    const bool cancelled = cancelled_;
    pthread_mutex_unlock(&lock_);
    return cancelled;
  }

  void CleanupPendingResults() {
    pthread_mutex_lock(&lock_);
    map<size_t, FetchResult *>::iterator i = results_.begin();
    for (; i != results_.end(); ++i) {
      if (!i->second->payload_path.empty()) {
        unlink(i->second->payload_path.c_str());
      }
      delete i->second;
    }
    results_.clear();
    pthread_mutex_unlock(&lock_);
  }

  const FetcherConfig config_;
  catalog::SimpleCatalogManager *catalog_manager_;
  const string tmp_dir_;
  Tube<FetchTask> queue_;
  vector<pthread_t> workers_;
  bool started_;
  unsigned active_workers_;

  pthread_mutex_t lock_;
  pthread_cond_t results_ready_;
  bool cancelled_;
  map<size_t, FetchResult *> results_;
};


bool WriteTarArchive(const string &output_path,
                     vector<TarEntry> *entries,
                     const FetcherConfig &fetcher_config,
                     catalog::SimpleCatalogManager *catalog_manager,
                     const string &tmp_dir,
                     string *error) {
  struct archive *archive = archive_write_new();
  if (archive == NULL) {
    *error = "failed to allocate libarchive writer";
    return false;
  }

  if ((archive_write_add_filter_none(archive) != ARCHIVE_OK)
      || (archive_write_set_format_pax_restricted(archive) != ARCHIVE_OK)
      || (archive_write_open_filename(archive, output_path.c_str()) != ARCHIVE_OK)) {
    *error = string("failed to open output tarball: ")
             + archive_error_string(archive);
    archive_write_free(archive);
    return false;
  }

  struct archive_entry *archive_entry = archive_entry_new();
  if (archive_entry == NULL) {
    *error = "failed to allocate archive entry";
    archive_write_free(archive);
    return false;
  }

  size_t payload_jobs = 0;
  for (size_t i = 0; i < entries->size(); ++i) {
    if ((*entries)[i].requires_payload) {
      ++payload_jobs;
    }
  }

  const unsigned num_workers = std::min(
      static_cast<unsigned>(payload_jobs),
      std::min(GetNumberOfCpuCores(), kMaxExportWorkers));
  const unsigned queue_limit =
      std::max(1U, num_workers * kWorkerQueueFactor);
  UniquePtr<PayloadPipeline> pipeline;
  if (num_workers > 0) {
    pipeline = new PayloadPipeline(fetcher_config, catalog_manager, tmp_dir,
                                   num_workers, queue_limit);
    if (!pipeline->Start(error)) {
      archive_entry_free(archive_entry);
      archive_write_free(archive);
      return false;
    }
  }

  size_t next_to_schedule = 0;
  size_t inflight = 0;
  bool success = true;
  for (size_t i = 0; i < entries->size(); ++i) {
    while ((pipeline.IsValid()) && (inflight < queue_limit)
           && (next_to_schedule < entries->size())) {
      TarEntry *candidate = &(*entries)[next_to_schedule++];
      if (!candidate->requires_payload || candidate->scheduled) {
        continue;
      }
      candidate->scheduled = true;
      pipeline->Schedule(candidate);
      ++inflight;
    }

    TarEntry &entry = (*entries)[i];
    string payload_path;
    unsigned char *mem_data = NULL;
    size_t mem_size = 0;
    FetchResult *result = NULL;
    if (entry.requires_payload) {
      result = pipeline->WaitFor(entry.sequence);
      --inflight;
      if (!result->success) {
        *error = result->error;
        delete result;
        success = false;
        break;
      }
      payload_path = result->payload_path;
      mem_data = result->mem_data;
      mem_size = result->mem_size;
      // Prevent FetchResult destructor from freeing mem_data we took
      result->mem_data = NULL;
      result->mem_size = 0;
    }

    if (!PopulateArchiveEntry(entry, archive_entry, error)) {
      if (!payload_path.empty()) unlink(payload_path.c_str());
      free(mem_data);
      delete result;
      success = false;
      break;
    }
    if (archive_write_header(archive, archive_entry) != ARCHIVE_OK) {
      *error = string("failed to write tar header for '") + entry.tar_path
               + "': " + archive_error_string(archive);
      if (!payload_path.empty()) unlink(payload_path.c_str());
      free(mem_data);
      delete result;
      success = false;
      break;
    }

    // Write payload: prefer in-memory data, fall back to file path
    bool write_ok = true;
    if (mem_data != NULL) {
      la_ssize_t bytes_written = archive_write_data(archive, mem_data,
                                                     mem_size);
      if (bytes_written < 0
          || static_cast<size_t>(bytes_written) != mem_size) {
        *error = string("failed to write tar payload from memory: ")
                 + archive_error_string(archive);
        write_ok = false;
      }
      free(mem_data);
      mem_data = NULL;
    } else if (!payload_path.empty()) {
      if (!WriteArchiveDataFromPath(archive, payload_path, error)) {
        write_ok = false;
      }
      unlink(payload_path.c_str());
    }

    if (!write_ok) {
      delete result;
      success = false;
      break;
    }

    delete result;
  }

  if (pipeline.IsValid()) {
    if (!success) {
      pipeline->Cancel();
    }
    pipeline->Stop();
  }

  archive_entry_free(archive_entry);
  const int close_result = archive_write_close(archive);
  const int free_result = archive_write_free(archive);
  if (success && ((close_result != ARCHIVE_OK) || (free_result != ARCHIVE_OK))) {
    *error = "failed to finalize tar archive";
    return false;
  }
  return success;
}

}  // namespace

namespace swissknife {

ParameterList CommandCreateTarball::GetParams() const {
  ParameterList r;
  r.push_back(Parameter::Mandatory(
      'r', "repository URL (absolute local path or remote URL)"));
  r.push_back(Parameter::Mandatory('p', "repository subpath to export"));
  r.push_back(Parameter::Mandatory('o', "output tarball path"));
  r.push_back(Parameter::Optional('n', "fully qualified repository name"));
  r.push_back(Parameter::Optional(
      'k', "repository public key(s) / dir; required for remote trunk discovery"));
  r.push_back(Parameter::Optional('l', "temporary directory"));
  r.push_back(Parameter::Optional('h', "root hash (other than trunk)"));
  r.push_back(Parameter::Optional('@', "proxy url"));
  r.push_back(Parameter::Switch('L', "follow HTTP redirects"));
  return r;
}


int CommandCreateTarball::Main(const ArgumentList &args) {
  const string repository_arg = *args.find('r')->second;
  string subpath = MakeCanonicalPath(*args.find('p')->second);
  const string output_path = MakeCanonicalPath(*args.find('o')->second);
  const string repo_name = (args.count('n') > 0) ? *args.find('n')->second : "";
  string actual_repo_name = repo_name;
  string repo_keys = (args.count('k') > 0) ? *args.find('k')->second : "";
  const string tmp_dir = (args.count('l') > 0)
                             ? MakeCanonicalPath(*args.find('l')->second)
                             : "/tmp";
  const bool follow_redirects = (args.count('L') > 0);
  const string proxy = (args.count('@') > 0) ? *args.find('@')->second : "";

  if (DirectoryExists(repo_keys)) {
    repo_keys = JoinStrings(FindFilesBySuffix(repo_keys, ".pub"), ":");
  }
  if (!DirectoryExists(tmp_dir)) {
    LogCreateTarballError("temporary directory does not exist");
    return 1;
  }
  if (subpath.empty()) {
    subpath = "/";
  } else if (subpath[0] != '/') {
    subpath = "/" + subpath;
  }
  const string output_parent = GetParentPath(output_path);
  if (!output_parent.empty() && !DirectoryExists(output_parent)) {
    LogCreateTarballError("output parent directory does not exist");
    return 1;
  }

  const bool is_remote = IsHttpUrl(repository_arg);
  const bool is_file_url = HasPrefix(repository_arg, "file://", false);
  const string local_repository = is_remote
                                      ? ""
                                      : (is_file_url ? repository_arg.substr(7)
                                                     : MakeCanonicalPath(
                                                           repository_arg));
  if (!is_remote && !DirectoryExists(local_repository)) {
    LogCreateTarballError("local repository path does not exist");
    return 1;
  }
  if (is_remote && repo_name.empty() && (args.count('h') == 0)) {
    LogCreateTarballError(
        "remote repositories require -n when discovering the trunk root hash");
    return 1;
  }
  if (is_remote && (args.count('h') == 0) && repo_keys.empty()) {
    LogCreateTarballError(
        "remote repositories require -k when discovering the trunk root hash");
    return 1;
  }

  shash::Any root_hash;
  if (args.count('h') > 0) {
    root_hash = shash::MkFromHexPtr(shash::HexPtr(*args.find('h')->second),
                                    shash::kSuffixCatalog);
    if (root_hash.IsNull()) {
      LogCreateTarballError("invalid root hash");
      return 1;
    }
  }

  if (!InitDownloadManager(follow_redirects, proxy)) {
    LogCreateTarballError("failed to initialize download manager");
    return 1;
  }

  if (is_remote && !InitSignatureManager(repo_keys)) {
    LogCreateTarballError("failed to initialize signature manager");
    return 1;
  }

  if (root_hash.IsNull()) {
    if (is_remote) {
      const UniquePtr<manifest::Manifest> manifest(
          FetchRemoteManifest(repository_arg, repo_name));
      if (!manifest.IsValid()) {
        LogCreateTarballError("failed to load repository manifest");
        return 1;
      }
      actual_repo_name = manifest->repository_name();
      root_hash = manifest->catalog_hash();
    } else {
      const UniquePtr<manifest::Manifest> manifest(
          OpenLocalManifest(local_repository + "/.cvmfspublished"));
      if (!manifest.IsValid()) {
        LogCreateTarballError("failed to load local repository manifest");
        return 1;
      }
      actual_repo_name = manifest->repository_name();
      root_hash = manifest->catalog_hash();
    }
  } else if (!is_remote && actual_repo_name.empty()) {
    const UniquePtr<manifest::Manifest> manifest(
        OpenLocalManifest(local_repository + "/.cvmfspublished"));
    if (manifest.IsValid()) {
      actual_repo_name = manifest->repository_name();
    }
  }

  const string catalog_url = is_remote ? repository_arg : "file://" + local_repository;
  catalog::SimpleCatalogManager catalog_manager(
      root_hash, catalog_url, tmp_dir, download_manager(), statistics(),
      true /* manage_catalog_files */);
  if (!catalog_manager.Init()) {
    LogCreateTarballError("failed to initialize read-only catalog manager");
    return 1;
  }

  catalog::DirectoryEntry entry;
  const bool found = (subpath == "/")
      ? catalog_manager.LookupPath(PathString(), catalog::kLookupDefault,
                                   &entry)
      : catalog_manager.LookupPath(subpath, catalog::kLookupDefault, &entry);
  if (!found) {
    LogCreateTarballError("requested repository subpath does not exist");
    return 1;
  }

  vector<TarEntry> entries;
  map<uint32_t, string> hardlink_targets;
  const string root_tar_path = entry.IsDirectory() ? "" : GetFileName(StripLeadingSlash(subpath));
  const bool emit_root_entry = !entry.IsDirectory();
  string error;
  if (!EnumerateSubtree(&catalog_manager, subpath, root_tar_path, entry,
                        emit_root_entry, &hardlink_targets, &entries, &error)) {
    LogCreateTarballError(error);
    return 1;
  }

  FetcherConfig fetcher_config;
  fetcher_config.type = is_remote ? FetcherConfig::kHttp : FetcherConfig::kLocal;
  fetcher_config.local_repository = local_repository;
  fetcher_config.repository_url = repository_arg;
  fetcher_config.repository_name = actual_repo_name;
  fetcher_config.tmp_dir = tmp_dir;
  fetcher_config.download_manager = download_manager();
  if (is_remote) {
    fetcher_config.signature_manager = signature_manager();
  }
  if (!WriteTarArchive(output_path, &entries, fetcher_config, &catalog_manager,
                       tmp_dir, &error)) {
    LogCreateTarballError(error);
    return 1;
  }

  LogCvmfs(kLogCvmfs, kLogStdout,
           "Exported %zu entries from %s at root hash %s to %s",
           entries.size(), subpath.c_str(), root_hash.ToString().c_str(),
           output_path.c_str());
  return 0;
}

}  // namespace swissknife
