/**
 * @file persistent.cpp
 * @brief Impl method definitions for the persistent concern (Phase C split).
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/Property/FObjectProperty.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <base_resource_sharing/hook_manifest.hpp>
#include <base_resource_sharing/impl.hpp>
#include <base_resource_sharing/pal_base_resource_runtime.hpp>
#include <base_resource_sharing/pal_base_resources.hpp>
#include <base_resource_sharing/impl.hpp>
#include <base_resource_sharing/persistent_union.hpp>
#include <common/function_hook_registry.hpp>
#include <common/diagnostics.hpp>
#include <common/game_reflection.hpp>
#include <common/reflection_cache.hpp>
#include <common/timing.hpp>


namespace base_resource_sharing {
using namespace RC;
using namespace RC::Unreal;


auto PalBaseResourceBridge::Impl::reset_pending_work() noexcept -> void {
        work_ = {};
        removalIndex_ = 0;
        additionIndex_ = 0;
        workPrepared_ = false;
    }


auto PalBaseResourceBridge::Impl::prepare_restore_work() -> void {
        work_ = {};
        work_.removed.assign(unionLedger_.edges().begin(), unionLedger_.edges().end());
        removalIndex_ = 0;
        additionIndex_ = 0;
        workPrepared_ = true;
        snapshotDirty_.mark();
    }


auto PalBaseResourceBridge::Impl::finalize_disabled_state() -> void {
        desiredPlan_ = {};
        reset_pending_work();
        catalog_ = {};
        registrationIndex_.clear();
        baseCount_ = 0;
        containerCount_ = 0;
        pendingContainerCount_ = 0;
        worldContextFullName_.clear();
        runtimeError_.clear();
        reconcileRequested_ = false;
        waitingForStructure_ = false;
        publish_capabilities();
        snapshotDirty_.mark();
    }


auto PalBaseResourceBridge::Impl::fail_persistent_union(std::string error) -> void {
        const bool wasDisabled = safetyDisabled_;
        runtimeError_ = std::move(error);
        safetyDisabled_ = true;
        reconcileRequested_ = false;
        waitingForStructure_ = false;
        unionLifecycle_.fail(runtime_.generation());
        disable_operation(ResourceOperation::crafting, runtimeError_);
        disable_operation(ResourceOperation::building, runtimeError_);
        reset_pending_work();
        snapshotDirty_.mark();
        // The reason used to live only in the snapshot, so a world where sharing silently did
        // nothing produced an empty log. Report the transition into the disabled state once per
        // world so users can tell a client/dedicated-server run from an actual failure.
        if (!wasDisabled && !runtimeError_.empty()) {
            Output::send<LogLevel::Warning>(
                STR("[CrossBaseCrafting] cross-base sharing disabled: {}\n"),
                std::wstring{runtimeError_.begin(), runtimeError_.end()});
        }
    }


[[nodiscard]] auto PalBaseResourceBridge::Impl::prepare_reconcile_work() -> bool {
        auto* worldContext = resolve_world_context(nullptr);
        if (worldContext == nullptr) {
            runtimeError_ = "Waiting for a game context suitable for base catalog discovery.";
            waitingForStructure_ = true;
            snapshotDirty_.mark();
            return false;
        }

        const auto generation = runtime_.generation();
        const auto started = std::chrono::steady_clock::now();
        auto next = detail::discover_catalog(worldContext, generation, unionLedger_.edges());
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        const bool structuralFailure = !next.error.empty() || next.generation != generation;
        record_catalog_attempt(elapsed, !structuralFailure);
        if (structuralFailure) {
            fail_persistent_union(next.error.empty() ? "Persistent union catalog world generation has expired."
                                                     : std::move(next.error));
            return false;
        }

        accept_catalog(std::move(next));
        auto modules = detail::persistent_modules(catalog_);
        const auto missingEdges = missing_observed_persistent_edges(
            unionLedger_.edges(), catalog_.observedAppliedEdges, modules);
        for (const auto& edge : missingEdges) {
            static_cast<void>(unionLedger_.erase(edge));
        }
        modules = remove_applied_target_edges(std::move(modules), unionLedger_.edges());
        containerCount_ = 0;
        for (const auto& module : modules) {
            containerCount_ += module.containers.size();
        }
        auto plan = make_persistent_union_plan(modules);
        if (!plan.error.empty()) {
            runtimeError_ = "Waiting for guild base storage modules to finish loading.";
            waitingForStructure_ = true;
            snapshotDirty_.mark();
            return false;
        }

        auto difference = diff_persistent_union(plan.edges, unionLedger_.edges());
        desiredPlan_ = std::move(plan);
        work_ = std::move(difference);
        removalIndex_ = 0;
        additionIndex_ = 0;
        workPrepared_ = true;
        waitingForStructure_ = false;
        runtimeError_.clear();
        snapshotDirty_.mark();
        if (!work_.added.empty() || !work_.removed.empty()) {
            Output::send<LogLevel::Verbose>(
                STR("PalworldEditor: persistent storage graph prepared in {:.3f} ms, bases={}, "
                    "containers={}, add={}, remove={}, pending={}\n"),
                elapsed, baseCount_, containerCount_, work_.added.size(), work_.removed.size(),
                pendingContainerCount_);
        }
        return true;
    }


auto PalBaseResourceBridge::Impl::complete_persistent_work() -> void {
        const auto generation = runtime_.generation();
        const auto phase = unionLifecycle_.phase(generation);
        if (phase == PersistentUnionPhase::restoring) {
            if (!unionLedger_.empty()) {
                fail_persistent_union("Persistent union ledger is non-empty after restore completed.");
                return;
            }
            static_cast<void>(unionLifecycle_.complete_restore(generation));
            finalize_disabled_state();
        } else {
            static_cast<void>(unionLifecycle_.complete_apply(generation));
            if (reconcileRequested_ &&
                std::chrono::steady_clock::now() >= nextReconcileAttempt_) {
                reconcileRequested_ = false;
                if (unionLifecycle_.invalidate(generation)) {
                    nextReconcileAttempt_ =
                        std::chrono::steady_clock::now() + kReconcileDebounce;
                }
            }
            publish_capabilities();
            snapshotDirty_.mark();
        }
        reset_pending_work();
    }


auto PalBaseResourceBridge::Impl::process_persistent_work() -> void {
        const auto generation = runtime_.generation();
        const auto phase = unionLifecycle_.phase(generation);
        if (!workPrepared_) {
            if (phase == PersistentUnionPhase::restoring) {
                prepare_restore_work();
            } else if (!prepare_reconcile_work()) {
                return;
            }
        }

        auto* worldContext = resolve_world_context(nullptr);
        if (worldContext == nullptr && phase != PersistentUnionPhase::restoring) {
            fail_persistent_union("Game context has become invalid while executing persistent union.");
            return;
        }

        PersistentUnionWorkBudget budget;
        const auto started = std::chrono::steady_clock::now();
        const auto within_budget = [&] {
            return budget.can_process(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started));
        };

        while (removalIndex_ < work_.removed.size() && within_budget()) {
            const auto edge = work_.removed[removalIndex_++];
            detail::PersistentEdgeMutationResult result;
            {
                MutationScope mutation{selfMutation_};
                result = detail::remove_persistent_edge(worldContext, catalog_, edge);
            }
            budget.record_operation();
            if (!result) {
                if (result.mutation == PersistentEdgeMutation::removed) {
                    static_cast<void>(unionLedger_.erase(edge));
                    static_cast<void>(registrationIndex_.erase(registration_key(edge)));
                }
                fail_persistent_union(std::move(result.error));
                return;
            }
            static_cast<void>(unionLedger_.erase(edge));
            static_cast<void>(registrationIndex_.erase(registration_key(edge)));
        }

        while (additionIndex_ < work_.added.size() && within_budget()) {
            const auto& edge = work_.added[additionIndex_++];
            detail::PersistentEdgeMutationResult result;
            {
                MutationScope mutation{selfMutation_};
                result = detail::apply_persistent_edge(worldContext, catalog_, edge);
            }
            budget.record_operation();
            if (!result) {
                if (result.mutation == PersistentEdgeMutation::added) {
                    static_cast<void>(unionLedger_.record(edge));
                    static_cast<void>(registrationIndex_.record(registration_key(edge)));
                }
                fail_persistent_union(std::move(result.error));
                return;
            }
            if (result.mutation == PersistentEdgeMutation::added) {
                static_cast<void>(unionLedger_.record(edge));
            }
            static_cast<void>(registrationIndex_.record(registration_key(edge)));
        }

        snapshotDirty_.mark();
        if (removalIndex_ == work_.removed.size() && additionIndex_ == work_.added.size()) {
            complete_persistent_work();
        }
    }


auto PalBaseResourceBridge::Impl::restore_all_synchronously(const std::string_view reason) -> void {
        if (unionLedger_.empty()) {
            return;
        }
        auto* worldContext = resolve_world_context(nullptr);
        const auto edges = std::vector<PersistentUnionEdge>{unionLedger_.edges().begin(),
                                                            unionLedger_.edges().end()};
        const auto started = std::chrono::steady_clock::now();
        std::string firstError;
        for (const auto& edge : edges) {
            detail::PersistentEdgeMutationResult result;
            {
                MutationScope mutation{selfMutation_};
                result = detail::remove_persistent_edge(worldContext, catalog_, edge);
            }
            if (result) {
                static_cast<void>(unionLedger_.erase(edge));
            } else {
                if (result.mutation == PersistentEdgeMutation::removed) {
                    static_cast<void>(unionLedger_.erase(edge));
                }
                if (firstError.empty()) {
                    firstError = std::move(result.error);
                }
            }
        }
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        const std::wstring wideReason{reason.begin(), reason.end()};
        const bool restored = unionLedger_.empty() && firstError.empty();
        Output::send<LogLevel::Verbose>(
            restored
                ? STR("PalworldEditor: persistent storage graph restored in {:.3f} ms ({})\n")
                : STR("PalworldEditor: persistent storage graph restore incomplete in {:.3f} ms "
                      "({})\n"),
            elapsed, wideReason);
        if (!restored) {
            fail_persistent_union(firstError.empty() ? "Persistent union could not be fully restored." : firstError);
        }
    }


auto PalBaseResourceBridge::Impl::accept_catalog(detail::ResourceCatalogSnapshot next) -> void {
        catalog_ = std::move(next);
        std::vector<std::wstring> moduleNames;
        moduleNames.reserve(catalog_.modules.size());
        for (const auto& module : catalog_.modules) {
            moduleNames.push_back(module.objectFullName);
        }
        registrationIndex_.reset(moduleNames, catalog_.ignoredModuleNames, catalog_.registrations,
                                 catalog_.ignoredRegistrations);
        baseCount_ = catalog_.sameGuildBaseCount;
        containerCount_ = catalog_.plan.ordered.size();
        pendingContainerCount_ = catalog_.pendingContainerCount;
        runtimeError_.clear();
        snapshotDirty_.mark();
    }


auto PalBaseResourceBridge::Impl::record_catalog_attempt(const double elapsed, const bool successful) -> void {
        lastCatalogMilliseconds_ = elapsed;
        maximumCatalogMilliseconds_ = std::max(maximumCatalogMilliseconds_, elapsed);
        ++catalogAttemptCount_;
        if (successful) {
            lastSuccessfulCatalogMilliseconds_ = elapsed;
        }
    }

}  // namespace base_resource_sharing

