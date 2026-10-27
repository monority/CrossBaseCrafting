/**
 * @file current_base_resolution.hpp
 * @brief Defines the current-base reflection routing and its pure-value acceptance rules.
 */
#pragma once

#include <optional>
#include <string_view>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
/**
 * @brief Provides the unique set of current-base reflection names for a UE4SS character type.
 * @tparam Character The character type used by the UE4SS reflection API.
 */
template <typename Character>
struct CurrentBaseReflectionNames;

template <>
struct CurrentBaseReflectionNames<char> {
    static constexpr std::string_view controllerPawnFunction{"K2_GetPawn"};
    static constexpr std::string_view insideComponentProperty{"InsideBaseCampCheckComponent"};
    static constexpr std::string_view insideBaseModelFunction{"GetInsideBaseCampModel"};
    static constexpr std::string_view baseIdFunction{"GetId"};
};

template <>
struct CurrentBaseReflectionNames<wchar_t> {
    static constexpr std::wstring_view controllerPawnFunction{L"K2_GetPawn"};
    static constexpr std::wstring_view insideComponentProperty{L"InsideBaseCampCheckComponent"};
    static constexpr std::wstring_view insideBaseModelFunction{L"GetInsideBaseCampModel"};
    static constexpr std::wstring_view baseIdFunction{L"GetId"};
};

/** @brief Current-base resolution must not fall back to nearest-base distance guessing. */
inline constexpr bool kAllowsNearestBaseFallback{false};

/**
 * @brief Only accepts a current base that is valid and has a guild storage module.
 */
[[nodiscard]] constexpr auto accept_current_base(const GuidKey candidate,
                                                 const bool hasStorageModule) noexcept
    -> std::optional<GuidKey> {
    return candidate.valid() && hasStorageModule ? std::optional{candidate} : std::nullopt;
}
}  // namespace base_resource_sharing
