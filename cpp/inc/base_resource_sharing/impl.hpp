#pragma once
/**
 * @file impl.hpp
 * @brief Declaration of PalBaseResourceBridge::Impl plus shared inline helpers.
 * @details Extracted mechanically from pal_base_resources.cpp (Phase C1). Method
 *          definitions live in cpp/base_resource_sharing/*.cpp; this header carries
 *          only declarations, members, and small shared helpers.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
#include <base_resource_sharing/pal_base_resource_runtime.hpp>
#include <base_resource_sharing/pal_base_resources.hpp>
#include <base_resource_sharing/persistent_union.hpp>
#include <common/function_hook_registry.hpp>
#include <common/diagnostics.hpp>
#include <common/game_reflection.hpp>
#include <common/reflection_cache.hpp>
#include <common/timing.hpp>


namespace base_resource_sharing {
using pal_game::find_object_by_full_name;
using namespace RC;
using namespace RC::Unreal;

inline auto log_shutdown_error_noexcept(const TCHAR* message) noexcept -> void {
    try {
        Output::send<LogLevel::Error>(message);
    } catch (...) {
        static_cast<void>(0);
    }
}


[[nodiscard]] inline auto object_name(UObject* object) -> std::wstring {
    return object == nullptr ? std::wstring{} : std::wstring{object->GetFullName()};
}


/**
 * @brief Reads a `CROSSBASE_*` diagnostic kill switch from the environment.
 * @details Absent, empty, or starting with '0' means off. Lets users bisect a crash
 *          between subsystems without rebuilding: `CROSSBASE_NO_DETOUR=1` skips the native
 *          material-count detour, `CROSSBASE_NO_LIVE_UNION=1` skips the player-helper
 *          injection (persistent base-module union keeps running).
 */
[[nodiscard]] inline auto env_flag(const char* name) -> bool {
    const char* value = std::getenv(name);
    if (value != nullptr && value[0] != '\0' && value[0] != '0') {
        return true;
    }
    // File twin for zero-friction bisecting with GUI launchers (Steam): creating
    // "crossbase_no_detour.txt" / "crossbase_no_live_union.txt" NEXT TO
    // Palworld-Win64-Shipping.exe has the same effect as the environment variable.
    // Resolved from the exe path (CWD of a launcher is unreliable). Probed only at
    // the few call sites that already run a few times per second.
    wchar_t path[512]{};
    constexpr std::size_t kCapacity = 512;
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(kCapacity));
    if (length == 0 || length >= kCapacity) {
        return false;
    }
    std::size_t end = length;
    while (end > 0 && path[end - 1] != L'\\' && path[end - 1] != L'/') {
        --end;
    }
    std::size_t out = end;
    for (std::size_t index{}; name[index] != '\0' && out + 5 < kCapacity; ++index) {
        const char c = name[index];
        path[out++] = (c >= 'A' && c <= 'Z') ? static_cast<wchar_t>(c - 'A' + 'a')
                                             : static_cast<wchar_t>(c);
    }
    path[out++] = L'.';
    path[out++] = L't';
    path[out++] = L'x';
    path[out++] = L't';
    path[out] = L'\0';
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}


[[nodiscard]] inline auto registration_key(const PersistentUnionEdge& edge)
    -> ConcreteModelRegistrationKey {
    return {.moduleFullName = edge.targetModuleFullName, .ownerMapObjectId = edge.ownerMapObjectId};
}


class MutationScope {
public:
    explicit MutationScope(bool& flag) noexcept : flag_{flag} {
        flag_ = true;
    }

    ~MutationScope() {
        flag_ = false;
    }

    MutationScope(const MutationScope&) = delete;
    auto operator=(const MutationScope&) -> MutationScope& = delete;

private:
    bool& flag_;
};

