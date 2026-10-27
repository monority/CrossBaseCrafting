/**
 * @file game_reflection.hpp
 * @brief Cross-feature UE4SS reflection primitives: UObject validity checks and zero-parameter reflection calls.
 * @details These primitives are shared by backpack/item, skill, property, shape, and other feature modules.
 *          All interfaces must be called from the game thread after Unreal has finished initialising.
 *          Returned Unreal raw pointers are non-owning observation pointers and do not extend object lifetimes.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <Unreal/UClass.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/Property/FArrayProperty.hpp>
#include <Unreal/Property/FBoolProperty.hpp>
#include <Unreal/Property/FObjectProperty.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/Property/NumericPropertyTypes.hpp>
#include <Unreal/Property/ScriptArrayHelper.hpp>

#include <common/reflection_cache.hpp>

namespace pal_game {
/** @brief Unreal class name for the main player backpack data object. */
inline constexpr const TCHAR* kInventoryClassName = STR("PalPlayerInventoryData");

/**
 * @brief Performs a lightweight validity check on a UObject observation pointer.
 * @param[in] obj Non-owning UObject pointer to check.
 * @retval true Pointer is non-null and class metadata is still accessible.
 * @retval false Pointer is null or the object class metadata has become invalid.
 * @warning This check does not extend the object lifetime and cannot guarantee validity in subsequent frames.
 */
inline auto is_valid(RC::Unreal::UObject* obj) -> bool {
    return obj != nullptr && obj->GetClassPrivate() != nullptr;
}

/**
 * @brief RAII wrapper around a UFunction parameter buffer (InitializeStruct on construction, DestroyStruct on destruction).
 * @details No-op when function is null; memory returned by data() is only valid for the lifetime of this object.
 */
class FunctionParams final {
public:
    explicit FunctionParams(RC::Unreal::UFunction* function)
        : function_{function},
          storage_(function == nullptr ? 0U : static_cast<std::size_t>(function->GetParmsSize())) {
        if (function_ != nullptr) {
            function_->InitializeStruct(storage_.data());
        }
    }

    FunctionParams(const FunctionParams&) = delete;
    auto operator=(const FunctionParams&) -> FunctionParams& = delete;
    FunctionParams(FunctionParams&&) = delete;
    auto operator=(FunctionParams&&) -> FunctionParams& = delete;

    ~FunctionParams() {
        if (function_ != nullptr) {
            function_->DestroyStruct(storage_.data());
        }
    }

    [[nodiscard]] auto data() noexcept -> void* {
        return storage_.data();
    }

private:
    RC::Unreal::UFunction* function_{};
    std::vector<std::byte> storage_;
};

/** @brief Checks whether the total CPF_Parm property count of a UFunction exactly matches the expected count. */
[[nodiscard]] inline auto has_exact_parameter_count(RC::Unreal::UFunction* function,
                                                    const std::size_t expected) -> bool {
    if (function == nullptr) {
        return false;
    }
    std::size_t count{};
    for (auto* property : RC::Unreal::TFieldRange<RC::Unreal::FProperty>(
             function, RC::Unreal::EFieldIterationFlags::IncludeDeprecated)) {
        if (property->HasAnyPropertyFlags(RC::Unreal::CPF_Parm)) {
            ++count;
        }
    }
    return count == expected;
}

/**
 * @brief Exact-parameter-count check that memoises the count in the reflection cache.
 * @details Same semantics as the non-cached overload, but stores the computed count per UFunction
 *          (immutable within a world) so repeated hot-path checks skip the property iteration.
 *          Falls through to the plain computation on a cache miss.
 */
[[nodiscard]] inline auto has_exact_parameter_count(RC::Unreal::UFunction* function,
                                                    const std::size_t expected,
                                                    ReflectionCache& cache) -> bool {
    if (function == nullptr) {
        return false;
    }
    // try_emplace yields {iterator, insertedFlag}; write the computed count through the iterator.
    auto [slot, inserted] = cache.paramCountByFunction.try_emplace(function, std::size_t{0});
    if (inserted) {
        std::size_t count{};
        for (auto* property : RC::Unreal::TFieldRange<RC::Unreal::FProperty>(
                 function, RC::Unreal::EFieldIterationFlags::IncludeDeprecated)) {
            if (property->HasAnyPropertyFlags(RC::Unreal::CPF_Parm)) {
                ++count;
            }
        }
        slot->second = count;
    }
    return slot->second == expected;
}

/**
 * @brief Determines whether a property is an input parameter.
 * @details Unreal may set CPF_ConstParm, CPF_ReferenceParm, and CPF_OutParm for a const-ref parameter.
 *          Such parameters are semantically read-only input and must not be rejected because of the OutParm flag.
 */
[[nodiscard]] inline auto is_input_parameter(RC::Unreal::FProperty* property) -> bool {
    using namespace RC::Unreal;
    return property != nullptr && property->HasAnyPropertyFlags(CPF_Parm) &&
           !property->HasAnyPropertyFlags(CPF_ReturnParm) &&
           (!property->HasAnyPropertyFlags(CPF_OutParm) ||
            property->HasAnyPropertyFlags(CPF_ConstParm));
}

