/**
 * @file function_hook_registry.hpp
 * @brief Declares the reversible hook registry that supports both native UFunction and Blueprint script functions.
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Unreal/Hooks.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <common/function_hook_backend.hpp>

namespace RC::Unreal {
class UFunction;
}

namespace pal_game {

/**
 * @brief Manages a group of UFunction pre/post hooks with unified Blueprint script dispatch.
 * @details The registry only holds a non-owning UFunction handle for the hook lifetime; callers must call
 *          unregister_all() on the game thread before LoadMap or before the owning module is unloaded.
 *          The destructor does not access Unreal -- it only closes remaining callback gates.
 */
class FunctionHookRegistry final {
public:
    using Callback = RC::Unreal::UnrealScriptFunctionCallable;
    using ScriptPreCallback = std::function<bool(
        RC::Unreal::UnrealScriptFunctionCallableContext&)>;

    /** @param[in] callbackNamePrefix Diagnostic name prefix for the global script callback. */
    explicit FunctionHookRegistry(std::wstring_view callbackNamePrefix);
    ~FunctionHookRegistry();
    FunctionHookRegistry(const FunctionHookRegistry&) = delete;
    auto operator=(const FunctionHookRegistry&) -> FunctionHookRegistry& = delete;
    FunctionHookRegistry(FunctionHookRegistry&&) = delete;
    auto operator=(FunctionHookRegistry&&) -> FunctionHookRegistry& = delete;

    /**
     * @brief Registers callbacks for a signature-verified UFunction.
     * @param[in] function Non-owning runtime UFunction handle.
     * @param[in] preCallback Optional pre-hook.
     * @param[in] postCallback Optional post-hook.
     * @retval true Successfully registered.
     * @retval false Backend not supported or registration failed; no partial registration is left behind.
     */
    [[nodiscard]] auto register_hook(RC::Unreal::UFunction* function, Callback preCallback,
                                     Callback postCallback,
                                     ScriptPreCallback scriptPreCallback = {}) -> bool;

    /**
     * @brief Unregisters all native hooks and the global script dispatch callback in reverse order.
     * @warning Must be called on the game thread; the destructor does not access Unreal instead.
     */
    auto unregister_all() noexcept -> void;

    /** @retval true No hooks are currently registered. */
    [[nodiscard]] auto empty() const noexcept -> bool;

private:
    struct CallbackGate {
        std::atomic<bool> active{true};
    };

    struct Binding {
        RC::Unreal::UFunction* function{};
        FunctionHookBackend backend{FunctionHookBackend::unsupported};
        std::shared_ptr<CallbackGate> gate;
        Callback preCallback;
        Callback postCallback;
        ScriptPreCallback scriptPreCallback;
        RC::Unreal::CallbackId preId{-1};
        RC::Unreal::CallbackId postId{-1};
    };

    [[nodiscard]] auto ensure_script_dispatcher_registered() -> bool;
    auto unregister_script_dispatcher() noexcept -> void;
    auto dispatch_script(RC::Unreal::Hook::TCallbackIterationData<void>& iteration,
                         RC::Unreal::UObject* context, RC::Unreal::FFrame& stack,
                         void* result) -> void;
    static auto invoke_safely(const Callback& callback,
                              RC::Unreal::UnrealScriptFunctionCallableContext& context) -> void;
    static auto invoke_script_pre_safely(const ScriptPreCallback& callback,
                                         RC::Unreal::UnrealScriptFunctionCallableContext& context)
        -> bool;

    std::wstring callbackNamePrefix_;
    std::vector<Binding> bindings_;
    // ProcessLocalScriptFunction is global; avoid scanning every binding for every Blueprint call.
    std::unordered_map<RC::Unreal::UFunction*, std::size_t> scriptBindingIndex_;
    std::shared_ptr<CallbackGate> scriptDispatcherGate_;
    RC::Unreal::Hook::GlobalCallbackId scriptDispatcherPreId_{
        RC::Unreal::Hook::ERROR_ID};
    RC::Unreal::Hook::GlobalCallbackId scriptDispatcherPostId_{
        RC::Unreal::Hook::ERROR_ID};
    bool scriptDispatcherRegistered_{};
};

}  // namespace pal_game

