/**
 * @file hooks.cpp
 * @brief Impl method definitions for the hooks concern (Phase C split).
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
#include <base_resource_sharing/material_detour.hpp>
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


auto PalBaseResourceBridge::Impl::ensure_hooks_registered() -> void {
        const bool restoring =
            unionLifecycle_.phase(runtime_.generation()) == PersistentUnionPhase::restoring;
        if (!resource_hooks_required(runtime_.enabled(), runtime_.accessible()) && !restoring) {
            if (!hooks_.empty()) {
                unregister_resource_hooks();
                publish_snapshot();
            }
            return;
        }
        if (required_hooks_ready()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < nextHookAttempt_) {
            return;
        }
        nextHookAttempt_ = now + kHookRetryInterval;

        for (const auto& spec : palworld_1_0_1_hook_manifest()) {
            if (std::ranges::any_of(
                    hooks_, [&](const auto& hook) { return hook.spec.path == spec.path; })) {
                continue;
            }
            const std::wstring path{spec.path.begin(), spec.path.end()};
            auto* function =
                UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path.c_str());
            if (function == nullptr) {
                continue;
            }
            UnrealScriptFunctionCallable preCallback;
            UnrealScriptFunctionCallable postCallback;
            pal_game::FunctionHookRegistry::ScriptPreCallback scriptPreCallback;
            if (spec.preEvent != HookEvent::none) {
                preCallback = [this, function, spec](UnrealScriptFunctionCallableContext& context,
                                                     void*) {
                    static_cast<void>(on_hook_pre(function, spec, context));
                };
                scriptPreCallback = [this, function, spec](
                                        UnrealScriptFunctionCallableContext& context) {
                    return on_hook_pre(function, spec, context);
                };
            }
            if (spec.postEvent != HookEvent::none) {
                postCallback = [this, function, spec](UnrealScriptFunctionCallableContext& context,
                                                      void*) {
                    on_hook_post(function, spec, context);
                };
            }
            if (hookRegistry_.register_hook(function, std::move(preCallback),
                                            std::move(postCallback),
                                            std::move(scriptPreCallback))) {
                hooks_.push_back({.function = function, .spec = spec});
            }
        }

        install_material_count_detour();

        if (!hookDiagnosticsLogged_) {
            Output::send<LogLevel::Verbose>(
                STR("[CrossBaseCrafting] hooks actifs: {}/{}\n"), hooks_.size(),
                palworld_1_0_1_hook_manifest().size());
            hookDiagnosticsLogged_ = true;
        }

        rebuild_resolutions();
        publish_capabilities();
        snapshotDirty_.mark();
        publish_snapshot();
    }


[[nodiscard]] auto PalBaseResourceBridge::Impl::snapshot() const -> BaseResourceSharingSnapshot {
        const std::lock_guard lock(snapshotMutex_);
        return snapshot_;
    }


[[nodiscard]] auto PalBaseResourceBridge::Impl::concrete_model_membership(UFunction* function,
                                                 UnrealScriptFunctionCallableContext& context) const
        -> ConcreteModelRegistrationMembership {
        if (!catalog_.initialized || function == nullptr || context.Context == nullptr ||
            context.TheStack.Locals() == nullptr) {
            return ConcreteModelRegistrationMembership::unknown;
        }

        auto* modelProperty = CastField<FObjectPropertyBase>(
            function->FindProperty(FName(STR("ConcreteModel"), FNAME_Find)));
        auto* concreteModel =
            modelProperty == nullptr
                ? nullptr
                : modelProperty->GetObjectPropertyValue(
                      modelProperty->ContainerPtrToValuePtr<void>(context.TheStack.Locals()));
        auto* instanceIdProperty =
            concreteModel == nullptr
                ? nullptr
                : CastField<FStructProperty>(
                      concreteModel->GetPropertyByNameInChain(STR("InstanceId")));
        if (instanceIdProperty == nullptr ||
            instanceIdProperty->GetElementSize() != sizeof(FGuid)) {
            return ConcreteModelRegistrationMembership::unknown;
        }

        FGuid instanceId{};
        instanceIdProperty->CopyCompleteValue(
            &instanceId, instanceIdProperty->ContainerPtrToValuePtr<void>(concreteModel));
        const GuidKey ownerId{{instanceId.A, instanceId.B, instanceId.C, instanceId.D}};
        if (!ownerId.valid()) {
            return ConcreteModelRegistrationMembership::unknown;
        }

        return registrationIndex_.membership(
            {.moduleFullName = object_name(context.Context), .ownerMapObjectId = ownerId});
    }


auto PalBaseResourceBridge::Impl::handle_structure_changed(const StructureChangeSource source,
                                  const ConcreteModelRegistrationMembership membership) -> void {
        if (!runtime_.enabled() || safetyDisabled_ ||
            !should_forward_structure_change(source, waitingForStructure_, membership)) {
            return;
        }
        // Diagnostic kill switch: never start reconcile work.
        if (env_flag("CROSSBASE_NO_PERSISTENT")) {
            return;
        }

        const auto generation = runtime_.generation();
        switch (structure_change_action(unionLifecycle_.phase(generation))) {
            case PersistentStructureChangeAction::ignore:
                return;
            case PersistentStructureChangeAction::startReconcile:
                // Chunk streaming fires concrete-available events constantly (hundreds per
                // minute near a base); each pass costs a ~15 ms catalog scan on the game
                // thread, which reads as a mini stutter. Debounce: the first event wins,
                // the rest park as one follow-up consumed by the tick once due.
                if (std::chrono::steady_clock::now() < nextReconcileAttempt_) {
                    reconcileRequested_ = true;
                    break;
                }
                if (unionLifecycle_.invalidate(generation)) {
                    reset_pending_work();
                    nextReconcileAttempt_ =
                        std::chrono::steady_clock::now() + kReconcileDebounce;
                }
                break;
            case PersistentStructureChangeAction::queueFollowUp:
                // A notification that only unblocks initial discovery does not require another
                // pass. Once work is prepared, any number of further notifications collapse into
                // one follow-up reconcile after the current diff reaches a stable ledger.
                if (workPrepared_) {
                    reconcileRequested_ = true;
                }
                break;
        }

        waitingForStructure_ = false;
        publish_capabilities();
        snapshotDirty_.mark();
    }


    /**
     * @brief Traps a game-thread fault: disables all game interaction for this session.
     * @details Never touches game objects itself (the fault may have left them unstable):
     *          only flips the inert flag and publishes a snapshot. Hooks degrade to
     *          passthrough, the tick stops reconciling, the game keeps running vanilla.
     */
