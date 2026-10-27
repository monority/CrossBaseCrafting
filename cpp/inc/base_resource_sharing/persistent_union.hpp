#pragma once

#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
/** @brief A single storage chest that can be registered into a base storage module. */
struct PersistentStorageContainer {
    /** @brief Palworld item-container GUID. */
    GuidKey containerId;
    /** @brief Map-object GUID used to re-resolve the ConcreteModel across frames. */
    GuidKey ownerMapObjectId;

    auto operator<=>(const PersistentStorageContainer&) const = default;
};

/** @brief Cross-frame-stable storage-module ConcreteModel registration key; holds no Unreal object. */
struct ConcreteModelRegistrationKey {
    std::wstring moduleFullName;
    GuidKey ownerMapObjectId;

    auto operator<=>(const ConcreteModelRegistrationKey&) const = default;
};

/** @brief Whether the ConcreteModel matching a hook argument is currently registered in that storage module. */
enum class ConcreteModelRegistrationMembership : std::uint8_t {
    unknown,
    untrackedModule,
    ignoredModule,
    absent,
    present,
};

/**
 * @brief Pure-value registration index for zero-allocation binary search by high-frequency structure hooks.
 * @details Rebuilt entirely on catalog discovery; incrementally updated after each verified persistent edge.
 */
class ConcreteModelRegistrationIndex {
public:
    auto reset(const std::span<const std::wstring> moduleNames,
               const std::span<const std::wstring> ignoredModuleNames,
               const std::span<const ConcreteModelRegistrationKey> registrations,
               const std::span<const ConcreteModelRegistrationKey> ignoredRegistrations = {})
        -> void {
        moduleNames_.assign(moduleNames.begin(), moduleNames.end());
        std::ranges::sort(moduleNames_);
        const auto duplicateModule = std::ranges::unique(moduleNames_);
        moduleNames_.erase(duplicateModule.begin(), duplicateModule.end());

        ignoredModuleNames_.assign(ignoredModuleNames.begin(), ignoredModuleNames.end());
        std::ranges::sort(ignoredModuleNames_);
        const auto duplicateIgnoredModule = std::ranges::unique(ignoredModuleNames_);
        ignoredModuleNames_.erase(duplicateIgnoredModule.begin(), duplicateIgnoredModule.end());

        registrations_.assign(registrations.begin(), registrations.end());
        std::erase_if(registrations_, [&](const auto& registration) {
            return registration.moduleFullName.empty() || !registration.ownerMapObjectId.valid() ||
                   !std::ranges::binary_search(moduleNames_, registration.moduleFullName);
        });
        std::ranges::sort(registrations_);
        const auto duplicateRegistration = std::ranges::unique(registrations_);
        registrations_.erase(duplicateRegistration.begin(), duplicateRegistration.end());

        ignoredRegistrations_.assign(ignoredRegistrations.begin(), ignoredRegistrations.end());
        std::erase_if(ignoredRegistrations_, [&](const auto& registration) {
            return registration.moduleFullName.empty() || !registration.ownerMapObjectId.valid() ||
                   !std::ranges::binary_search(moduleNames_, registration.moduleFullName);
        });
        std::ranges::sort(ignoredRegistrations_);
        const auto duplicateIgnoredRegistration = std::ranges::unique(ignoredRegistrations_);
        ignoredRegistrations_.erase(duplicateIgnoredRegistration.begin(),
                                    duplicateIgnoredRegistration.end());
    }

    [[nodiscard]] auto membership(const ConcreteModelRegistrationKey& key) const noexcept
        -> ConcreteModelRegistrationMembership {
        if (key.moduleFullName.empty() || !key.ownerMapObjectId.valid() ||
            (std::ranges::binary_search(moduleNames_, key.moduleFullName) &&
             std::ranges::binary_search(ignoredModuleNames_, key.moduleFullName))) {
            return ConcreteModelRegistrationMembership::unknown;
        }
        if (std::ranges::binary_search(ignoredModuleNames_, key.moduleFullName)) {
            return ConcreteModelRegistrationMembership::ignoredModule;
        }
        if (!std::ranges::binary_search(moduleNames_, key.moduleFullName)) {
            return ConcreteModelRegistrationMembership::untrackedModule;
        }
        const bool registered = std::ranges::binary_search(registrations_, key);
        const bool ignored = std::ranges::binary_search(ignoredRegistrations_, key);
        if (registered && ignored) {
            return ConcreteModelRegistrationMembership::unknown;
        }
        if (ignored) {
            return ConcreteModelRegistrationMembership::ignoredModule;
        }
        return registered ? ConcreteModelRegistrationMembership::present
                          : ConcreteModelRegistrationMembership::absent;
    }

