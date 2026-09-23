#pragma once

#include <string>
#include <string_view>

// Loss Observatory -- vendor-neutral packet-loss observation, attribution, and
// localization runtime.
//
// Scope boundary (enforced by design, tests, and documentation):
//   OWNED      : observation, attribution, localization, evidence lifecycle,
//                history, persistence, and deterministic explanation of loss.
//   NOT OWNED  : path repair, rerouting, congestion control, blackhole
//                declaration, or any assumption that missing telemetry is loss.

namespace loss_observatory {

inline constexpr int kVersionMajor = LO_VERSION_MAJOR;
inline constexpr int kVersionMinor = LO_VERSION_MINOR;
inline constexpr int kVersionPatch = LO_VERSION_PATCH;

/// Product name used by every tool, report, and exported artifact.
inline constexpr std::string_view kProductName = "Loss Observatory";

/// Persistence format identity. Bumped whenever the on-disk encoding changes.
inline constexpr std::uint32_t kPersistFormatVersion = 1;

/// Stable identifier for the semantics revision of classification/attribution
/// rules. Any change to rule ordering or thresholds must bump this value so
/// persisted explanations remain interpretable.
inline constexpr std::uint32_t kSemanticsRevision = 1;

struct VersionInfo {
  int major{};
  int minor{};
  int patch{};
  std::uint32_t persist_format{};
  std::uint32_t semantics_revision{};
  bool address_sanitizer{};
  bool undefined_behavior_sanitizer{};
  bool sanitizer_build_supported{};
};

[[nodiscard]] VersionInfo version_info() noexcept;

/// "1.0.0"
[[nodiscard]] std::string version_string();

/// "Loss Observatory 1.0.0 (persist v1, semantics v1, asan:off, ubsan:off)"
[[nodiscard]] std::string full_version_string();

}  // namespace loss_observatory
