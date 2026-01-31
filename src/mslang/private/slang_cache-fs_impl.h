#pragma once

// c++ headers ------------------------------------------
#include <string>
#include <filesystem>


// public project headers -------------------------------
#include "mbase/public/access.h"
#include "mbase/public/type_util.h"

#include "mslang/public/slang_cache-fs.h"

// project headers --------------------------------------

namespace mslang {

class DefaultCacheFileSystem final : public ICacheFileSystem {
public:
  DefaultCacheFileSystem() = default;
  ~DefaultCacheFileSystem() override = default;
  MBASE_DISALLOW_COPY_MOVE(DefaultCacheFileSystem);

  bool Initialize(std::string_view cache_parent_path);

  //
  // ICacheFileSystem implementation
  //

  ICacheFileSystemFile* MBASE_NULLABLE OpenFile(
    std::string_view unresolved_full_path,
    OpenFileMode mode
  ) override;

private:
  std::filesystem::path ResolvePath(std::string_view full_path) const;

  std::string cache_root_dir_;
};

} // namespace mslang