    [[nodiscard]] auto record(const ConcreteModelRegistrationKey& key) -> bool {
        const auto current = membership(key);
        if (current != ConcreteModelRegistrationMembership::absent &&
            current != ConcreteModelRegistrationMembership::present) {
            return false;
        }
        const auto position = std::ranges::lower_bound(registrations_, key);
        if (position != registrations_.end() && *position == key) {
            return false;
        }
        registrations_.insert(position, key);
        return true;
    }

    [[nodiscard]] auto erase(const ConcreteModelRegistrationKey& key) -> bool {
        const auto position = std::ranges::lower_bound(registrations_, key);
        if (position == registrations_.end() || *position != key) {
            return false;
        }
        registrations_.erase(position);
        return true;
    }

    auto clear() noexcept -> void {
        moduleNames_.clear();
        ignoredModuleNames_.clear();
        registrations_.clear();
        ignoredRegistrations_.clear();
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return registrations_.size();
    }

private:
    std::vector<std::wstring> moduleNames_;
    std::vector<std::wstring> ignoredModuleNames_;
    std::vector<ConcreteModelRegistrationKey> registrations_;
    std::vector<ConcreteModelRegistrationKey> ignoredRegistrations_;
};

/** @brief A base camp storage module together with its native storage chests. */
struct PersistentStorageModule {
    /** @brief GUID of the base camp that owns this storage module. */
    GuidKey baseId;
    /** @brief Full object name needed to re-resolve the storage module across frames. */
    std::wstring objectFullName;
    /** @brief Native chests currently registered and loaded in this module. */
    std::vector<PersistentStorageContainer> containers;
};

/** @brief A single cross-base native storage registration edge established by this mod. */
struct PersistentUnionEdge {
    /** @brief Target base camp that receives the remote container registration. */
    GuidKey targetBaseId;
    /** @brief Full object name of the target storage module. */
    std::wstring targetModuleFullName;
    /** @brief Source base camp that owns the chest being registered. */
    GuidKey sourceBaseId;
    /** @brief Item-container GUID being registered. */
    GuidKey containerId;
    /** @brief Map-object GUID of the chest being registered. */
    GuidKey ownerMapObjectId;

    auto operator<=>(const PersistentUnionEdge&) const = default;
};

/** @brief The complete set of cross-base registration edges required by the current catalog. */
struct PersistentUnionPlan {
    /** @brief Desired cross-base registration edges after sorting and deduplication. */
    std::vector<PersistentUnionEdge> edges;
    /** @brief Error string when a safe plan cannot be generated. */
    std::string error;
};

/** @brief Minimal diff between the recorded edge set and the desired edge set. */
struct PersistentUnionDiff {
    /** @brief Edges missing from the current ledger that need to be added. */
    std::vector<PersistentUnionEdge> added;
    /** @brief Edges present in the current ledger that need to be removed. */
    std::vector<PersistentUnionEdge> removed;
};

/**
 * @brief Remove from the catalog snapshot all target-module edges already recorded by this mod,
 */
[[nodiscard]] inline auto remove_applied_target_edges(
    std::vector<PersistentStorageModule> modules,
    const std::span<const PersistentUnionEdge> applied) -> std::vector<PersistentStorageModule> {
    for (auto& module : modules) {
        std::erase_if(module.containers, [&](const auto& container) {
            return std::ranges::any_of(applied, [&](const auto& edge) {
                return edge.targetBaseId == module.baseId &&
                       edge.targetModuleFullName == module.objectFullName &&
                       edge.containerId == container.containerId;
            });
        });
    }
    return modules;
}

/** @brief Precise result produced by a single native registration or unregistration call relative to the pre-call sequence. */
enum class PersistentEdgeMutation : std::uint8_t {
    unchanged,
    added,
    removed,
    invalid,
};

/** @return Safe accounting result matching target-container occurrence counts before/after native registration. */
[[nodiscard]] constexpr auto classify_persistent_edge_add(const std::size_t beforeCount,
                                                          const std::size_t afterCount) noexcept
    -> PersistentEdgeMutation {
    if (beforeCount == 1 && afterCount == 1) {
        return PersistentEdgeMutation::unchanged;
    }
    if (beforeCount == 0 && afterCount == 1) {
        return PersistentEdgeMutation::added;
    }
    return PersistentEdgeMutation::invalid;
}

/** @return Safe recovery result matching target-container occurrence counts before/after native unregistration. */
[[nodiscard]] constexpr auto classify_persistent_edge_remove(const std::size_t beforeCount,
                                                             const std::size_t afterCount) noexcept
    -> PersistentEdgeMutation {
    if (beforeCount == 0 && afterCount == 0) {
        return PersistentEdgeMutation::unchanged;
    }
    if (beforeCount == 1 && afterCount == 0) {
        return PersistentEdgeMutation::removed;
    }
    return PersistentEdgeMutation::invalid;
}

