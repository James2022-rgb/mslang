// TU header --------------------------------------------
#include "slang_cache-fs_impl.h"

// platform detection headers ---------------------------
#include "mbase/public/platform.h"

// platform headers -------------------------------------
#if MBASE_PLATFORM_LINUX || MBASE_PLATFORM_ANDROID
# include <sys/file.h>
#endif

// public project headers -------------------------------
#include "mbase/public/log.h"
#include "mbase/public/trap.h"

namespace mslang {

//
// ICacheFileSystemFile
//

class DefaultCacheFileSystemFile final : public ICacheFileSystemFile {
public:
#if MBASE_PLATFORM_WINDOWS
  explicit DefaultCacheFileSystemFile(
    ICacheFileSystem::OpenFileMode mode,
    HANDLE handle
  ) :
      mode_(mode),
      handle_(handle)
  {}
#elif MBASE_PLATFORM_LINUX || MBASE_PLATFORM_ANDROID
  explicit DefaultCacheFileSystemFile(
    ICacheFileSystem::OpenFileMode mode,
    FILE* file
  ) :
      mode_(mode),
      file_(file)
  {}
#elif MBASE_PLATFORM_WEB
  explicit DefaultCacheFileSystemFile(
    ICacheFileSystem::OpenFileMode mode
  ) :
      mode_(mode)
  {
  }
#endif
  ~DefaultCacheFileSystemFile() override = default;
  MBASE_DISALLOW_COPY_MOVE(DefaultCacheFileSystemFile);

  void Close() override {
#if MBASE_PLATFORM_WINDOWS
    ::CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
#elif MBASE_PLATFORM_LINUX || MBASE_PLATFORM_ANDROID
    if (file_ != nullptr) {
      if (mode_ == ICacheFileSystem::OpenFileMode::kReadWriteExclusive) {
        int fd = fileno(file_);
        if (fd != -1) {
          // Ensure data is flushed to disk.
          fsync(fd);
          flock(fd, LOCK_UN);
        }
      }

      fclose(file_);
      file_ = nullptr;
    }
#elif MBASE_PLATFORM_WEB
    // No-op.
#endif

    // Deallocate self.
    delete this;
  }

  bool GetTimestamp(uint64_t& out_timestamp) override {
#if MBASE_PLATFORM_WINDOWS
    FILETIME last_write_time {};
    if (::GetFileTime(handle_, nullptr, nullptr, &last_write_time) == 0) {
      MBASE_LOG_ERROR("DefaultCacheFileSystemFile::GetTimestamp: GetFileTime failed.");
      return false;
    }

    // Convert FILETIME to Unix timestamp (seconds since epoch).
    auto to_unix_time = [](const FILETIME& ft) -> uint64_t {
      // FILETIME is in 100-nanosecond intervals since January 1, 1601 (UTC).
      // Unix epoch starts from January 1, 1970 (UTC).
      constexpr uint64_t kUnixEpochStart = 11644473600ULL; // Seconds between 1601 and 1970.
      ULARGE_INTEGER ull;
      ull.LowPart  = ft.dwLowDateTime;
      ull.HighPart = ft.dwHighDateTime;
      return (ull.QuadPart / 10000000ULL) - kUnixEpochStart; // Convert to seconds and adjust epoch.
    };

    out_timestamp = to_unix_time(last_write_time);
    return true;
#elif MBASE_PLATFORM_LINUX || MBASE_PLATFORM_ANDROID
    int fd = fileno(file_);
    if (fd == -1) {
      MBASE_LOG_ERROR("DefaultCacheFileSystemFile::GetTimestamp: fileno failed.");
      return false;
    }

    struct stat file_stat;
    int result = fstat(fd, &file_stat);
    if (result != 0) {
      MBASE_LOG_ERROR("DefaultCacheFileSystemFile::GetTimestamp: fstat failed.");
      return false;
    }

    out_timestamp = static_cast<uint64_t>(file_stat.st_mtime);
    return true;
#elif MBASE_PLATFORM_WEB
    (void)out_timestamp;
    return false;
#endif
  }

  bool SeekToBegin() override {
#if MBASE_PLATFORM_WINDOWS
    if (!::SetFilePointerEx(handle_, { 0 }, nullptr, FILE_BEGIN)) {
      MBASE_LOG_ERROR("DefaultCacheFileSystemFile::SeekToBegin: SetFilePointerEx(FILE_BEGIN) failed.");
      return false;
    }
    return true;
#elif MBASE_PLATFORM_LINUX || MBASE_PLATFORM_ANDROID
    fseek(file_, 0, SEEK_SET);
    return true;
#elif MBASE_PLATFORM_WEB
    return false;
#endif
  }

