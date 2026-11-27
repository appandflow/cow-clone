#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winioctl.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cow-clone.h"

#pragma comment(lib, "advapi32.lib")

#define SETTABLE_ATTRIBUTES                                                                                            \
  (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |                  \
   FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)

struct path {
  wchar_t *text;
  size_t length, capacity;
};

struct entry {
  DWORD attributes;
  FILETIME created, accessed, written;
};

#define MAX_THREADS 32
#define QUEUE_LIMIT 256

/* A regular file the threads clone, or a directory whose times and attributes are set once its files are in. */
struct job {
  wchar_t *source;
  wchar_t *destination;
  struct entry entry;
  /* Files: whether the destination's directories have integrity streams, which new files and directories inherit
   * from the destination's parent. */
  int integrity;
};

struct jobs {
  struct job *items;
  size_t count, capacity;
};

struct walker {
  struct failure *failure;
  struct path source, destination;
  int created, probed;
  /* Integrity streams on the destination's parent, which every directory the walk makes inherits. */
  int integrity;
  LONGLONG cluster;
  struct jobs files, directories;
  /* The walk queues files as it finds them and the threads clone them meanwhile, and the walk clones some itself
   * when the queue gets long, so a file is cloned soon after its directory was made: cloning them long after, as
   * after the whole walk, was more than three times slower on ReFS. */
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE queued;
  size_t next, wanted_threads;
  DWORD started;
  HANDLE threads[MAX_THREADS];
  int failed, walked;
};

static BYTE reparse[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];

static void path_set(struct path *path, const wchar_t *text) {
  path->length = wcslen(text);
  path->capacity = path->length + 256;
  path->text = malloc(path->capacity * sizeof *path->text);
  if (!path->text) out_of_memory();
  memcpy(path->text, text, (path->length + 1) * sizeof *path->text);
}

static size_t path_push(struct path *path, const wchar_t *name) {
  size_t before = path->length, length = wcslen(name);
  if (before + length + 2 > path->capacity) {
    path->capacity = (before + length + 2) * 2;
    path->text = realloc(path->text, path->capacity * sizeof *path->text);
    if (!path->text) out_of_memory();
  }
  path->text[before] = L'\\';
  memcpy(path->text + before + 1, name, (length + 1) * sizeof *path->text);
  path->length = before + length + 1;
  return before;
}

static void path_pop(struct path *path, size_t before) {
  path->length = before;
  path->text[before] = L'\0';
}

static wchar_t *widen(const char *utf8) {
  int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
  wchar_t *wide = malloc((size_t)(size > 0 ? size : 1) * sizeof *wide);
  if (!wide) out_of_memory();
  if (size <= 0 || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, size)) wide[0] = L'\0';
  return wide;
}

/* An absolute path with the \\?\ prefix, which lifts the MAX_PATH limit:
 * https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation */
static wchar_t *extended_path(const char *utf8) {
  wchar_t *wide = widen(utf8);
  if (wcsncmp(wide, L"\\\\?\\", 4) == 0) return wide;
  DWORD size = GetFullPathNameW(wide, 0, NULL, NULL);
  wchar_t *full = malloc(((size_t)size + 1) * sizeof *full);
  wchar_t *result = malloc(((size_t)size + 9) * sizeof *result);
  if (!full || !result) out_of_memory();
  if (size == 0 || GetFullPathNameW(wide, size + 1, full, NULL) == 0) {
    free(full);
    free(result);
    return wide;
  }
  if (wcsncmp(full, L"\\\\", 2) == 0)
    swprintf(result, (size_t)size + 9, L"\\\\?\\UNC\\%ls", full + 2);
  else
    swprintf(result, (size_t)size + 9, L"\\\\?\\%ls", full);
  free(full);
  free(wide);
  size_t length = wcslen(result);
  while (length > 7 && result[length - 1] == L'\\' && result[length - 2] != L':') result[--length] = L'\0';
  return result;
}

