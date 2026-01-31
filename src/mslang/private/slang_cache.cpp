// TU header --------------------------------------------
#include "mslang/public/slang_cache.h"

// c++ headers ------------------------------------------
#include <algorithm>
#include <memory>
#include <sstream>
#include <filesystem>

// public project headers -------------------------------
#include "mbase/public/platform.h"
#include "mbase/public/assert.h"
#include "mbase/public/log.h"
#include "mbase/public/trap.h"
#include "mbase/public/tsa.h"
#include "mbase/public/hash.h"
#include "mbase/public/profiling.h"

// project headers --------------------------------------
#include "slang_cache-fs_impl.h"

namespace mslang {

namespace {

char const* const kCapability = "GLSL_150";

} // namespace

//
// SlangSessionWrmKey
//

size_t PreprocessorMacro::Hasher::operator()(PreprocessorMacro const& v) const {
  mbase::HasherSizeT hasher;
  hasher.DoContiguousContainer(v.name);
  hasher.DoContiguousContainer(v.value);
  return hasher.Finish();
}

size_t SlangSessionWrmKey::Hasher::operator()(SlangSessionWrmKey const& v) const {
  mbase::HasherSizeT hasher;
  hasher.DoContiguousContainer(v.root_module_parent_path);
  hasher.DoContiguousContainer(v.root_module_name);
  {
    std::vector<PreprocessorMacro const*> sorted_macros;
    sorted_macros.reserve(v.preprocessor_macros.size());
    for (PreprocessorMacro const& macro : v.preprocessor_macros) {
      sorted_macros.emplace_back(&macro);
    }
    std::sort(sorted_macros.begin(), sorted_macros.end(), [](PreprocessorMacro const* a, PreprocessorMacro const* b) {
      return (*a) < (*b);
    });

    hasher.Do(uint64_t(sorted_macros.size()));
    for (PreprocessorMacro const* macro : sorted_macros) {
      hasher.Do(PreprocessorMacro::Hasher{}(*macro));
    }
  }
  return hasher.Finish();
}

//
// ISlangCache
//

class SlangCache final : public ISlangCache {
public:
  explicit SlangCache(std::string cache_parent_path) {
    default_cache_file_system_ = std::make_unique<DefaultCacheFileSystem>();
    if (!default_cache_file_system_->Initialize(cache_parent_path)) {
      MBASE_LOG_ERROR("SlangCache: Failed to initialize the default cache file system.");
      mbase::Trap();
    }
  }
  ~SlangCache() override = default;
  MBASE_DISALLOW_COPY_MOVE(SlangCache);

  //
  // ISlangCache implementation
  //

