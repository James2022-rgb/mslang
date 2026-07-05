// my header --------------------------------------------
#include "mslang/public/mslang-slang.h"

// external headers -------------------------------------
#include "slang.h"
#include "slang-com-helper.h"
#include "slang-com-ptr.h"

// public project headers -------------------------------
#include "mbase/public/assert.h"
#include "mbase/public/platform.h"
#include "mbase/public/trap.h"
#include "mbase/public/tsa.h"
#include "mbase/public/profiling.h"

// project headers --------------------------------------
#include "mslang-mprof.h"

namespace mslang {

namespace {

#define RETURN_NULLOPT_IF_DIAGNOSTICS_NON_NULL(diagnostics_blob)                                   \
  if (diagnostics_blob) {                                                                          \
    MBASE_LOG_ERROR("slang diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());   \
    mbase::Trap();                                                                                 \
    return std::nullopt;                                                                           \
  }
#define RETURN_FALSE_IF_DIAGNOSTICS_NON_NULL(diagnostics_blob)                                     \
  if (diagnostics_blob) {                                                                          \
    MBASE_LOG_ERROR("slang diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());   \
    mbase::Trap();                                                                                 \
    return false;                                                                                  \
  }

enum class SpirvVersion : uint32_t {
  V_1_0,
  V_1_1,
  V_1_2,
  V_1_3,
  V_1_4,
  V_1_5,
  V_1_6,
};

SlangProfileID ToProfileID(SpirvVersion version, slang::IGlobalSession* global_session) {
  switch (version) {
  case SpirvVersion::V_1_0:
    return global_session->findProfile("spirv_1_0");
  case SpirvVersion::V_1_1:
    return global_session->findProfile("spirv_1_1");
  case SpirvVersion::V_1_2:
    return global_session->findProfile("spirv_1_2");
  case SpirvVersion::V_1_3:
    return global_session->findProfile("spirv_1_3");
  case SpirvVersion::V_1_4:
    return global_session->findProfile("spirv_1_4");
  case SpirvVersion::V_1_5:
    return global_session->findProfile("spirv_1_5");
  case SpirvVersion::V_1_6:
    return global_session->findProfile("spirv_1_6");
  default:
    MBASE_LOG_ERROR("Unknown SpirvVersion: {}", static_cast<uint32_t>(version));
    mbase::Trap();
    return SLANG_PROFILE_UNKNOWN;
  }
}

SlangEmitSpirvMethod ToSlangEmitSpirvMethod(SpirvVersion version) {
  switch (version) {
  case SpirvVersion::V_1_0:
  case SpirvVersion::V_1_1:
  case SpirvVersion::V_1_2:
    return SLANG_EMIT_SPIRV_VIA_GLSL;
  case SpirvVersion::V_1_3:
  case SpirvVersion::V_1_4:
  case SpirvVersion::V_1_5:
  case SpirvVersion::V_1_6:
    return SLANG_EMIT_SPIRV_DIRECTLY;
  default:
    MBASE_LOG_ERROR("Unknown SpirvVersion: {}", static_cast<uint32_t>(version));
    mbase::Trap();
    return SLANG_EMIT_SPIRV_DEFAULT;
  }
}

//
// ISlangBlob implementation
//

class MySlangBlob final : public ISlangBlob {
public:
  explicit MySlangBlob(std::vector<std::byte> bytes) :
      bytes_(std::move(bytes))
  {
  }
  explicit MySlangBlob(std::byte const* bytes, uint32_t size) :
      bytes_(reinterpret_cast<std::byte const*>(bytes), reinterpret_cast<std::byte const*>(bytes) + size)
  {
  }
  ~MySlangBlob() = default;
  MBASE_DISALLOW_COPY_MOVE(MySlangBlob);

  // {6BAB75C8-4DFC-4797-9ABA-07DCAE2365CC}
  SLANG_COM_INTERFACE(
    0x6bab75c8,
    0x4dfc,
    0x4797,
    {0x9a, 0xba, 0x7, 0xdc, 0xae, 0x23, 0x65, 0xcc}
  );

  //
  // ISlangUnknown implementation
  //

  SLANG_IUNKNOWN_QUERY_INTERFACE;
  SLANG_IUNKNOWN_ADD_REF;
  SLANG_IUNKNOWN_RELEASE;

  //
  // ISlangBlob implementation
  //

  SLANG_NO_THROW void const* SLANG_MCALL getBufferPointer() override {
    return bytes_.data();
  }

  SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() override {
    return bytes_.size();
  }

private:
  ISlangUnknown* getInterface(SlangUUID const& uuid) {
    if (uuid == MySlangBlob::getTypeGuid() || uuid == ISlangBlob::getTypeGuid() || uuid == ISlangUnknown::getTypeGuid()) {
      // No static_cast needed, because of no multiple inheritance.
      return this;
    }
    return nullptr;
  }

  uint32_t m_refCount = 0;

  std::vector<std::byte> bytes_;
};

/// ISlangFileSystem implementation that uses a user-provided include handler.
class MySlangFileSystem final : public ISlangFileSystem {
public:
  using DependencyCaptureFunc = std::function<void(SlangModuleDependency const& dependency)>;

  explicit MySlangFileSystem(SlangIncludeHandler opt_include_handler) :
      opt_include_handler_(opt_include_handler)
  {
  }
  ~MySlangFileSystem() = default;
  MBASE_DISALLOW_COPY_MOVE(MySlangFileSystem);

  // {04720C84-651C-44A8-A430-9C55C719E33E}
  SLANG_COM_INTERFACE(
    0x4720c84,
    0x651c,
    0x44a8,
    {0xa4, 0x30, 0x9c, 0x55, 0xc7, 0x19, 0xe3, 0x3e}
  );

  //
  // ISlangUnknown implementation
  //

  SLANG_IUNKNOWN_QUERY_INTERFACE;
  SLANG_IUNKNOWN_ADD_REF;
  SLANG_IUNKNOWN_RELEASE;

  //
  // ISlangCastable implementation
  //

  SLANG_NO_THROW void* SLANG_MCALL castAs(const SlangUUID& guid) override {
    return this->getInterface(guid);
  }

  //
  // ISlangFileSystem implementation
  //

  SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(char const* path, ISlangBlob** outBlob) override {
    MSLANG_TracyZoneScoped;

    if (!opt_include_handler_) {
      return SLANG_E_NOT_AVAILABLE;
    }
    
    std::optional<SlangIncludeResult> opt_result = opt_include_handler_(path);
    if (!opt_result.has_value()) {
      return SLANG_E_NOT_FOUND;
    }

    std::vector<std::byte>& bytes = opt_result->bytes;

    MySlangBlob* blob = new MySlangBlob(std::move(bytes));
    blob->addRef();

    {
      if (opt_dependency_capture_func_) {
        SlangModuleDependency dependency;
        dependency.unresolved_path = path;
        dependency.resolved_path   = opt_result->resolved_path;
        dependency.timestamp       = opt_result->timestamp;
        opt_dependency_capture_func_(dependency);
      }
    }

    *outBlob = blob;
    return SLANG_OK;
  }

  //
  // Local
  //

  void SetDependencyCaptureFunc(DependencyCaptureFunc func) {
    opt_dependency_capture_func_ = std::move(func);
  }

private:
  ISlangUnknown* getInterface(SlangUUID const& uuid) {
    if (uuid == MySlangFileSystem::getTypeGuid() || uuid == ISlangFileSystem::getTypeGuid() || uuid == ISlangCastable::getTypeGuid() || uuid == ISlangUnknown::getTypeGuid()) {
      // `static_cast` should be a good practice, even if there is no multiple inheritance here.
      return static_cast<ISlangUnknown*>(this);
    }
    return nullptr;
  }

  // Requirement of `SLANG_IUNKNOWN_QUERY_INTERFACE` and its friends.
  uint32_t m_refCount = 0;

  SlangIncludeHandler opt_include_handler_;

  DependencyCaptureFunc opt_dependency_capture_func_;
};

using Slang::ComPtr;

// We need to lock this mutex every time we access the global session or any objects created from it.
// This is far from ideal and we need to do better in the future.
static std::recursive_mutex s_global_session_mutex;

static ComPtr<slang::IGlobalSession> GetGlobalSession() {
  MSLANG_TracyZoneScoped;

  static ComPtr<slang::IGlobalSession> s_global_session;
  {
    if (s_global_session == nullptr) {
      MBASE_SCOPED_TIMER("slang::createGlobalSession");
      slang::createGlobalSession(s_global_session.writeRef());
    }
  }

  return s_global_session;
}

std::optional<std::vector<uint32_t>> CompileSlangModuleEntryPointToSpirv(
  slang::ISession* slang_session,
  slang::IModule* slang_module,
  char const* entry_point_name
) {
  MSLANG_TracyZoneScoped;
  MBASE_SCOPED_TIMER("CompileSlangModuleEntryPointToSpirv");

  std::lock_guard lock(s_global_session_mutex);

  ComPtr<slang::IEntryPoint> entry_point;
  slang_module->findEntryPointByName(entry_point_name, entry_point.writeRef());
  if (entry_point == nullptr) {
    MBASE_LOG_ERROR("Failed to find entry point: \"{}\"", entry_point_name);
    return std::nullopt;
  }

  std::vector<slang::IComponentType*> component_types;
  component_types.emplace_back(slang_module);
  component_types.emplace_back(entry_point.get());

  ComPtr<slang::IComponentType> composed_program;
  {
    ComPtr<slang::IBlob> diagnostics_blob;
    slang_session->createCompositeComponentType(
      component_types.data(),
      SlangInt(component_types.size()),
      composed_program.writeRef(),
      diagnostics_blob.writeRef()
    );

    RETURN_NULLOPT_IF_DIAGNOSTICS_NON_NULL(diagnostics_blob);
  }

  ComPtr<slang::IComponentType> linked_composed_program;
  {
    ComPtr<slang::IBlob> diagnostics_blob;
    composed_program->link(
      linked_composed_program.writeRef(),
      diagnostics_blob.writeRef()
    );

    RETURN_NULLOPT_IF_DIAGNOSTICS_NON_NULL(diagnostics_blob);
  }

  ComPtr<slang::IBlob> spirv_code;
  {
    ComPtr<slang::IBlob> diagnostics_blob;
    SlangResult const result = linked_composed_program->getEntryPointCode(
      0,
      0,
      spirv_code.writeRef(),
      diagnostics_blob.writeRef()
    );

    if (diagnostics_blob != nullptr) {
      if (SLANG_FAILED(result)) {
        MBASE_LOG_ERROR("slang diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
        mbase::Trap();
        return std::nullopt;
      }
      else {
        MBASE_LOG_WARN("slang diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
      }
    }
  }

  MBASE_LOG_INFO("SPIR-V code size: {}, word count: {}", spirv_code->getBufferSize(), spirv_code->getBufferSize() / 4);

  std::vector<uint32_t> result;
  result.resize(spirv_code->getBufferSize() / 4);
  memcpy(result.data(), spirv_code->getBufferPointer(), spirv_code->getBufferSize());

  return result;
}

} // namespace

namespace {

constexpr SpirvVersion kSpirvVersion = SpirvVersion::V_1_3;

std::vector<slang::CompilerOptionEntry> MakeCompilerOptions(
  SlangCapabilityID additional_capability
) {
  std::vector<slang::CompilerOptionEntry> compiler_options;
  compiler_options.emplace_back(
    slang::CompilerOptionEntry {
      .name = slang::CompilerOptionName::Capability,
      .value = { slang::CompilerOptionValueKind::Int, additional_capability, 0, nullptr, nullptr },
    }
  );
  compiler_options.emplace_back(
    slang::CompilerOptionEntry {
      .name = slang::CompilerOptionName::EmitSpirvMethod,
      .value = { slang::CompilerOptionValueKind::Int, ToSlangEmitSpirvMethod(kSpirvVersion), 0, nullptr, nullptr},
    }
  );
#if 1
  compiler_options.emplace_back(
    slang::CompilerOptionEntry {
      .name = slang::CompilerOptionName::DebugInformation,
      .value = { slang::CompilerOptionValueKind::Int, SlangDebugInfoLevel::SLANG_DEBUG_INFO_LEVEL_NONE, 0, nullptr, nullptr },
    }
  );
#endif

  return compiler_options;
}

/// - `file_system`: MUST be valid until the use of `out_desc` has completed. Is not `addRef`ed by this function.
void MakeSlangSessionDesc(
  slang::SessionDesc& out_desc,
  slang::TargetDesc const& target_desc,
  ISlangFileSystem* file_system,
  std::span<slang::CompilerOptionEntry> compiler_options
) {
  out_desc.targetCount = 1;
  out_desc.targets = &target_desc;
  out_desc.flags = slang::kSessionFlags_None;
  out_desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
  out_desc.searchPathCount = 0;
  out_desc.searchPaths = nullptr;
  out_desc.preprocessorMacroCount = 0;
  out_desc.preprocessorMacros = nullptr;
  out_desc.fileSystem = file_system;
  out_desc.compilerOptionEntries = compiler_options.data();
  out_desc.compilerOptionEntryCount = static_cast<uint32_t>(compiler_options.size());
}

} // namespace

//
// ISlangSession implementation
//

class SlangSession final : public ISlangSession {
public:
  explicit SlangSession(
    ComPtr<slang::IGlobalSession> global_session,
    ComPtr<slang::ISession> session,
    ComPtr<MySlangFileSystem> file_system
  ) :
      global_session_(std::move(global_session)),
      session_(std::move(session)),
      file_system_(std::move(file_system))
  {
  }
  ~SlangSession() override = default;
  MBASE_DISALLOW_COPY_MOVE(SlangSession);

  //
  // ISlangSession implementation
  //

  void Destroy() override {
    delete this;
  }

  std::span<SlangModuleDependency const> GetModuleDependencies(slang::IModule* root_slang_module) override {
    if (root_slang_module == nullptr) {
      mbase::LockGuard lock(root_slang_modules_mutex_);

      if (root_slang_modules_.empty()) {
        MBASE_LOG_ERROR("ISlangSession::GetModuleDependencies: No module has been added.");
        return {};
      }

      root_slang_module = root_slang_modules_.back().get();
    }

    mbase::LockGuard lock(root_slang_modules_mutex_);

    auto it = module_dependencies_.find(root_slang_module);
    if (it == module_dependencies_.end()) {
      return {};
    }

    std::vector<SlangModuleDependency>& dependencies = it->second;
    return std::span<SlangModuleDependency const>(dependencies.data(), dependencies.size());
  }

  bool AddModuleFromCode(
    char const* module_name,
    char const* path,
    char const* slang_code,
    slang::IModule** opt_out_module
  ) override {
    MSLANG_TracyZoneScoped;
    MBASE_SCOPED_TIMER("ISlangSession::AddModuleFromCode");

    std::lock_guard global_session_lock(s_global_session_mutex);

    std::vector<SlangModuleDependency> captured_dependencies;
    file_system_->SetDependencyCaptureFunc(
      [&captured_dependencies](SlangModuleDependency const& dependency) {
        MBASE_LOG_TRACE("Dependency captured: unresolved_path=\"{}\", resolved_path=\"{}\", timestamp={}", dependency.unresolved_path, dependency.resolved_path, dependency.timestamp);
        captured_dependencies.emplace_back(dependency);
      }
    );

    slang::IModule* slang_module = nullptr;
    {
      ComPtr<slang::IBlob> diagnostics_blob;
      slang_module = session_->loadModuleFromSourceString(module_name, path, slang_code, diagnostics_blob.writeRef());

      if (slang_module == nullptr) {
        MBASE_LOG_ERROR("AddModuleFromCode: slang::ISession::loadModuleFromSourceString failed. module_name=\"{}\", path\"{}\"", module_name, path);
        if (diagnostics_blob) {
          MBASE_LOG_ERROR("AddModuleFromCode: slang diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
          mbase::Trap();

          file_system_->SetDependencyCaptureFunc(nullptr);
          return false;
        }
      }
      else if (diagnostics_blob) {
        MBASE_LOG_WARN("AddModuleFromCode: slang diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
      }
    }

    file_system_->SetDependencyCaptureFunc(nullptr);

    {
      int32_t dep_file_count = slang_module->getDependencyFileCount();
      for (int32_t i = 0; i < dep_file_count; ++i) {
        char const* dep_file_path = slang_module->getDependencyFilePath(i);
        MBASE_LOG_INFO("AddModuleFromCode: slang dependency file[{}]: {}", i, dep_file_path);
      }
    }

    {
      mbase::LockGuard lock(root_slang_modules_mutex_);

      // Keep the referece count at 1.
      root_slang_modules_.emplace_back(ComPtr(Slang::INIT_ATTACH, slang_module));

      module_dependencies_[slang_module] = std::move(captured_dependencies);
    }

    if (opt_out_module != nullptr) {
      *opt_out_module = slang_module;
      slang_module->addRef();
    }

    {
      SlangInt count = session_->getLoadedModuleCount();
      MBASE_LOG_INFO("AddModuleFromCode: Total loaded module count: {}", count);
      for (SlangInt i = 0; i < count; ++i) {
        slang::IModule* module = session_->getLoadedModule(i);
        MBASE_LOG_INFO("AddModuleFromCode:   Loaded module[{}]: name=\"{}\"", i, module->getName());
      }
    }

    return true;
  }

  bool AddModuleFromSerializedBytes(
    char const* module_name,
    char const* path,
    std::byte const* slang_module_bytes,
    uint32_t slang_module_size_in_bytes,
    slang::IModule** opt_out_module
  ) override {
    MSLANG_TracyZoneScoped;
    MBASE_SCOPED_TIMER("ISlangSession::AddModuleFromSerializedBytes");

    std::lock_guard global_session_lock(s_global_session_mutex);

    ComPtr<MySlangBlob> bytes_blob = ComPtr(new MySlangBlob(slang_module_bytes, slang_module_size_in_bytes));

    std::vector<SlangModuleDependency> captured_dependencies;
    file_system_->SetDependencyCaptureFunc(
      [&captured_dependencies](SlangModuleDependency const& dependency) {
        MBASE_LOG_TRACE("Dependency captured: unresolved_path=\"{}\", resolved_path=\"{}\", timestamp={}", dependency.unresolved_path, dependency.resolved_path, dependency.timestamp);
        captured_dependencies.emplace_back(dependency);
      }
    );

    slang::IModule* slang_module = nullptr;
    {
      ComPtr<slang::IBlob> diagnostics_blob;

      slang_module = session_->loadModuleFromIRBlob(
        module_name,
        path,
        bytes_blob.get(),
        diagnostics_blob.writeRef()
      );

      if (slang_module == nullptr) {
        MBASE_LOG_ERROR("AddModuleFromSerializedBytes: slang::ISession::loadModuleFromIRBlob failed. module_name=\"{}\", path\"{}\"", module_name, path);
        if (diagnostics_blob) {
          MBASE_LOG_ERROR("AddModuleFromSerializedBytes: slang diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
          mbase::Trap();

          file_system_->SetDependencyCaptureFunc(nullptr);
          return false;
        }
      }
      else if (diagnostics_blob) {
        MBASE_LOG_WARN("AddModuleFromSerializedBytes: slang diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
      }
    }

    file_system_->SetDependencyCaptureFunc(nullptr);

    {
      int32_t dep_file_count = slang_module->getDependencyFileCount();
      for (int32_t i = 0; i < dep_file_count; ++i) {
        char const* dep_file_path = slang_module->getDependencyFilePath(i);
        MBASE_LOG_INFO("AddModuleFromSerializedBytes: slang dependency file[{}]: {}", i, dep_file_path);
      }
    }

    {
      mbase::LockGuard lock(root_slang_modules_mutex_);

      // Keep the referece count at 1.
      root_slang_modules_.emplace_back(ComPtr(Slang::INIT_ATTACH, slang_module));

      module_dependencies_[slang_module] = std::move(captured_dependencies);
    }

    if (opt_out_module != nullptr) {
      *opt_out_module = slang_module;
      slang_module->addRef();
    }

    return true;
  }

  std::optional<std::vector<uint32_t>> CompileModuleEntryPointToSpirv(
    slang::IModule* root_slang_module,
    char const* entry_point_name
  ) override {
    MSLANG_TracyZoneScoped;
    MBASE_SCOPED_TIMER("ISlangSession::CompileModuleEntryPointToSpirv");

    if (root_slang_module == nullptr) {
      mbase::LockGuard lock(root_slang_modules_mutex_);

      if (root_slang_modules_.empty()) {
        MBASE_LOG_ERROR("ISlangSession::CompileModuleEntryPointToSpirv: No module has been added.");
        return std::nullopt;
      }

      root_slang_module = root_slang_modules_.back().get();
    }

    return CompileSlangModuleEntryPointToSpirv(
      session_.get(),
      root_slang_module,
      entry_point_name
    );
  }

  bool SerializeModule(
    slang::IModule* root_slang_module,
    std::vector<std::byte>& out_serialized_bytes
  ) override {
    MSLANG_TracyZoneScoped;

    if (root_slang_module == nullptr) {
      mbase::LockGuard lock(root_slang_modules_mutex_);

      if (root_slang_modules_.empty()) {
        MBASE_LOG_ERROR("ISlangSession::SerializeModule: No module has been added.");
        return false;
      }

      root_slang_module = root_slang_modules_.back().get();
    }

    MBASE_SCOPED_TIMER("ISlangSession::SerializeModule");

    ComPtr<slang::IBlob> serialized_blob;
    SlangResult result = root_slang_module->serialize(serialized_blob.writeRef());
    if (SLANG_FAILED(result)) {
      MBASE_LOG_ERROR("ISlangSession::SerializeModule: slang serialization failed");
      return false;
    }

    size_t const blob_size = serialized_blob->getBufferSize();
    out_serialized_bytes.resize(blob_size);
    memcpy(out_serialized_bytes.data(), serialized_blob->getBufferPointer(), blob_size);
    return true;
  }

private:
  ComPtr<slang::IGlobalSession> global_session_; // Hold on to the `slang::IGlobalSession`.
  ComPtr<slang::ISession> session_;
  ComPtr<MySlangFileSystem> file_system_;

  mbase::Lockable<std::mutex> root_slang_modules_mutex_;
  std::vector<ComPtr<slang::IModule>> root_slang_modules_ MBASE_GUARDED_BY(root_slang_modules_mutex_);

  std::unordered_map<slang::IModule*, std::vector<SlangModuleDependency>> module_dependencies_ MBASE_GUARDED_BY(root_slang_modules_mutex_);
};

ISlangSession* ISlangSession::Create(
  SlangIncludeHandler opt_include_handler,
  char const* capability_name
) {
  MSLANG_TracyZoneScoped;
  MBASE_SCOPED_TIMER("ISlangSession::Create");

  ComPtr<MySlangFileSystem> file_system = ComPtr(new MySlangFileSystem(opt_include_handler));

  std::lock_guard lock(s_global_session_mutex);
  ComPtr<slang::IGlobalSession> global_session = GetGlobalSession();

  SlangCapabilityID const additional_capability = global_session->findCapability(capability_name);

  slang::TargetDesc target_desc;
  target_desc.format = SLANG_SPIRV;
  target_desc.profile = ToProfileID(kSpirvVersion, global_session.get());
  target_desc.flags = 0; // Currently unused.

  std::vector<slang::CompilerOptionEntry> compiler_options = MakeCompilerOptions(additional_capability);

  slang::SessionDesc session_desc;
  MakeSlangSessionDesc(
    session_desc,
    target_desc,
    file_system.get(),
    std::span<slang::CompilerOptionEntry>(compiler_options.data(), compiler_options.size())
  );

  ComPtr<slang::ISession> session;
  global_session->createSession(session_desc, session.writeRef());

  // `file_system` should have had its reference count increased by `createSession`.

  return new SlangSession(
    global_session,
    std::move(session),
    std::move(file_system)
  );
}


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
) {
  MSLANG_TracyZoneScoped;
  MBASE_SCOPED_TIMER("mslang::CompileSlangToSpirv");

  std::lock_guard lock(s_global_session_mutex);

  ISlangSession* slang_session = ISlangSession::Create(
    opt_include_handler,
    capability_name
  );
  if (slang_session == nullptr) {
    MBASE_LOG_ERROR("Failed to create Slang session");
    return std::nullopt;
  }

  bool result = slang_session->AddModuleFromCode(
    module_name,
    path,
    slang_code,
    nullptr
  );
  if (!result) {
    MBASE_LOG_ERROR("Failed to add Slang module: \"{}\" at path: \"{}\"", module_name, path);
    slang_session->Destroy();
    return std::nullopt;
  }

  std::optional<std::vector<uint32_t>> opt_spirv_code = slang_session->CompileModuleEntryPointToSpirv(
    nullptr,
    entry_point_name
  );

  slang_session->Destroy();

  return opt_spirv_code;
}

std::optional<std::string> CompileSlangToWgsl(
  char const* module_name,
  char const* path,
  char const* slang_code,
  SlangIncludeHandler opt_include_handler,
  char const* entry_point_name
) {
  MSLANG_TracyZoneScoped;
  MBASE_SCOPED_TIMER("mslang::CompileSlangToWgsl");

  std::lock_guard lock(s_global_session_mutex);

  ComPtr<MySlangFileSystem> file_system = ComPtr(new MySlangFileSystem(opt_include_handler));
  ComPtr<slang::IGlobalSession> global_session = GetGlobalSession();

  // Configure for WGSL output
  slang::TargetDesc target_desc {};
  target_desc.format = SLANG_WGSL;
  target_desc.profile = SLANG_PROFILE_UNKNOWN; // WGSL doesn't use profiles
  target_desc.flags = 0;

  slang::SessionDesc session_desc {};
  session_desc.targetCount = 1;
  session_desc.targets = &target_desc;
  session_desc.flags = slang::kSessionFlags_None;
  session_desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
  session_desc.fileSystem = file_system.get();

  ComPtr<slang::ISession> session;
  global_session->createSession(session_desc, session.writeRef());
  if (session == nullptr) {
    MBASE_LOG_ERROR("Failed to create Slang session for WGSL");
    return std::nullopt;
  }

  // Load module from source
  slang::IModule* slang_module = nullptr;
  {
    ComPtr<slang::IBlob> diagnostics_blob;
    slang_module = session->loadModuleFromSourceString(
      module_name,
      path,
      slang_code,
      diagnostics_blob.writeRef()
    );

    if (slang_module == nullptr) {
      MBASE_LOG_ERROR("CompileSlangToWgsl: Failed to load module \"{}\" from \"{}\"", module_name, path);
      if (diagnostics_blob) {
        MBASE_LOG_ERROR("CompileSlangToWgsl: diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
      }
      return std::nullopt;
    }
    else if (diagnostics_blob) {
      MBASE_LOG_WARN("CompileSlangToWgsl: diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
    }
  }

  // Find entry point
  ComPtr<slang::IEntryPoint> entry_point;
  slang_module->findEntryPointByName(entry_point_name, entry_point.writeRef());
  if (entry_point == nullptr) {
    MBASE_LOG_ERROR("CompileSlangToWgsl: Failed to find entry point \"{}\"", entry_point_name);
    return std::nullopt;
  }

  // Create composite and link
  std::vector<slang::IComponentType*> component_types;
  component_types.emplace_back(slang_module);
  component_types.emplace_back(entry_point.get());

  ComPtr<slang::IComponentType> composed_program;
  {
    ComPtr<slang::IBlob> diagnostics_blob;
    session->createCompositeComponentType(
      component_types.data(),
      SlangInt(component_types.size()),
      composed_program.writeRef(),
      diagnostics_blob.writeRef()
    );

    if (diagnostics_blob) {
      MBASE_LOG_ERROR("CompileSlangToWgsl: composite diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
      return std::nullopt;
    }
  }

  ComPtr<slang::IComponentType> linked_program;
  {
    ComPtr<slang::IBlob> diagnostics_blob;
    composed_program->link(
      linked_program.writeRef(),
      diagnostics_blob.writeRef()
    );

    if (diagnostics_blob) {
      MBASE_LOG_ERROR("CompileSlangToWgsl: link diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
      return std::nullopt;
    }
  }

  // Get WGSL code
  ComPtr<slang::IBlob> wgsl_code;
  {
    ComPtr<slang::IBlob> diagnostics_blob;
    SlangResult const result = linked_program->getEntryPointCode(
      0, // entry point index
      0, // target index
      wgsl_code.writeRef(),
      diagnostics_blob.writeRef()
    );

    if (diagnostics_blob != nullptr) {
      if (SLANG_FAILED(result)) {
        MBASE_LOG_ERROR("CompileSlangToWgsl: getEntryPointCode diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
        return std::nullopt;
      }
      else {
        MBASE_LOG_WARN("CompileSlangToWgsl: getEntryPointCode diagnostics: {}", (char const*)diagnostics_blob->getBufferPointer());
      }
    }
  }

  // Convert blob to string
  std::string wgsl_string(
    static_cast<char const*>(wgsl_code->getBufferPointer()),
    wgsl_code->getBufferSize()
  );

  MBASE_LOG_INFO("CompileSlangToWgsl: WGSL code size: {} bytes", wgsl_string.size());

  return wgsl_string;
}

} // namespace mslang
