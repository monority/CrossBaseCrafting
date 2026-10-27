#pragma once
/**
 * @file material_detour.hpp
 * @brief Installs the guarded material-count detour; no-op on non-matching game builds.
 */
namespace base_resource_sharing {
auto install_material_count_detour() -> void;
}  // namespace base_resource_sharing
