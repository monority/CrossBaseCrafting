/**
 * @file diagnostics.hpp
 * @brief Compile-time gated verbose diagnostics shared by the resource-sharing modules.
 * @details Keeping the switch here (instead of duplicating it per translation unit) guarantees that
 *          every `[CBC-DIAG]` trace is compiled out of release builds in exactly one place.
 */
#pragma once

#include <utility>

#include <DynamicOutput/DynamicOutput.hpp>

namespace base_resource_sharing {
/** @brief Master switch for the verbose `[CBC-DIAG]` traces. Release builds keep it off. */
inline constexpr bool kDiagnostics = false;

/**
 * @brief Emits a verbose diagnostic only when kDiagnostics is enabled.
 * @details Implemented as a discarded `if constexpr` branch so the format arguments (including any
 *          std::wstring conversions) are not even evaluated when diagnostics are compiled out.
 */
template <typename Format, typename... Args>
auto log_diag(Format&& format, Args&&... args) -> void {
    if constexpr (kDiagnostics) {
        RC::Output::send<RC::LogLevel::Verbose>(std::forward<Format>(format),
                                                std::forward<Args>(args)...);
    }
}
}  // namespace base_resource_sharing