  bool RequestSlangSessionWithRootModule(
    SlangSessionWrmKey const& key,
    ICacheFileSystem* MBASE_NULLABLE cache_file_system,
    ISlangCodeProvider* MBASE_NOT_NULL slang_code_provider,
    ISlangDependencyIncludeHandler* MBASE_NULLABLE slang_include_handler,
    mslang::ISlangSession** MBASE_NOT_NULL out_session
  ) override {
    // TODO: Preprocessor macros should be actually used for compilation.

    if (cache_file_system == nullptr) {
      cache_file_system = default_cache_file_system_.get();
    }

    SlangModuleIdentity root_module_identity {
      .parent_path = std::string(key.root_module_parent_path),
      .module_name = std::string(key.root_module_name),
    };

    ICacheFileSystemFile* ssc_meta_file = nullptr;
    uint64_t hash = 0;
    std::string identity_str;
    {
      SscMeta ssc_meta;
      Ssc ssc;

      FindCachedSessionResult find_result {};
      find_result.ssc_meta = &ssc_meta;
      find_result.ssc = &ssc;

      bool success = this->FindCachedSessionWithRootModule(
        key,
        cache_file_system,
        slang_code_provider,
        find_result
      );
      if (!success) {
        MBASE_LOG_ERROR(
          "SlangCache::RequestSlangSessionWithRootModule: Unexpected error trying to find cached Slang session for root module: \"{}\" at path: \"{}\"",
          key.root_module_name, key.root_module_parent_path
        );
        return false;
      }

      if (find_result.hit) {
        mslang::ISlangSession* mslang_slang_session = CreateSlangSession(slang_include_handler);

        SscModule const& root_module = ssc.root_module;

        // TODO: Cache dependencies as well.
        bool result = mslang_slang_session->AddModuleFromSerializedBytes(
          root_module_identity.module_name.c_str(),
          root_module_identity.parent_path.c_str(),
          root_module.serialized_bytes.data(),
          static_cast<uint32_t>(root_module.serialized_bytes.size()),
          nullptr
        );

        if (result) {
          *out_session = mslang_slang_session;
          return true;
        }
        else {
          MBASE_LOG_ERROR(
            "SlangCache::RequestSlangSessionWithRootModule: Failed to create Slang session from cached serialized bytes for root module: \"{}\" at path: \"{}\"",
            key.root_module_name, key.root_module_parent_path
          );
          return false;
        }
      }

      ssc_meta_file = find_result.ssc_meta_file;
      hash = find_result.hash;
      identity_str = std::move(find_result.identity_str);
    }

    // If we reach here, we need to create a new session, compile the root module, and cache it.
    {
      mslang::ISlangSession* mslang_slang_session = this->DoCreateSlangSessionAndCompileModule(
        root_module_identity,
        slang_code_provider,
        slang_include_handler
      );
      if (mslang_slang_session == nullptr) {
        MBASE_LOG_ERROR(
          "SlangCache::RequestSlangSessionWithRootModule: Failed to compile Slang module: \"{}\" at path: \"{}\"",
          key.root_module_name, key.root_module_parent_path
        );
        return false;
      }

      std::vector<std::byte> slang_module_bytes;
      mslang_slang_session->SerializeModule(nullptr, slang_module_bytes);

      {
        SscMeta ssc_meta;
        ssc_meta.hash = hash;
        ssc_meta.root_module_identity = root_module_identity;

        for (PreprocessorMacro const& macro : key.preprocessor_macros) {
          ssc_meta.preprocessor_macros.emplace_back(macro.ToOwned());
        }

        SscModuleMeta& root_module_meta = ssc_meta.root_module_meta;

        std::span<mslang::SlangModuleDependency const> dependencies = mslang_slang_session->GetModuleDependencies(nullptr);
        for (mslang::SlangModuleDependency const& dep : dependencies) {
          root_module_meta.code_dependencies.emplace_back() = SlangCodeDependency {
            .unresolved_path = dep.unresolved_path,
            .resolved_path = dep.resolved_path,
            .meta = SlangCodeMeta {
              .timestamp = dep.timestamp,
            },
            .type = SlangCodeDependencyType::kReferencedAsset,
          };
        }

        std::vector<std::byte> ssc_meta_bytes;
        ssc_meta.Serialize(ssc_meta_bytes);

        ssc_meta_file->SeekToBegin();
        ssc_meta_file->WriteBytes(ssc_meta_bytes.data(), static_cast<uint32_t>(ssc_meta_bytes.size()));
      }

      {
        Ssc ssc;
        SscModule& root_module = ssc.root_module;

        root_module.serialized_bytes = std::move(slang_module_bytes);

        std::vector<std::byte> ssc_bytes;
        ssc.Serialize(ssc_bytes);

        cache_file_system->WriteFile(
          fmt::format("{}.ssc", identity_str),
          ssc_bytes.data(),
          static_cast<uint32_t>(ssc_bytes.size())
        );
      }

      // Hold onto the `.ssc-meta` file until we are done writing to the `.ssc` file.
      ssc_meta_file->Close();

      *out_session = mslang_slang_session;
      return true;
    }
  }

private:
  struct FindCachedSessionResult final {
    bool hit = false;
    ICacheFileSystemFile* MBASE_NULLABLE ssc_meta_file = nullptr; // Written to if miss.
    std::string identity_str;
    uint64_t hash = 0;
    SscMeta* MBASE_NULLABLE ssc_meta = nullptr; // Written to if hit.
    Ssc* MBASE_NULLABLE ssc = nullptr; // Written to if hit.
  };

