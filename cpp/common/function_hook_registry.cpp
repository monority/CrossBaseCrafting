/**
 * @file function_hook_registry.cpp
 * @brief Implements symmetric registration and unregistration of native/Blueprint
 *        UFunction hooks.
 */
#include <algorithm>
#include <exception>
#include <ranges>
#include <utility>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <common/function_hook_registry.hpp>

namespace pal_game {
using namespace RC;
using namespace RC::Unreal;

namespace {
auto log_hook_error(const TCHAR* message) noexcept -> void {
    try {
        Output::send<LogLevel::Warning>(message);
    } catch (...) {
        // The log device may already be gone before the mod closes; callbacks
        // must not fail because of this.
        static_cast<void>(0);
    }
}

auto unregister_native_hook(UFunction* function, const CallbackId callbackId) noexcept -> void {
    if (function == nullptr || callbackId < 0) {
        return;
    }
    try {
        static_cast<void>(function->UnregisterHook(callbackId));
    } catch (...) {
        log_hook_error(STR("PalworldEditor: native UFunction hook unregistration threw.\n"));
    }
}
}  // namespace

FunctionHookRegistry::FunctionHookRegistry(const std::wstring_view callbackNamePrefix)
    : callbackNamePrefix_{callbackNamePrefix} {
    scriptBindingIndex_.reserve(16);
}

FunctionHookRegistry::~FunctionHookRegistry() {
    for (const auto& binding : bindings_) {
        if (binding.gate != nullptr) {
            binding.gate->active.store(false, std::memory_order_release);
        }
    }
    if (scriptDispatcherGate_ != nullptr) {
        scriptDispatcherGate_->active.store(false, std::memory_order_release);
    }
    if (!bindings_.empty() || scriptDispatcherRegistered_) {
        log_hook_error(
            STR("PalworldEditor: FunctionHookRegistry destroyed before game-thread hook "
                "unregistration; callbacks were made inert.\n"));
    }
}

auto FunctionHookRegistry::register_hook(UFunction* function, Callback preCallback,
                                         Callback postCallback,
                                         ScriptPreCallback scriptPreCallback) -> bool {
    if (function == nullptr || (!preCallback && !postCallback)) {
        return false;
    }

    const auto functionPointer = function->GetFuncPtr();
    const auto backend = select_function_hook_backend(
        functionPointer != nullptr,
        functionPointer == UObject::ProcessInternalInternal.get_function_address(),
        function->HasAnyFunctionFlags(EFunctionFlags::FUNC_Native));

    if (backend == FunctionHookBackend::scriptFunction) {
        if (!ensure_script_dispatcher_registered()) {
            return false;
        }
        try {
            bindings_.push_back({.function = function,
                                 .backend = backend,
                                 .gate = nullptr,
                                 .preCallback = std::move(preCallback),
                                 .postCallback = std::move(postCallback),
                                 .scriptPreCallback = std::move(scriptPreCallback)});
            try {
                scriptBindingIndex_.emplace(function, bindings_.size() - 1);
            } catch (...) {
                bindings_.pop_back();
                throw;
            }
        } catch (...) {
            const bool hasScriptBinding = std::ranges::any_of(bindings_, [](const auto& binding) {
                return binding.backend == FunctionHookBackend::scriptFunction;
            });
            if (!hasScriptBinding) {
                unregister_script_dispatcher();
            }
            return false;
        }
        return true;
    }
    if (backend != FunctionHookBackend::nativeFunction) {
        return false;
    }

    CallbackId preId{-1};
    CallbackId postId{-1};
    std::shared_ptr<CallbackGate> gate;
    try {
        gate = std::make_shared<CallbackGate>();
    } catch (...) {
        return false;
    }
    try {
        if (preCallback) {
            preId =
                function->RegisterPreHook([gate, callback = preCallback](
                                              UnrealScriptFunctionCallableContext& context, void*) {
                    if (gate->active.load(std::memory_order_acquire)) {
                        invoke_safely(callback, context);
                    }
                });
        }
        if (postCallback) {
            postId = function->RegisterPostHook(
                [gate, callback = postCallback](UnrealScriptFunctionCallableContext& context,
                                                void*) {
                    if (gate->active.load(std::memory_order_acquire)) {
                        invoke_safely(callback, context);
                    }
                });
        }
    } catch (...) {
        gate->active.store(false, std::memory_order_release);
        unregister_native_hook(function, preId);
        unregister_native_hook(function, postId);
        return false;
    }

    const bool preReady = !preCallback || preId >= 0;
    const bool postReady = !postCallback || postId >= 0;
    if (!preReady || !postReady) {
        gate->active.store(false, std::memory_order_release);
        unregister_native_hook(function, preId);
        unregister_native_hook(function, postId);
        return false;
    }

    try {
        bindings_.push_back({.function = function,
                             .backend = backend,
                             .gate = gate,
                             .preCallback = std::move(preCallback),
                             .postCallback = std::move(postCallback),
                             .preId = preId,
                             .postId = postId});
    } catch (...) {
        gate->active.store(false, std::memory_order_release);
        unregister_native_hook(function, preId);
        unregister_native_hook(function, postId);
        return false;
    }
    return true;
}

auto FunctionHookRegistry::unregister_all() noexcept -> void {
    for (const auto& binding : bindings_) {
        if (binding.gate != nullptr) {
            binding.gate->active.store(false, std::memory_order_release);
        }
    }
    if (scriptDispatcherGate_ != nullptr) {
        scriptDispatcherGate_->active.store(false, std::memory_order_release);
    }
    for (auto& binding : std::views::reverse(bindings_)) {
        if (binding.backend != FunctionHookBackend::nativeFunction || binding.function == nullptr) {
            continue;
        }
        unregister_native_hook(binding.function, binding.preId);
        unregister_native_hook(binding.function, binding.postId);
    }
    unregister_script_dispatcher();
    bindings_.clear();
    scriptBindingIndex_.clear();
    scriptDispatcherGate_.reset();
}

auto FunctionHookRegistry::empty() const noexcept -> bool {
    return bindings_.empty();
}

auto FunctionHookRegistry::ensure_script_dispatcher_registered() -> bool {
    if (scriptDispatcherRegistered_) {
        return true;
    }
    if (scriptDispatcherGate_ == nullptr ||
        !scriptDispatcherGate_->active.load(std::memory_order_acquire)) {
        try {
            scriptDispatcherGate_ = std::make_shared<CallbackGate>();
        } catch (...) {
            return false;
        }
    }

    try {
        const auto gate = scriptDispatcherGate_;
        Hook::FCallbackOptions preOptions;
        preOptions.OwnerModName = STR("CrossBaseCrafting");
        preOptions.HookName = STR("ScriptDispatcher.Pre");
        scriptDispatcherPreId_ = Hook::RegisterProcessLocalScriptFunctionPreCallback(
            [this, gate](auto& iteration, UObject* context, FFrame& stack, void* result) {
                if (gate->active.load(std::memory_order_acquire)) {
                    dispatch_script(iteration, context, stack, result);
                }
            }, std::move(preOptions));

        // All current script hooks are pre-hooks.  Do not install a global post dispatcher:
        // ProcessLocalScriptFunction is a very hot UE4SS path and the unused post callback
        // caused an avoidable extra callback traversal for every Blueprint call.
        scriptDispatcherRegistered_ = scriptDispatcherPreId_ != Hook::ERROR_ID;
        if (!scriptDispatcherRegistered_) {
            unregister_script_dispatcher();
        }
    } catch (...) {
        unregister_script_dispatcher();
        return false;
    }

    return scriptDispatcherRegistered_;
}

auto FunctionHookRegistry::unregister_script_dispatcher() noexcept -> void {
    if (scriptDispatcherGate_ != nullptr) {
        scriptDispatcherGate_->active.store(false, std::memory_order_release);
    }
    if (scriptDispatcherPreId_ != Hook::ERROR_ID) {
        static_cast<void>(Hook::UnregisterCallback(scriptDispatcherPreId_));
        scriptDispatcherPreId_ = Hook::ERROR_ID;
    }
    if (scriptDispatcherPostId_ != Hook::ERROR_ID) {
        static_cast<void>(Hook::UnregisterCallback(scriptDispatcherPostId_));
        scriptDispatcherPostId_ = Hook::ERROR_ID;
    }
    scriptDispatcherRegistered_ = false;
}

auto FunctionHookRegistry::dispatch_script(Hook::TCallbackIterationData<void>& iteration,
                                           UObject* context, FFrame& stack, void* result) -> void {
    auto* const function = stack.Node();
    const auto bindingIt = scriptBindingIndex_.find(function);
    if (bindingIt == scriptBindingIndex_.end()) {
        return;
    }
    const auto& binding = bindings_[bindingIt->second];
    UnrealScriptFunctionCallableContext callableContext{context, stack, result};
    if (binding.scriptPreCallback) {
        if (!invoke_script_pre_safely(binding.scriptPreCallback, callableContext)) {
            iteration.PreventOriginalFunctionCall();
        }
        return;
    }
    invoke_safely(binding.preCallback, callableContext);
}

auto FunctionHookRegistry::invoke_script_pre_safely(
    const ScriptPreCallback& callback, UnrealScriptFunctionCallableContext& context) -> bool {
    if (!callback) {
        return true;
    }
    try {
        return callback(context);
    } catch (const std::exception&) {
        log_hook_error(STR("PalworldEditor: script UFunction hook callback rejected an exception\n"));
    } catch (...) {
        log_hook_error(
            STR("PalworldEditor: script UFunction hook callback rejected an unknown exception\n"));
    }
    return false;
}

auto FunctionHookRegistry::invoke_safely(const Callback& callback,
                                         UnrealScriptFunctionCallableContext& context) -> void {
    if (!callback) {
        return;
    }
    try {
        callback(context, nullptr);
    } catch (const std::exception&) {
        log_hook_error(STR("PalworldEditor: UFunction hook callback rejected an exception\n"));
    } catch (...) {
        log_hook_error(
            STR("PalworldEditor: UFunction hook callback rejected an unknown exception\n"));
    }
}

}  // namespace pal_game
