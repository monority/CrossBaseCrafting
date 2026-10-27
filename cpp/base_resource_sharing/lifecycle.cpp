/**
 * @file lifecycle.cpp
 * @brief Impl method definitions for the lifecycle concern (Phase C split).
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


auto PalBaseResourceBridge::Impl::set_enabled(const bool enabled) -> void {
        if (runtime_.enabled() == enabled) {
            return;
        }

        runtime_.set_preference(enabled);
        const auto generation = runtime_.generation();
        if (enabled && runtime_.accessible() && !safetyDisabled_) {
            static_cast<void>(unionLifecycle_.request_enable(generation));
            runtimeError_.clear();
            reconcileRequested_ = false;
            waitingForStructure_ = false;
            reset_pending_work();
        } else if (!enabled && runtime_.accessible()) {
            if (unionLifecycle_.request_disable(generation)) {
                prepare_restore_work();
            } else if (unionLifecycle_.phase(generation) == PersistentUnionPhase::off) {
                finalize_disabled_state();
            }
        }
        publish_capabilities();
        snapshotDirty_.mark();
        publish_snapshot();
    }


auto PalBaseResourceBridge::Impl::on_world_begin(const std::uint64_t generation) -> void {
        // Drop reflection metadata cached for the outgoing world before any object is touched.
        pal_game::reflection_cache().invalidate();
        restore_live_union();
        restore_all_synchronously("world change");
        unregister_resource_hooks();
        worldDisabledErrors_ = {};
        safetyDisabled_ = false;
        unionLedger_.clear();
        desiredPlan_ = {};
        unionLifecycle_.begin_world(generation);
        reconcileRequested_ = false;
        nextReconcileAttempt_ = {};
        waitingForStructure_ = false;
        reset_pending_work();
        catalog_ = {};
        registrationIndex_.clear();
        baseCount_ = 0;
        containerCount_ = 0;
        pendingContainerCount_ = 0;
        lastCatalogMilliseconds_ = 0.0;
        lastSuccessfulCatalogMilliseconds_ = 0.0;
        maximumCatalogMilliseconds_ = 0.0;
        catalogAttemptCount_ = 0;
        worldContextFullName_.clear();
        runtime_.begin_world_transition(generation);
        snapshotDirty_.mark();
        publish_snapshot();
    }


auto PalBaseResourceBridge::Impl::on_world_ready(const std::uint64_t generation) -> void {
        runtime_.finish_world_transition(generation);
        unionLifecycle_.begin_world(generation);
        waitingForStructure_ = false;
        if (runtime_.enabled() && !safetyDisabled_) {
            static_cast<void>(unionLifecycle_.request_enable(generation));
        }
        publish_capabilities();
        snapshotDirty_.mark();
        publish_snapshot();
    }


auto PalBaseResourceBridge::Impl::tick(const float deltaSeconds) -> void {
        // Any fault while touching game objects (teardown races, unloaded worlds) must
        // degrade to an inert mod, never take the game down (cf. Oct 2026 build-menu AVs).
        __try {
            tick_impl(deltaSeconds);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            on_game_fault();
        }
    }


auto PalBaseResourceBridge::Impl::tick_impl(const float deltaSeconds) -> void {
        static_cast<void>(deltaSeconds);
        if (!runtime_.accessible()) {
            publish_snapshot();
            return;
        }

        // Diagnostic kill switch: keep the persistent engine parked in `off` (no catalog
        // discovery, no edge apply/remove, no reconcile). Hooks still fire but early-out.
        const bool persistentOff = env_flag("CROSSBASE_NO_PERSISTENT");
        const auto phase = unionLifecycle_.phase(runtime_.generation());
        if (phase == PersistentUnionPhase::restoring && !persistentOff) {
            process_persistent_work();
        } else if (!persistentOff && runtime_.enabled() && !safetyDisabled_ && !faultDisabled_ &&
                   required_hooks_ready()) {
            if (phase == PersistentUnionPhase::off) {
                static_cast<void>(unionLifecycle_.request_enable(runtime_.generation()));
            }
            const auto currentPhase = unionLifecycle_.phase(runtime_.generation());
            if (currentPhase == PersistentUnionPhase::initializing ||
                currentPhase == PersistentUnionPhase::reconciling) {
                if (!waitingForStructure_) {
                    process_persistent_work();
                }
            } else if (currentPhase == PersistentUnionPhase::ready) {
                // Debounced follow-up: notifications that arrived while a pass was running (or
                // inside the debounce window) park here instead of starting a back-to-back scan.
                if (reconcileRequested_ &&
                    std::chrono::steady_clock::now() >= nextReconcileAttempt_) {
                    reconcileRequested_ = false;
                    if (unionLifecycle_.invalidate(runtime_.generation())) {
                        reset_pending_work();
                        nextReconcileAttempt_ =
                            std::chrono::steady_clock::now() + kReconcileDebounce;
                    }
                }
                maintain_live_union();
            }
        }
        publish_snapshot();
    }


    
    
auto PalBaseResourceBridge::Impl::shutdown_hooks() -> void {
        restore_live_union();
        // Restoration and hook removal are independent cleanup responsibilities. In particular,
        // an allocation or reflection failure while restoring must never leave callbacks pointing
        // into a DLL that UE4SS is about to unload.
        try {
        restore_all_synchronously("mod unload");
        } catch (...) {
            log_shutdown_error_noexcept(
                STR("PalworldEditor: persistent storage restore threw during shutdown.\n"));
        }
        try {
            unregister_resource_hooks();
        } catch (...) {
            log_shutdown_error_noexcept(
                STR("PalworldEditor: resource hook removal threw during shutdown.\n"));
        }
        runtime_.begin_world_transition(runtime_.generation() + 1);
        unionLifecycle_.begin_world(runtime_.generation());
        desiredPlan_ = {};
        reset_pending_work();
        baseCount_ = 0;
        containerCount_ = 0;
        pendingContainerCount_ = 0;
        if (unionLedger_.empty()) {
            catalog_ = {};
            registrationIndex_.clear();
            worldContextFullName_.clear();
        } else {
            safetyDisabled_ = true;
        }
        snapshotDirty_.mark();
        publish_snapshot();
    }

}  // namespace base_resource_sharing