static char *display_path(const wchar_t *wide) {
  if (wcsncmp(wide, L"\\\\?\\UNC\\", 8) == 0)
    wide += 7;
  else if (wcsncmp(wide, L"\\\\?\\", 4) == 0)
    wide += 4;
  int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  char *utf8 = malloc((size_t)(size > 0 ? size : 1));
  if (!utf8) out_of_memory();
  if (size <= 0 || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, size, NULL, NULL)) utf8[0] = '\0';
  if (wide[0] == L'\\' && wide[1] != L'\\' && size > 0) {
    /* Put back the leading backslash of a UNC path that the prefix took. */
    char *unc = malloc((size_t)size + 1);
    if (!unc) out_of_memory();
    unc[0] = '\\';
    memcpy(unc + 1, utf8, (size_t)size);
    free(utf8);
    return unc;
  }
  return utf8;
}

static const struct {
  DWORD error;
  const char *code;
} error_codes[] = {
    {ERROR_FILE_NOT_FOUND, "ENOENT"},    {ERROR_PATH_NOT_FOUND, "ENOENT"},     {ERROR_ACCESS_DENIED, "EACCES"},
    {ERROR_SHARING_VIOLATION, "EBUSY"},  {ERROR_PRIVILEGE_NOT_HELD, "EPERM"},  {ERROR_FILE_EXISTS, "EEXIST"},
    {ERROR_ALREADY_EXISTS, "EEXIST"},    {ERROR_NOT_SAME_DEVICE, "EXDEV"},     {ERROR_INVALID_FUNCTION, "ENOTSUP"},
    {ERROR_NOT_SUPPORTED, "ENOTSUP"},    {ERROR_DISK_FULL, "ENOSPC"},          {ERROR_HANDLE_DISK_FULL, "ENOSPC"},
    {ERROR_INVALID_PARAMETER, "EINVAL"}, {ERROR_FILENAME_EXCED_RANGE, "ENAMETOOLONG"},
    {ERROR_DIRECTORY, "ENOTDIR"},        {ERROR_WRITE_PROTECT, "EROFS"},
};

static int fail_at(struct failure *failure, DWORD error, enum reason reason, const wchar_t *path) {
  failure->reason = reason;
  snprintf(failure->code, sizeof failure->code, "WIN32_%lu", (unsigned long)error);
  for (size_t i = 0; i < sizeof error_codes / sizeof *error_codes; i++)
    if (error_codes[i].error == error) {
      snprintf(failure->code, sizeof failure->code, "%s", error_codes[i].code);
      break;
    }
  wchar_t message[256];
  DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, message,
                                sizeof message / sizeof *message, NULL);
  while (length > 0 && (message[length - 1] == L'\r' || message[length - 1] == L'\n' || message[length - 1] == L'.'))
    message[--length] = L'\0';
  if (length == 0 ||
      !WideCharToMultiByte(CP_UTF8, 0, message, -1, failure->message, sizeof failure->message, NULL, NULL))
    snprintf(failure->message, sizeof failure->message, "Windows error %lu", (unsigned long)error);
  free(failure->path);
  failure->path = path ? display_path(path) : NULL;
  return 0;
}

/* The walk's failure, unless a thread failed first. */
static int fail(struct walker *walker, DWORD error, enum reason reason, const struct path *path) {
  EnterCriticalSection(&walker->lock);
  if (!walker->failed) {
    walker->failed = 1;
    fail_at(walker->failure, error, reason, path ? path->text : NULL);
  }
  LeaveCriticalSection(&walker->lock);
  return 0;
}

/* FSCTL_DUPLICATE_EXTENTS_TO_FILE fails with these on a volume without block cloning, such as NTFS. */
static enum reason clone_reason(DWORD error) {
  if (error == ERROR_NOT_SAME_DEVICE) return REASON_CROSS_DEVICE;
  if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED) return REASON_UNSUPPORTED;
  return REASON_FAILED;
}

static int control(HANDLE handle, DWORD code, void *input, DWORD input_size, void *output, DWORD output_size) {
  DWORD bytes;
  return DeviceIoControl(handle, code, input, input_size, output, output_size, &bytes, NULL);
}

