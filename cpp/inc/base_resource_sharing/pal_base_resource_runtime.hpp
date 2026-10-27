#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <base_resource_sharing/persistent_union.hpp>
#include <base_resource_sharing/resource_pool.hpp>

namespace RC::Unreal {
class UObject;
}

namespace base_resource_sharing::detail {
struct CatalogContainer {
    GuidKey containerId;
    GuidKey ownerMapObjectId;
};

struct CatalogModule {
    GuidKey baseId;
    std::wstring objectFullName;
    RC::Unreal::UObject* moduleObject{};
    std::vector<CatalogContainer> containers;
};

struct ResourceCatalogSnapshot {
    std::uint64_t generation{};
    GuidKey guildId;
    bool initialized{};
    std::size_t sameGuildBaseCount{};
    std::size_t registeredContainerCount{};
    std::size_t pendingContainerCount{};
    ResourceUnionPlan plan;
    std::vector<CatalogModule> modules;
    /** @brief Storage modules discovered but not belonging to the local guild; structure hooks must ignore them. */
    std::vector<std::wstring> ignoredModuleNames;
    /** @brief All native and injected registrations actually present in the target modules at catalog read time. */
    std::vector<ConcreteModelRegistrationKey> registrations;
    /** @brief Registrations existing in a guild module but whose type is not plain storage; structure hooks must ignore them. */
    std::vector<ConcreteModelRegistrationKey> ignoredRegistrations;
    /** @brief Subset of ledger edges confirmed to still exist during the current catalog read. */
    std::vector<PersistentUnionEdge> observedAppliedEdges;
    std::string error;
};

struct UnionLedgerEntry {
    std::wstring objectFullName;
    bool helperArray{};
    std::vector<GuidKey> original;
    std::vector<GuidKey> injected;
};

struct LiveUnion {
    std::uint64_t generation{};
    GuidKey guildId;
    ResourceExposurePlan exposure;
    std::optional<UnionLedgerEntry> entry;
    bool active{};
};

/** @brief Verifiable result of a single native persistent-edge registration or unregistration call. */
struct PersistentEdgeMutationResult {
    /** @brief The edge mutation the native call was verified to have produced. */
    PersistentEdgeMutation mutation{PersistentEdgeMutation::invalid};
    /** @brief Error string when the operation failed or could not be fully verified. */
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return mutation != PersistentEdgeMutation::invalid && error.empty();
    }
};

[[nodiscard]] auto local_authority_ready(RC::Unreal::UObject* worldContext, std::string& error)
    -> bool;
[[nodiscard]] auto discover_catalog(RC::Unreal::UObject* worldContext, std::uint64_t generation,
                                    std::span<const PersistentUnionEdge> appliedEdges = {})
    -> ResourceCatalogSnapshot;
[[nodiscard]] auto persistent_modules(const ResourceCatalogSnapshot& catalog)
    -> std::vector<PersistentStorageModule>;
[[nodiscard]] auto apply_persistent_edge(RC::Unreal::UObject* worldContext,
                                         const ResourceCatalogSnapshot& catalog,
                                         const PersistentUnionEdge& edge)
    -> PersistentEdgeMutationResult;
[[nodiscard]] auto remove_persistent_edge(RC::Unreal::UObject* worldContext,
                                          const ResourceCatalogSnapshot& catalog,
                                          const PersistentUnionEdge& edge)
    -> PersistentEdgeMutationResult;
[[nodiscard]] auto read_base_id(RC::Unreal::UObject* baseModel) -> std::optional<GuidKey>;
[[nodiscard]] auto resolve_inside_base_id(RC::Unreal::UObject* worldContext,
                                          const ResourceCatalogSnapshot& catalog,
                                          std::string& error) -> std::optional<GuidKey>;
[[nodiscard]] auto apply_union(RC::Unreal::UObject* worldContext,
                               const ResourceCatalogSnapshot& catalog,
                               const ResourceExposurePlan& exposure, LiveUnion& liveUnion,
                               std::string& error) -> bool;
[[nodiscard]] auto validate_union(const LiveUnion& liveUnion, std::string& error) -> bool;
/** @return The containers appended to the player helper by the active union (empty when inactive). */
[[nodiscard]] auto live_union_containers() -> std::span<RC::Unreal::UObject*>;
[[nodiscard]] auto restore_union(LiveUnion& liveUnion, std::string& error) -> bool;
}  // namespace base_resource_sharing::detail