  bool ReadAllBytes(std::vector<std::byte>& out_bytes) override {
#if MBASE_PLATFORM_WINDOWS
    LARGE_INTEGER file_size_li {};
    if (!::GetFileSizeEx(handle_, &file_size_li)) {
      MBASE_LOG_ERROR("DefaultCacheFileSystemFile::ReadAllBytes: GetFileSizeEx failed.");
      return false;
    }
    int64_t const file_size = file_size_li.QuadPart;

    LARGE_INTEGER old_pos_li {};
    if (!::SetFilePointerEx(handle_, { 0 }, &old_pos_li, FILE_CURRENT)) {
      MBASE_LOG_ERROR("DefaultCacheFileSystemFile::ReadAllBytes: SetFilePointerEx(FILE_CURRENT) failed.");
      return false;
    }

    // Get ready to read all bytes.
    if (!::SetFilePointerEx(handle_, { 0 }, nullptr, FILE_BEGIN)) {
      MBASE_LOG_ERROR("DefaultCacheFileSystemFile::ReadAllBytes: SetFilePointerEx(FILE_BEGIN) failed.");
      return false;
    }

    size_t const old_buffer_size = out_bytes.size();
    out_bytes.resize(static_cast<size_t>(file_size));
    DWORD bytes_read = 0;
    if (!::ReadFile(handle_, out_bytes.data(), static_cast<DWORD>(file_size), &bytes_read, nullptr) || bytes_read != static_cast<DWORD>(file_size)) {
      MBASE_LOG_ERROR("DefaultCacheFileSystemFile::ReadAllBytes: Failed to read all bytes from file.");

      // Restore previous state.
      ::SetFilePointerEx(handle_, old_pos_li, nullptr, FILE_BEGIN);
      out_bytes.resize(old_buffer_size);
      return false;
    }

    ::SetFilePointerEx(handle_, old_pos_li, nullptr, FILE_BEGIN);
    return true;
#elif MBASE_PLATFORM_LINUX || MBASE_PLATFORM_ANDROID
    int64_t const old_pos = ftell(file_);

    // Get file size.
    fseek(file_, 0, SEEK_END);
    int64_t const file_size = ftell(file_);

    // Get ready to read all bytes.
    fseek(file_, 0, SEEK_SET);

    size_t const old_buffer_size = out_bytes.size();

    out_bytes.resize(static_cast<size_t>(file_size));
    if (fread(out_bytes.data(), 1, file_size, file_) != static_cast<size_t>(file_size)) {
      MBASE_LOG_ERROR("DefaultCacheFileSystemFile::ReadAllBytes: Failed to read all bytes from file.");

      // Restore previous state.
      fseek(file_, old_pos, SEEK_SET);
      out_bytes.resize(old_buffer_size);
      return false;
    }

    fseek(file_, old_pos, SEEK_SET);
    return true;
#elif MBASE_PLATFORM_WEB
    (void)out_bytes;
    return false;
#endif
  }