static wchar_t *copy(const wchar_t *text) {
  size_t length = wcslen(text) + 1;
  wchar_t *result = malloc(length * sizeof *result);
  if (!result) out_of_memory();
  memcpy(result, text, length * sizeof *result);
  return result;
}

static void add(struct jobs *jobs, struct job job) {
  if (jobs->count == jobs->capacity) {
    jobs->capacity = jobs->capacity ? jobs->capacity * 2 : 1024;
    jobs->items = realloc(jobs->items, jobs->capacity * sizeof *jobs->items);
    if (!jobs->items) out_of_memory();
  }
  jobs->items[jobs->count++] = job;
}

static void release(struct jobs *jobs) {
  for (size_t i = 0; i < jobs->count; i++) {
    free(jobs->items[i].source);
    free(jobs->items[i].destination);
  }
  free(jobs->items);
}

/* Sets times and attributes in one call on an open handle. A ChangeTime of 0 leaves it alone, and so do attributes of
 * 0, so a directory keeps its directory attribute. */
static BOOL set_basic(HANDLE handle, const struct entry *entry, DWORD fallback) {
  FILE_BASIC_INFO basic;
  basic.CreationTime.LowPart = entry->created.dwLowDateTime;
  basic.CreationTime.HighPart = (LONG)entry->created.dwHighDateTime;
  basic.LastAccessTime.LowPart = entry->accessed.dwLowDateTime;
  basic.LastAccessTime.HighPart = (LONG)entry->accessed.dwHighDateTime;
  basic.LastWriteTime.LowPart = entry->written.dwLowDateTime;
  basic.LastWriteTime.HighPart = (LONG)entry->written.dwHighDateTime;
  basic.ChangeTime.QuadPart = 0;
  basic.FileAttributes = entry->attributes & SETTABLE_ATTRIBUTES ? entry->attributes & SETTABLE_ATTRIBUTES : fallback;
  return SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof basic);
}

/* Block cloning on ReFS: regions must start and end on a cluster boundary, each call must clone less than 4 GiB, the
 * destination must already be as long as the data, and both files must have the same integrity-stream setting and
 * be sparse when the source is. https://learn.microsoft.com/en-us/windows/win32/fileio/block-cloning
 * Like reflink-copy (github.com/cargo-bins/reflink-copy, src/sys/windows_impl.rs), the last region is rounded up to a
 * whole cluster past the end of the file. Integrity settings are only touched when the source or the destination's
 * directory has them. */
