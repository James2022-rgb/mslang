#pragma once

// c++ headers ------------------------------------------
#include <string_view>
#include <string>
#include <span>

// public project headers -------------------------------
#include "mbase/public/access.h"
#include "mbase/public/type_util.h"

#include "mslang/public/mslang-slang.h"

#include "mslang/public/slang_cache-fs.h"
#include "mslang/public/slang_cache-data.h" // Here for `PreprocessorMacro`.

namespace mslang {

class ISlangCodeProvider {
public:
  virtual ~ISlangCodeProvider() = default;

  virtual bool ProvideSlangCode(
    std::string_view parent_path,
    std::string_view module_name,
    std::string* MBASE_NULLABLE out_slang_code,
    uint64_t& out_slang_code_timestamp
  ) = 0;

  virtual bool ProvideSlangCodeTimestampResolvedPath(
    std::string_view resolved_path,
    uint64_t& out_slang_code_timestamp
  ) = 0;
};

/// Handler to resolve `import` directives.
/// Invoked to find `.slang-module` binary IR files as well as `.slang` text code files files.
class ISlangDependencyIncludeHandler {
public:
  virtual ~ISlangDependencyIncludeHandler() = default;

  /// - `path`: The path as requested by the Slang compiler. May end with `.slang-module` or `.slang`.
  virtual bool HandleInclude(
    char const* MBASE_NOT_NULL path,
    mslang::SlangIncludeResult& out_result
  ) = 0;
};

//
// SlangSessionWrmKey
//

/// Key to identify a Slang session with a specific root module.
struct SlangSessionWrmKey final {
  std::string_view root_module_parent_path;
  std::string_view root_module_name;
  std::span<PreprocessorMacro const> preprocessor_macros;

  struct Hasher final {
    size_t operator()(SlangSessionWrmKey const& v) const;
  };
};

//
// ISlangCache
//

class ISlangCache {
public:
  virtual ~ISlangCache() = default;
  MBASE_DISALLOW_COPY_MOVE(ISlangCache);

  static void InitializeGlobal(std::string_view cache_parent_path);

  /// Get the global instance.
  static ISlangCache* MBASE_NULLABLE GetGlobal();

  /// ## Lifetime requirements
  /// - `file_system`: MUST be valid until the function call returns.
  /// - `slang_code_provider`: MUST be valid until the function call returns.
  /// - `include_handler`: MUST be valid until the `ISlangSession` instance is destroyed.
  virtual bool RequestSlangSessionWithRootModule(
    SlangSessionWrmKey const& key,
    ICacheFileSystem* MBASE_NULLABLE cache_file_system,
    ISlangCodeProvider* MBASE_NOT_NULL slang_code_provider,
    ISlangDependencyIncludeHandler* MBASE_NULLABLE slang_include_handler,
    mslang::ISlangSession* MBASE_NOT_NULL * MBASE_NOT_NULL out_session
  ) = 0;

protected:
  ISlangCache() = default;
};

} // namespace mslang