namespace detail {
[[nodiscard]] inline auto normalized_edges(const std::span<const PersistentUnionEdge> edges)
    -> std::vector<PersistentUnionEdge> {
    std::vector<PersistentUnionEdge> result{edges.begin(), edges.end()};
    std::ranges::sort(result);
    const auto duplicate = std::ranges::unique(result);
    result.erase(duplicate.begin(), duplicate.end());
    return result;
}
}  // namespace detail

/**
 * @brief Build a stable registration plan pointing every valid base module to all chests in every other base module.
 * @details Same-base containers, invalid IDs, and duplicate containers do not form edges; single-base and empty catalogs produce a valid empty plan.
 */
[[nodiscard]] inline auto make_persistent_union_plan(
    const std::span<const PersistentStorageModule> modules) -> PersistentUnionPlan {
    PersistentUnionPlan result;
    std::vector<PersistentStorageModule> validModules;
    validModules.reserve(modules.size());
    for (const auto& module : modules) {
        if (!module.baseId.valid() || module.objectFullName.empty()) {
            continue;
        }
        auto copy = module;
        std::erase_if(copy.containers, [](const auto& container) {
            return !container.containerId.valid() || !container.ownerMapObjectId.valid();
        });
        std::ranges::sort(copy.containers);
        const auto duplicate =
            std::ranges::unique(copy.containers, {}, &PersistentStorageContainer::containerId);
        copy.containers.erase(duplicate.begin(), duplicate.end());
        validModules.push_back(std::move(copy));
    }
    std::ranges::sort(validModules, [](const auto& left, const auto& right) {
        if (left.baseId != right.baseId) {
            return left.baseId < right.baseId;
        }
        return left.objectFullName < right.objectFullName;
    });
    if (validModules.empty()) {
        result.error = "No guild base storage modules found for establishing a persistent union.";
        return result;
    }

    for (const auto& target : validModules) {
        for (const auto& source : validModules) {
            if (target.baseId == source.baseId) {
                continue;
            }
            for (const auto& container : source.containers) {
                result.edges.push_back({
                    .targetBaseId = target.baseId,
                    .targetModuleFullName = target.objectFullName,
                    .sourceBaseId = source.baseId,
                    .containerId = container.containerId,
                    .ownerMapObjectId = container.ownerMapObjectId,
                });
            }
        }
    }
    result.edges = detail::normalized_edges(result.edges);
    return result;
}

/** @return New and removed edges needed to transition from the applied edge set to the desired edge set. */
[[nodiscard]] inline auto diff_persistent_union(const std::span<const PersistentUnionEdge> desired,
                                                const std::span<const PersistentUnionEdge> applied)
    -> PersistentUnionDiff {
    const auto normalizedDesired = detail::normalized_edges(desired);
    const auto normalizedApplied = detail::normalized_edges(applied);
    PersistentUnionDiff result;
    for (const auto& edge : normalizedDesired) {
        if (!std::ranges::binary_search(normalizedApplied, edge)) {
            result.added.push_back(edge);
        }
    }
    for (const auto& edge : normalizedApplied) {
        if (!std::ranges::binary_search(normalizedDesired, edge)) {
            result.removed.push_back(edge);
        }
    }
    return result;
}

/**
 * @brief Find ledger edges that are claimed to exist but are no longer present in the safely-read target module.
 * @details Edges whose target module is not loaded or not yet discovered are kept in the ledger to preserve future restore responsibility.
 */
[[nodiscard]] inline auto missing_observed_persistent_edges(
    const std::span<const PersistentUnionEdge> applied,
    const std::span<const PersistentUnionEdge> observed,
    const std::span<const PersistentStorageModule> discoveredModules)
    -> std::vector<PersistentUnionEdge> {
    const auto normalizedApplied = detail::normalized_edges(applied);
    const auto normalizedObserved = detail::normalized_edges(observed);
    std::vector<PersistentUnionEdge> result;
    for (const auto& edge : normalizedApplied) {
        const bool targetDiscovered =
            std::ranges::any_of(discoveredModules, [&](const auto& module) {
                return module.baseId == edge.targetBaseId &&
                       module.objectFullName == edge.targetModuleFullName;
            });
        if (targetDiscovered && !std::ranges::binary_search(normalizedObserved, edge)) {
            result.push_back(edge);
        }
    }
    return result;
}

/** @brief Lifecycle phase of the persistent union within a single world generation. */
enum class PersistentUnionPhase : std::uint8_t {
    off,
    initializing,
    ready,
    reconciling,
    restoring,
    failed,
};

/** @brief Action the persistent-union reconciler should take after receiving a storage structure notification. */
enum class PersistentStructureChangeAction : std::uint8_t {
    ignore,
    startReconcile,
    queueFollowUp,
};