/** @brief Determines whether a property is a non-return output parameter. */
[[nodiscard]] inline auto is_output_parameter(RC::Unreal::FProperty* property) -> bool {
    using namespace RC::Unreal;
    return property != nullptr && property->HasAnyPropertyFlags(CPF_Parm) &&
           property->HasAnyPropertyFlags(CPF_OutParm) &&
           !property->HasAnyPropertyFlags(CPF_ReturnParm);
}

/** @brief Determines whether a property is the function return value. */
[[nodiscard]] inline auto is_return_parameter(RC::Unreal::FProperty* property) -> bool {
    using namespace RC::Unreal;
    return property != nullptr && property->HasAnyPropertyFlags(CPF_Parm) &&
           property->HasAnyPropertyFlags(CPF_ReturnParm);
}

namespace detail {
/** @brief Checks whether an integer C++ type precisely corresponds to a specific Unreal numeric property type. */
template <typename T>
[[nodiscard]] inline auto matches_integral_property(RC::Unreal::FProperty* property) -> bool {
    using namespace RC::Unreal;
    using Value = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<Value, std::int8_t>) {
        return CastField<FInt8Property>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::uint8_t>) {
        return CastField<FByteProperty>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::int16_t>) {
        return CastField<FInt16Property>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::uint16_t>) {
        return CastField<FUInt16Property>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::int32_t>) {
        return CastField<FIntProperty>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::uint32_t>) {
        return CastField<FUInt32Property>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::int64_t>) {
        return CastField<FInt64Property>(property) != nullptr;
    } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
        return CastField<FUInt64Property>(property) != nullptr;
    } else {
        return false;
    }
}
}  // namespace detail

/**
 * @brief Calls a zero-input-parameter function on an object actual class chain and returns a value of the specified type.
 * @tparam T Supports UObject*, bool, signed/unsigned integers, and FName.
 * @return Call result when the signature matches exactly; std::nullopt when the object or metadata is unavailable.
 */
template <typename T>
[[nodiscard]] inline auto invoke(RC::Unreal::UObject* object, const TCHAR* functionName)
    -> std::optional<T> {
    using namespace RC::Unreal;
    if (!is_valid(object)) {
        return std::nullopt;
    }
    auto* const function = object->GetFunctionByNameInChain(functionName);
    auto* const returnProperty = function == nullptr ? nullptr : function->GetReturnProperty();
    if (!has_exact_parameter_count(function, 1) || !is_return_parameter(returnProperty)) {
        return std::nullopt;
    }

    if constexpr (std::is_same_v<T, UObject*>) {
        auto* const property = CastField<FObjectPropertyBase>(returnProperty);
        if (property == nullptr) {
            return std::nullopt;
        }
        FunctionParams params{function};
        object->ProcessEvent(function, params.data());
        auto* const result =
            property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(params.data()));
        return is_valid(result) ? std::optional<T>{result} : std::nullopt;
    } else if constexpr (std::is_same_v<T, bool>) {
        auto* const property = CastField<FBoolProperty>(returnProperty);
        if (property == nullptr) {
            return std::nullopt;
        }
        FunctionParams params{function};
        object->ProcessEvent(function, params.data());
        return property->GetPropertyValueInContainer(params.data());
    } else if constexpr (std::is_integral_v<T>) {
        auto* const property = CastField<FNumericProperty>(returnProperty);
        if (property == nullptr || !detail::matches_integral_property<T>(property)) {
            return std::nullopt;
        }
        FunctionParams params{function};
        object->ProcessEvent(function, params.data());
        auto* const value = property->ContainerPtrToValuePtr<void>(params.data());
        if constexpr (std::is_signed_v<T>) {
            return static_cast<T>(property->GetSignedIntPropertyValue(value));
        } else {
            return static_cast<T>(property->GetUnsignedIntPropertyValue(value));
        }
    } else if constexpr (std::is_same_v<T, FName>) {
        auto* const property = CastField<FNameProperty>(returnProperty);
        if (property == nullptr) {
            return std::nullopt;
        }
        FunctionParams params{function};
        object->ProcessEvent(function, params.data());
        return property->GetPropertyValueInContainer(params.data());
    } else {
        static_assert(!sizeof(T), "pal_game::invoke does not support this return type");
    }
}

/** @brief Recovers a UObject from its full object name; only passes the suffix path when the class prefix is present. */
[[nodiscard]] inline auto find_object_by_full_name(const std::wstring& fullName)
    -> RC::Unreal::UObject* {
    if (fullName.empty()) {
        return nullptr;
    }
    const auto separator = fullName.find(L' ');
    const auto objectPath =
        separator == std::wstring::npos ? fullName : fullName.substr(separator + 1);
    return RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(nullptr, nullptr,
                                                                              objectPath.c_str());
}
}  // namespace pal_game

