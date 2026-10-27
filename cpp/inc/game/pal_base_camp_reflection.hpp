/**
 * @file pal_base_camp_reflection.hpp
 * @brief Single-call base-camp reflection primitives shared by the remote terminal and resource sharing.
 * @details Unified signature verification and single call; no per-feature permissions, error tolerance, or retry logic.
 */
#pragma once

#include <vector>

#include <Unreal/UnrealCoreStructs.hpp>

namespace RC::Unreal {
class UObject;
}

namespace pal_base_camp_reflection {
/** @brief Calls PalBaseCampManager:GetBaseCampIds. */
[[nodiscard]] auto read_base_ids(RC::Unreal::UObject* manager,
                                 std::vector<RC::Unreal::FGuid>& output) -> bool;

/** @brief Calls PalBaseCampManager:TryGetModel. */
[[nodiscard]] auto try_get_base_model(RC::Unreal::UObject* manager, const RC::Unreal::FGuid& baseId,
                                      RC::Unreal::UObject*& model) -> bool;

/** @brief Calls PalMapObjectManager:FindConcreteModel. */
[[nodiscard]] auto find_concrete_model(RC::Unreal::UObject* manager,
                                       const RC::Unreal::FGuid& instanceId,
                                       RC::Unreal::UObject*& concreteModel) -> bool;
}  // namespace pal_base_camp_reflection