static int clone_file(struct failure *failure, const struct job *job, LONGLONG *cluster) {
  HANDLE source = CreateFileW(job->source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, 0, NULL);
  if (source == INVALID_HANDLE_VALUE) return fail_at(failure, GetLastError(), REASON_FAILED, job->source);
  /* The times and attributes FindFirstFileExW reports come from the parent directory's index, which can be stale;
   * the file's own are not. */
  FILE_BASIC_INFO basic;
  FILE_STANDARD_INFO standard;
  if (!GetFileInformationByHandleEx(source, FileBasicInfo, &basic, sizeof basic) ||
      !GetFileInformationByHandleEx(source, FileStandardInfo, &standard, sizeof standard)) {
    DWORD error = GetLastError();
    CloseHandle(source);
    return fail_at(failure, error, REASON_FAILED, job->source);
  }
  HANDLE target = CreateFileW(job->destination, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
  if (target == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    CloseHandle(source);
    return fail_at(failure, error, REASON_FAILED, job->destination);
  }
  DWORD error = 0;
  enum reason reason = REASON_FAILED;
  int integrity_streams = (basic.FileAttributes & FILE_ATTRIBUTE_INTEGRITY_STREAM) || job->integrity;
  if (integrity_streams || *cluster == 0) {
    /* The first file of a pair reads the volume's cluster size here, before any thread starts. */
    FSCTL_GET_INTEGRITY_INFORMATION_BUFFER integrity;
    if (!control(source, FSCTL_GET_INTEGRITY_INFORMATION, NULL, 0, &integrity, sizeof integrity)) {
      error = GetLastError();
      reason = clone_reason(error);
    } else {
      if (*cluster == 0) *cluster = integrity.ClusterSizeInBytes ? integrity.ClusterSizeInBytes : 4096;
      FSCTL_SET_INTEGRITY_INFORMATION_BUFFER same = {integrity.ChecksumAlgorithm, 0, integrity.Flags};
      /* Dev Drive refuses this call; the clone still works there (reflink-copy, same file). */
      if (integrity_streams) control(target, FSCTL_SET_INTEGRITY_INFORMATION, &same, sizeof same, NULL, 0);
    }
  }
  /* Sparse while the end of file moves, so ReFS does not allocate the clusters the clone then replaces; measured on
   * Windows Server 2025, a dense destination allocated every cluster. */
  if (!error && !control(target, FSCTL_SET_SPARSE, NULL, 0, NULL, 0)) error = GetLastError();
  FILE_END_OF_FILE_INFO end;
  end.EndOfFile = standard.EndOfFile;
  if (!error && !SetFileInformationByHandle(target, FileEndOfFileInfo, &end, sizeof end)) error = GetLastError();
  LONGLONG total = error ? 0 : (standard.EndOfFile.QuadPart + *cluster - 1) / *cluster * *cluster;
  const LONGLONG chunk = 1LL << 30;
  for (LONGLONG offset = 0; !error && offset < total; offset += chunk) {
    DUPLICATE_EXTENTS_DATA extents;
    extents.FileHandle = source;
    extents.SourceFileOffset.QuadPart = offset;
    extents.TargetFileOffset.QuadPart = offset;
    extents.ByteCount.QuadPart = total - offset < chunk ? total - offset : chunk;
    if (!control(target, FSCTL_DUPLICATE_EXTENTS_TO_FILE, &extents, sizeof extents, NULL, 0)) {
      error = GetLastError();
      reason = clone_reason(error);
    }
  }
  if (!error && !(basic.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE)) {
    FILE_SET_SPARSE_BUFFER dense = {FALSE};
    if (!control(target, FSCTL_SET_SPARSE, &dense, sizeof dense, NULL, 0)) error = GetLastError();
  }
  struct entry entry = {basic.FileAttributes, {0}, {0}, {0}};
  entry.created.dwLowDateTime = basic.CreationTime.LowPart;
  entry.created.dwHighDateTime = (DWORD)basic.CreationTime.HighPart;
  entry.accessed.dwLowDateTime = basic.LastAccessTime.LowPart;
  entry.accessed.dwHighDateTime = (DWORD)basic.LastAccessTime.HighPart;
  entry.written.dwLowDateTime = basic.LastWriteTime.LowPart;
  entry.written.dwHighDateTime = (DWORD)basic.LastWriteTime.HighPart;
  if (!error && !set_basic(target, &entry, FILE_ATTRIBUTE_NORMAL)) error = GetLastError();
  if (error) {
    FILE_DISPOSITION_INFO remove = {TRUE};
    SetFileInformationByHandle(target, FileDispositionInfo, &remove, sizeof remove);
  }
  CloseHandle(target);
  CloseHandle(source);
  return error ? fail_at(failure, error, reason, job->destination) : 1;
}

/* Symbolic links and junctions are copied as their reparse data. Setting a symbolic link's data needs the
 * create-symbolic-link privilege, which elevated processes and Developer Mode grant:
 * https://learn.microsoft.com/en-us/windows/win32/fileio/creating-symbolic-links */
static void enable_symlink_privilege(void) {
  static int done;
  if (done) return;
  done = 1;
  /* FSCTL_SET_REPARSE_POINT checks that the privilege is enabled, not only held; CreateSymbolicLinkW enables it
   * itself, a raw control code does not. */
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) return;
  TOKEN_PRIVILEGES privileges;
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (LookupPrivilegeValueW(NULL, SE_CREATE_SYMBOLIC_LINK_NAME, &privileges.Privileges[0].Luid))
    AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL);
  CloseHandle(token);
}

