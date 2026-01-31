#pragma once

// c++ headers ------------------------------------------
#include <cstddef>

#include <string_view>
#include <span>
#include <vector>

// public project headers -------------------------------
#include "mbase/public/type_util.h"

namespace mslang {

//
// ICacheFileSystem
//

class ICacheFileSystemFile {
public:
  virtual ~ICacheFileSystemFile() = default;

  // These should do for now.

  /// Close the file. After this call, the object is no longer usable.
  virtual void Close() = 0;

  virtual bool GetTimestamp(uint64_t& out_timestamp) = 0;
  
  virtual bool SeekToBegin() = 0;

  virtual bool ReadAllBytes(std::vector<std::byte>& out_bytes) = 0;
  virtual bool WriteBytes(std::byte const* MBASE_NOT_NULL bytes , uint32_t byte_count) = 0;
};

class ICacheFileSystem {
public:
  virtual ~ICacheFileSystem() = default;

  enum class OpenFileMode : uint32_t {
    /// Error if missing.
    kRead,
    /// Always create.
    kWrite,
    /// Create if missing. File pointer is at the beginning of the file.
    kReadWriteExclusive,
  };

  virtual ICacheFileSystemFile* MBASE_NULLABLE OpenFile(
    std::string_view unresolved_full_path,
    OpenFileMode mode
  ) = 0;

  bool GetTimestamp(
    std::string_view unresolved_full_path,
    uint64_t& out_timestamp
  ) {
    ICacheFileSystemFile* file = this->OpenFile(unresolved_full_path, OpenFileMode::kRead);
    if (file == nullptr) {
      return false;
    }

    bool const result = file->GetTimestamp(out_timestamp);
    file->Close();
    return result;
  }

  bool ReadFile(
    std::string_view unresolved_full_path,
    std::vector<std::byte>& out_bytes,
    uint64_t& out_timestamp
  ) {
    ICacheFileSystemFile* file = this->OpenFile(unresolved_full_path, OpenFileMode::kRead);
    if (file == nullptr) {
      return false;
    }

    bool const read_result = file->ReadAllBytes(out_bytes);
    if (!read_result) {
      file->Close();
      return false;
    }

    bool const ts_result = file->GetTimestamp(out_timestamp);
    file->Close();
    if (!ts_result) {
      return false;
    }

    return true;
  }

  bool WriteFile(
    std::string_view unresolved_full_path,
    std::byte const* MBASE_NOT_NULL bytes,
    uint32_t byte_count
  ) {
    ICacheFileSystemFile* file = this->OpenFile(unresolved_full_path, OpenFileMode::kWrite);
    if (file == nullptr) {
      return false;
    }

    bool const result = file->WriteBytes(bytes, byte_count);
    file->Close();
    return result;
  }

protected:
  ICacheFileSystem() = default;
};

} // namespace mslang 