auto PalBaseResourceBridge::Impl::on_game_fault() -> void {
        faultDisabled_ = true;
        snapshotDirty_.mark();
        publish_snapshot();
        try {
            Output::send<LogLevel::Warning>(
                STR("[CrossBaseCrafting] trapped a game-thread fault; sharing inert "
                    "for this session\n"));
        } catch (...) {
        }
    }


auto PalBaseResourceBridge::Impl::dispatch_hook(UFunction* function, const HookPhase phase, const HookSpec& spec,
                       UnrealScriptFunctionCallableContext& context) -> bool {
        bool allow = true;
        __try {
            allow = dispatch_hook_impl(function, phase, spec, context);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            on_game_fault();
        }
        return allow;
    }


auto PalBaseResourceBridge::Impl::dispatch_hook_impl(UFunction* function, const HookPhase phase, const HookSpec& spec,
                       UnrealScriptFunctionCallableContext& context) -> bool {
        if (selfMutation_ || !runtime_.accessible()) {
            return true;
        }
        const auto event = event_for_phase(spec, phase);
        if (event == HookEvent::none) {
            return true;
        }
        if (faultDisabled_) {
            // Inert passthrough: never touch the game, never block it.
            return true;
        }

        // [DIAG-E] Log every meaningful hook event so we can see the exact call sequence.
        log_diag(
            STR("[CBC-DIAG] hook: event={} op={} phase={} path={}\n"),
            static_cast<int>(event),
            static_cast<int>(spec.operation),
            static_cast<int>(phase),
            std::wstring{spec.path.begin(), spec.path.end()});

        bool allowOriginalCall = true;

        switch (event) {
            case HookEvent::none:
                break;
            case HookEvent::structureChanged:
                if (!runtime_.enabled() || safetyDisabled_) {
                    break;
                }
                handle_structure_changed(
                    spec.structureSource,
                    spec.structureSource == StructureChangeSource::concreteModelAvailable ||
                            spec.structureSource == StructureChangeSource::concreteModelUnavailable
                        ? concrete_model_membership(function, context)
                        : ConcreteModelRegistrationMembership::unknown);
                break;
            case HookEvent::ensurePersistentUnion:
                handle_ensure_persistent_union(context.Context, spec.operation);
                break;
            case HookEvent::restoreLiveUnion:
                // Menu/model teardown path: remove injected containers before native code
                // walks the helper. No-op when no union is active.
                restore_live_union();
                break;
            case HookEvent::validatePersistentUnion:
                allowOriginalCall = validate_persistent_union(spec.operation, context.Context);
                // [DIAG-F] Log whether we allowed or blocked RequestBuild_ToServer.
                log_diag(
                    STR("[CBC-DIAG] hook: validatePersistentUnion result={} union_active={} injected={}\n"),
                    allowOriginalCall ? 1 : 0,
                    liveUnion_.active ? 1 : 0,
                    liveUnion_.entry.has_value() ? liveUnion_.entry->injected.size() : 0u);
                break;
        }
        if (event != HookEvent::structureChanged) {
            snapshotDirty_.mark();
        }
        return allowOriginalCall;
    }