static int clone_link(struct walker *walker, const struct entry *entry, DWORD length, int depth) {
  DWORD tag;
  memcpy(&tag, reparse, sizeof tag);
  if (tag == IO_REPARSE_TAG_SYMLINK) enable_symlink_privilege();
  if (entry->attributes & FILE_ATTRIBUTE_DIRECTORY) {
    if (!CreateDirectoryW(walker->destination.text, NULL))
      return fail(walker, GetLastError(), REASON_FAILED, &walker->destination);
  } else {
    HANDLE file = CreateFileW(walker->destination.text, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return fail(walker, GetLastError(), REASON_FAILED, &walker->destination);
    CloseHandle(file);
  }
  if (depth == 0) walker->created = 1;
  HANDLE link = CreateFileW(walker->destination.text, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (link == INVALID_HANDLE_VALUE) return fail(walker, GetLastError(), REASON_FAILED, &walker->destination);
  int ok = control(link, FSCTL_SET_REPARSE_POINT, reparse, length, NULL, 0) &&
           SetFileTime(link, &entry->created, &entry->accessed, &entry->written);
  if (!ok) fail(walker, GetLastError(), REASON_FAILED, &walker->destination);
  CloseHandle(link);
  return ok;
}

static int plan_entry(struct walker *walker, const struct entry *entry, int depth);
static void record(struct walker *walker, struct failure *failure);
static DWORD WINAPI clone_files(void *argument);

static int plan_directory(struct walker *walker, const struct entry *listed, int depth) {
  /* A directory's times in its parent's index (what FindFirstFileExW reports) can lag behind its own, so read them
   * from the directory, before listing it. */
  struct job job = {NULL, NULL, *listed, 0};
  WIN32_FILE_ATTRIBUTE_DATA own;
  if (!GetFileAttributesExW(walker->source.text, GetFileExInfoStandard, &own))
    return fail(walker, GetLastError(), REASON_FAILED, &walker->source);
  job.entry.created = own.ftCreationTime;
  job.entry.accessed = own.ftLastAccessTime;
  job.entry.written = own.ftLastWriteTime;
  DWORD error;
  if (!CreateDirectoryW(walker->destination.text, NULL))
    return fail(walker, GetLastError(), REASON_FAILED, &walker->destination);
  if (depth == 0) walker->created = 1;
  job.destination = copy(walker->destination.text);
  add(&walker->directories, job);
  WIN32_FIND_DATAW data;
  size_t pattern = path_push(&walker->source, L"*");
  HANDLE find = FindFirstFileExW(walker->source.text, FindExInfoBasic, &data, FindExSearchNameMatch, NULL,
                                 FIND_FIRST_EX_LARGE_FETCH);
  error = GetLastError();
  path_pop(&walker->source, pattern);
  if (find == INVALID_HANDLE_VALUE) return fail(walker, error, REASON_FAILED, &walker->source);
  int ok = 1;
  for (;;) {
    if (wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0) {
      struct entry child = {data.dwFileAttributes, data.ftCreationTime, data.ftLastAccessTime, data.ftLastWriteTime};
      size_t source_before = path_push(&walker->source, data.cFileName);
      size_t before = path_push(&walker->destination, data.cFileName);
      ok = plan_entry(walker, &child, depth + 1);
      if (!ok) break;
      path_pop(&walker->source, source_before);
      path_pop(&walker->destination, before);
    }
    if (!FindNextFileW(find, &data)) {
      error = GetLastError();
      if (error != ERROR_NO_MORE_FILES) ok = fail(walker, error, REASON_FAILED, &walker->source);
      break;
    }
  }
  FindClose(find);
  return ok;
}

static int plan_entry(struct walker *walker, const struct entry *entry, int depth) {
  if (entry->attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    HANDLE handle = CreateFileW(walker->source.text, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) return fail(walker, GetLastError(), REASON_FAILED, &walker->source);
    DWORD length = 0;
    BOOL read = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0, reparse, sizeof reparse, &length, NULL);
    DWORD error = GetLastError();
    CloseHandle(handle);
    if (!read) return fail(walker, error, REASON_FAILED, &walker->source);
    DWORD tag;
    memcpy(&tag, reparse, sizeof tag);
    if (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT)
      return clone_link(walker, entry, length, depth);
  }
  if (entry->attributes & FILE_ATTRIBUTE_DIRECTORY) return plan_directory(walker, entry, depth);
  struct job job = {walker->source.text, walker->destination.text, *entry, walker->integrity};
  if (!walker->probed) {
    /* The first file is cloned at once, so a volume that cannot clone fails before the rest is planned. */
    walker->probed = 1;
    if (depth == 0) walker->created = 1;
    struct failure failure = {REASON_NONE, "", "", NULL};
    if (clone_file(&failure, &job, &walker->cluster)) return 1;
    record(walker, &failure);
    free(failure.path);
    return 0;
  }
  job.source = copy(job.source);
  job.destination = copy(job.destination);
  EnterCriticalSection(&walker->lock);
  add(&walker->files, job);
  LeaveCriticalSection(&walker->lock);
  WakeConditionVariable(&walker->queued);
  for (;;) {
    EnterCriticalSection(&walker->lock);
    int behind = !walker->failed && walker->files.count - walker->next > QUEUE_LIMIT;
    struct job next = {NULL, NULL, {0, {0, 0}, {0, 0}, {0, 0}}, 0};
    if (behind) next = walker->files.items[walker->next++];
    LeaveCriticalSection(&walker->lock);
    if (!behind) break;
    struct failure failure = {REASON_NONE, "", "", NULL};
    if (!clone_file(&failure, &next, &walker->cluster)) record(walker, &failure);
    free(failure.path);
  }
  /* A thread start costs more than cloning a few files, so the threads start once there are some to share. */
  if (walker->files.count == 64)
    while (walker->started + 1 < walker->wanted_threads &&
           (walker->threads[walker->started] = CreateThread(NULL, 0, clone_files, walker, 0, NULL)) != NULL)
      walker->started++;
  EnterCriticalSection(&walker->lock);
  int failed = walker->failed;
  LeaveCriticalSection(&walker->lock);
  return !failed;
}

/* Keeps a file's failure as the pair's, unless another came first. */
static void record(struct walker *walker, struct failure *failure) {
  EnterCriticalSection(&walker->lock);
  if (!walker->failed) {
    walker->failed = 1;
    free(walker->failure->path);
    *walker->failure = *failure;
    failure->path = NULL;
  }
  LeaveCriticalSection(&walker->lock);
  WakeAllConditionVariable(&walker->queued);
}

static DWORD WINAPI clone_files(void *argument) {
  struct walker *walker = argument;
  struct failure failure = {REASON_NONE, "", "", NULL};
  for (;;) {
    EnterCriticalSection(&walker->lock);
    while (!walker->failed && !walker->walked && walker->next == walker->files.count)
      SleepConditionVariableCS(&walker->queued, &walker->lock, INFINITE);
    if (walker->failed || walker->next == walker->files.count) {
      LeaveCriticalSection(&walker->lock);
      break;
    }
    /* A copy, since the walk can grow the array meanwhile. */
    struct job job = walker->files.items[walker->next++];
    LeaveCriticalSection(&walker->lock);
    if (!clone_file(&failure, &job, &walker->cluster)) record(walker, &failure);
  }
  free(failure.path);
  return 0;
}

static size_t thread_count(void) {
  /* Each clone mostly waits on ReFS. On a 4-processor runner, 32 threads cloned a 40,000-file node_modules in about
   * half the time of one thread and in less time than 16. */
  long count = 8 * (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  const char *setting = getenv("COW_CLONE_THREADS");
  if (setting && atoi(setting) > 0) count = atoi(setting);
  if (count < 1) count = 1;
  if (count > MAX_THREADS) count = MAX_THREADS;
  return (size_t)count;
}

static int finish_directories(struct walker *walker) {
  /* Deepest first: setting a directory's times after its parent's would not change the parent's, but its attributes
   * might be read-only. */
  for (size_t i = walker->directories.count; i-- > 0;) {
    const struct job *job = &walker->directories.items[i];
    HANDLE directory = CreateFileW(job->destination, FILE_WRITE_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (directory == INVALID_HANDLE_VALUE) return fail_at(walker->failure, GetLastError(), REASON_FAILED, job->destination);
    BOOL set = set_basic(directory, &job->entry, 0);
    DWORD error = GetLastError();
    CloseHandle(directory);
    if (!set) return fail_at(walker->failure, error, REASON_FAILED, job->destination);
  }
  return 1;
}

static int remove_tree(struct path *path) {
  DWORD attributes = GetFileAttributesW(path->text);
  if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
  if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path->text, attributes & ~FILE_ATTRIBUTE_READONLY);
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path->text) != 0;
  if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    WIN32_FIND_DATAW data;
    size_t pattern = path_push(path, L"*");
    HANDLE find = FindFirstFileExW(path->text, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, 0);
    path_pop(path, pattern);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        size_t before = path_push(path, data.cFileName);
        remove_tree(path);
        path_pop(path, before);
      } while (FindNextFileW(find, &data));
      FindClose(find);
    }
  }
  return RemoveDirectoryW(path->text) != 0;
}

