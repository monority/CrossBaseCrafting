/**
 * @file live_union.cpp
 * @brief Impl method definitions for the live_union concern (Phase C split).
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


    /**
     * @brief Keeps the cross-base containers exposed to the local player while inside a base.
     * @details The build menu computes its material plan when it opens, and that path only reads the
     *          player helper. Waiting for the crafting UI or for RequestBuild_ToServer is too late:
     *          the plan sent to the server would then miss every cross-base container and the server
     *          would reject the build with FailedNotEnoughMaterials (17). Keeping the union applied as
     *          soon as the player stands in a base makes availability and consumption agree again.
     *
     *          The injection is only valid while the player stands inside the union's target base.
     *          Every other game system reading the helper (death-drop collection, loot/craft output
     *          routing) would otherwise treat foreign chests as player inventory: base contents ending
     *          up in the corpse, or chest-bound mats landing in the backpack. Presence is therefore
     *          re-verified on every tick while active; leaving (or switching) base tears the union
     *          down immediately instead of waiting for the periodic completeness check.
     *          The re-verification runs on a 500 ms throttle, never every tick: the
     *          StaticFindObject + controller/pawn/base reflective chain every frame cost
     *          measurable FPS. 500 ms keeps the worst-case stale window (corpse/loot
     *          pollution right after leaving a base) short while dividing reflective
     *          calls by ~30.
     */
auto PalBaseResourceBridge::Impl::maintain_live_union() -> void {
        const auto now = std::chrono::steady_clock::now();
        if (liveUnion_.active) {
            // Throttled presence check (see docstring): a full resolve every tick killed FPS.
            if (now < nextPresenceCheck_) {
                return;
            }
            nextPresenceCheck_ = now + kPresenceInterval;
            auto* activeContext = resolve_world_context(nullptr);
            if (activeContext == nullptr) {
                return;
            }
            std::string activeBaseError;
            const auto insideBase =
                detail::resolve_inside_base_id(activeContext, catalog_, activeBaseError);
            const bool stillInsideTarget =
                insideBase.has_value() && liveUnion_.exposure.targetBaseId.has_value() &&
                *insideBase == *liveUnion_.exposure.targetBaseId;
            if (!stillInsideTarget) {
                restore_live_union();
                if (!insideBase.has_value()) {
                    // Outside any catalog base: nothing to expose.
                    return;
                }
                // Switched to another base: fall through and re-apply below.
            } else {
                // The catalog can still grow after the union was first applied (containers were loading
                // when the player entered the base). A stale union is otherwise reused forever, so
                // completeness is rechecked periodically and a grown plan forces a restore + re-apply.
                if (now < nextUnionCompletenessCheck_) {
                    return;
                }
                nextUnionCompletenessCheck_ = now + kUnionCompletenessInterval;
                const auto expected =
                    liveUnion_.exposure.targetBaseId.has_value()
                        ? select_shared_container_ids(catalog_.plan.ordered,
                                                      *liveUnion_.exposure.targetBaseId)
                              .size()
                        : 0u;
                if (liveUnion_.entry.has_value() && liveUnion_.entry->injected.size() >= expected) {
                    return;
                }
                restore_live_union();
            }
        } else {
            if (now < nextUnionMaintenance_) {
                return;
            }
            nextUnionMaintenance_ = now + kUnionMaintenanceInterval;
        }

        auto* worldContext = resolve_world_context(nullptr);
        if (worldContext == nullptr) {
            return;
        }
        std::string baseError;
        if (!detail::resolve_inside_base_id(worldContext, catalog_, baseError).has_value()) {
            // Not standing in a game-confirmed base yet: nothing to expose.
            return;
        }
        handle_ensure_persistent_union(worldContext, ResourceOperation::building);
    }


auto PalBaseResourceBridge::Impl::remember_world_context(UObject* context) -> void {
        if (context != nullptr) {
            worldContextFullName_ = object_name(context);
        }
    }


[[nodiscard]] auto PalBaseResourceBridge::Impl::resolve_world_context(UObject* hint) -> UObject* {
        if (hint != nullptr) {
            remember_world_context(hint);
            return hint;
        }
        if (auto* saved = find_object_by_full_name(worldContextFullName_); saved != nullptr) {
            return saved;
        }

        // This is a one-time bootstrap per world. Subsequent reconciliations use StaticFindObject.
        auto* inventory = UObjectGlobals::FindFirstOf(STR("PalPlayerInventoryData"));
        remember_world_context(inventory);
        return inventory;
    }


