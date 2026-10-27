#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace base_resource_sharing {
struct GuidKey {
    std::array<std::uint32_t, 4> words{};

    [[nodiscard]] auto valid() const noexcept -> bool {
        return std::ranges::any_of(words, [](const auto word) { return word != 0; });
    }

    auto operator<=>(const GuidKey&) const = default;
};

enum class ContainerKind : std::uint8_t { normal, food, player, other };

struct ContainerDescriptor {
    GuidKey baseId;
    GuidKey groupId;
    GuidKey containerId;
    ContainerKind kind{ContainerKind::other};
    bool currentBase{};
};

struct ResourceUnionPlan {
    std::vector<ContainerDescriptor> ordered;
    std::size_t baseCount{};
    std::string error;
};

[[nodiscard]] inline auto make_resource_union_plan(
    const std::span<const ContainerDescriptor> containers, const GuidKey& currentGuild)
    -> ResourceUnionPlan {
    ResourceUnionPlan result;
    if (!currentGuild.valid()) {
        result.error = "Current player's guild identifier is invalid.";
        return result;
    }

    for (const auto& descriptor : containers) {
        if (descriptor.kind == ContainerKind::normal && descriptor.baseId.valid() &&
            descriptor.containerId.valid() && descriptor.groupId == currentGuild) {
            result.ordered.push_back(descriptor);
        }
    }

    std::ranges::sort(result.ordered, [](const auto& left, const auto& right) {
        if (left.currentBase != right.currentBase) {
            return left.currentBase;
        }
        if (left.baseId != right.baseId) {
            return left.baseId < right.baseId;
        }
        return left.containerId < right.containerId;
    });
    const auto duplicate = std::ranges::unique(
        result.ordered, {}, [](const auto& descriptor) { return descriptor.containerId; });
    result.ordered.erase(duplicate.begin(), duplicate.end());

    std::vector<GuidKey> bases;
    for (const auto& descriptor : result.ordered) {
        if (std::ranges::find(bases, descriptor.baseId) == bases.end()) {
            bases.push_back(descriptor.baseId);
        }
    }
    result.baseCount = bases.size();
    if (result.ordered.empty()) {
        result.error = "No loaded plain-storage base resource containers found for the current guild.";
    }
    return result;
}

enum class ResourceOperation : std::uint8_t { crafting, building, repair };

[[nodiscard]] constexpr auto operation_index(const ResourceOperation operation) noexcept
    -> std::size_t {
    return static_cast<std::size_t>(operation);
}

/** @brief The single Palworld consumer surface allowed for one material operation. */
enum class ResourceConsumerSurface : std::uint8_t {
    none,
    playerHelper,
    currentBaseModule,
    guildBaseModules,
};

/** @brief A foreground material operation and its sole consumer surface; never combines multiple surfaces. */
struct ResourceExposurePlan {
    ResourceOperation operation{ResourceOperation::repair};
    ResourceConsumerSurface surface{ResourceConsumerSurface::none};
    std::optional<GuidKey> targetBaseId;

    auto operator<=>(const ResourceExposurePlan&) const = default;
};

/**
 * @brief Whether two exposure plans resolve to the exact same injected container set.
 * @details The physical union only depends on the consumer surface and the target base: which
 *          containers are appended never depends on the foreground operation. Crafting and
 *          building share the player helper surface, so an active crafting union already carries
 *          every container a building union would inject. Treating them as interchangeable avoids
 *          tearing the helper down (which removed the cross-base chests before the native build
 *          consume ran) and re-appending the same tail on every build request.
 */
[[nodiscard]] constexpr auto same_exposure_content(const ResourceExposurePlan& left,
                                                   const ResourceExposurePlan& right) noexcept
    -> bool {
    return left.surface == right.surface && left.targetBaseId == right.targetBaseId;
}

/**
 * @brief Selects the unique consumer surface for crafting or building; refuses to expand when the current base cannot be confirmed.
 * @details Both operations consume from the player's inventory multi-helper: that is the list the native
 *          material collector and material resolver actually read (the crafting path, verified working),
 *          so building must expose the shared containers there too. The base camp storage module is kept
 *          in sync separately by the persistent union; injecting into it alone never reached the consume path.
 */