  bool WriteBytes(std::byte const* bytes, uint32_t byte_count) override {
#if MBASE_PLATFORM_WINDOWS
    DWORD bytes_written = 0;
    if (!::WriteFile(handle_, bytes, static_cast<DWORD>(byte_count), &bytes_written, nullptr) || bytes_written != static_cast<DWORD>(byte_count)) {
      MBASE_LOG_ERROR("DefaultCacheFileSystemFile::WriteBytes: Failed to write all bytes to file.");
      return false;
    }
    return true;
#elif MBASE_PLATFORM_LINUX || MBASE_PLATFORM_ANDROID
    if (fwrite(bytes, 1, byte_count, file_) != static_cast<size_t>(byte_count)) {
      MBASE_LOG_ERROR("DefaultCacheFileSystemFile::WriteBytes: Failed to write all bytes to file.");
      return false;
    }

    return true;
#elif MBASE_PLATFORM_WEB
    (void)bytes;
    (void)byte_count;
    return false;
#endif
  }

private:
  [[maybe_unused]] ICacheFileSystem::OpenFileMode mode_ = ICacheFileSystem::OpenFileMode::kRead;
#if MBASE_PLATFORM_WINDOWS
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#elif MBASE_PLATFORM_LINUX || MBASE_PLATFORM_ANDROID
  FILE* file_ = nullptr;
#endif
};

//
// DefaultCacheFileSystem
//

bool DefaultCacheFileSystem::Initialize(std::string_view cache_parent_path) {
  cache_root_dir_ = (std::filesystem::path(cache_parent_path) / "slang_cache").string();

  std::filesystem::create_directories(cache_root_dir_);
  return true;
}

ICacheFileSystemFile* MBASE_NULLABLE DefaultCacheFileSystem::OpenFile(
  std::string_view unresolved_full_path,
  OpenFileMode mode
) {
  std::filesystem::path resolved_path = this->ResolvePath(unresolved_full_path);

#if MBASE_PLATFORM_WINDOWS
  DWORD desired_access = 0;
  DWORD share_mode = 0;
  DWORD creation_disposition = 0;
  switch (mode) {
  case OpenFileMode::kRead:
    desired_access = GENERIC_READ;
    share_mode = FILE_SHARE_READ; // Allow read sharing.
    creation_disposition = OPEN_EXISTING;
    break;
  case OpenFileMode::kWrite:
    desired_access = GENERIC_WRITE;
    share_mode = 0; // No sharing.
    creation_disposition = CREATE_ALWAYS;
    break;
  case OpenFileMode::kReadWriteExclusive:
    desired_access = GENERIC_READ | GENERIC_WRITE;
    share_mode = 0; // No sharing.
    creation_disposition = OPEN_ALWAYS; // Create if missing.
    break;
  default:
    MBASE_LOG_ERROR("DefaultCacheFileSystem::OpenFile: Invalid OpenFileMode value.");
    return nullptr;
  }

  // TODO: We could use `CreateFileW` for Unicode support, assuming the input path is UTF-8 and converting it to UTF-16.

  HANDLE handle = ::CreateFileA(
    resolved_path.string().c_str(),
    desired_access,
    share_mode,
    nullptr,
    creation_disposition,
    FILE_ATTRIBUTE_NORMAL,
    nullptr
  );

  if (handle == INVALID_HANDLE_VALUE) {
    DWORD const err = ::GetLastError();
    MBASE_LOG_ERROR("DefaultCacheFileSystem::OpenFile: CreateFileA failed to open file: \"{}\" with mode: {}. Error: 0x{:08x}", resolved_path.string(), static_cast<int>(mode), err);
    return nullptr;
  }

  return new DefaultCacheFileSystemFile(mode, handle);
#elif MBASE_PLATFORM_LINUX || MBASE_PLATFORM_ANDROID
  FILE* file = nullptr;
  switch (mode) {
  case OpenFileMode::kRead:
  case OpenFileMode::kWrite:
    {
      const char* mode_str = nullptr;
      switch (mode) {
      case OpenFileMode::kRead:
        mode_str = "rb";
        break;
      case OpenFileMode::kWrite:
        mode_str = "wb";
        break;
      default:
        mbase::Trap(); // Should not reach here.
      }

      file = fopen(resolved_path.string().c_str(), mode_str);
      if (!file) {
        MBASE_LOG_ERROR("DefaultCacheFileSystem::OpenFile: Failed to open file: \"{}\" with mode: {}", resolved_path.string(), mode_str);
        return nullptr;
      }
    }
    break;
  case OpenFileMode::kReadWriteExclusive:
    {
      // OPEN_ALWAYS behavior; create if missing., no truncation.
      int fd = open(resolved_path.string().c_str(), O_RDWR | O_CREAT, 0666);
      if (fd == -1) {
        MBASE_LOG_ERROR("DefaultCacheFileSystem::OpenFile: Failed to open file: \"{}\" with O_RDWR | O_CREAT", resolved_path.string());
        return nullptr;
      }

      if (flock(fd, LOCK_EX) != 0) {
        MBASE_LOG_ERROR("DefaultCacheFileSystem::OpenFile: Failed to acquire exclusive lock on file: \"{}\".", resolved_path.string());

        close(fd);
        return nullptr;
      }

      file = fdopen(fd, "r+");
    }
    break;
  default:
    MBASE_LOG_ERROR("DefaultCacheFileSystem::OpenFile: Invalid OpenFileMode value.");
    return nullptr;
  };

  return new DefaultCacheFileSystemFile(mode, file);
#elif MBASE_PLATFORM_WEB
  (void)resolved_path;
  (void)mode;
  // Web platform does not support file system operations.
  MBASE_LOG_ERROR("DefaultCacheFileSystem::OpenFile: File system operations are not supported on Web platform.");
  return nullptr;
#endif
}

std::filesystem::path DefaultCacheFileSystem::ResolvePath(std::string_view full_path) const {
  std::filesystem::path resolved_path = std::filesystem::path(cache_root_dir_) / std::filesystem::path(full_path);
  resolved_path = resolved_path.make_preferred();
  resolved_path = resolved_path.lexically_normal();
  return resolved_path;
}

} // namespace mslang
