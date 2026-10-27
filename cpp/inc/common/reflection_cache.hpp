#pragma once
#include <cstddef>
#include <unordered_map>

#include <Unreal/FProperty.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>

namespace pal_game {
/**
 * @brief Cache of reflection metadata (never gameplay instances), invalidated on world change.
 * @details Holds UClass, UFunction, and FProperty pointers only; all stable for the life of one
 *          Unreal world. Any miss falls back to the existing by-name resolution (reference
 *          behaviour). invalidate() must run on world begin so a prior world's pointers are
 *          never reused. Read/write is game-thread only; no locking required.
 */
struct ReflectionCache {
    // CDO /Script/Pal.Default__PalUtility
    RC::Unreal::UObject* palUtility{};
    // /Script/Pal.PalBaseCampModuleItemStorage class
    RC::Unreal::UClass* storageClass{};
    // Return/input property per hooked UFunction (ConcreteModel, ...)
    std::unordered_map<RC::Unreal::UFunction*, RC::Unreal::FProperty*> paramByFunction;
    // Exact parameter count per UFunction (immutable within a world)
    std::unordered_map<RC::Unreal::UFunction*, std::size_t> paramCountByFunction;

    auto invalidate() noexcept -> void {
        palUtility = nullptr;
        storageClass = nullptr;
        paramByFunction.clear();
        paramCountByFunction.clear();
    }
};

// Process-wide cache instance (single game thread).
inline auto reflection_cache() -> ReflectionCache& {
    static ReflectionCache cache;
    return cache;
}
}  // namespace pal_game