[[nodiscard]] inline auto make_exposure_plan(
    const ResourceOperation operation,
    const std::optional<GuidKey> currentBaseId = std::nullopt) noexcept -> ResourceExposurePlan {
    switch (operation) {
        case ResourceOperation::crafting:
            if (currentBaseId.has_value() && currentBaseId->valid()) {
                return {
                    .operation = operation,
                    .surface = ResourceConsumerSurface::playerHelper,
                    .targetBaseId = currentBaseId,
                };
            }
            return {.operation = operation};
        case ResourceOperation::building:
            if (currentBaseId.has_value() && currentBaseId->valid()) {
                return {
                    .operation = operation,
                    .surface = ResourceConsumerSurface::playerHelper,
                    .targetBaseId = currentBaseId,
                };
            }
            return {.operation = operation};
        case ResourceOperation::repair:
            return {
                .operation = operation,
            };
    }
    return {.operation = operation};
}

/**
 * @brief Selects containers from other bases to avoid double-counting containers the current base exposes through both native and shared paths.
 */
[[nodiscard]] inline auto select_shared_container_ids(
    const std::span<const ContainerDescriptor> containers, const GuidKey& currentBaseId)
    -> std::vector<GuidKey> {
    std::vector<GuidKey> result;
    result.reserve(containers.size());
    for (const auto& container : containers) {
        if (container.baseId != currentBaseId &&
            std::ranges::find(result, container.containerId) == result.end()) {
            result.push_back(container.containerId);
        }
    }
    return result;
}

enum class BuildingInventoryRefreshTarget : std::uint8_t {
    none,
    buildModel,
};

/** @return The sole native target allowed to receive an inventory-update notification after a building union is established. */
[[nodiscard]] constexpr auto select_building_inventory_refresh_target(
    const bool unionActive, const ResourceExposurePlan& exposure) noexcept
    -> BuildingInventoryRefreshTarget {
    if (unionActive && exposure.operation == ResourceOperation::building &&
        exposure.surface == ResourceConsumerSurface::currentBaseModule) {
        return BuildingInventoryRefreshTarget::buildModel;
    }
    return BuildingInventoryRefreshTarget::none;
}

enum class BuildingMenuBoundaryAction : std::uint8_t { acquire, reuse, replace };

/** @return Whether a new build-menu boundary should acquire, reuse, or replace the current union. */
[[nodiscard]] constexpr auto decide_building_menu_boundary(
    const bool unionActive, const std::uint64_t unionGeneration,
    const std::uint64_t currentGeneration, const ResourceExposurePlan& exposure,
    const std::optional<GuidKey> observedBase) noexcept -> BuildingMenuBoundaryAction {
    if (!unionActive) {
        return BuildingMenuBoundaryAction::acquire;
    }
    const bool sameBase = unionGeneration == currentGeneration && observedBase.has_value() &&
                          exposure.operation == ResourceOperation::building &&
                          exposure.surface == ResourceConsumerSurface::currentBaseModule &&
                          exposure.targetBaseId == observedBase;
    return sameBase ? BuildingMenuBoundaryAction::reuse : BuildingMenuBoundaryAction::replace;
}

/** @return Whether the current Setup post should send one native inventory-update to BuildModel. */
[[nodiscard]] constexpr auto should_refresh_building_inventory(
    const bool refreshNeeded, const bool unionActive, const std::uint64_t unionGeneration,
    const std::uint64_t currentGeneration, const ResourceExposurePlan& exposure,
    const bool hasLedgerEntry, const bool helperArray) noexcept -> bool {
    return refreshNeeded && unionGeneration == currentGeneration && hasLedgerEntry &&
           !helperArray &&
           select_building_inventory_refresh_target(unionActive, exposure) ==
               BuildingInventoryRefreshTarget::buildModel;
}

[[nodiscard]] constexpr auto resource_hooks_required(const bool enabled,
                                                     const bool worldAccessible) noexcept -> bool {
    return enabled && worldAccessible;
}

