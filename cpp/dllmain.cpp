#define NOMINMAX
#include <cstdint>
#include <utility>
#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/Hooks.hpp>
#include <base_resource_sharing/pal_base_resources.hpp>

namespace {
using namespace RC;
using namespace RC::Unreal;

class CrossBaseCraftingMod final : public CppUserModBase {
public:
    CrossBaseCraftingMod() {
        ModName = STR("CrossBaseCrafting");
        ModVersion = STR("3.0.0-cpp");
        ModAuthors = STR("ddva");
        ModDescription = STR("Event-driven cross-base chest and food-box sharing");
        Output::send<LogLevel::Verbose>(STR("[CrossBaseCrafting] C++ mod loaded\n"));

        // UE4SS keeps callbacks in global storage. Pinning the module prevents
        // the callback code from becoming invalid during a hot-unload.
        HMODULE module{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&CrossBaseCraftingMod::pin), &module);
    }

    void on_unreal_init() override {
        Hook::FCallbackOptions tickOptions;
        tickOptions.OwnerModName = STR("CrossBaseCrafting");
        tickOptions.HookName = STR("Lifecycle.EngineTick");
        lifecycleTickId_ = Hook::RegisterEngineTickPreCallback(
            [this](auto&, UEngine*, float deltaSeconds, bool) {
                if (!started_) {
                    started_ = true;
                    bridge_.on_world_ready(worldGeneration_);
                    bridge_.set_enabled(true);
                    Output::send<LogLevel::Verbose>(
                        STR("[CrossBaseCrafting] runtime enabled\n"));
                }
                bridge_.ensure_hooks_registered();
                bridge_.tick(deltaSeconds);
            }, std::move(tickOptions));

        Hook::FCallbackOptions loadPreOptions;
        loadPreOptions.OwnerModName = STR("CrossBaseCrafting");
        loadPreOptions.HookName = STR("Lifecycle.LoadMap.Pre");
        lifecycleLoadPreId_ = Hook::RegisterLoadMapPreCallback(
            [this](auto&, UEngine*, FWorldContext&, FURL, UPendingNetGame*, FString&) {
                bridge_.on_world_begin(++worldGeneration_);
                return std::pair<bool, bool>{true, true};
            }, std::move(loadPreOptions));

        Hook::FCallbackOptions loadPostOptions;
        loadPostOptions.OwnerModName = STR("CrossBaseCrafting");
        loadPostOptions.HookName = STR("Lifecycle.LoadMap.Post");
        lifecycleLoadPostId_ = Hook::RegisterLoadMapPostCallback(
            [this](auto&, UEngine*, FWorldContext&, FURL, UPendingNetGame*, FString&) {
                bridge_.on_world_ready(worldGeneration_);
                return std::pair<bool, bool>{true, true};
            }, std::move(loadPostOptions));

        Output::send<LogLevel::Verbose>(
            STR("[CrossBaseCrafting] lifecycle hooks registered\n"));
    }

private:
    static void pin() {}

    base_resource_sharing::PalBaseResourceBridge bridge_;
    std::uint64_t worldGeneration_{1};
    bool started_{};
    Hook::GlobalCallbackId lifecycleTickId_{Hook::ERROR_ID};
    Hook::GlobalCallbackId lifecycleLoadPreId_{Hook::ERROR_ID};
    Hook::GlobalCallbackId lifecycleLoadPostId_{Hook::ERROR_ID};
};
}  // namespace

extern "C" {
__declspec(dllexport) RC::CppUserModBase* start_mod() { return new CrossBaseCraftingMod(); }

// The current UE4SS callback API has no callback removal function. The DLL is
// pinned above, so keeping the instance alive is required for safe callbacks.
__declspec(dllexport) void uninstall_mod(RC::CppUserModBase*) {}
}