class PalBaseResourceBridge::Impl {
public:
    struct HookBinding {
        UFunction* function{};
        HookSpec spec;
    };
    auto set_enabled(const bool enabled) -> void;
    auto on_world_begin(const std::uint64_t generation) -> void;
    auto on_world_ready(const std::uint64_t generation) -> void;
    auto tick(const float deltaSeconds) -> void;
    auto tick_impl(const float deltaSeconds) -> void;
    auto maintain_live_union() -> void;
    auto ensure_hooks_registered() -> void;
    auto shutdown_hooks() -> void;
    [[nodiscard]] auto snapshot() const -> BaseResourceSharingSnapshot;
private:
    auto remember_world_context(UObject* context) -> void;
    [[nodiscard]] auto resolve_world_context(UObject* hint) -> UObject*;
    auto reset_pending_work() noexcept -> void;
    auto prepare_restore_work() -> void;
    auto finalize_disabled_state() -> void;
    auto fail_persistent_union(std::string error) -> void;
    [[nodiscard]] auto prepare_reconcile_work() -> bool;
    auto complete_persistent_work() -> void;
    auto process_persistent_work() -> void;
    auto restore_all_synchronously(const std::string_view reason) -> void;
    auto handle_ensure_persistent_union(UObject* context, const ResourceOperation operation) -> void;
    auto restore_live_union() -> void;
    [[nodiscard]] auto validate_persistent_union(const ResourceOperation operation, UObject* context) -> bool;
    auto accept_catalog(detail::ResourceCatalogSnapshot next) -> void;
    auto record_catalog_attempt(const double elapsed, const bool successful) -> void;
    [[nodiscard]] auto concrete_model_membership(UFunction* function,
                                                 UnrealScriptFunctionCallableContext& context) const
        -> ConcreteModelRegistrationMembership;
    auto handle_structure_changed(const StructureChangeSource source,
                                  const ConcreteModelRegistrationMembership membership) -> void;
    auto on_game_fault() -> void;
    auto dispatch_hook(UFunction* function, const HookPhase phase, const HookSpec& spec,
                       UnrealScriptFunctionCallableContext& context) -> bool;
    auto dispatch_hook_impl(UFunction* function, const HookPhase phase, const HookSpec& spec,
                       UnrealScriptFunctionCallableContext& context) -> bool;
    auto on_hook_pre(UFunction* function, const HookSpec& spec,
                     UnrealScriptFunctionCallableContext& context) -> bool;
    auto on_hook_post(UFunction* function, const HookSpec& spec,
                      UnrealScriptFunctionCallableContext& context) -> void;
    auto unregister_resource_hooks() -> void;
    auto rebuild_resolutions() -> void;
    [[nodiscard]] auto required_hooks_ready() const -> bool;
    auto publish_capabilities() -> void;
    auto disable_operation(const ResourceOperation operation, std::string error) -> void;
    auto publish_snapshot() -> void;

    RuntimeState runtime_;
    detail::ResourceCatalogSnapshot catalog_;
    detail::LiveUnion liveUnion_;
    ConcreteModelRegistrationIndex registrationIndex_;
    PersistentUnionLifecycle unionLifecycle_;
    PersistentUnionLedger unionLedger_;
    PersistentUnionPlan desiredPlan_;
    PersistentUnionDiff work_;
    std::size_t removalIndex_{};
    std::size_t additionIndex_{};
    bool workPrepared_{};
    bool reconcileRequested_{};
    /** @brief Earliest time the next reconcile pass may start (structure-event debounce). */
    std::chrono::steady_clock::time_point nextReconcileAttempt_{};
    bool waitingForStructure_{};
    std::vector<HookResolution> resolutions_{all_hook_resolutions(false)};
    std::vector<HookBinding> hooks_;
    bool hooksReady_{};
    pal_game::FunctionHookRegistry hookRegistry_{STR("BaseResourceScript")};
    std::array<std::string, 3> worldDisabledErrors_;
    std::uint64_t capabilitiesGeneration_{};
    std::size_t baseCount_{};
    std::size_t containerCount_{};
    std::size_t pendingContainerCount_{};
    std::wstring worldContextFullName_;
    std::string runtimeError_;
    std::chrono::steady_clock::time_point nextHookAttempt_{};
    bool hookDiagnosticsLogged_{};
    double lastCatalogMilliseconds_{};
    double lastSuccessfulCatalogMilliseconds_{};
    double maximumCatalogMilliseconds_{};
    std::size_t catalogAttemptCount_{};
    bool selfMutation_{};
    bool safetyDisabled_{};
    bool faultDisabled_{};
    std::chrono::steady_clock::time_point nextUnionMaintenance_{};
    std::chrono::steady_clock::time_point nextUnionCompletenessCheck_{};
    std::chrono::steady_clock::time_point nextPresenceCheck_{};
    SnapshotDirtyFlag snapshotDirty_;
    mutable std::mutex snapshotMutex_;
    BaseResourceSharingSnapshot snapshot_;
};
}  // namespace base_resource_sharing
