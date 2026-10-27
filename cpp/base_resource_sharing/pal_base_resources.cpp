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
#include <base_resource_sharing/persistent_union.hpp>
#include <common/function_hook_registry.hpp>
#include <common/diagnostics.hpp>
#include <common/game_reflection.hpp>
#include <common/reflection_cache.hpp>
#include <common/timing.hpp>


namespace base_resource_sharing {

PalBaseResourceBridge::PalBaseResourceBridge() : impl_{std::make_unique<Impl>()} {}
PalBaseResourceBridge::~PalBaseResourceBridge() = default;
PalBaseResourceBridge::PalBaseResourceBridge(PalBaseResourceBridge&&) noexcept = default;
auto PalBaseResourceBridge::operator=(PalBaseResourceBridge&&) noexcept
    -> PalBaseResourceBridge& = default;

auto PalBaseResourceBridge::set_enabled(const bool enabled) -> void {
    impl_->set_enabled(enabled);
}

auto PalBaseResourceBridge::on_world_begin(const std::uint64_t generation) -> void {
    impl_->on_world_begin(generation);
}

auto PalBaseResourceBridge::on_world_ready(const std::uint64_t generation) -> void {
    impl_->on_world_ready(generation);
}

auto PalBaseResourceBridge::tick(const float deltaSeconds) -> void {
    impl_->tick(deltaSeconds);
}

auto PalBaseResourceBridge::ensure_hooks_registered() -> void {
    impl_->ensure_hooks_registered();
}

auto PalBaseResourceBridge::shutdown_hooks() -> void {
    impl_->shutdown_hooks();
}

auto PalBaseResourceBridge::snapshot() const -> BaseResourceSharingSnapshot {
    return impl_->snapshot();
}
}  // namespace base_resource_sharing