auto PalBaseResourceBridge::Impl::handle_ensure_persistent_union(UObject* context, const ResourceOperation operation) -> void {
        remember_world_context(context);
        // Diagnostic kill switch: skip the player-helper injection entirely. NO_PERSISTENT
        // implies it too (live union reads the catalog, which stays empty with the engine off).
        if (env_flag("CROSSBASE_NO_LIVE_UNION") || env_flag("CROSSBASE_NO_PERSISTENT")) {
            return;
        }
        if (waitingForStructure_) {
            waitingForStructure_ = false;
            reset_pending_work();
        }
        if (!runtime_.enabled() || safetyDisabled_ ||
            unionLifecycle_.phase(runtime_.generation()) != PersistentUnionPhase::ready) {
            return;
        }

        auto* worldContext = resolve_world_context(context);
        const auto targetBase = detail::resolve_inside_base_id(worldContext, catalog_, runtimeError_);
        const auto exposure = make_exposure_plan(operation, targetBase);

        // [DIAG-1] Log what base and surface we resolved for this operation.
        log_diag(
            STR("[CBC-DIAG] ensure_union: op={} surface={} base_valid={} base_error={}\n"),
            static_cast<int>(operation),
            static_cast<int>(exposure.surface),
            exposure.targetBaseId.has_value() ? 1 : 0,
            std::wstring{runtimeError_.begin(), runtimeError_.end()});

        if (worldContext == nullptr || !exposure.targetBaseId.has_value()) {
            runtimeError_ = "Cannot determine the active base for resource union.";
            snapshotDirty_.mark();
            return;
        }

        if (liveUnion_.active && same_exposure_content(liveUnion_.exposure, exposure) &&
            liveUnion_.generation == runtime_.generation()) {
            // [DIAG-2] Union already carries the exact same containers — reused.
            // [!] Comparison is operation-agnostic on purpose: crafting and building share the
            //     player helper surface, so a live crafting union must never be torn down just
            //     because a build request arrives (that was removing the cross-base chests right
            //     before the native material consume ran).
            log_diag(
                STR("[CBC-DIAG] ensure_union: reusing existing union injected={}\n"),
                liveUnion_.entry.has_value() ? liveUnion_.entry->injected.size() : 0u);
            return;
        }
        restore_live_union();

        std::string error;
        if (!detail::apply_union(worldContext, catalog_, exposure, liveUnion_, error)) {
            runtimeError_ = error.empty() ? "Failed to inject shared containers." : error;
            Output::send<LogLevel::Warning>(
                STR("[CrossBaseCrafting] gameplay union failed: {}\n"),
                std::wstring{runtimeError_.begin(), runtimeError_.end()});
            snapshotDirty_.mark();
            return;
        }

        // [DIAG-3] Union applied — how many containers were injected?
        log_diag(
            STR("[CBC-DIAG] ensure_union: union APPLIED op={} surface={} injected={}\n"),
            static_cast<int>(operation),
            static_cast<int>(exposure.surface),
            liveUnion_.entry.has_value() ? liveUnion_.entry->injected.size() : 0u);

        snapshotDirty_.mark();
    }


auto PalBaseResourceBridge::Impl::restore_live_union() -> void {
        if (!liveUnion_.active) {
            return;
        }
        // [DIAG-5] Log every time the union is torn down — this reveals premature restores.
        log_diag(
            STR("[CBC-DIAG] restore_live_union: tearing down union op={} surface={} injected={}\n"),
            static_cast<int>(liveUnion_.exposure.operation),
            static_cast<int>(liveUnion_.exposure.surface),
            liveUnion_.entry.has_value() ? liveUnion_.entry->injected.size() : 0u);
        std::string error;
        if (!detail::restore_union(liveUnion_, error) && !error.empty()) {
            Output::send<LogLevel::Warning>(
                STR("[CrossBaseCrafting] union restore failed: {}\n"),
                std::wstring{error.begin(), error.end()});
        }
        liveUnion_ = {};
    }