  static bool FindCachedSessionWithRootModule(
    SlangSessionWrmKey const& key,
    ICacheFileSystem* MBASE_NULLABLE cache_file_system,
    ISlangCodeProvider* MBASE_NOT_NULL slang_code_provider,
    FindCachedSessionResult& out_result
  ) {
    MBASE_SCOPED_TIMER("SlangCache::FindCachedSessionWithRootModule");

    // Compute the hash of the key.
    uint64_t const key_hash = SlangSessionWrmKey::Hasher{}(key);

    uint64_t slang_code_last_modified_timestamp = 0;
    if (!slang_code_provider->ProvideSlangCode(key.root_module_parent_path, key.root_module_name, nullptr, slang_code_last_modified_timestamp)) {
      MBASE_LOG_ERROR(
        "SlangCache::FindCachedSessionWithRootModule: Failed to get last modified timestamp for Slang module: \"{}\" at path: \"{}\"",
       key.root_module_name, key.root_module_parent_path
      );
      return false;
    }

    // `<escaped_path>-<hash>-`
    std::string identity_str_base;
    {
      static std::string const kSlashPlaceholder = "_slash_";

      std::ostringstream oss;
      oss << key.root_module_parent_path << "/" << key.root_module_name;
      identity_str_base = oss.str();

      // Replace all occurrences of `/` with kSlashPlaceholder to make it a valid file name.
      // We assume that `parent_path` and `module_name` do not contain kSlashPlaceholder.
      {
        size_t pos = 0;
        while ((pos = identity_str_base.find('/', pos)) != std::string::npos) {
          identity_str_base.replace(pos, 1, kSlashPlaceholder);
          pos += kSlashPlaceholder.size();
        }
      }

      identity_str_base += fmt::format("-{:016x}-", key_hash);
    }

    std::string identity_str;
    SscMeta ssc_meta;

    // Generate an identity string in the form of `<escaped_path>-<hash>-<attempt>`.
    // For this we need to try opening `.ssc_meta` files to handle potential hash collisions.
    // In most cases, there should be no collisions, so we expect to only try once and `attempt` to be `0`.
    {
      uint32_t attempt = 0;
      bool successful_cache_miss = false;

      ICacheFileSystemFile* ssc_meta_file = nullptr;

      while (true) {
        identity_str = fmt::format("{}{}", identity_str_base, attempt);

        std::string const ssc_meta_filename = identity_str + ".ssc-meta";

        // Try to open the `.ssc-meta` file with exclusive read/write access to prevent other processes/threads from modifying it while we read from or write to it.
        // We keep the file open until we have finished reading (in case of cache hit) or modifying (in case of cache miss) it, or we fail to do so (in which case we abort the operation).
        ssc_meta_file = cache_file_system->OpenFile(ssc_meta_filename, ICacheFileSystem::OpenFileMode::kReadWriteExclusive);
        if (ssc_meta_file == nullptr) {
          MBASE_LOG_ERROR(
            "SlangCache::FindCachedSessionWithRootModule: Failed to open .ssc-meta file (R/W EX) \"{}\" for Slang module: \"{}\" at path: \"{}\"",
            ssc_meta_filename, key.root_module_name, key.root_module_parent_path
          );
          return false;
        }

        uint64_t ssc_meta_last_modified_timestamp = 0;
        if (!ssc_meta_file->GetTimestamp(ssc_meta_last_modified_timestamp)) {
          MBASE_LOG_ERROR(
            "SlangCache::FindCachedSessionWithRootModule: Failed to get last modified timestamp of .ssc-meta file \"{}\" for Slang module: \"{}\" at path: \"{}\"",
            ssc_meta_filename, key.root_module_name, key.root_module_parent_path
          );
          ssc_meta_file->Close();
          return false;
        }

        if (ssc_meta_last_modified_timestamp < slang_code_last_modified_timestamp) {
          // The `.ssc-meta` file is older than the Slang source code; treat as a successful cache miss.
          MBASE_LOG_TRACE(
            "SlangCache::FindCachedSessionWithRootModule: Outdated .ssc-meta file \"{}\" for Slang module: \"{}\" at path: \"{}\". Slang source last modified timestamp: {}, .ssc-meta last modified timestamp: {}.",
            ssc_meta_filename, key.root_module_name, key.root_module_parent_path,
            slang_code_last_modified_timestamp, ssc_meta_last_modified_timestamp
          );

          successful_cache_miss = true;
          break;
        }

        std::vector<std::byte> ssc_meta_bytes;
        if (!ssc_meta_file->ReadAllBytes(ssc_meta_bytes)) {
          // This should not happen; successfully opened the file but failed to read from it.
          MBASE_LOG_ERROR(
            "SlangCache::FindCachedSessionWithRootModule: Failed to read from .ssc-meta file (R/W EX) \"{}\" for Slang module: \"{}\" at path: \"{}\"",
            ssc_meta_filename, key.root_module_name, key.root_module_parent_path
          );

          ssc_meta_file->Close();
          return false;
        }

        if (ssc_meta_bytes.empty()) {
          // File is empty.
          // This is a successful cache miss, because of `kReadWriteExclusive` mode; we have just created the file and it is empty.
          MBASE_LOG_TRACE(
            "SlangCache::FindCachedSessionWithRootModule: Empty .ssc-meta file \"{}\" for Slang module: \"{}\" at path: \"{}\".",
            ssc_meta_filename, key.root_module_name, key.root_module_parent_path
          );

          successful_cache_miss = true;
          break;
        }

        if (!ssc_meta.Deserialize(ssc_meta_bytes)) {
          // Invalid/corrupted `.ssc-meta` file. Treat as a successful cache miss.
          MBASE_LOG_WARN(
            "SlangCache::FindCachedSessionWithRootModule: Invalid/corrupted .ssc-meta file \"{}\" for Slang module: \"{}\" at path: \"{}\".",
            ssc_meta_filename, key.root_module_name, key.root_module_parent_path
          );

          successful_cache_miss = true;
          break;
        }

        if (ssc_meta.hash != key_hash) {
          MBASE_LOG_ERROR(
            "SlangCache::FindCachedSessionWithRootModule: Unexpected hash mismatch in .ssc-meta file\"{}\" for Slang module: \"{}\" at path: \"{}\". Expected: \"{:016x}\", Found: \"{:016x}\"",
            ssc_meta_filename, key.root_module_name, key.root_module_parent_path, key_hash, ssc_meta.hash
          );

          ssc_meta_file->Close();
          return false;
        }

        // Successfully opened the file and validated the hash.
        // Now to validate the actual identity (parent path, module name, preprocessor macros) that was used to generate the hash,
        // to see if we have a hash collision or not.

        bool hash_collision = false;
        if (ssc_meta.root_module_identity.parent_path != key.root_module_parent_path) {
          MBASE_LOG_ERROR(
            "SlangCache::FindCachedSessionWithRootModule: Identity (parent path) mismatch in .ssc-meta file \"{}\" for Slang module: \"{}\" at path: \"{}\". Expected: \"{}\", Found: \"{}\"",
            ssc_meta_filename, key.root_module_name, key.root_module_parent_path, key.root_module_parent_path, ssc_meta.root_module_identity.parent_path
          );

          hash_collision = true;
        }
        if (ssc_meta.root_module_identity.module_name != key.root_module_name) {
          MBASE_LOG_ERROR(
            "SlangCache::FindCachedSessionWithRootModule: Identity (module name) mismatch in .ssc-meta file \"{}\" for Slang module: \"{}\" at path: \"{}\". Expected: \"{}\", Found: \"{}\"",
            ssc_meta_filename, key.root_module_name, key.root_module_parent_path, key.root_module_name, ssc_meta.root_module_identity.module_name
          );

          hash_collision = true;
        }
        {
          if (ssc_meta.preprocessor_macros.size() != key.preprocessor_macros.size()) {
            MBASE_LOG_ERROR(
              "SlangCache::FindCachedSessionWithRootModule: Identity (preprocessor macros count) mismatch in .ssc-meta file \"{}\" for Slang module: \"{}\" at path: \"{}\". Expected: \"{}\", Found: \"{}\"",
              ssc_meta_filename, key.root_module_name, key.root_module_parent_path, key.preprocessor_macros.size(), ssc_meta.preprocessor_macros.size()
            );

            hash_collision = true;
          } else {
            // Compare preprocessor macros ignoring order.
            std::vector<PreprocessorMacro> sorted_a;
            sorted_a.reserve(key.preprocessor_macros.size());
            for (PreprocessorMacro const& macro : key.preprocessor_macros) {
              sorted_a.emplace_back(macro);
            }
            std::sort(sorted_a.begin(), sorted_a.end(), [](PreprocessorMacro const& a, PreprocessorMacro const& b) {
              return a < b;
            });

            std::vector<PreprocessorMacro> sorted_b;
            sorted_b.reserve(ssc_meta.preprocessor_macros.size());
            for (PreprocessorMacroOwned const& macro : ssc_meta.preprocessor_macros) {
              sorted_b.emplace_back(macro);
            }
            std::sort(sorted_b.begin(), sorted_b.end(), [](PreprocessorMacro const& a, PreprocessorMacro const& b) {
              return a < b;
            });

            for (size_t i = 0; i < sorted_a.size(); ++i) {
              if ((sorted_a[i]) != (sorted_b[i])) {
                MBASE_LOG_ERROR(
                  "SlangCache::FindCachedSessionWithRootModule: Identity (preprocessor macro) mismatch in .ssc-meta file \"{}\" for Slang module: \"{}\" at path: \"{}\". Expected: {{ name=\"{}\", value=\"{}\" }}, Found: {{ name=\"{}\", value=\"{}\" }}",
                  ssc_meta_filename, key.root_module_name, key.root_module_parent_path,
                  sorted_a[i].name, sorted_a[i].value,
                  sorted_b[i].name, sorted_b[i].value
                );

                hash_collision = true;
                break;
              }
            }
          }
        }

        if (hash_collision) {
          // Hash collision. Try the next attempt.
          MBASE_LOG_WARN(
            "SlangCache::FindCachedSessionWithRootModule: Hash collision detected in .ssc-meta file \"{}\" for Slang module: \"{}\" at path: \"{}\". Trying next attempt...",
            ssc_meta_filename, key.root_module_name, key.root_module_parent_path
          );

          ssc_meta_file->Close();
          ++attempt;
          continue;
        }

        {
          SscModuleMeta const& root_module_meta = ssc_meta.root_module_meta;

          for (SlangCodeDependency const& dep : root_module_meta.code_dependencies) {
            uint64_t dep_timestamp = 0;
            if (!slang_code_provider->ProvideSlangCodeTimestampResolvedPath(dep.resolved_path.c_str(), dep_timestamp)) {
              MBASE_LOG_ERROR("SlangCache::FindCachedSessionWithRootModule: Failed to get timestamp for dependency: \"{}\"", dep.resolved_path);
              continue;
            }

            if (dep.meta.timestamp < dep_timestamp) {
              MBASE_LOG_DEBUG(
                "SlangCache::FindCachedSessionWithRootModule: Dependency out of date for: \"{}\". Expected: {} < Found: {}",
                dep.resolved_path, dep.meta.timestamp, dep_timestamp
              );
              
              successful_cache_miss = true;
              break;
            }
          }
        }

        // Successfully validated the identity; cache hit.
        break;
      } // while (true)

      if (successful_cache_miss) {
        out_result.hit = false;
        out_result.ssc_meta_file = ssc_meta_file;
        out_result.identity_str = identity_str;
        out_result.hash = key_hash;
        return true;
      }
    }

    // If we reach here, we have successfully opened and validated the `.ssc-meta` file, and `ssc_meta` is populated, but `out_ssc_meta_file` is still open.

    if (out_result.ssc != nullptr) {
      std::string const ssc_filename = identity_str + ".ssc";

      std::vector<std::byte> ssc_bytes;
      uint64_t ssc_timestamp = 0;
      if (!cache_file_system->ReadFile(ssc_filename, ssc_bytes, ssc_timestamp)) {
        MBASE_LOG_ERROR(
          "SlangCache::FindCachedSessionWithRootModule: Failed to read .ssc file \"{}\" for Slang module: \"{}\" at path: \"{}\"",
          ssc_filename, key.root_module_name, key.root_module_parent_path
        );
        return false;
      }

      if (!out_result.ssc->Deserialize(ssc_bytes)) {
        MBASE_LOG_ERROR(
          "SlangCache::FindCachedSessionWithRootModule: Failed to deserialize .ssc file \"{}\" for Slang module: \"{}\" at path: \"{}\"",
          ssc_filename, key.root_module_name, key.root_module_parent_path
        );
        return false;
      }
    }

    out_result.hit = true;
    out_result.identity_str = identity_str;
    out_result.hash = key_hash;

    if (out_result.ssc_meta != nullptr) {
      *out_result.ssc_meta = std::move(ssc_meta);
    }

    return true;
  }

