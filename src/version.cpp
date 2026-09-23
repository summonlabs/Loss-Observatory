#include "loss_observatory/version.hpp"

#include <string>

namespace loss_observatory {

VersionInfo version_info() noexcept {
  VersionInfo info{};
  info.major = kVersionMajor;
  info.minor = kVersionMinor;
  info.patch = kVersionPatch;
  info.persist_format = kPersistFormatVersion;
  info.semantics_revision = kSemanticsRevision;
#if defined(LO_WITH_ASAN)
  info.address_sanitizer = true;
#endif
#if defined(LO_WITH_UBSAN)
  info.undefined_behavior_sanitizer = true;
#endif
#if defined(LO_ASAN_SUPPORTED) || defined(LO_UBSAN_SUPPORTED)
  info.sanitizer_build_supported = true;
#endif
  return info;
}

std::string version_string() {
  return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." +
         std::to_string(kVersionPatch);
}

std::string full_version_string() {
  const VersionInfo info = version_info();
  std::string result(kProductName);
  result.push_back(' ');
  result.append(version_string());
  result.append(" (persist v");
  result.append(std::to_string(info.persist_format));
  result.append(", semantics v");
  result.append(std::to_string(info.semantics_revision));
  result.append(", asan:");
  result.append(info.address_sanitizer ? "on" : "off");
  result.append(", ubsan:");
  result.append(info.undefined_behavior_sanitizer ? "on" : "off");
  result.push_back(')');
  return result;
}

}  // namespace loss_observatory