[[nodiscard]] auto PalBaseResourceBridge::Impl::validate_persistent_union(const ResourceOperation operation, UObject* context) -> bool {
        // Paired with CROSSBASE_NO_LIVE_UNION / CROSSBASE_NO_PERSISTENT: vanilla passthrough,
        // never block the game.
        if (env_flag("CROSSBASE_NO_LIVE_UNION") || env_flag("CROSSBASE_NO_PERSISTENT")) {
            return true;
        }
        const auto generation = runtime_.generation();
        if (!runtime_.can_extend(operation, generation) ||
            unionLifecycle_.phase(generation) != PersistentUnionPhase::ready) {
            // [DIAG-6] Blocked before even checking the union — capability or lifecycle failure.
            log_diag(
                STR("[CBC-DIAG] validate_union: BLOCKED can_extend={} phase={}\n"),
                runtime_.can_extend(operation, generation) ? 1 : 0,
                static_cast<int>(unionLifecycle_.phase(generation)));
            return false;
        }
        if (operation != ResourceOperation::building) {
            return true;
        }

        // Proactively ensure the building union is established from RequestBuild_ToServer's
        // context. OnOpenMenu may not fire in all Palworld builds, making this the last
        // reliable hook before materials are consumed.
        //
        // [!] Readiness is judged on the injected container set (surface + target base), not on the
        //     foreground operation: the building union uses the same player-helper surface as crafting,
        //     so an already-active crafting union is exactly what the native build consume needs.
        const auto requiredSurface =
            make_exposure_plan(operation, liveUnion_.active ? liveUnion_.exposure.targetBaseId
                                                            : std::nullopt)
                .surface;
        const bool unionReadyForBuilding =
            liveUnion_.active && liveUnion_.generation == generation &&
            liveUnion_.exposure.surface == requiredSurface;
        if (!unionReadyForBuilding) {
            log_diag(
                STR("[CBC-DIAG] validate_union: no building union (active={} surface={}) — applying now\n"),
                liveUnion_.active ? 1 : 0,
                liveUnion_.active ? static_cast<int>(liveUnion_.exposure.surface) : -1);
            handle_ensure_persistent_union(context, operation);
        }
        // [FIX] Verify the union is now on the surface this operation consumes from before validating.
        if (!liveUnion_.active || liveUnion_.exposure.surface != requiredSurface) {
            Output::send<LogLevel::Warning>(
                STR("[CrossBaseCrafting] validate_union: building union application failed (active={} surface={})\n"),
                liveUnion_.active ? 1 : 0,
                liveUnion_.active ? static_cast<int>(liveUnion_.exposure.surface) : -1);
            runtimeError_ = "Failed to establish building resource union before construction.";
            snapshotDirty_.mark();
            return false;
        }
        std::string validationError;
        if (detail::validate_union(liveUnion_, validationError)) {
            // [DIAG-7] Happy path — union is intact at RequestBuild_ToServer time.
            log_diag(
                STR("[CBC-DIAG] validate_union: union INTACT injected={} — allowing RequestBuild_ToServer\n"),
                liveUnion_.entry.has_value() ? liveUnion_.entry->injected.size() : 0u);
            return true;
        }

        // [DIAG-8] Union was invalid — had to restore and re-apply.
        log_diag(
            STR("[CBC-DIAG] validate_union: union INVALID err={} — restoring and re-applying\n"),
            std::wstring{validationError.begin(), validationError.end()});

        restore_live_union();
        handle_ensure_persistent_union(context, operation);
        validationError.clear();
        if (detail::validate_union(liveUnion_, validationError)) {
            log_diag(
                STR("[CBC-DIAG] validate_union: union re-applied OK injected={}\n"),
                liveUnion_.entry.has_value() ? liveUnion_.entry->injected.size() : 0u);
            return true;
        }

        runtimeError_ = validationError.empty()
                            ? "Final build validation could not re-establish the union."
                            : std::move(validationError);
        Output::send<LogLevel::Warning>(
            STR("[CrossBaseCrafting] building union invalid before RequestBuild_ToServer\n"));
        snapshotDirty_.mark();
        return false;
    }

}  // namespace base_resource_sharing