  static mslang::ISlangSession* DoCreateSlangSessionAndCompileModule(
    SlangModuleIdentity const& root_module_identity,
    ISlangCodeProvider* MBASE_NOT_NULL slang_code_provider,
    ISlangDependencyIncludeHandler* MBASE_NOT_NULL include_handler
  ) {
    MBASE_SCOPED_TIMER("SlangCache::DoCreateSlangSessionAndCompileModule");

    mslang::ISlangSession* mslang_slang_session = CreateSlangSession(include_handler);

    std::string slang_code;
    uint64_t slang_code_timestamp = 0;
    bool result = slang_code_provider->ProvideSlangCode(
      root_module_identity.parent_path.c_str(),
      root_module_identity.module_name.c_str(),
      &slang_code,
      slang_code_timestamp
    );
    if (!result) {
      MBASE_LOG_ERROR("SlangCache::RequestSlangSessionWithRootModule: Failed to get Slang code for root module: \"{}\" at path: \"{}\"", root_module_identity.module_name, root_module_identity.parent_path);
      if (mslang_slang_session != nullptr) {
        mslang_slang_session->Destroy();
      }
      return nullptr;
    }

    slang::IModule* slang_module = nullptr;
    result = mslang_slang_session->AddModuleFromCode(
      root_module_identity.module_name.c_str(),
      root_module_identity.parent_path.c_str(),
      slang_code.c_str(),
      &slang_module
    );
    if (!result) {
      MBASE_LOG_ERROR("SlangCache::DoCompile: Failed to add Slang module: \"{}\" at path: \"{}\"", root_module_identity.module_name, root_module_identity.parent_path);
      if (mslang_slang_session != nullptr) {
        mslang_slang_session->Destroy();
      }
      return nullptr;
    }

    return mslang_slang_session;
  }