auto PalBaseResourceBridge::Impl::on_hook_pre(UFunction* function, const HookSpec& spec,
                     UnrealScriptFunctionCallableContext& context) -> bool {
        return dispatch_hook(function, HookPhase::pre, spec, context);
    }


auto PalBaseResourceBridge::Impl::on_hook_post(UFunction* function, const HookSpec& spec,
                      UnrealScriptFunctionCallableContext& context) -> void {
        static_cast<void>(dispatch_hook(function, HookPhase::post, spec, context));
    }


auto PalBaseResourceBridge::Impl::unregister_resource_hooks() -> void {
        hookRegistry_.unregister_all();
        hooks_.clear();
        resolutions_ = all_hook_resolutions(false);
        hooksReady_ = false;
        capabilitiesGeneration_ = 0;
        nextHookAttempt_ = {};
        hookDiagnosticsLogged_ = false;
        publish_capabilities();
        snapshotDirty_.mark();
    }


auto PalBaseResourceBridge::Impl::rebuild_resolutions() -> void {
        resolutions_ = all_hook_resolutions(false);
        for (const auto& hook : hooks_) {
            mark_resolved(resolutions_, hook.spec.path);
        }
        const auto capabilities = evaluate_capabilities(resolutions_);
        hooksReady_ = capabilities[operation_index(ResourceOperation::crafting)].available() &&
                      capabilities[operation_index(ResourceOperation::building)].available();
    }


[[nodiscard]] auto PalBaseResourceBridge::Impl::required_hooks_ready() const -> bool {
        return hooksReady_;
    }


