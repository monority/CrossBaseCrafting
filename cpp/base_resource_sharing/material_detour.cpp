/**
 * @file material_detour.cpp
 * @brief Native detour on the authoritative build material counter (guarded by an
 *        exact build-size + prologue check). Isolated in its own TU because hooks.cpp
 *        calls install_material_count_detour(); the detour internals stay internal-linkage.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

#include <DynamicOutput/DynamicOutput.hpp>

#include <base_resource_sharing/impl.hpp>
#include <base_resource_sharing/pal_base_resource_runtime.hpp>

#pragma push_macro("ensure")
#pragma push_macro("check")
#undef ensure
#undef check
#include <polyhook2/Detour/x64Detour.hpp>
#pragma pop_macro("check")
#pragma pop_macro("ensure")

namespace base_resource_sharing {
using namespace RC;
namespace {

// ---------------------------------------------------------------------------
//  Native detour on the authoritative build material counter (0x31A3AC0).
//
//  The authoritative build check resolves a container per material from the
//  player's six fixed inventory container ids (UPalPlayerDataInventoryInfo at
//  InventoryData+0x100) and looks each one up by exact id inside the player's
//  InventoryMultiHelper. Cross-base chests are present in that helper but are
//  never selected, so the build is rejected with FailedNotEnoughMaterials even
//  though the materials are visible. This detour adds the shared containers the
//  mod appended to the helper (and only those) to the per-material count.
//
//  Guarded by an exact image-size + prologue check (Palworld build 161,802,312
//  bytes); it is a no-op on any other build.
// ---------------------------------------------------------------------------
constexpr std::uintptr_t kMaterialCountRva = 0x31A3AC0;
constexpr std::uintptr_t kExpectedImageSize = 0xA011000;
constexpr std::array<std::uint8_t, 8> kMaterialCountPrologue{0x48, 0x89, 0x5C, 0x24,
                                                             0x08, 0x48, 0x89, 0x74};

using MaterialCountFn = int (*)(void*, void*, int, void*);
PLH::x64Detour* g_materialCountDetour{};
std::uint64_t g_materialCountTrampoline{};
std::uintptr_t g_materialCountAddress{};

int __fastcall hook_material_count(void* inventoryData, void* material, int required,
                                   void* container) {
    auto* original = reinterpret_cast<MaterialCountFn>(g_materialCountTrampoline);
    if (original == nullptr) {
        return 0;
    }
    int total = original(inventoryData, material, required, container);
    if (total >= required || required <= 0) {
        return total;
    }
    // Only the containers the mod appended to the player helper are added: when the union is
    // inactive the span is empty, so vanilla behaviour is never altered.
    const auto sharedContainers = detail::live_union_containers();
    __try {
        for (auto* candidate : sharedContainers) {
            if (candidate == nullptr || candidate == container) {
                continue;
            }
            total += original(inventoryData, material, required - total, candidate);
            if (total >= required) {
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Probing a borrowed game container must never take the process down: keep the game's own
        // count when anything faults.
    }
    return total;
}

/** @brief Installs the material-count detour, guarded by an exact build-size + prologue check. */
}  // namespace

auto install_material_count_detour() -> void {
    if (g_materialCountDetour != nullptr) {
        return;
    }
    if (env_flag("CROSSBASE_NO_DETOUR")) {
        Output::send<LogLevel::Warning>(
            STR("[CBC] material-count detour disabled by CROSSBASE_NO_DETOUR\n"));
        return;
    }
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (moduleBase == 0) {
        return;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.SizeOfImage != kExpectedImageSize) {
        Output::send<LogLevel::Warning>(
            STR("[CBC] material-count detour skipped: unexpected game build\n"));
        return;
    }
    g_materialCountAddress = moduleBase + kMaterialCountRva;
    if (std::memcmp(reinterpret_cast<const void*>(g_materialCountAddress),
                    kMaterialCountPrologue.data(), kMaterialCountPrologue.size()) != 0) {
        Output::send<LogLevel::Warning>(
            STR("[CBC] material-count detour skipped: prologue mismatch\n"));
        return;
    }
    g_materialCountDetour = new PLH::x64Detour(
        g_materialCountAddress, reinterpret_cast<std::uint64_t>(&hook_material_count),
        &g_materialCountTrampoline);
    if (g_materialCountDetour->hook()) {
        Output::send<LogLevel::Verbose>(
            STR("[CBC] material-count detour installed at {:#x}\n"), g_materialCountAddress);
    } else {
        delete g_materialCountDetour;
        g_materialCountDetour = nullptr;
        g_materialCountTrampoline = 0;
        Output::send<LogLevel::Warning>(STR("[CBC] material-count detour install failed\n"));
    }
}
}  // namespace base_resource_sharing

