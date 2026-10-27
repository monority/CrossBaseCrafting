#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/Property/FArrayProperty.hpp>
#include <Unreal/Property/FObjectProperty.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/Property/ScriptArrayHelper.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <base_resource_sharing/current_base_resolution.hpp>
#include <base_resource_sharing/pal_base_resource_runtime.hpp>
#include <common/diagnostics.hpp>
#include <common/game_reflection.hpp>
#include <game/pal_base_camp_reflection.hpp>

namespace base_resource_sharing::detail {
using pal_game::find_object_by_full_name;
using pal_game::FunctionParams;
using namespace RC;
using namespace RC::Unreal;

namespace {

constexpr int32 kMaximumModulesPerBase = 4'096;
constexpr int32 kMaximumContainersPerModule = 10'000;

/**
 * @brief Containers currently appended to the player helper by the live union.
 * @details Borrowed, non-owning pointers valid only while the union is active. Published so the
 *          native build-material detour can count exactly the shared containers instead of every
 *          container the helper happens to hold.
 */
std::vector<UObject*> g_liveUnionContainers;

[[nodiscard]] auto to_key(const FGuid& guid) -> GuidKey {
    return {{guid.A, guid.B, guid.C, guid.D}};
}

[[nodiscard]] auto to_guid(const GuidKey& key) -> FGuid {
    return FGuid{key.words[0], key.words[1], key.words[2], key.words[3]};
}

[[nodiscard]] auto object_name(UObject* object) -> std::wstring {
    return object == nullptr ? std::wstring{} : std::wstring{object->GetFullName()};
}

[[nodiscard]] auto is_shareable_concrete(UObject* concrete) -> bool {
    if (!pal_game::is_valid(concrete)) {
        return false;
    }
    const auto name = object_name(concrete);
    return name.find(L"FoodBox") != std::wstring::npos ||
           name.find(L"ItemChest") != std::wstring::npos ||
           name.find(L"GuildChest") != std::wstring::npos;
}

[[nodiscard]] auto pal_utility() -> UObject* {
    // CDO is stable for a world; cache the resolve (was a StaticFindObject string lookup on
    // every reflection call). invalidate() on world begin drops it.
    auto& cache = pal_game::reflection_cache();
    if (pal_game::is_valid(cache.palUtility)) {
        return cache.palUtility;
    }
    cache.palUtility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    return cache.palUtility;
}


/** @brief Casts a UFunction's return property to the concrete type backing ReturnT. */
template <typename ReturnT>
[[nodiscard]] auto cast_return(UFunction* function) {
    if constexpr (std::is_same_v<ReturnT, bool>) {
        return function == nullptr
                   ? nullptr
                   : CastField<FBoolProperty>(function->GetReturnProperty());
    } else {
        return function == nullptr
                   ? nullptr
                   : CastField<FObjectPropertyBase>(function->GetReturnProperty());
    }
}

/**
 * @brief Resolves a single-argument PalUtility helper (WorldContextObject + return), validates,
 *        then calls it. Shared implementation behind call_utility_bool / call_utility_object; the
 *        only per-type difference is how the return property is cast and read (cast_return + the
 *        trailing if-constexpr). Fail-closed: any missing object, function, or signature mismatch
 *        returns false without mutating the game — identical to the two originals it replaces.
 */
template <typename ReturnT>
[[nodiscard]] auto call_pal_utility(UObject* worldContext, const CharType* functionName,
                                    ReturnT& value) -> bool {
    value = ReturnT{};
    auto* utility = pal_utility();
    auto* function = utility == nullptr ? nullptr : utility->GetFunctionByNameInChain(functionName);
    auto* const context = function == nullptr
                              ? nullptr
                              : CastField<FObjectPropertyBase>(function->FindProperty(
                                    FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const result = cast_return<ReturnT>(function);
    if (!pal_game::is_valid(utility) || !pal_game::is_valid(worldContext) ||
        !pal_game::has_exact_parameter_count(function, 2) ||
        !pal_game::is_input_parameter(context) || !pal_game::is_return_parameter(result)) {
        return false;
    }
    FunctionParams params{function};
    context->SetObjectPropertyValue(context->ContainerPtrToValuePtr<void>(params.data()),
                                    worldContext);
    utility->ProcessEvent(function, params.data());
    if constexpr (std::is_same_v<ReturnT, bool>) {
        value = result->GetPropertyValueInContainer(params.data());
        return true;
    } else {
        value = result->GetObjectPropertyValue(result->ContainerPtrToValuePtr<void>(params.data()));
        return pal_game::is_valid(value);
    }
}

[[nodiscard]] auto call_utility_bool(UObject* worldContext, const CharType* functionName,
                                     bool& value) -> bool {
    return call_pal_utility<bool>(worldContext, functionName, value);
}

[[nodiscard]] auto call_utility_object(UObject* worldContext, const CharType* functionName,
                                       UObject*& value) -> bool {
    return call_pal_utility<UObject*>(worldContext, functionName, value);
}

[[nodiscard]] auto try_get_guid(UObject* target, const CharType* functionName, FGuid& output)
    -> bool {
    if (!pal_game::is_valid(target)) {
        return false;
    }
    auto* function = target->GetFunctionByNameInChain(functionName);
    auto* const result =
        function == nullptr ? nullptr : CastField<FStructProperty>(function->GetReturnProperty());
    if (!pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_return_parameter(result) || result->GetElementSize() != sizeof(FGuid)) {
        return false;
    }
    FunctionParams params{function};
    target->ProcessEvent(function, params.data());
    result->CopyCompleteValue(&output, result->ContainerPtrToValuePtr<void>(params.data()));
    return to_key(output).valid();
}

[[nodiscard]] auto try_get_object(UObject* target, const CharType* functionName, UObject*& output)
    -> bool {
    const auto result = pal_game::invoke<UObject*>(target, functionName);
    output = result.value_or(nullptr);
    return result.has_value();
}

[[nodiscard]] auto read_object_property(UObject* object, const CharType* propertyName) -> UObject* {
    auto* property =
        object == nullptr
            ? nullptr
            : CastField<FObjectPropertyBase>(object->GetPropertyByNameInChain(propertyName));
    return property == nullptr
               ? nullptr
               : property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(object));
}

[[nodiscard]] auto try_get_player_guild(UObject* worldContext, const FGuid& playerId,
                                        UObject*& guild) -> bool {
    guild = nullptr;
    auto* utility = pal_utility();
    auto* function = utility == nullptr
                         ? nullptr
                         : utility->GetFunctionByNameInChain(STR("GetGuildByPlayerUId"));
    auto* const context = function == nullptr
                              ? nullptr
                              : CastField<FObjectPropertyBase>(function->FindProperty(
                                    FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const player = function == nullptr ? nullptr
                                             : CastField<FStructProperty>(function->FindProperty(
                                                   FName(STR("PlayerUId"), FNAME_Find)));
    auto* const result = function == nullptr
                             ? nullptr
                             : CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (!pal_game::is_valid(utility) || !pal_game::is_valid(worldContext) ||
        !pal_game::has_exact_parameter_count(function, 3) ||
        !pal_game::is_input_parameter(context) || !pal_game::is_input_parameter(player) ||
        player->GetElementSize() != sizeof(FGuid) || !pal_game::is_return_parameter(result)) {
        return false;
    }
    FunctionParams params{function};
    context->SetObjectPropertyValue(context->ContainerPtrToValuePtr<void>(params.data()),
                                    worldContext);
    player->CopyCompleteValue(player->ContainerPtrToValuePtr<void>(params.data()), &playerId);
    utility->ProcessEvent(function, params.data());
    guild = result->GetObjectPropertyValue(result->ContainerPtrToValuePtr<void>(params.data()));
    return pal_game::is_valid(guild);
}

[[nodiscard]] auto try_resolve_local_guild(UObject* worldContext, FGuid& guildId) -> bool {
    UObject* controller{};
    if (!call_utility_object(worldContext, STR("GetLocalPalPlayerController"), controller)) {
        return false;
    }

    FGuid playerId{};
    UObject* guild{};
    return try_get_guid(controller, STR("GetPlayerUId"), playerId) &&
           try_get_player_guild(controller, playerId, guild) &&
           try_get_guid(guild, STR("GetId"), guildId);
}

[[nodiscard]] auto find_concrete_model(UObject* manager, const GuidKey& ownerMapObjectId)
    -> UObject* {
    if (manager == nullptr || !ownerMapObjectId.valid()) {
        return nullptr;
    }
    UObject* concreteModel{};
    const auto instanceId = to_guid(ownerMapObjectId);
    return pal_base_camp_reflection::find_concrete_model(manager, instanceId, concreteModel)
               ? concreteModel
               : nullptr;
}

[[nodiscard]] auto try_get_container_info(FArrayProperty* arrayProperty, void* containerInfo,
                                          FGuid& containerId, FGuid& ownerMapObjectId,
                                          std::uint8_t* containerType = nullptr) -> bool {
    auto* infoProperty =
        arrayProperty == nullptr ? nullptr : CastField<FStructProperty>(arrayProperty->GetInner());
    auto* infoStruct = infoProperty == nullptr ? nullptr : infoProperty->GetStruct().Get();
    auto* ownerProperty = infoStruct == nullptr
                              ? nullptr
                              : CastField<FStructProperty>(infoStruct->GetPropertyByNameInChain(
                                    STR("OwnerMapObjectConcreteModelInstanceId")));
    auto* containerIdProperty =
        infoStruct == nullptr ? nullptr
                              : CastField<FStructProperty>(
                                    infoStruct->GetPropertyByNameInChain(STR("ContainerIdCache")));
    auto* containerIdStruct =
        containerIdProperty == nullptr ? nullptr : containerIdProperty->GetStruct().Get();
    auto* idProperty =
        containerIdStruct == nullptr
            ? nullptr
            : CastField<FStructProperty>(containerIdStruct->GetPropertyByNameInChain(STR("ID")));
    auto* typeProperty =
        infoStruct == nullptr ? nullptr : infoStruct->GetPropertyByNameInChain(STR("Type"));
    if (containerInfo == nullptr || ownerProperty == nullptr || containerIdProperty == nullptr ||
        idProperty == nullptr || (containerType != nullptr && typeProperty == nullptr)) {
        return false;
    }

    ownerProperty->CopyCompleteValue(&ownerMapObjectId,
                                     ownerProperty->ContainerPtrToValuePtr<void>(containerInfo));
    auto* cachedId = containerIdProperty->ContainerPtrToValuePtr<void>(containerInfo);
    idProperty->CopyCompleteValue(&containerId, idProperty->ContainerPtrToValuePtr<void>(cachedId));
    if (containerType != nullptr) {
        typeProperty->CopyCompleteValue(containerType,
                                        typeProperty->ContainerPtrToValuePtr<void>(containerInfo));
    }
    return to_key(containerId).valid() && to_key(ownerMapObjectId).valid();
}

[[nodiscard]] auto read_module_sequence(UObject* module, FArrayProperty* property,
                                        std::vector<GuidKey>& output) -> bool {
    output.clear();
    if (module == nullptr || property == nullptr) {
        return false;
    }
    FScriptArrayHelper_InContainer infos(property, module);
    const int32 count = infos.Num();
    if (count < 0 || count > kMaximumContainersPerModule) {
        return false;
    }
    output.reserve(static_cast<std::size_t>(count));
    for (int32 index{}; index < count; ++index) {
        FGuid containerId{};
        FGuid ownerMapObjectId{};
        if (!try_get_container_info(property, infos.GetRawPtr(index), containerId,
                                    ownerMapObjectId)) {
            return false;
        }
        output.push_back(to_key(containerId));
    }
    return true;
}

[[nodiscard]] auto read_helper_sequence(UObject* helper, FArrayProperty* property,
                                        std::vector<GuidKey>& output) -> bool {
    output.clear();
    auto* objectProperty =
        property == nullptr ? nullptr : CastField<FObjectPropertyBase>(property->GetInner());
    if (helper == nullptr || property == nullptr || objectProperty == nullptr) {
        return false;
    }
    FScriptArrayHelper_InContainer containers(property, helper);
    const int32 count = containers.Num();
    if (count < 0 || count > kMaximumContainersPerModule) {
        return false;
    }
    output.reserve(static_cast<std::size_t>(count));
    for (int32 index{}; index < count; ++index) {
        auto* container = objectProperty->GetObjectPropertyValue(containers.GetRawPtr(index));
        FGuid id{};
        if (!try_get_guid(container, STR("GetId"), id)) {
            return false;
        }
        output.push_back(to_key(id));
    }
    return true;
}

[[nodiscard]] auto append_array_copy(FArrayProperty* property, UObject* target, const void* source)
    -> bool {
    if (property == nullptr || target == nullptr || source == nullptr) {
        return false;
    }
    auto* inner = property->GetInner();
    auto* array = property->ContainerPtrToValuePtr<FScriptArray>(target);
    if (inner == nullptr || array == nullptr) {
        return false;
    }
    const auto elementSize = inner->GetElementSize();
    const auto alignment = inner->GetMinAlignment();
    const auto index = array->Add(1, elementSize, alignment);
    auto* destination = static_cast<std::uint8_t*>(array->GetData()) +
                        static_cast<std::size_t>(index) * elementSize;
    inner->InitializeValue(destination);
    inner->CopyCompleteValue(destination, source);
    return true;
}

[[nodiscard]] auto remove_array_indices(FArrayProperty* property, UObject* target,
                                        std::vector<int32> indices) -> bool {
    if (property == nullptr || target == nullptr) {
        return false;
    }
    auto* inner = property->GetInner();
    auto* array = property->ContainerPtrToValuePtr<FScriptArray>(target);
    if (inner == nullptr || array == nullptr) {
        return false;
    }
    std::ranges::sort(indices, std::greater{});
    for (const auto index : indices) {
        if (index < 0 || index >= array->Num()) {
            return false;
        }
        auto* element = static_cast<std::uint8_t*>(array->GetData()) +
                        static_cast<std::size_t>(index) * inner->GetElementSize();
        inner->DestroyValue(element);
        array->Remove(index, 1, inner->GetElementSize(), inner->GetMinAlignment());
    }
    return true;
}

auto notify_array_changed(UObject* object, const CharType* functionName) -> void {
    if (!pal_game::is_valid(object)) {
        return;
    }
    if (auto* function = object->GetFunctionByNameInChain(functionName);
        pal_game::has_exact_parameter_count(function, 0) &&
        function->GetReturnProperty() == nullptr) {
        object->ProcessEvent(function, nullptr);
    }
}

[[nodiscard]] auto resolve_live_container(UObject* mapObjectManager, const CatalogContainer& entry)
    -> UObject* {
    auto* concrete = find_concrete_model(mapObjectManager, entry.ownerMapObjectId);
    UObject* itemContainerModule{};
    UObject* container{};
    FGuid resolvedId{};
    if (!try_get_object(concrete, STR("GetItemContainerModule"), itemContainerModule) ||
        !try_get_object(itemContainerModule, STR("GetContainer"), container) ||
        !try_get_guid(container, STR("GetId"), resolvedId) ||
        to_key(resolvedId) != entry.containerId) {
        return nullptr;
    }
    return container;
}

[[nodiscard]] auto locate_catalog_container(const ResourceCatalogSnapshot& catalog,
                                            const GuidKey& containerId)
    -> std::pair<const CatalogModule*, const CatalogContainer*> {
    for (const auto& module : catalog.modules) {
        const auto found =
            std::ranges::find(module.containers, containerId, &CatalogContainer::containerId);
        if (found != module.containers.end()) {
            return {&module, &*found};
        }
    }
    return {};
}

[[nodiscard]] auto append_container_info(UObject* targetModule, FArrayProperty* targetProperty,
                                         const CatalogModule& sourceModule,
                                         const GuidKey& containerId) -> bool {
    auto* sourceObject = find_object_by_full_name(sourceModule.objectFullName);
    auto* sourceProperty = sourceObject == nullptr
                               ? nullptr
                               : CastField<FArrayProperty>(
                                     sourceObject->GetPropertyByNameInChain(STR("ContainerInfos")));
    if (targetModule == nullptr || targetProperty == nullptr || sourceProperty == nullptr) {
        return false;
    }
    FScriptArrayHelper_InContainer infos(sourceProperty, sourceObject);
    const int32 count = infos.Num();
    if (count < 0 || count > kMaximumContainersPerModule) {
        return false;
    }
    for (int32 index{}; index < count; ++index) {
        FGuid id{};
        FGuid ownerMapObjectId{};
        if (try_get_container_info(sourceProperty, infos.GetRawPtr(index), id, ownerMapObjectId) &&
            to_key(id) == containerId) {
            return append_array_copy(targetProperty, targetModule, infos.GetRawPtr(index));
        }
    }
    return false;
}

[[nodiscard]] auto indices_removed_from_sequences(const std::span<const GuidKey> current,
                                                  const std::span<const GuidKey> kept,
                                                  std::vector<int32>& indices) -> bool {
    indices.clear();
    std::size_t keptIndex{};
    for (std::size_t index{}; index < current.size(); ++index) {
        if (keptIndex < kept.size() && current[index] == kept[keptIndex]) {
            ++keptIndex;
        } else {
            indices.push_back(static_cast<int32>(index));
        }
    }
    return keptIndex == kept.size();
}

[[nodiscard]] auto restore_entry(const UnionLedgerEntry& entry) -> bool {
    auto* object = find_object_by_full_name(entry.objectFullName);
    if (object == nullptr) {
        return true;
    }
    const auto* propertyName = entry.helperArray ? STR("Containers") : STR("ContainerInfos");
    auto* property = CastField<FArrayProperty>(object->GetPropertyByNameInChain(propertyName));
    std::vector<GuidKey> current;
    const bool read = entry.helperArray ? read_helper_sequence(object, property, current)
                                        : read_module_sequence(object, property, current);
    if (!read) {
        return false;
    }
    const auto removal = remove_recorded_injections(current, entry.original, entry.injected);
    std::vector<int32> indices;
    if (!removal.complete || !indices_removed_from_sequences(current, removal.kept, indices) ||
        !remove_array_indices(property, object, std::move(indices))) {
        return false;
    }
    notify_array_changed(object,
                         entry.helperArray ? STR("OnRep_Containers") : STR("OnRep_ContainerInfos"));
    std::vector<GuidKey> restored;
    const bool finalRead = entry.helperArray ? read_helper_sequence(object, property, restored)
                                             : read_module_sequence(object, property, restored);
    return finalRead && restored == removal.kept;
}

[[nodiscard]] auto sequence_status_text(const SequenceValidationStatus status) -> std::string_view {
    switch (status) {
        case SequenceValidationStatus::valid:
            return "valid";
        case SequenceValidationStatus::originalPrefixChanged:
            return "originalPrefixChanged";
        case SequenceValidationStatus::injectedCountMismatch:
            return "injectedCountMismatch";
        case SequenceValidationStatus::duplicateInjectedId:
            return "duplicateInjectedId";
        case SequenceValidationStatus::unexpectedTail:
            return "unexpectedTail";
    }
    return "unknown";
}
}  // namespace

auto local_authority_ready(UObject* worldContext, std::string& error) -> bool {
    bool isServer{};
    bool isDedicated{};
    if (worldContext == nullptr || !call_utility_bool(worldContext, STR("IsServer"), isServer) ||
        !call_utility_bool(worldContext, STR("IsDedicatedServer"), isDedicated)) {
        error = "Cannot confirm local host authority for the current world.";
        return false;
    }
    if (!isServer) {
        // A network client can neither see the authoritative base storage modules nor mutate them;
        // acting there would only produce a display that the server refuses to honour.
        error =
            "Cross-base sharing runs on the world authority; this process is a network client.";
        return false;
    }
    if (isDedicated) {
        // The shared-container surface exposed to the local player helper (build menu plan and the
        // native material-count detour) needs a local Pawn, which a dedicated server has none of.
        error =
            "Dedicated servers are not supported by this build (single-player or listen-server "
            "host only).";
        return false;
    }
    error.clear();
    return true;
}

auto read_base_id(UObject* baseModel) -> std::optional<GuidKey> {
    using Names = CurrentBaseReflectionNames<CharType>;
    FGuid value{};
    if (!try_get_guid(baseModel, Names::baseIdFunction.data(), value)) {
        return std::nullopt;
    }
    const auto key = to_key(value);
    return key.valid() ? std::optional{key} : std::nullopt;
}

auto resolve_inside_base_id(UObject* worldContext, const ResourceCatalogSnapshot& catalog,
                            std::string& error) -> std::optional<GuidKey> {
    using Names = CurrentBaseReflectionNames<CharType>;
    error.clear();

    UObject* controller{};
    if (!call_utility_object(worldContext, STR("GetLocalPalPlayerController"), controller)) {
        error = "Cannot resolve the local player controller.";
        return std::nullopt;
    }

    UObject* pawn{};
    if (!try_get_object(controller, Names::controllerPawnFunction.data(), pawn)) {
        error = "Local player controller cannot return a Pawn via K2_GetPawn.";
        return std::nullopt;
    }

    auto* insideComponent = read_object_property(pawn, Names::insideComponentProperty.data());
    if (insideComponent == nullptr) {
        error = "Local Pawn is missing InsideBaseCampCheckComponent.";
        return std::nullopt;
    }

    UObject* baseModel{};
    if (!try_get_object(insideComponent, Names::insideBaseModelFunction.data(), baseModel)) {
        error = "Player is not inside a game-confirmed base.";
        return std::nullopt;
    }

    const auto candidate = read_base_id(baseModel);
    if (!candidate.has_value()) {
        error = "Current base model GetId did not return a valid GUID.";
        return std::nullopt;
    }

    const bool hasStorageModule = std::ranges::any_of(
        catalog.modules, [&](const auto& module) { return module.baseId == *candidate; });
    const auto accepted = accept_current_base(*candidate, hasStorageModule);
    if (!accepted.has_value()) {
        error = "Current base is not in the guild storage catalog.";
    }
    return accepted;
}

auto discover_catalog(UObject* worldContext, const std::uint64_t generation,
                      const std::span<const PersistentUnionEdge> appliedEdges)
    -> ResourceCatalogSnapshot {
    ResourceCatalogSnapshot result{.generation = generation};
    std::string authorityError;
    if (!local_authority_ready(worldContext, authorityError)) {
        result.error = std::move(authorityError);
        return result;
    }

    FGuid guildId{};
    UObject* baseCampManager{};
    UObject* mapObjectManager{};
    if (!try_resolve_local_guild(worldContext, guildId) ||
        !call_utility_object(worldContext, STR("GetBaseCampManager"), baseCampManager) ||
        !call_utility_object(worldContext, STR("GetMapObjectManager"), mapObjectManager)) {
        result.error = "Cannot resolve local guild, base-camp manager, or map-object manager.";
        return result;
    }
    result.guildId = to_key(guildId);

    auto* storageClass = UObjectGlobals::StaticFindObject<UClass*>(
        nullptr, nullptr, STR("/Script/Pal.PalBaseCampModuleItemStorage"));
    std::vector<FGuid> baseIds;
    if (storageClass == nullptr ||
        !pal_base_camp_reflection::read_base_ids(baseCampManager, baseIds)) {
        result.error = "Cannot read base list or base storage module class.";
        return result;
    }
    std::erase_if(baseIds, [](const FGuid& id) { return !to_key(id).valid(); });

    std::vector<ContainerDescriptor> descriptors;
    std::set<GuidKey> catalogIds;
    for (const auto& baseId : baseIds) {
        UObject* baseModel{};
        FGuid ownerGuild{};
        if (!pal_base_camp_reflection::try_get_base_model(baseCampManager, baseId, baseModel) ||
            !try_get_guid(baseModel, STR("GetGroupIdBelongTo"), ownerGuild)) {
            continue;
        }
        const bool sameGuild = to_key(ownerGuild) == result.guildId;
        if (sameGuild) {
            ++result.sameGuildBaseCount;
        }

        auto* moduleArray =
            CastField<FArrayProperty>(baseModel->GetPropertyByNameInChain(STR("ModuleArray")));
        auto* moduleProperty = moduleArray == nullptr
                                   ? nullptr
                                   : CastField<FObjectPropertyBase>(moduleArray->GetInner());
        if (moduleArray == nullptr || moduleProperty == nullptr) {
            if (sameGuild) {
                result.error = "Base model is missing ModuleArray object array.";
                return result;
            }
            continue;
        }

        UObject* storageModule{};
        FScriptArrayHelper_InContainer modules(moduleArray, baseModel);
        const int32 moduleCount = modules.Num();
        if (moduleCount < 0 || moduleCount > kMaximumModulesPerBase) {
            result.error = "Base module array count exceeds safety limit.";
            return result;
        }
        for (int32 index{}; index < moduleCount; ++index) {
            auto* candidate = moduleProperty->GetObjectPropertyValue(modules.GetRawPtr(index));
            if (candidate != nullptr && candidate->IsA(storageClass)) {
                storageModule = candidate;
                break;
            }
        }
        if (storageModule == nullptr) {
            continue;
        }

        const auto storageModuleName = object_name(storageModule);
        if (!sameGuild) {
            result.ignoredModuleNames.push_back(storageModuleName);
            continue;
        }

        auto* containerInfos = CastField<FArrayProperty>(
            storageModule->GetPropertyByNameInChain(STR("ContainerInfos")));
        if (containerInfos == nullptr) {
            result.error = "Base storage module is missing ContainerInfos.";
            return result;
        }

        CatalogModule catalogModule{.baseId = to_key(baseId),
                                    .objectFullName = std::move(storageModuleName),
                                    .moduleObject = storageModule};
        std::set<GuidKey> moduleIds;
        FScriptArrayHelper_InContainer infos(containerInfos, storageModule);
        const int32 containerCount = infos.Num();
        if (containerCount < 0 || containerCount > kMaximumContainersPerModule) {
            result.error = "Base storage registration count exceeds safety limit.";
            return result;
        }
        for (int32 index{}; index < containerCount; ++index) {
            FGuid containerId{};
            FGuid ownerMapObjectId{};
            if (!try_get_container_info(containerInfos, infos.GetRawPtr(index), containerId,
                                        ownerMapObjectId)) {
                result.error = "Storage registration entry is missing a valid container or map-object ID.";
                return result;
            }
            const CatalogContainer entry{.containerId = to_key(containerId),
                                         .ownerMapObjectId = to_key(ownerMapObjectId)};
            const ConcreteModelRegistrationKey registration{
                .moduleFullName = catalogModule.objectFullName,
                .ownerMapObjectId = entry.ownerMapObjectId,
            };
            result.registrations.push_back(registration);
            const auto appliedEdge = std::ranges::find_if(appliedEdges, [&](const auto& edge) {
                return edge.targetBaseId == catalogModule.baseId &&
                       edge.targetModuleFullName == catalogModule.objectFullName &&
                       edge.containerId == entry.containerId &&
                       edge.ownerMapObjectId == entry.ownerMapObjectId;
            });
            if (appliedEdge != appliedEdges.end()) {
                result.observedAppliedEdges.push_back(*appliedEdge);
                continue;
            }
            if (moduleIds.insert(entry.containerId).second) {
                const bool firstGlobalOccurrence = catalogIds.insert(entry.containerId).second;
                if (firstGlobalOccurrence) {
                    ++result.registeredContainerCount;
                }
            auto* concreteModel = find_concrete_model(mapObjectManager, entry.ownerMapObjectId);
            if (!is_shareable_concrete(concreteModel) ||
                resolve_live_container(mapObjectManager, entry) == nullptr) {
                result.registrations.pop_back();
                result.ignoredRegistrations.push_back(registration);
                if (firstGlobalOccurrence) {
                    ++result.pendingContainerCount;
                }
                    continue;
                }
                catalogModule.containers.push_back(entry);
                if (firstGlobalOccurrence) {
                    descriptors.push_back({.baseId = catalogModule.baseId,
                                           .groupId = result.guildId,
                                           .containerId = entry.containerId,
                                           .kind = ContainerKind::normal});
                }
            }
        }
        result.modules.push_back(std::move(catalogModule));
    }

    result.observedAppliedEdges = normalized_edges(result.observedAppliedEdges);

    if (!descriptors.empty()) {
        result.plan = make_resource_union_plan(descriptors, result.guildId);
        if (!result.plan.error.empty()) {
            result.error = result.plan.error;
            return result;
        }
    }
    result.initialized = true;
    return result;
}

auto apply_union(UObject* worldContext, const ResourceCatalogSnapshot& catalog,
                 const ResourceExposurePlan& exposure, LiveUnion& liveUnion, std::string& error)
    -> bool {
    error.clear();
    if (liveUnion.active) {
        error = "A base resource union is already active.";
        return false;
    }
    if (exposure.surface == ResourceConsumerSurface::none ||
        exposure.operation == ResourceOperation::repair) {
        error = "Current material operation has no available single consumer surface.";
        return false;
    }
    if (worldContext == nullptr || !catalog.initialized || catalog.generation == 0 ||
        !catalog.guildId.valid() || !catalog.error.empty() || catalog.plan.ordered.empty()) {
        error = "Resource catalog has not completed safety calibration yet.";
        return false;
    }

    liveUnion = {
        .generation = catalog.generation,
        .guildId = catalog.guildId,
        .exposure = exposure,
        .active = true,
    };
    g_liveUnionContainers.clear();
    const auto rollback = [&](std::string primaryError) {
        std::string restoreError;
        if (!restore_union(liveUnion, restoreError)) {
            primaryError += "; " + restoreError;
        }
        error = std::move(primaryError);
        return false;
    };

    if (!exposure.targetBaseId.has_value()) {
        return rollback("Material union is missing the current base identifier.");
    }
    const auto globalIds =
        select_shared_container_ids(catalog.plan.ordered, *exposure.targetBaseId);

    // [DIAG-A] Log how many cross-base containers are selected for injection.
    log_diag(
        STR("[CBC-DIAG] apply_union: surface={} globalIds_to_inject={} total_plan_entries={}\n"),
        static_cast<int>(exposure.surface),
        globalIds.size(),
        catalog.plan.ordered.size());

    if (exposure.surface == ResourceConsumerSurface::currentBaseModule) {
        const auto module =
            std::ranges::find(catalog.modules, *exposure.targetBaseId, &CatalogModule::baseId);
        if (module == catalog.modules.end()) {
            return rollback("Current base has no available storage module.");
        }
        auto* object = find_object_by_full_name(module->objectFullName);
        auto* property = object == nullptr
                             ? nullptr
                             : CastField<FArrayProperty>(
                                   object->GetPropertyByNameInChain(STR("ContainerInfos")));
        UnionLedgerEntry ledger{.objectFullName = module->objectFullName};
        if (!read_module_sequence(object, property, ledger.original)) {
            return rollback("Cannot read current base storage sequence before building union.");
        }

        // [DIAG-B] Log the module name and how many entries it had before injection.
        log_diag(
            STR("[CBC-DIAG] apply_union: module={} original_size={} to_inject={}\n"),
            module->objectFullName,
            ledger.original.size(),
            globalIds.size());

        liveUnion.entry = std::move(ledger);
        const auto missing = missing_union_tail(liveUnion.entry->original, globalIds);
        for (const auto& id : missing) {
            const auto [sourceModule, sourceContainer] = locate_catalog_container(catalog, id);
            static_cast<void>(sourceContainer);
            if (sourceModule == nullptr ||
                !append_container_info(object, property, *sourceModule, id)) {
                return rollback("Failed to append shared chest registration to current base storage.");
            }
            liveUnion.entry->injected.push_back(id);

            // [DIAG-C] Log each container injected and its source module.
            log_diag(
                STR("[CBC-DIAG] apply_union: injected container from module={}\n"),
                sourceModule->objectFullName);
        }
        if (!liveUnion.entry->injected.empty()) {
            notify_array_changed(object, STR("OnRep_ContainerInfos"));
        }
        std::vector<GuidKey> current;
        if (!read_module_sequence(object, property, current)) {
            return rollback("Cannot re-read current base storage sequence after building union.");
        }
        const auto validation = validate_applied_sequence(liveUnion.entry->original,
                                                          liveUnion.entry->injected, current);

        // [DIAG-D] Log the final ContainerInfos size and whether validation passed.
        log_diag(
            STR("[CBC-DIAG] apply_union: post-inject size={} injected={} validation={}\n"),
            current.size(),
            liveUnion.entry->injected.size(),
            validation ? 1 : 0);

        if (!validation) {
            return rollback("Building union sequence validation failed: " +
                            std::string{sequence_status_text(validation.status)});
        }
        return true;
    }

    UObject* mapObjectManager{};
    UObject* inventoryData{};
    if (!call_utility_object(worldContext, STR("GetMapObjectManager"), mapObjectManager) ||
        !call_utility_object(worldContext, STR("GetLocalInventoryData"), inventoryData)) {
        return rollback("Cannot resolve map-object manager or local main backpack.");
    }
    auto* helperProperty = CastField<FObjectPropertyBase>(
        inventoryData->GetPropertyByNameInChain(STR("InventoryMultiHelper")));
    auto* helper = helperProperty == nullptr
                       ? nullptr
                       : helperProperty->GetObjectPropertyValue(
                             helperProperty->ContainerPtrToValuePtr<void>(inventoryData));
    auto* containersProperty =
        helper == nullptr
            ? nullptr
            : CastField<FArrayProperty>(helper->GetPropertyByNameInChain(STR("Containers")));
    auto* containerProperty = containersProperty == nullptr
                                  ? nullptr
                                  : CastField<FObjectPropertyBase>(containersProperty->GetInner());
    UnionLedgerEntry helperLedger{
        .objectFullName = object_name(helper),
        .helperArray = true,
    };
    if (containerProperty == nullptr ||
        !read_helper_sequence(helper, containersProperty, helperLedger.original)) {
        return rollback("Local main backpack resource helper is not available.");
    }
    liveUnion.entry = std::move(helperLedger);

    const auto missingHelperIds = missing_union_tail(liveUnion.entry->original, globalIds);
    for (const auto& id : missingHelperIds) {
        const auto [sourceModule, sourceContainer] = locate_catalog_container(catalog, id);
        static_cast<void>(sourceModule);
        auto* container = sourceContainer == nullptr
                              ? nullptr
                              : resolve_live_container(mapObjectManager, *sourceContainer);
        if (container == nullptr || !append_array_copy(containersProperty, helper, &container)) {
            return rollback("Failed to append shared chest to local main backpack resource helper.");
        }
        g_liveUnionContainers.push_back(container);
        liveUnion.entry->injected.push_back(id);
    }
    if (!liveUnion.entry->injected.empty()) {
        notify_array_changed(helper, STR("OnRep_Containers"));
    }
    std::vector<GuidKey> current;
    if (!read_helper_sequence(helper, containersProperty, current)) {
        return rollback("Cannot re-read local main backpack resource helper sequence after crafting union.");
    }
    const auto validation =
        validate_applied_sequence(liveUnion.entry->original, liveUnion.entry->injected, current);
    if (!validation) {
        return rollback("Crafting union sequence validation failed: " +
                        std::string{sequence_status_text(validation.status)});
    }
    return true;
}

auto persistent_modules(const ResourceCatalogSnapshot& catalog)
    -> std::vector<PersistentStorageModule> {
    std::vector<PersistentStorageModule> result;
    result.reserve(catalog.modules.size());
    for (const auto& module : catalog.modules) {
        PersistentStorageModule persistent{
            .baseId = module.baseId,
            .objectFullName = module.objectFullName,
        };
        persistent.containers.reserve(module.containers.size());
        for (const auto& container : module.containers) {
            persistent.containers.push_back({
                .containerId = container.containerId,
                .ownerMapObjectId = container.ownerMapObjectId,
            });
        }
        result.push_back(std::move(persistent));
    }
    return result;
}

namespace {
[[nodiscard]] auto persistent_edge_target(const ResourceCatalogSnapshot& catalog,
                                          const PersistentUnionEdge& edge) -> const CatalogModule* {
    const auto target = std::ranges::find_if(catalog.modules, [&](const auto& module) {
        return module.baseId == edge.targetBaseId &&
               module.objectFullName == edge.targetModuleFullName;
    });
    return target == catalog.modules.end() ? nullptr : &*target;
}

[[nodiscard]] auto persistent_edge_source(const ResourceCatalogSnapshot& catalog,
                                          const PersistentUnionEdge& edge)
    -> const CatalogContainer* {
    const auto module =
        std::ranges::find(catalog.modules, edge.sourceBaseId, &CatalogModule::baseId);
    if (module == catalog.modules.end()) {
        return nullptr;
    }
    const auto container = std::ranges::find_if(module->containers, [&](const auto& candidate) {
        return candidate.containerId == edge.containerId &&
               candidate.ownerMapObjectId == edge.ownerMapObjectId;
    });
    return container == module->containers.end() ? nullptr : &*container;
}

[[nodiscard]] auto module_property(UObject* module) -> FArrayProperty* {
    return module == nullptr
               ? nullptr
               : CastField<FArrayProperty>(module->GetPropertyByNameInChain(STR("ContainerInfos")));
}

[[nodiscard]] auto count_container(const std::span<const GuidKey> sequence,
                                   const GuidKey& containerId) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count(sequence, containerId));
}

[[nodiscard]] auto call_concrete_model_event(UObject* module, const CharType* functionName,
                                             UObject* concreteModel) -> bool {
    auto* function = module == nullptr ? nullptr : module->GetFunctionByNameInChain(functionName);
    auto* concreteProperty =
        function == nullptr ? nullptr
                            : CastField<FObjectPropertyBase>(
                                  function->FindProperty(FName(STR("ConcreteModel"), FNAME_Find)));
    if (!pal_game::is_valid(module) || !pal_game::is_valid(concreteModel) ||
        !pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_input_parameter(concreteProperty) ||
        function->GetReturnProperty() != nullptr) {
        return false;
    }
    FunctionParams params{function};
    concreteProperty->SetObjectPropertyValue(
        concreteProperty->ContainerPtrToValuePtr<void>(params.data()), concreteModel);
    module->ProcessEvent(function, params.data());
    return true;
}

[[nodiscard]] auto remove_container_id_fallback(UObject* module, FArrayProperty* property,
                                                const GuidKey& containerId,
                                                const std::span<const GuidKey> before) -> bool {
    const auto position = std::ranges::find(before, containerId);
    if (position == before.end()) {
        return true;
    }
    const auto index = static_cast<int32>(std::distance(before.begin(), position));
    if (!remove_array_indices(property, module, {index})) {
        return false;
    }
    notify_array_changed(module, STR("OnRep_ContainerInfos"));
    std::vector<GuidKey> after;
    if (!read_module_sequence(module, property, after)) {
        return false;
    }
    std::vector<GuidKey> expected{before.begin(), before.end()};
    expected.erase(expected.begin() + index);
    return after == expected;
}
}  // namespace

auto apply_persistent_edge(UObject* worldContext, const ResourceCatalogSnapshot& catalog,
                           const PersistentUnionEdge& edge) -> PersistentEdgeMutationResult {
    const auto* target = persistent_edge_target(catalog, edge);
    if (worldContext == nullptr || catalog.generation == 0 || target == nullptr ||
        persistent_edge_source(catalog, edge) == nullptr) {
        return {.error = "Persistent union edge does not belong to the current safe catalog."};
    }
    // Module UObject resolved during discovery (fresh this pass) — avoids the ~13 ms
    // StaticFindObject-by-full-path that dominated every edge apply. Fallback keeps the old
    // resolution when discovery did not capture a module (e.g. stale/empty catalog).
    auto* module = target->moduleObject != nullptr
                       ? target->moduleObject
                       : find_object_by_full_name(edge.targetModuleFullName);
    auto* property = module_property(module);
    std::vector<GuidKey> before;
    if (!read_module_sequence(module, property, before)) {
        return {.error = "Cannot read target base storage sequence before persistent union registration."};
    }
    const auto beforeCount = count_container(before, edge.containerId);
    if (beforeCount == 1) {
        return {.mutation = PersistentEdgeMutation::unchanged};
    }
    if (beforeCount != 0) {
        return {.error = "Target container is already duplicated before persistent union registration."};
    }

    UObject* mapObjectManager{};
    if (!call_utility_object(worldContext, STR("GetMapObjectManager"), mapObjectManager)) {
        return {.error = "Cannot resolve map-object manager during persistent union registration."};
    }
    auto* concreteModel = find_concrete_model(mapObjectManager, edge.ownerMapObjectId);
    if (!call_concrete_model_event(module, STR("OnAvailableConcreteModel_ServerInternal"),
                                   concreteModel)) {
        return {.error = "Cannot call native storage ConcreteModel registration interface."};
    }

    std::vector<GuidKey> after;
    if (!read_module_sequence(module, property, after)) {
        static_cast<void>(call_concrete_model_event(
            module, STR("OnNotAvailableConcreteModel_ServerInternal"), concreteModel));
        return {.mutation = PersistentEdgeMutation::added,
                .error = "Cannot re-read target base storage sequence after registration; native rollback requested but unverifiable."};
    }
    const auto mutation =
        classify_persistent_edge_add(beforeCount, count_container(after, edge.containerId));
    auto expected = before;
    expected.push_back(edge.containerId);
    if (mutation != PersistentEdgeMutation::added || after != expected) {
        static_cast<void>(call_concrete_model_event(
            module, STR("OnNotAvailableConcreteModel_ServerInternal"), concreteModel));
        std::vector<GuidKey> restored;
        if (!read_module_sequence(module, property, restored) || restored != before) {
            return {.mutation = PersistentEdgeMutation::added,
                    .error = "Native storage registration sequence anomaly, and rollback verification failed."};
        }
        return {.error = "Native storage registration did not produce the exact expected +1 container change; fully rolled back."};
    }
    return {.mutation = mutation};
}

/**
 * @brief Forces the native storage subsystem to rebuild its internal state for an already-recorded
 *        cross-base container by unregistering and immediately re-registering it.
 * @details Re-calling only the registration is a no-op when the container is already present, so a
 *          stale server-side consume list can never be repaired that way. Removing first proves the
 *          native call really ran, and the re-registration restores the exact prior sequence.
 */


/**
 * @brief Logs the inventory-data object the mod injects into and its helper container count.
 * @details Compared against the player-state inventory the authoritative build uses, this reveals
 *          whether the consume reads a different UPalItemContainerMultiHelper than the one the mod
 *          populated.
 */

auto remove_persistent_edge(UObject* worldContext, const ResourceCatalogSnapshot& catalog,
                            const PersistentUnionEdge& edge) -> PersistentEdgeMutationResult {
    if (edge.targetModuleFullName.empty() || !edge.containerId.valid()) {
        return {.error = "Persistent union unregistration edge is invalid."};
    }
    // Prefer the module UObject resolved during discovery (fresh this pass) to skip the
    // ~13 ms StaticFindObject. A base that streamed out has no catalog target and no live
    // object => already removed, matching the previous name-lookup semantics.
    const auto* target = persistent_edge_target(catalog, edge);
    auto* module = target != nullptr && target->moduleObject != nullptr
                       ? target->moduleObject
                       : find_object_by_full_name(edge.targetModuleFullName);
    if (module == nullptr) {
        return {.mutation = PersistentEdgeMutation::removed};
    }
    auto* property = module_property(module);
    std::vector<GuidKey> before;
    if (!read_module_sequence(module, property, before)) {
        return {.error = "Cannot read target base storage sequence before persistent union unregistration."};
    }
    const auto beforeCount = count_container(before, edge.containerId);
    if (beforeCount == 0) {
        return {.mutation = PersistentEdgeMutation::unchanged};
    }
    if (beforeCount != 1) {
        return {.error = "Target container occurrence count is anomalous before persistent union unregistration."};
    }

    UObject* mapObjectManager{};
    UObject* concreteModel{};
    if (worldContext != nullptr &&
        call_utility_object(worldContext, STR("GetMapObjectManager"), mapObjectManager)) {
        concreteModel = find_concrete_model(mapObjectManager, edge.ownerMapObjectId);
    }
    if (concreteModel == nullptr) {
        if (!remove_container_id_fallback(module, property, edge.containerId, before)) {
            return {.error = "ConcreteModel already unloaded; precise array fallback also failed."};
        }
        return {.mutation = PersistentEdgeMutation::removed};
    }
    if (!call_concrete_model_event(module, STR("OnNotAvailableConcreteModel_ServerInternal"),
                                   concreteModel)) {
        return {.error = "Cannot call native storage ConcreteModel unregistration interface."};
    }

    std::vector<GuidKey> after;
    if (!read_module_sequence(module, property, after)) {
        return {.error = "Cannot re-read target base storage sequence after persistent union unregistration."};
    }
    const auto mutation =
        classify_persistent_edge_remove(beforeCount, count_container(after, edge.containerId));
    if (mutation != PersistentEdgeMutation::removed) {
        return {.error = "Native storage unregistration did not produce the exact expected 1->0 container change."};
    }
    std::vector<GuidKey> expected = before;
    std::erase(expected, edge.containerId);
    if (after != expected) {
        return {.mutation = PersistentEdgeMutation::removed,
                .error = "Native storage unregistration modified non-injected container sequence."};
    }
    return {.mutation = mutation};
}

auto validate_union(const LiveUnion& liveUnion, std::string& error) -> bool {
    error.clear();
    if (!liveUnion.active || !liveUnion.entry.has_value()) {
        error = "No active union ledger before material submission.";
        return false;
    }

    auto* object = find_object_by_full_name(liveUnion.entry->objectFullName);
    const auto* propertyName =
        liveUnion.entry->helperArray ? STR("Containers") : STR("ContainerInfos");
    auto* property =
        object == nullptr
            ? nullptr
            : CastField<FArrayProperty>(object->GetPropertyByNameInChain(propertyName));
    std::vector<GuidKey> current;
    const bool read = liveUnion.entry->helperArray
                          ? read_helper_sequence(object, property, current)
                          : read_module_sequence(object, property, current);
    if (!read) {
        error = "Cannot re-read active union sequence before material submission.";
        return false;
    }
    const auto validation =
        validate_applied_sequence(liveUnion.entry->original, liveUnion.entry->injected, current);
    if (!validation) {
        error =
            "Material submission union sequence validation failed: " + std::string{sequence_status_text(validation.status)};
        return false;
    }
    return true;
}


auto restore_union(LiveUnion& liveUnion, std::string& error) -> bool {
    if (!liveUnion.active) {
        error.clear();
        return true;
    }

    const bool restored = !liveUnion.entry.has_value() || restore_entry(*liveUnion.entry);
    g_liveUnionContainers.clear();
    liveUnion = {};
    if (restored) {
        error.clear();
    } else {
        error = "Could not verify the base resource union was fully restored.";
    }
    return restored;
}

[[nodiscard]] auto live_union_containers() -> std::span<UObject*> {
    return g_liveUnionContainers;
}
}  // namespace base_resource_sharing::detail

