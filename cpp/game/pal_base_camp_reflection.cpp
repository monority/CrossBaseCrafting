/**
 * @file pal_base_camp_reflection.cpp
 * @brief Implements signature-verified reflection calls for the base-camp manager
 *        and the map-object manager.
 */
#include <cstddef>

#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/Property/FArrayProperty.hpp>
#include <Unreal/Property/FObjectProperty.hpp>
#include <Unreal/Property/FBoolProperty.hpp>
#include <Unreal/Property/ScriptArrayHelper.hpp>
#include <common/game_reflection.hpp>
#include <game/pal_base_camp_reflection.hpp>

using namespace RC::Unreal;

namespace pal_base_camp_reflection {
namespace {
constexpr int32 kMaximumBaseCampCount = 1'024;
}

auto read_base_ids(UObject* manager, std::vector<FGuid>& output) -> bool {
    output.clear();
    auto* const function = pal_game::is_valid(manager)
                               ? manager->GetFunctionByNameInChain(STR("GetBaseCampIds"))
                               : nullptr;
    auto* const arrayProperty =
        function == nullptr
            ? nullptr
            : CastField<FArrayProperty>(function->FindProperty(FName(STR("OutIds"), FNAME_Find)));
    auto* const guidProperty =
        arrayProperty == nullptr ? nullptr : CastField<FStructProperty>(arrayProperty->GetInner());
    if (!pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_output_parameter(arrayProperty) || guidProperty == nullptr ||
        static_cast<std::size_t>(guidProperty->GetElementSize()) != sizeof(FGuid)) {
        return false;
    }

    pal_game::FunctionParams params{function};
    manager->ProcessEvent(function, params.data());
    FScriptArrayHelper_InContainer values(arrayProperty, params.data());
    const int32 count = values.Num();
    if (count < 0 || count > kMaximumBaseCampCount) {
        return false;
    }
    output.reserve(static_cast<std::size_t>(count));
    for (int32 index{}; index < count; ++index) {
        FGuid id{};
        guidProperty->CopyCompleteValue(&id, values.GetRawPtr(index));
        output.push_back(id);
    }
    return true;
}

auto try_get_base_model(UObject* manager, const FGuid& baseId, UObject*& model) -> bool {
    model = nullptr;
    auto* const function = pal_game::is_valid(manager)
                               ? manager->GetFunctionByNameInChain(STR("TryGetModel"))
                               : nullptr;
    auto* const idProperty =
        function == nullptr ? nullptr
                            : CastField<FStructProperty>(
                                  function->FindProperty(FName(STR("BaseCampId"), FNAME_Find)));
    auto* const modelProperty =
        function == nullptr ? nullptr
                            : CastField<FObjectPropertyBase>(
                                  function->FindProperty(FName(STR("OutModel"), FNAME_Find)));
    auto* const returnProperty =
        function == nullptr ? nullptr : CastField<FBoolProperty>(function->GetReturnProperty());
    if (!pal_game::has_exact_parameter_count(function, 3) ||
        !pal_game::is_input_parameter(idProperty) ||
        static_cast<std::size_t>(idProperty->GetElementSize()) != sizeof(FGuid) ||
        !pal_game::is_output_parameter(modelProperty) ||
        !pal_game::is_return_parameter(returnProperty)) {
        return false;
    }

    pal_game::FunctionParams params{function};
    idProperty->CopyCompleteValue(idProperty->ContainerPtrToValuePtr<void>(params.data()), &baseId);
    manager->ProcessEvent(function, params.data());
    auto* const result = modelProperty->GetObjectPropertyValue(
        modelProperty->ContainerPtrToValuePtr<void>(params.data()));
    if (returnProperty->GetPropertyValueInContainer(params.data()) && pal_game::is_valid(result)) {
        model = result;
    }
    return model != nullptr;
}

auto find_concrete_model(UObject* manager, const FGuid& instanceId, UObject*& concreteModel)
    -> bool {
    concreteModel = nullptr;
    auto* const function = pal_game::is_valid(manager)
                               ? manager->GetFunctionByNameInChain(STR("FindConcreteModel"))
                               : nullptr;
    auto* const inputProperty =
        function == nullptr ? nullptr
                            : CastField<FStructProperty>(
                                  function->FindProperty(FName(STR("InstanceId"), FNAME_Find)));
    auto* const returnProperty =
        function == nullptr ? nullptr
                            : CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (!pal_game::has_exact_parameter_count(function, 2) ||
        !pal_game::is_input_parameter(inputProperty) ||
        static_cast<std::size_t>(inputProperty->GetElementSize()) != sizeof(FGuid) ||
        !pal_game::is_return_parameter(returnProperty)) {
        return false;
    }

    pal_game::FunctionParams params{function};
    inputProperty->CopyCompleteValue(inputProperty->ContainerPtrToValuePtr<void>(params.data()),
                                     &instanceId);
    manager->ProcessEvent(function, params.data());
    auto* const result = returnProperty->GetObjectPropertyValue(
        returnProperty->ContainerPtrToValuePtr<void>(params.data()));
    concreteModel = pal_game::is_valid(result) ? result : nullptr;
    return concreteModel != nullptr;
}
}  // namespace pal_base_camp_reflection
