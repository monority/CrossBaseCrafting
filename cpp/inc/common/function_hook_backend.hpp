/**
 * @file function_hook_backend.hpp
 * @brief Pure-value determination of the native/Blueprint-script hook backend for a UFunction.
 */
#pragma once

#include <cstdint>

namespace pal_game {

/** @brief Callback backend available for a UFunction. */
enum class FunctionHookBackend : std::uint8_t {
    nativeFunction,
    scriptFunction,
    unsupported,
};

/**
 * @brief Selects the hook backend based on the runtime function entry point and flags.
 * @param[in] hasFunctionPointer Whether the UFunction has an underlying call entry point.
 * @param[in] usesProcessInternal Whether the underlying entry point is the Blueprint VM ProcessInternal.
 * @param[in] nativeFlag Whether the UFunction has the FUNC_Native flag.
 * @return The single safe backend; returns unsupported for inconsistent combinations.
 */
[[nodiscard]] constexpr auto select_function_hook_backend(const bool hasFunctionPointer,
                                                          const bool usesProcessInternal,
                                                          const bool nativeFlag) noexcept
    -> FunctionHookBackend {
    if (!hasFunctionPointer) {
        return FunctionHookBackend::unsupported;
    }
    if (!usesProcessInternal && nativeFlag) {
        return FunctionHookBackend::nativeFunction;
    }
    if (usesProcessInternal && !nativeFlag) {
        return FunctionHookBackend::scriptFunction;
    }
    return FunctionHookBackend::unsupported;
}

}  // namespace pal_game