class SnapshotDirtyFlag {
public:
    auto mark() noexcept -> void {
        dirty_ = true;
    }

    [[nodiscard]] auto consume() noexcept -> bool {
        return std::exchange(dirty_, false);
    }

private:
    bool dirty_{true};
};

struct CapabilityState {
    bool previewReady{};
    bool consumeReady{};
    std::string error;

    [[nodiscard]] auto available() const noexcept -> bool {
        return previewReady && consumeReady;
    }
};

class RuntimeState {
public:
    auto set_preference(const bool enabled) noexcept -> void {
        enabled_ = enabled;
    }

    auto begin_world_transition(const std::uint64_t generation) -> void {
        generation_ = generation;
        accessible_ = false;
        capabilities_ = {};
    }

    auto finish_world_transition(const std::uint64_t generation) -> void {
        generation_ = generation;
        accessible_ = true;
        capabilities_ = {};
    }

    auto set_capability(const ResourceOperation operation, CapabilityState capability) -> void {
        capabilities_[operation_index(operation)] = std::move(capability);
    }

    [[nodiscard]] auto can_extend(const ResourceOperation operation,
                                  const std::uint64_t generation) const -> bool {
        return enabled_ && accessible_ && generation == generation_ &&
               capabilities_[operation_index(operation)].available();
    }

    [[nodiscard]] auto generation() const noexcept -> std::uint64_t {
        return generation_;
    }

    [[nodiscard]] auto enabled() const noexcept -> bool {
        return enabled_;
    }

    [[nodiscard]] auto accessible() const noexcept -> bool {
        return accessible_;
    }

    [[nodiscard]] auto capability(const ResourceOperation operation) const
        -> const CapabilityState& {
        return capabilities_[operation_index(operation)];
    }

private:
    bool enabled_{};
    bool accessible_{};
    std::uint64_t generation_{};
    std::array<CapabilityState, 3> capabilities_{};
};

struct InjectionRemovalPlan {
    std::vector<GuidKey> kept;
    bool complete{};
};

[[nodiscard]] inline auto remove_recorded_injections(const std::span<const GuidKey> current,
                                                     const std::span<const GuidKey> original,
                                                     const std::span<const GuidKey> injected)
    -> InjectionRemovalPlan {
    std::map<GuidKey, std::size_t> originalCounts;
    std::map<GuidKey, std::size_t> injectedCounts;
    for (const auto& id : original) {
        ++originalCounts[id];
    }
    for (const auto& id : injected) {
        ++injectedCounts[id];
    }

    std::map<GuidKey, std::size_t> encounteredCounts;
    std::map<GuidKey, std::size_t> removedCounts;
    InjectionRemovalPlan result;
    result.kept.reserve(current.size());
    for (const auto& id : current) {
        const auto encountered = ++encounteredCounts[id];
        if (encountered <= originalCounts[id]) {
            result.kept.push_back(id);
            continue;
        }
        if (removedCounts[id] < injectedCounts[id]) {
            ++removedCounts[id];
            continue;
        }
        result.kept.push_back(id);
    }

    result.complete = std::ranges::all_of(injectedCounts, [&](const auto& entry) {
        const auto removed = removedCounts.find(entry.first);
        return removed != removedCounts.end() && removed->second == entry.second;
    });
    return result;
}

[[nodiscard]] inline auto missing_union_tail(const std::span<const GuidKey> existing,
                                             const std::span<const GuidKey> globalPlan)
    -> std::vector<GuidKey> {
    std::vector<GuidKey> missing;
    for (const auto& id : globalPlan) {
        if (std::ranges::find(existing, id) == existing.end() &&
            std::ranges::find(missing, id) == missing.end()) {
            missing.push_back(id);
        }
    }
    return missing;
}

/** @brief Validation result of the sequence after injection. */
enum class SequenceValidationStatus : std::uint8_t {
    valid,
    originalPrefixChanged,
    injectedCountMismatch,
    duplicateInjectedId,
    unexpectedTail,
};

