#pragma once

// c++ headers ------------------------------------------
#include <string>
#include <vector>
#include <span>

// external headers -------------------------------------
#include <cereal/archives/portable_binary.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>

// public project headers -------------------------------
#include "mbase/public/log.h"

// project headers --------------------------------------
#include "slang_cache-stream.h"

namespace mslang {

struct SlangModuleIdentity final {
  /// Full path to the `.slang` file, excluding the file name. Directory separators MUST be normalized to `/`.
  std::string parent_path;
  /// File name without `.slang` or `.slang-module` extension.
  std::string module_name;

  auto operator<=>(SlangModuleIdentity const&) const = default;

  template<class Archive>
  void serialize(Archive& ar) {
    ar(this->parent_path);
    ar(this->module_name);
  }
};

struct PreprocessorMacroOwned final {
  std::string name;
  std::string value;

  auto operator<=>(PreprocessorMacroOwned const&) const = default;

  template<class Archive>
  void serialize(Archive& ar) {
    ar(this->name);
    ar(this->value);
  }
};

struct PreprocessorMacro final {
  std::string_view name;
  std::string_view value;

  PreprocessorMacro() = default;
  ~PreprocessorMacro() = default;
  explicit PreprocessorMacro(PreprocessorMacroOwned const& owned) :
      name(owned.name),
      value(owned.value)
  {
  }
  MBASE_DEFAULT_COPY_MOVE(PreprocessorMacro);

  PreprocessorMacroOwned ToOwned() const {
    return PreprocessorMacroOwned {
      .name = std::string(this->name),
      .value = std::string(this->value),
    };
  }

  auto operator<=>(PreprocessorMacro const&) const = default;

  struct Hasher final {
    size_t operator()(PreprocessorMacro const& v) const;
  };
};

//
// SscMeta
//

struct SlangCodeMeta final {
  /// UNIX timestamp of the `.slang` file.
  uint64_t timestamp = 0;

  template <class Archive>
  void serialize(Archive& ar) {
    ar(this->timestamp);
  }
};

enum class SlangCodeDependencyType : uint32_t {
  kReferencedAsset,
};

struct SlangCodeDependency final {
  /// The path as requested by the Slang `import` declaration.
  std::string unresolved_path;
  /// Resolved path, including the extension.
  std::string resolved_path;
  SlangCodeMeta meta;
  SlangCodeDependencyType type = SlangCodeDependencyType::kReferencedAsset;

  template<class Archive>
  void serialize(Archive& ar) {
    ar(this->unresolved_path);
    ar(this->resolved_path);
    ar(this->meta);
    ar(this->type);
  }
};

struct SscModuleMeta final {
  std::vector<SlangCodeDependency> code_dependencies;

  template<class Archive>
  void serialize(Archive& ar) {
    ar(this->code_dependencies);
  }
};

struct SscMeta final {
  uint64_t hash = 0;
  SlangModuleIdentity root_module_identity;
  std::vector<PreprocessorMacroOwned> preprocessor_macros;

  SscModuleMeta root_module_meta;

  template<class Archive>
  void serialize(Archive& ar) {
    ar(this->hash);
    ar(this->root_module_identity);
    ar(this->preprocessor_macros);

    ar(this->root_module_meta);
  }

  void Serialize(std::vector<std::byte>& out_bytes) const {
    ByteVectorOutputStream stream(out_bytes);
    cereal::PortableBinaryOutputArchive archive(stream);
    archive(*this);
  }

  bool Deserialize(std::span<std::byte const> bytes) {
    SpanStreambuf streambuf(bytes);
    std::istream stream(&streambuf);

    try {
      cereal::PortableBinaryInputArchive archive(stream);

      archive(*this);
    }
    catch (cereal::Exception const& e) {
      MBASE_LOG_ERROR("SscMeta::Deserialize: Cereal exception: {}", e.what());
      return false;
    }
    return true;
  }
};

//
// Ssc
//

struct SscModule final {
  /// Serialized Slang module bytes.
  std::vector<std::byte> serialized_bytes;

  template<class Archive>
  void serialize(Archive& ar) {
    ar(this->serialized_bytes);
  }
};

struct Ssc final {
  SscModule root_module;

  template<class Archive>
  void serialize(Archive& ar) {
    ar(this->root_module);
  }

  void Serialize(std::vector<std::byte>& out_bytes) const {
    ByteVectorOutputStream stream(out_bytes);
    cereal::PortableBinaryOutputArchive archive(stream);
    archive(*this);
  }

  bool Deserialize(std::span<std::byte const> bytes) {
    SpanStreambuf streambuf(bytes);
    std::istream stream(&streambuf);

    try {
      cereal::PortableBinaryInputArchive archive(stream);

      archive(*this);
    }
    catch (cereal::Exception const& e) {
      MBASE_LOG_ERROR("Ssc::Deserialize: Cereal exception: {}", e.what());
      return false;
    }
    return true;
  }
};

} // namespace mslang
