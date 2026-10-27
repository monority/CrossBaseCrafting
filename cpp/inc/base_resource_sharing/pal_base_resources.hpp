#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <base_resource_sharing/persistent_union.hpp>
#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
struct BaseResourceSharingSnapshot {
    /** @brief Whether the user has enabled in-process resource sharing. */
    bool enabled{};
    /** @brief Whether the current world allows access to Unreal objects. */
    bool worldAccessible{};
    /** @brief Current LoadMap world generation number. */
    std::uint64_t worldGeneration{};
    /** @brief Number of guild base camps currently discovered. */
    std::size_t baseCount{};
    /** @brief Number of native storage chests already loaded. */
    std::size_t containerCount{};
    /** @brief Number of registered chests whose ConcreteModel is not yet loaded. */
    std::size_t pendingContainerCount{};
    /** @brief Current lifecycle phase of the persistent guild storage graph. */
    PersistentUnionPhase persistentPhase{PersistentUnionPhase::off};
    /** @brief Number of native registration edges verified and recorded by this mod. */
    std::size_t appliedEdgeCount{};
    /** @brief Number of pending add/remove edges in the current-frame work queue. */
    std::size_t pendingEdgeCount{};
    /** @brief Capability states for crafting, building, and repair. */
    std::array<CapabilityState, 3> capabilities{};
    /** @brief The native consumer surface currently exposed to Palworld for shared materials. */
    ResourceConsumerSurface consumerSurface{ResourceConsumerSurface::none};
    /** @brief Duration of the most recent catalog discovery pass, in milliseconds. */
    double lastCatalogMilliseconds{};
    /** @brief Duration of the most recent successful catalog discovery pass, in milliseconds. */
    double lastSuccessfulCatalogMilliseconds{};
    /** @brief Maximum catalog discovery duration this world, in milliseconds. */
    double maximumCatalogMilliseconds{};
    /** @brief Total catalog discovery attempts this world. */
    std::size_t catalogAttemptCount{};
    /** @brief Whether the system was safety-disabled due to an unverifiable write or restore error. */
    bool safetyDisabled{};
    /** @brief Human-readable status string suitable for display in the GUI. */
    std::string status;
};

class PalBaseResourceBridge final {
public:
    PalBaseResourceBridge();
    ~PalBaseResourceBridge();
    PalBaseResourceBridge(const PalBaseResourceBridge&) = delete;
    auto operator=(const PalBaseResourceBridge&) -> PalBaseResourceBridge& = delete;
    PalBaseResourceBridge(PalBaseResourceBridge&&) noexcept;
    auto operator=(PalBaseResourceBridge&&) noexcept -> PalBaseResourceBridge&;

    auto set_enabled(bool enabled) -> void;
    auto on_world_begin(std::uint64_t generation) -> void;
    auto on_world_ready(std::uint64_t generation) -> void;
    auto tick(float deltaSeconds) -> void;
    auto ensure_hooks_registered() -> void;
    /**
     * @brief Restore all persistent registration edges and unregister all hooks from this module.
     * @warning Must be called on the game thread only; the destructor does not access Unreal instead.
     */
    auto shutdown_hooks() -> void;
    [[nodiscard]] auto snapshot() const -> BaseResourceSharingSnapshot;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace base_resource_sharing