/**
 * @brief Collapse high-frequency or reentrant storage structure notifications into at most one subsequent reconcile.
 * @details Start a new reconcile from the ready state; notifications during initializing or reconciling must not
 *          clear the current diff -- they queue one follow-up reconcile. Closed, restoring, and failed states do not accept new reconcile work.
 */
[[nodiscard]] constexpr auto structure_change_action(const PersistentUnionPhase phase) noexcept
    -> PersistentStructureChangeAction {
    switch (phase) {
        case PersistentUnionPhase::ready:
            return PersistentStructureChangeAction::startReconcile;
        case PersistentUnionPhase::initializing:
        case PersistentUnionPhase::reconciling:
            return PersistentStructureChangeAction::queueFollowUp;
        default:
            return PersistentStructureChangeAction::ignore;
    }
}

/** @brief Persistent-union lifecycle state machine that holds no Unreal objects. */
class PersistentUnionLifecycle {
public:
    auto begin_world(const std::uint64_t generation) noexcept -> void {
        generation_ = generation;
        phase_ = PersistentUnionPhase::off;
    }

    [[nodiscard]] auto request_enable(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || phase_ != PersistentUnionPhase::off) {
            return false;
        }
        phase_ = PersistentUnionPhase::initializing;
        return true;
    }

    [[nodiscard]] auto complete_apply(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || (phase_ != PersistentUnionPhase::initializing &&
                                          phase_ != PersistentUnionPhase::reconciling)) {
            return false;
        }
        phase_ = PersistentUnionPhase::ready;
        return true;
    }

    [[nodiscard]] auto invalidate(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || phase_ != PersistentUnionPhase::ready) {
            return false;
        }
        phase_ = PersistentUnionPhase::reconciling;
        return true;
    }

    [[nodiscard]] auto request_disable(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || phase_ == PersistentUnionPhase::off ||
            phase_ == PersistentUnionPhase::restoring) {
            return false;
        }
        phase_ = PersistentUnionPhase::restoring;
        return true;
    }

    [[nodiscard]] auto complete_restore(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || phase_ != PersistentUnionPhase::restoring) {
            return false;
        }
        phase_ = PersistentUnionPhase::off;
        return true;
    }

    auto fail(const std::uint64_t generation) noexcept -> void {
        if (generation == generation_) {
            phase_ = PersistentUnionPhase::failed;
        }
    }

    [[nodiscard]] auto phase(const std::uint64_t generation) const noexcept
        -> PersistentUnionPhase {
        return generation == generation_ ? phase_ : PersistentUnionPhase::off;
    }

private:
    std::uint64_t generation_{};
    PersistentUnionPhase phase_{PersistentUnionPhase::off};
};

/** @brief Records only edges that have been verified as newly added by this mod. */
class PersistentUnionLedger {
public:
    [[nodiscard]] auto record(const PersistentUnionEdge& edge) -> bool {
        const auto position = std::ranges::lower_bound(edges_, edge);
        if (position != edges_.end() && *position == edge) {
            return false;
        }
        edges_.insert(position, edge);
        return true;
    }

    [[nodiscard]] auto erase(const PersistentUnionEdge& edge) -> bool {
        const auto position = std::ranges::lower_bound(edges_, edge);
        if (position == edges_.end() || *position != edge) {
            return false;
        }
        edges_.erase(position);
        return true;
    }

    [[nodiscard]] auto contains(const PersistentUnionEdge& edge) const noexcept -> bool {
        return std::ranges::binary_search(edges_, edge);
    }

    [[nodiscard]] auto edges() const noexcept -> std::span<const PersistentUnionEdge> {
        return edges_;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return edges_.empty();
    }

    auto clear() noexcept -> void {
        edges_.clear();
    }

private:
    std::vector<PersistentUnionEdge> edges_;
};

/** @brief Per-EngineTick work budget for the persistent union. */
class PersistentUnionWorkBudget {
public:
    // Keep storage notifications below a visible frame hitch. The work is resumed on the next
    // engine tick, so lowering this budget trades a slightly longer initial sync for smoother UI.
    static constexpr std::size_t MaxOperationsPerFrame{2};
    static constexpr std::chrono::microseconds MaxElapsedPerFrame{250};

    [[nodiscard]] auto can_process(const std::chrono::microseconds elapsed) const noexcept -> bool {
        return processed_ < MaxOperationsPerFrame && elapsed < MaxElapsedPerFrame;
    }

    auto record_operation() noexcept -> void {
        ++processed_;
    }

    auto reset() noexcept -> void {
        processed_ = 0;
    }

private:
    std::size_t processed_{};
};
}  // namespace base_resource_sharing

