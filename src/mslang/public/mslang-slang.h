#pragma once

// c++ headers ------------------------------------------
#include <cstdint>
#include <cstddef>

#include <optional>
#include <vector>
#include <string>
#include <span>
#include <functional>

// public project headers -------------------------------
#include "mbase/public/access.h"

// forward declarations ---------------------------------
namespace slang {
struct IModule;
};

namespace mslang {

struct SlangIncludeResult final {
  /// Slang module IR or slang code text.
  std::vector<std::byte> bytes;
  std::string resolved_path;
  /// Unix timestamp of the file when it was loaded.
  uint64_t timestamp = 0;
};

using SlangIncludeHandler = std::function<std::optional<SlangIncludeResult>(char const*)>;

struct SlangModuleDependency final {
  /// The path as requested by the Slang `import` declaration.
  std::string unresolved_path;
  /// Resolved path, including the extension.
  std::string resolved_path;
  /// Unix timestamp of the file when it was loaded.
  uint64_t timestamp = 0;
};

/// Our abstraction of a Slang session.
class ISlangSession {
public:
  virtual ~ISlangSession() = default;
  MBASE_DISALLOW_COPY_MOVE(ISlangSession);

  /// ## Lifetime requirements
  /// - `opt_include_handler`: MUST be valid until the `ISlangSession` instance is destroyed.
  static ISlangSession* Create(
    SlangIncludeHandler opt_include_handler = nullptr,
    char const* capability_name = "GLSL_150"
  );

  /// Destroy the instance. Methods MUST NOT be called after this. All modules become invalid.
  virtual void Destroy() = 0;

  virtual std::span<SlangModuleDependency const> GetModuleDependencies(slang::IModule* root_slang_module) = 0;

  virtual bool AddModuleFromCode(
    char const* module_name,
    char const* path,
    char const* slang_code,
    slang::IModule** opt_out_module
  ) = 0;

  virtual bool AddModuleFromSerializedBytes(
    char const* module_name,
    char const* path,
    std::byte const* slang_module_bytes,
    uint32_t slang_module_size_in_bytes,
    slang::IModule** opt_out_module
  ) = 0;

  virtual std::optional<std::vector<uint32_t>> CompileModuleEntryPointToSpirv(
    slang::IModule* root_slang_module,
    char const* entry_point_name
  ) = 0;

  virtual bool SerializeModule(
    slang::IModule* root_slang_module,
    std::vector<std::byte>& out_serialized_bytes
  ) = 0;

protected:
  ISlangSession() = default;
};

//
//
//

std::optional<std::vector<uint32_t>> CompileSlangToSpirv(
  char const* module_name,
  char const* path,
  char const* slang_code,
  SlangIncludeHandler opt_include_handler,
  char const* capability_name,
  char const* entry_point_name
);

/// Compile Slang source code to WGSL (WebGPU Shading Language).
/// This is suitable for WebGPU/Emscripten builds where SPIRV emission is not supported.
std::optional<std::string> CompileSlangToWgsl(
  char const* module_name,
  char const* path,
  char const* slang_code,
  SlangIncludeHandler opt_include_handler,
  char const* entry_point_name
);

} // namespace mslang