/* The pair's volumes, checked once so a volume without block cloning fails before the walk. */
static int check_volumes(struct walker *walker) {
  HANDLE source = CreateFileW(walker->source.text, FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (source == INVALID_HANDLE_VALUE) return fail(walker, GetLastError(), REASON_FAILED, &walker->source);
  size_t slash = walker->destination.length;
  while (slash > 0 && walker->destination.text[slash - 1] != L'\\') slash--;
  struct path parent;
  path_set(&parent, walker->destination.text);
  if (slash > 0) path_pop(&parent, slash - 1);
  if (parent.length > 0 && parent.text[parent.length - 1] == L':') path_push(&parent, L"");
  HANDLE target = CreateFileW(parent.text, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  int ok = 1;
  if (target == INVALID_HANDLE_VALUE)
    ok = fail(walker, GetLastError(), REASON_FAILED, &walker->destination);
  else {
    BY_HANDLE_FILE_INFORMATION from, to;
    DWORD flags = 0;
    if (!GetFileInformationByHandle(source, &from) || !GetFileInformationByHandle(target, &to))
      ok = fail(walker, GetLastError(), REASON_FAILED, &walker->destination);
    else if (from.dwVolumeSerialNumber != to.dwVolumeSerialNumber)
      ok = fail(walker, ERROR_NOT_SAME_DEVICE, REASON_CROSS_DEVICE, &walker->destination);
    else if (!GetVolumeInformationByHandleW(target, NULL, 0, NULL, NULL, &flags, NULL, 0))
      ok = fail(walker, GetLastError(), REASON_FAILED, &walker->destination);
    else if (!(flags & FILE_SUPPORTS_BLOCK_REFCOUNTING)) {
      ok = fail(walker, ERROR_NOT_SUPPORTED, REASON_UNSUPPORTED, &walker->destination);
      snprintf(walker->failure->message, sizeof walker->failure->message,
               "The volume does not support block cloning (ReFS or a Dev Drive does)");
    } else {
      DWORD attributes = GetFileAttributesW(parent.text);
      walker->integrity = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_INTEGRITY_STREAM);
    }
    CloseHandle(target);
  }
  CloseHandle(source);
  free(parent.text);
  return ok;
}

int clone_pair(const char *source, const char *destination, struct failure *failure) {
  struct walker walker;
  memset(&walker, 0, sizeof walker);
  walker.failure = failure;
  InitializeCriticalSection(&walker.lock);
  InitializeConditionVariable(&walker.queued);
  wchar_t *wide = extended_path(source);
  path_set(&walker.source, wide);
  free(wide);
  wide = extended_path(destination);
  path_set(&walker.destination, wide);
  free(wide);
  int ok;
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (GetFileAttributesExW(walker.destination.text, GetFileExInfoStandard, &data))
    ok = fail(&walker, ERROR_FILE_EXISTS, REASON_FAILED, &walker.destination);
  else if (!GetFileAttributesExW(walker.source.text, GetFileExInfoStandard, &data))
    ok = fail(&walker, GetLastError(), REASON_FAILED, &walker.source);
  else if (check_volumes(&walker)) {
    struct entry entry = {data.dwFileAttributes, data.ftCreationTime, data.ftLastAccessTime, data.ftLastWriteTime};
    size_t root = walker.destination.length;
    walker.wanted_threads = thread_count();
    plan_entry(&walker, &entry, 0);
    EnterCriticalSection(&walker.lock);
    walker.walked = 1;
    LeaveCriticalSection(&walker.lock);
    WakeAllConditionVariable(&walker.queued);
    clone_files(&walker);
    if (walker.started) WaitForMultipleObjects(walker.started, walker.threads, TRUE, INFINITE);
    for (DWORD i = 0; i < walker.started; i++) CloseHandle(walker.threads[i]);
    ok = !walker.failed;
    if (ok) ok = finish_directories(&walker);
    /* A failure in the walk leaves the path at the entry that failed. */
    if (!ok && walker.created) {
      path_pop(&walker.destination, root);
      remove_tree(&walker.destination);
    }
  } else
    ok = 0;
  release(&walker.files);
  release(&walker.directories);
  DeleteCriticalSection(&walker.lock);
  free(walker.source.text);
  free(walker.destination.text);
  return ok;
}

#if defined(_M_X64) && !defined(_M_ARM64EC)
/* The npm and pnpm launchers of `cow-clone-windows` start the x64 binary. On an arm64 machine it starts the arm64
 * binary next to it, prebuilds\win32-arm64\cow-clone.exe, with the same arguments and standard handles, and returns
 * its exit status. IsWow64Process2 reports the machine's native architecture to an emulated x64 process:
 * https://learn.microsoft.com/en-us/windows/win32/api/wow64apiset/nf-wow64apiset-iswow64process2 */
typedef BOOL(WINAPI *wow64_function)(HANDLE, USHORT *, USHORT *);

static int run_native(DWORD *status) {
  wow64_function wow64 =
      (wow64_function)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
  USHORT process_machine, native_machine;
  if (!wow64 || !wow64(GetCurrentProcess(), &process_machine, &native_machine) ||
      native_machine != IMAGE_FILE_MACHINE_ARM64)
    return 0;
  static wchar_t target[32768];
  DWORD length = GetModuleFileNameW(NULL, target, sizeof target / sizeof *target);
  const wchar_t suffix[] = L"\\win32-x64\\cow-clone.exe";
  size_t suffix_length = wcslen(suffix);
  if (length == 0 || length >= sizeof target / sizeof *target || length < suffix_length ||
      _wcsicmp(target + length - suffix_length, suffix) != 0)
    return 0;
  wcscpy(target + length - suffix_length, L"\\win32-arm64\\cow-clone.exe");
  if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) return 0;
  const wchar_t *arguments = GetCommandLineW();
  if (*arguments == L'"') {
    arguments++;
    while (*arguments && *arguments != L'"') arguments++;
    if (*arguments) arguments++;
  } else
    while (*arguments && *arguments != L' ' && *arguments != L'\t') arguments++;
  size_t size = wcslen(target) + wcslen(arguments) + 3;
  wchar_t *command = malloc(size * sizeof *command);
  if (!command) return 0;
  swprintf(command, size, L"\"%ls\"%ls", target, arguments);
  STARTUPINFOW startup = {sizeof startup};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION child;
  if (!CreateProcessW(target, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child)) return 0;
  WaitForSingleObject(child.hProcess, INFINITE);
  if (!GetExitCodeProcess(child.hProcess, status)) *status = 1;
  return 1;
}
#endif

static UINT console_code_page;

static void restore_console(void) { SetConsoleOutputCP(console_code_page); }

int wmain(int argc, wchar_t **wide) {
#if defined(_M_X64) && !defined(_M_ARM64EC)
  DWORD status;
  if (run_native(&status)) return (int)status;
#endif
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
  char **argv = calloc((size_t)argc + 1, sizeof *argv);
  if (!argv) return 1;
  for (int i = 0; i < argc; i++) {
    int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
    argv[i] = malloc((size_t)size);
    if (!argv[i] || !WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, argv[i], size, NULL, NULL)) return 1;
  }
  console_code_page = GetConsoleOutputCP();
  if (console_code_page && SetConsoleOutputCP(CP_UTF8)) atexit(restore_console);
  return cow_clone_main(argc, argv);
}