auto PalBaseResourceBridge::Impl::publish_capabilities() -> void {
        auto capabilities = evaluate_capabilities(resolutions_);
        const auto phase = unionLifecycle_.phase(runtime_.generation());
        if (runtime_.enabled() && runtime_.accessible() && required_hooks_ready() &&
            phase != PersistentUnionPhase::ready) {
            std::string stageError;
            switch (phase) {
                case PersistentUnionPhase::off:
                case PersistentUnionPhase::initializing:
                    stageError = "Persistent resource union is initializing.";
                    break;
                case PersistentUnionPhase::reconciling:
                    stageError = "Persistent resource union is reconciling.";
                    break;
                case PersistentUnionPhase::restoring:
                    stageError = "Persistent resource union is restoring original registrations.";
                    break;
                case PersistentUnionPhase::failed:
                    stageError = runtimeError_.empty() ? "Persistent resource union has been safely disabled." : runtimeError_;
                    break;
                case PersistentUnionPhase::ready:
                    break;
            }
            capabilities[operation_index(ResourceOperation::crafting)] = {.error = stageError};
            capabilities[operation_index(ResourceOperation::building)] = {.error = stageError};
        }
        for (std::size_t index{}; index < capabilities.size(); ++index) {
            if (!worldDisabledErrors_[index].empty()) {
                capabilities[index] = {.error = worldDisabledErrors_[index]};
            }
            runtime_.set_capability(static_cast<ResourceOperation>(index), capabilities[index]);
        }
        capabilitiesGeneration_ = runtime_.generation();
        snapshotDirty_.mark();
    }


auto PalBaseResourceBridge::Impl::disable_operation(const ResourceOperation operation, std::string error) -> void {
        worldDisabledErrors_[operation_index(operation)] = std::move(error);
        runtime_.set_capability(
            operation, CapabilityState{.error = worldDisabledErrors_[operation_index(operation)]});
        runtimeError_ = runtime_.capability(operation).error;
        snapshotDirty_.mark();
    }


auto PalBaseResourceBridge::Impl::publish_snapshot() -> void {
        if (!snapshotDirty_.consume()) {
            return;
        }
        BaseResourceSharingSnapshot next;
        next.enabled = runtime_.enabled();
        next.worldAccessible = runtime_.accessible();
        next.worldGeneration = runtime_.generation();
        next.baseCount = baseCount_;
        next.containerCount = containerCount_;
        next.pendingContainerCount = pendingContainerCount_;
        next.persistentPhase = unionLifecycle_.phase(next.worldGeneration);
        next.appliedEdgeCount = unionLedger_.edges().size();
        next.pendingEdgeCount = workPrepared_ ? (work_.removed.size() - removalIndex_) +
                                                    (work_.added.size() - additionIndex_)
                                              : 0;
        next.consumerSurface = next.persistentPhase == PersistentUnionPhase::ready
                                   ? ResourceConsumerSurface::guildBaseModules
                                   : ResourceConsumerSurface::none;
        next.lastCatalogMilliseconds = lastCatalogMilliseconds_;
        next.lastSuccessfulCatalogMilliseconds = lastSuccessfulCatalogMilliseconds_;
        next.maximumCatalogMilliseconds = maximumCatalogMilliseconds_;
        next.catalogAttemptCount = catalogAttemptCount_;
        next.safetyDisabled = safetyDisabled_;
        for (std::size_t index{}; index < next.capabilities.size(); ++index) {
            next.capabilities[index] = runtime_.capability(static_cast<ResourceOperation>(index));
        }
        next.status = format_status(
            {.enabled = next.enabled,
             .worldAccessible = next.worldAccessible,
             .detectingCapabilities = next.worldAccessible && !required_hooks_ready(),
             .baseCount = next.baseCount,
             .containerCount = next.containerCount,
             .pendingContainerCount = next.pendingContainerCount,
             .craftingAvailable =
                 next.capabilities[operation_index(ResourceOperation::crafting)].available(),
             .buildingAvailable =
                 next.capabilities[operation_index(ResourceOperation::building)].available(),
             .repairAvailable =
                 next.capabilities[operation_index(ResourceOperation::repair)].available(),
             .craftingError = next.capabilities[operation_index(ResourceOperation::crafting)].error,
             .buildingError = next.capabilities[operation_index(ResourceOperation::building)].error,
             .repairError = next.capabilities[operation_index(ResourceOperation::repair)].error,
             .runtimeError = runtimeError_});

        const std::lock_guard lock(snapshotMutex_);
        snapshot_ = std::move(next);
    }

}  // namespace base_resource_sharing