/** @brief Precisely identifies the failure category and the first offending container. */
struct SequenceValidationResult {
    SequenceValidationStatus status{SequenceValidationStatus::valid};
    std::optional<GuidKey> offendingId;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == SequenceValidationStatus::valid;
    }
};

/**
 * @brief Verifies the current sequence equals the original sequence plus exactly one unique injection in recorded order.
 * @details Pre-existing duplicates in the original are allowed to remain; injections must not duplicate the original or each other.
 */
[[nodiscard]] inline auto validate_applied_sequence(const std::span<const GuidKey> original,
                                                    const std::span<const GuidKey> injected,
                                                    const std::span<const GuidKey> current) noexcept
    -> SequenceValidationResult {
    for (std::size_t index = 0; index < injected.size(); ++index) {
        const auto id = injected[index];
        if (std::ranges::find(original, id) != original.end() ||
            std::ranges::find(injected.first(index), id) != injected.first(index).end()) {
            return {
                .status = SequenceValidationStatus::duplicateInjectedId,
                .offendingId = id,
            };
        }
    }

    if (current.size() < original.size()) {
        return {.status = SequenceValidationStatus::originalPrefixChanged};
    }
    for (std::size_t index = 0; index < original.size(); ++index) {
        if (current[index] != original[index]) {
            return {
                .status = SequenceValidationStatus::originalPrefixChanged,
                .offendingId = current[index],
            };
        }
    }

    const auto expectedSize = original.size() + injected.size();
    if (current.size() < expectedSize) {
        return {.status = SequenceValidationStatus::injectedCountMismatch};
    }
    for (std::size_t index = 0; index < injected.size(); ++index) {
        if (current[original.size() + index] != injected[index]) {
            return {
                .status = SequenceValidationStatus::injectedCountMismatch,
                .offendingId = current[original.size() + index],
            };
        }
    }
    if (current.size() > expectedSize) {
        return {
            .status = SequenceValidationStatus::unexpectedTail,
            .offendingId = current[expectedSize],
        };
    }
    return {};
}

struct BaseResourceSharingStatus {
    bool enabled{};
    bool worldAccessible{true};
    bool detectingCapabilities{};
    std::size_t baseCount{};
    std::size_t containerCount{};
    std::size_t pendingContainerCount{};
    bool craftingAvailable{};
    bool buildingAvailable{};
    bool repairAvailable{};
    std::string craftingError;
    std::string buildingError;
    std::string repairError;
    std::string runtimeError;
};

[[nodiscard]] inline auto format_status(const BaseResourceSharingStatus& status) -> std::string {
    if (!status.enabled) {
        return "Base resource sharing is disabled.";
    }
    if (!status.worldAccessible) {
        return "Waiting for an accessible game world.";
    }
    if (status.detectingCapabilities) {
        std::string text = "Detecting Palworld resource interfaces.";
        if (!status.craftingError.empty()) {
            text += "\nCrafting: " + status.craftingError;
        }
        if (!status.buildingError.empty()) {
            text += "\nBuilding: " + status.buildingError;
        }
        return text;
    }

    std::string text = "Discovered " + std::to_string(status.baseCount) + " base(s), " +
                       std::to_string(status.containerCount) + " resource container(s).\n";
    if (status.pendingContainerCount > 0) {
        text += std::to_string(status.pendingContainerCount) +
                 " container(s) pending load - excluded, awaiting structure event to retry.\n";
    }
    text += status.craftingAvailable ? "Crafting: available" : "Crafting: unavailable";
    text += status.buildingAvailable ? "; Building: available" : "; Building: unavailable";
    text += status.repairAvailable ? "; Repair: available" : "; Repair: unavailable";
    if (!status.craftingAvailable && !status.craftingError.empty()) {
        text += "\nCrafting: " + status.craftingError;
    }
    if (!status.buildingAvailable && !status.buildingError.empty()) {
        text += "\nBuilding: " + status.buildingError;
    }
    if (!status.repairAvailable && !status.repairError.empty()) {
        text += "\n" + status.repairError;
    }
    if (!status.runtimeError.empty()) {
        text += "\nError: " + status.runtimeError;
    }
    return text;
}
}  // namespace base_resource_sharing