  static bool SerializeExtSlangSession(
    mslang::ISlangSession* mslang_slang_session,
    std::vector<std::byte>& out_bytes
  ) {
    std::vector<std::byte> root_module_bytes;

    if (!mslang_slang_session->SerializeModule(nullptr, root_module_bytes)) {
      return false;
    }

    out_bytes = std::move(root_module_bytes);
    return true;
  }

  /// ## Lifetime requirements
  /// - `include_handler` MUST be valid until the `ISlangSession` is destroyed.
  static mslang::ISlangSession* MBASE_NOT_NULL CreateSlangSession(
    ISlangDependencyIncludeHandler* MBASE_NOT_NULL include_handler
  ) {
    mslang::SlangIncludeHandler handler = [include_handler](char const* path) -> std::optional<mslang::SlangIncludeResult> {

      mslang::SlangIncludeResult result {};
      if (!include_handler->HandleInclude(path, result)) {
        return std::nullopt;
      }

      return result;
    };

    mslang::ISlangSession* ext_slang_session = nullptr;
    ext_slang_session = mslang::ISlangSession::Create(
      handler,
      kCapability
    );
    return ext_slang_session;
  }

  std::unique_ptr<DefaultCacheFileSystem> default_cache_file_system_;
};

static mbase::Lockable<std::mutex> s_global_slang_cache_mutex;
static std::unique_ptr<ISlangCache> s_global_slang_cache MBASE_GUARDED_BY(s_global_slang_cache_mutex);

namespace {

void InitializeGlobalSlangCache(std::string_view cache_parent_path) {
  mbase::LockGuard lock(s_global_slang_cache_mutex);
  if (s_global_slang_cache) {
    MBASE_LOG_WARN("InitializeGlobalSlangCache: Global Slang cache is already initialized.");
    return;
  }
  s_global_slang_cache = std::make_unique<SlangCache>(std::string(cache_parent_path));
}

}

void ISlangCache::InitializeGlobal(std::string_view cache_parent_path) {
  InitializeGlobalSlangCache(cache_parent_path);
}

ISlangCache* MBASE_NULLABLE ISlangCache::GetGlobal() {
  mbase::LockGuard lock(s_global_slang_cache_mutex);
  if (!s_global_slang_cache) {
    MBASE_LOG_ERROR("ISlangCache::GetGlobal: Global Slang cache is not initialized.");
    return nullptr;
  }
  return s_global_slang_cache.get();
}

} // namespace mslang
