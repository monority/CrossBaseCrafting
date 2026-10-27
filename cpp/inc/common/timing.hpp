#pragma once
#include <chrono>

namespace base_resource_sharing {
/**
 * @brief Cadences du mod — un seul endroit à ajuster.
 * @details Centralisées pour le tuning : chaque littéral de délai éparpillé dans le .cpp devenait
 *          invisible. Changer la cadence = changer la valeur ici, jamais chercher dans la logique.
 */
inline constexpr auto kPresenceInterval = std::chrono::milliseconds{500};
inline constexpr auto kUnionMaintenanceInterval = std::chrono::milliseconds{500};
inline constexpr auto kUnionCompletenessInterval = std::chrono::seconds{10};
inline constexpr auto kReconcileDebounce = std::chrono::seconds{2};
inline constexpr auto kHookRetryInterval = std::chrono::seconds{5};
}  // namespace base_resource_sharing
