# CrossBaseCrafting — Refactor / Performance Implementation Plan

> **REQUIRED SUB-SKILL:** Use the executing-plans skill to implement this plan task-by-task.

**Goal:** Réduire les lignes de code, découper la god-class `Impl`, et accélérer les chemins réflexes (scan catalog + présence) — **sans casser le mod**, chaque étape vérifiée avant la suivante.

**Architecture:** Mod C++ UE4SS (PIMPL : `PalBaseResourceBridge` public, `Impl` privé). Deux moteurs : (1) *union persistante* = registre d'arêtes cross-base dans les modules de stockage ; (2) *union live* = injection des coffres étrangers dans le helper joueur pour le build/craft. La couche réflexe (`ProcessEvent` + `FProperty`) est le goulot d'étranglement → on l'introduit via un cache invalidé par génération de monde.

**Tech Stack:** C++23, UE4SS (RE-UE4SS, UEPseudo), MSVC 2022 BuildTools, CMake + Ninja. Build : `build-v204.bat`. Aucun framework de test — voir adaptation ci-dessous.

**Base :** v2.0.11 installée. Cible finale : **v2.1.0** (refactor) puis **v3.0.0** (tout vert).

---

## Adaptation « pas de harness de test »

Ce projet est un mod injecté dans un binaire de jeu fermé : pas de `pytest`, pas d'assertions unitaires exécutables en CI. Chaque tâche se termine par un **GATE** qui remplace le cycle rouge/vert :

1. **GATE BUILD** (automatisé) : `build-v204.bat` → `BUILD OK` ; `dumpbin /exports` montre `start_mod` + `uninstall_mod` ; bump de version ; copie vers `dlls/main.dll`.
2. **GATE IN-GAME** (manuel, user) : la « Smoke Test » ci-dessous. Exigé **après chaque phase**, et après toute tâche marquée 🔴 (risque moyen/élevé). Les tâches 🟢 (mécaniques, sûres) n'exigent que le GATE BUILD.

### Smoke Test in-game (définition de « le mod fonctionne toujours »)

À lancer depuis le dossier jeu, puis vérifier dans cet ordre :

1. `UE4SS.log` contient : `[CrossBaseCrafting] C++ mod loaded`, `runtime enabled`, `hooks actifs: 5/7`, `[CBC] material-count detour installed`, et une ligne `persistent storage graph prepared ... bases=N` (N ≥ 2 si 2 bases).
2. Marcher **dans** une base → dans le menu build, les matériaux d'un coffre d'**une autre base** sont comptés.
3. Placer 1 bâtiment consommant des matériaux cross-base → le build réussit (pas de `FailedNotEnoughMaterials`).
4. **Sortir** de la base, puis mourir → le cadavre ne contient que l'inventaire du joueur (pas le contenu des bases).
5. Aucun crash sur ~3 min, pas de mini-stutter en marchant près d'une base.

> Points 3+4 = régression des bugs déjà corrigés (corpse contamination v2.0.4, crash menu build v2.0.8). Si ils reparaissent → **STOP**, on analyse.

### Boutons d'urgence (bisect, déjà en place v2.0.11)

Fichiers à créer **dans `Palworld\Binaries\Win64\`** (à côté de `Palworld-Win64-Shipping.exe`) :

- `crossbase_no_detour.txt` → coupe le detour matières.
- `crossbase_no_live_union.txt` → coupe toute injection (quasi vanilla).

À garder sous la main : si un GATE IN-GAME échoue, ces fichiers disent si la faute vient du detour ou de l'injection.

### Commande de build/vérification réutilisable (tout au long du plan)

```bash
export MSYS_NO_PATHCONV=1
rm -f build-v204/CrossBaseCrafting.pdb
cmd.exe /c build-v204.bat > build-v204.log 2>&1; echo "exit=$?"
rg -i "error C| LNK|FAILED|BUILD OK" build-v204.log | head -8   # attendu: BUILD OK, exit=0
DUMP="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/dumpbin.exe"
"$DUMP" /exports build-v204/CrossBaseCrafting.dll | rg "start_mod|uninstall_mod"   # attendu: 2 lignes
cp build-v204/CrossBaseCrafting.dll dlls/main.dll && echo INSTALLED
node -e "const b=require('fs').readFileSync('dlls/main.dll').toString('utf16le');const m=b.match(/2\.0\.\d+-cpp|3\.0\.\d+-cpp/g);console.log('installed:',m?[...new Set(m)].join(','):NaN)"
rm -f build-v204.log
```

**Annexe versions :** `cpp/dllmain.cpp` (`ModVersion = STR("X.Y.Z-cpp")`) **et** `Info.json` (`"Version": "X.Y.Z-cpp"`) doivent toujours matcher.

---

## Vue d'ensemble & ordre

| Phase | Objectif | Risk | LOC nette | Gate |
|---|---|---|---|---|
| **0** | Filet de sécurité + baseline | 🟢 | 0 | BUILD |
| **A** | Code mort + dédup (réduction pure) | 🟢 | **−350 ≈** | BUILD |
| **B** | ReflectionCache (perf P0) | 🔴 | +120 (cache) −60 (dédup) | BUILD + IN-GAME |
| **C** | Split `Impl` en 4 (maintenabilité) | 🔴 | 0 (découpage) | BUILD + IN-GAME |
| **D** | Polish perf (SBO, TTL, déplacement headers) | 🟡 | −10 ≈ | BUILD + IN-GAME |

**Honnêteté LOC :** la réduction *nette* vient de A (code mort + templates). B ajoute une couche cache mais supprime de la réflexion dispersée (net ≈ neutre). C ne réduit pas les lignes : il **découpe** 1 392 lignes en 4 fichiers cohésifs (gain maintenabilité, LOC identique). Objectif final realiste : **≈ −300 lignes** + chemins chauds ~2-3× plus rapides + code découpé.

**Règle d'or :** une tâche à la fois. GATE vert → commit → tâche suivante. GATE rouge → on ne passe pas, on diagnostique (les flags de bisect + `kDiagnostics=true` temporaire sont nos amis).

---

# Phase 0 — Filet de sécurité & baseline

### Task 0 : Baseline & ardoise

**Files:** aucun code. Crée `docs/plans/baseline-LOC.txt` (optionnel).

**Step 1 :** Confirmer v2.0.11 installée et verte.

```bash
node -e "const b=require('fs').readFileSync('dlls/main.dll').toString('utf16le');const m=b.match(/2\.\d+\.\d+-cpp/g);console.log(m?[...new Set(m)].join(','):NaN)"   # attendu: 2.0.11-cpp
```

**Step 2 :** Lancer le jeu, exécuter la **Smoke Test**. Si vert → baseline établie. Noter les compteurs LOC :

```bash
wc -l cpp/*.cpp cpp/*/*.cpp old/cpp/inc/*/*.hpp
```

**Step 3 :** Pas de commit (baseline only). → **GATE BUILD + IN-GAME** ( Smoke Test verte sur v2.0.11 ).

---

# Phase A — Code mort & déduplication (réduction LOC pure, 🟢)

Objectif immédiat : descendre les lignes sans toucher au comportement. Aucune tâche ne change l'algorithme.

### Task A1 : Supprimer `resource_session.hpp` (−285 lignes)

**Preuve que c'est mort :** `rg -n "resource_session|classify_catalog_attempt|CatalogReconcileOutcome" cpp/ old/cpp/inc/` ne retourne que des lignes **dans** `resource_session.hpp` lui-même. Aucun `.cpp` ne l'inclut.

**Files:**
- Delete: `old/cpp/inc/base_resource_sharing/resource_session.hpp`

**Step 1 :** Re-confirmer que rien ne l'inclut.

```bash
rg -l "resource_session" cpp/ ; echo "exit=$?"   # attendu: vide, exit=1
```

**Step 2 :** Supprimer le fichier.

**Step 3 :** GATE BUILD. → `BUILD OK`, exports OK. Aucun changement de version nécessaire (code mort), mais on bump quand même pour tracer : `2.0.12-cpp` dans `cpp/dllmain.cpp` + `Info.json`. **GATE BUILD.**

### Task A2 : Dédupliquer la couche réflexe utilitaire (−60 ≈ lignes)

**Problème :** trois copies quasi identiques de « résolve une UFunction sur PalUtility, vérifie la signature, ProcessEvent » :
- `call_utility_bool` (runtime.cpp:77) — retour bool
- `call_utility_object` (runtime.cpp:101) — retour UObject*
- `try_get_player_guild` (runtime.cpp:161) — 3 params (WorldContext + PlayerUId + retour objet)

Seul le nombre/type de params et le type de retour changent. On fusionne en **un template** `call_pal_utility` + wrappers fins qui gardent la signature actuelle (donc zéro changement aux call-sites).

**Files:**
- Modify: `cpp/base_resource_sharing/pal_base_resource_runtime.cpp` (remplace les 3 fonctions par template + wrappers)

**Step 1 :** Écrire le template dans l'`anonymous namespace` du runtime (avant les wrappers), qui factorise la validation + `FunctionParams` + `ProcessEvent` :

```cpp
// Résout une fonction sur le CDO PalUtility, valide les paramètres, appelle, lit le retour.
// Retour : false si l'objet/fonction/signature est indisponible (fail-closed, comportement inchangé).
template <typename ReturnT>
[[nodiscard]] auto call_pal_utility(UObject* worldContext, const CharType* functionName,
                                    ReturnT& value) -> bool {
    value = nullptr;
    auto* utility = pal_utility();
    auto* function = utility == nullptr ? nullptr : utility->GetFunctionByNameInChain(functionName);
    auto* const context = function == nullptr ? nullptr
                       : CastField<FObjectPropertyBase>(function->FindProperty(
                             FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const result = function == nullptr ? nullptr
                       : CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (!pal_game::is_valid(utility) || !pal_game::is_valid(worldContext) ||
        !pal_game::has_exact_parameter_count(function, 2) ||
        !pal_game::is_input_parameter(context) || !pal_game::is_return_parameter(result)) {
        return false;
    }
    pal_game::FunctionParams params{function};
    context->SetObjectPropertyValue(context->ContainerPtrToValuePtr<void>(params.data()),
                                    worldContext);
    utility->ProcessEvent(function, params.data());
    if constexpr (std::is_same_v<ReturnT, bool>) {
        value = CastField<FBoolProperty>(result)->GetPropertyValueInContainer(params.data());
        return true;
    } else {
        value = result->GetObjectPropertyValue(result->ContainerPtrToValuePtr<void>(params.data()));
        return pal_game::is_valid(value);
    }
}
```

**Step 2 :** Réécrire les wrappers pour déléguer (corps de 4 lignes chacun, signatures identiques) :

```cpp
[[nodiscard]] auto call_utility_bool(UObject* worldContext, const CharType* functionName,
                                     bool& value) -> bool {
    return call_pal_utility<bool>(worldContext, functionName, value);
}
[[nodiscard]] auto call_utility_object(UObject* worldContext, const CharType* functionName,
                                       UObject*& value) -> bool {
    return call_pal_utility<UObject*>(worldContext, functionName, value);
}
```

`try_get_player_guild` garde ses 3 params mais réutilise `pal_utility()` + la même garde — on ne le force pas dans le template (signature différente, YAGNI).

**Step 3 :** GATE BUILD. → `BUILD OK`. Bump `2.0.13-cpp`. **GATE BUILD**, puis **GATE IN-GAME (Smoke Test)** — première tâche qui touche du code vivant de la couche réflexe.

> ⚠️ Note : `call_pal_utility<bool>` cast `result` en `FBoolProperty` — l'original utilisait `FBoolProperty` pour `call_utility_bool` et `FObjectPropertyBase` pour `call_utility_object`. Le `if constexpr` préserve exactement ces deux casts. Ne pas « simplifier » en un seul cast.

### Task A3 : Centraliser les constantes de temps (lisibilité)

**Problème :** `500ms`, `2s`, `5s`, `10s` en littéraux éparpillés dans `maintain_live_union`, `tick`, `ensure_hooks_registered`, debounce. Un changement de cadence = chasse à la ligne.

**Files:**
- Create: `old/cpp/inc/common/timing.hpp`
- Modify: `cpp/base_resource_sharing/pal_base_resources.cpp` (remplace les littéraux)

**Step 1 :** Créer le header :

```cpp
#pragma once
#include <chrono>
namespace base_resource_sharing {
/** @brief Cadences du mod — un seul endroit à ajuster. */
inline constexpr auto kPresenceInterval = std::chrono::milliseconds{500};   // vérif présence hors base
inline constexpr auto kUnionMaintenanceInterval = std::chrono::milliseconds{500};
inline constexpr auto kUnionCompletenessInterval = std::chrono::seconds{10};
inline constexpr auto kReconcileDebounce = std::chrono::seconds{2};
inline constexpr auto kHookRetryInterval = std::chrono::seconds{5};
}  // namespace base_resource_sharing
```

**Step 2 :** `#include <common/timing.hpp>` dans `pal_base_resources.cpp`, remplacer chaque littéral par la constante. (`kReconcileDebounce` existe déjà dans l'anonymous namespace → supprimer la locale.)

**Step 3 :** GATE BUILD. Bump `2.0.14-cpp`. **GATE BUILD.**

### Task A4 : Dédupliquer les deux `anonymous namespace` du runtime (propreté)

**Problème :** `pal_base_resource_runtime.cpp` a **deux** `namespace { ... }` (≈ ligne 45 et ≈ ligne 900) séparés par des fonctions exportées. Confusant.

**Files:** Modify: `cpp/base_resource_sharing/pal_base_resource_runtime.cpp`

**Step 1 :** Déplacer les helpers privés du 2ᵉ bloc (les `persistent_edge_*`, `module_property`, `count_container`, `call_concrete_model_event`, `remove_container_id_fallback`) dans le 1ᵉʳ `anonymous namespace`, ou — plus propre — les laisser dans un `namespace { }` mais **renommé et documenté** `namespace persistent_detail { }`. Choix : garder `anonymous namespace` unique en tête de fichier. Purement cosmétique, comportement identique (tous `static`/internal).

**Step 2 :** GATE BUILD. Bump `2.0.15-cpp`. **GATE BUILD.** Pas de test in-game obligatoire (cosmétique), mais on l'inclut dans la Phase A finale.

→ **FIN PHASE A : GATE IN-GAME (Smoke Test) sur `2.0.15-cpp`.** Attendu : LOC ≈ −350, tout le reste identique.

---

# Phase B — ReflectionCache (perf P0, 🔴)

**Le gain :** le scan catalog coûte ~15 ms parce qu'il re-résout à chaque fois : `PalUtility` CDO (`StaticFindObject`), ~8 `UFunction*`, ~40 `FProperty*` (chaînes `FName`), la classe storage, plus `GetFullName()`→`wstring` par container. La cache invalide par **génération de monde** → rien ne devient périmé entre les scans. Cible : scan ~15 ms → ~6 ms ; chaîne présence (2×/s) ~×3 plus légère.

**Principe de sûreté :** on ne cache que des **pointeurs de métadonnées** (UClass*, UFunction*, FProperty*), jamais des instances de gameplay. Les UFunction/UProperty sont stables dans la vie d'un monde. Invalidation = `on_world_begin` (nouvelle génération) → on vide. Si un pointeur cache est `nullptr` au lookup → on retombe sur la résolution par chaîne (le comportement actuel) → fail-safe.

### Task B1 : Créer le `ReflectionCache`

**Files:**
- Create: `old/cpp/inc/common/reflection_cache.hpp`
- Modify: `cpp/base_resource_sharing/pal_base_resource_runtime.cpp` (l'utiliser)
- Modify: `cpp/base_resource_sharing/pal_base_resources.cpp` (l'invalider sur `on_world_begin`)

**Step 1 :** Header du cache. Clé = `UFunction*` pour les propriétés de params (hash map), plus quelques pointeurs nommés :

```cpp
#pragma once
#include <cstdint>
#include <unordered_map>
#include <Unreal/UFunction.hpp>
#include <Unreal/FProperty.hpp>
namespace RC::Unreal { class UObject; class UClass; }
namespace pal_game {
/**
 * @brief Cache de métadonnées réflexe, invalidé par génération de monde.
 * @details Ne stocke QUE des pointeurs de métadonnées ( UClass*/UFunction*/FProperty* ), jamais
 *          des instances de gameplay. Toute valeur absente = résolution par chaîne (comportement
 *          de référence). Toute valeur périmée est purgée via invalidate() à chaque changement de monde.
 */
struct ReflectionCache {
    RC::Unreal::UObject* palUtility{};                 // CDO /Script/Pal.Default__PalUtility
    RC::Unreal::UClass* storageClass{};                // PalBaseCampModuleItemStorage
    // param property par UFunction* (WorldContextObject / InstanceId / ConcreteModel / ...)
    std::unordered_map<RC::Unreal::UFunction*, RC::Unreal::FProperty*> paramByFunction;
    // count "exact param" par UFunction* (immuable dans la vie d'un monde)
    std::unordered_map<RC::Unreal::UFunction*, std::size_t> paramCountByFunction;

    auto invalidate() noexcept -> void {
        palUtility = nullptr;
        storageClass = nullptr;
        paramByFunction.clear();
        paramCountByFunction.clear();
    }
};
/** @brief Instance process-wide (jeu mono-thread sur le game thread). */
ReflectionCache& reflection_cache();
}  // namespace pal_game
```

**Step 2 :** Implémenter `reflection_cache()` (static local) dans le runtime.cpp. **Ne pas encore** rebrancher les call-sites.

**Step 3 :** Brancher `on_world_begin` (Impl, `pal_base_resources.cpp:~260`) pour appeler `pal_game::reflection_cache().invalidate()` au début du reset. Inclure le header.

**Step 4 :** GATE BUILD. Bump `2.1.0-cpp` (début de la grande refonte). **GATE BUILD + IN-GAME** — le cache existe mais n'est encore lu par personne : comportement strictement identique. C'est le commit « infrastructure sûre ».

### Task B2 : Cacher `pal_utility()` + `has_exact_parameter_count`

**Files:** Modify: `cpp/base_resource_sharing/pal_base_resource_runtime.cpp`, `old/cpp/inc/common/game_reflection.hpp`

**Step 1 :** `pal_utility()` (runtime.cpp:69) devient :

```cpp
[[nodiscard]] auto pal_utility() -> UObject* {
    auto& cache = pal_game::reflection_cache();
    if (pal_game::is_valid(cache.palUtility)) {
        return cache.palUtility;
    }
    cache.palUtility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    return cache.palUtility;
}
```

**Step 2 :** `has_exact_parameter_count` (`game_reflection.hpp`) — version cached : surcharge qui prend un `ReflectionCache&` et consulte `paramCountByFunction`. Garder l'ancienne (non-cachée) pour les sites froids. On ne rebranche que les sites chauds (Task B3).

**Step 3 :** GATE BUILD. Bump `2.1.1-cpp`. **GATE BUILD + IN-GAME.**

### Task B3 : Rebrancher les helpers chauds + `pal_base_camp_reflection`

**Files:** Modify: `cpp/base_resource_sharing/pal_base_resource_runtime.cpp`, `cpp/game/pal_base_camp_reflection.cpp`

**Step 1 :** Pour chaque helper chaud, remplacer `function->FindProperty(FName(STR("WorldContextObject"), FNAME_Find))` par un lookup cache : une petite fonction `cached_param(cache, function, name)` qui consulte `paramByFunction` et, sur miss, résout par chaîne puis stocke.

**Step 2 :** `pal_base_camp_reflection.cpp` — les 3 fonctions (`read_base_ids`, `try_get_base_model`, `find_concrete_model`) reçoivent le cache et utilisent `cached_param` + `has_exact_parameter_count` cached. **C'est le gros gain** : `find_concrete_model` est appelé ~6×/scan, chacune re-résolvait `FindConcreteModel` + 2 params.

**Step 3 :** `is_shareable_concrete` (runtime.cpp:59) : éviter `GetFullName()`→`wstring`→`find` par container. Alternative sûre : comparer le **nom de classe** (`GetClassPrivate()->GetName()`) plutôt que le nom d'objet complet (qui inclut le suffixe d'instance). Garde la sémantique « contient FoodBox/ItemChest/GuildChest » mais sans allouer la chaîne d'objet complète. Si le nom de classe est ambigu, **on garde l'ancien** (ne pas risquer d'exclure un coffre).

> 🔴 Ce sous-pas touche la sélection des coffres partageables → **GATE IN-GAME stricte** (Smoke Test point 2 : les coffres cross-base doivent toujours apparaître).

**Step 4 :** GATE BUILD. Bump `2.1.2-cpp`. **GATE BUILD + IN-GAME (point 2 accentué).**

### Task B4 : Pré-calculer la param property dans `HookBinding`

**Problème :** `concrete_model_membership` (pal_base_resources.cpp:1017) fait `function->FindProperty(FName(STR("ConcreteModel")))` **à chaque event** structurel (jusqu'à 437/3 min). La propriété est immuable par fonction → on la calcule **une fois** à l'enregistrement.

**Files:** Modify: `cpp/base_resource_sharing/pal_base_resources.cpp`

**Step 1 :** Étendre `HookBinding` (ligne 230) :

```cpp
struct HookBinding {
    UFunction* function{};
    HookSpec spec;
    FObjectPropertyBase* concreteModelParam{};   // nullptr si la fonction n'en a pas
};
```

**Step 2 :** Dans la boucle d'enregistrement (ligne ~490, après `register_hook(...)`), peupler `concreteModelParam` en une fois. Dans `concrete_model_membership`, utiliser `hook.concreteModelParam` au lieu de `FindProperty` — il faut passer le binding ou faire un petit lookup `hooks_` par `function`. Simple : linear-scan `hooks_` (≤7 éléments) pour retrouver le binding par `function`.

**Step 3 :** GATE BUILD. Bump `2.1.3-cpp`. **GATE BUILD + IN-GAME.**

### Task B5 : Cacher le chemin présence (chaîne `resolve_inside_base_id`)

**Fichier :** `cpp/base_resource_sharing/pal_base_resource_runtime.cpp` (`resolve_inside_base_id`, ligne 510).

Avec B2/B3 en place, la chaîne présence (2×/s) appelle : `call_utility_object(GetLocalPalPlayerController)` + `try_get_object(K2_GetPawn)` + `read_object_property(InsideBaseCampCheckComponent)` + `try_get_object(GetInsideBaseCampModel)` + `read_base_id(GetId)`. Chaque `try_get_object` = `pal_game::invoke` = `GetFunctionByNameInChain` + `has_exact_parameter_count` + `FunctionParams` + `ProcessEvent`.

**Step 1 :** S'assurer que chaque `try_get_*`/`invoke` passe par le cache (B2/B3 le font). Mesurer : ajouter **temporairement** un log Verbose timestampé autour de `resolve_inside_base_id` pour comparer avant/après (ou s'appuyer sur le timing `lastCatalogMilliseconds` déjà publié dans le snapshot).

**Step 2 :** GATE BUILD. Bump `2.1.4-cpp`. **GATE BUILD + IN-GAME.**

→ **FIN PHASE B : GATE IN-GAME (Smoke Test) sur `2.1.4-cpp`.** Comparer les FPS en mode build / en marchant près d'une base vs v2.0.11. Attendu : scan plus court, présence plus légère, aucun changement fonctionnel.

---

# Phase C — Split `Impl` (maintenabilité, 🔴 mécanique)

**But :** `pal_base_resources.cpp` = 1 392 lignes, `Impl` = god-class (~50 méthodes, 30+ membres). Découper en 4 cohésifs. **L'API publique `PalBaseResourceBridge` (`pal_base_resources.hpp`) ne change PAS** (PIMPL) → risque contenu. On ne **déplace** que des définitions de méthodes `Impl` vers de nouveaux `.cpp`, en gardant `class Impl` déclarée dans un en-tête partagé.

**Files:**
- Create: `old/cpp/inc/base_resource_sharing/impl.hpp` (déclaration de `class PalBaseResourceBridge::Impl` + ses membres)
- Create: `cpp/base_resource_sharing/lifecycle.cpp` (on_world_*, tick, set_enabled, shutdown)
- Create: `cpp/base_resource_sharing/live_union.cpp` (maintain/apply/restore/validate live)
- Create: `cpp/base_resource_sharing/persistent.cpp` (prepare/process/complete work, reconcile)
- Create: `cpp/base_resource_sharing/hooks.cpp` (dispatch, membership, registration)
- Modify: `CMakeLists.txt` (ajouter les 4 .cpp)
- Modify: `cpp/base_resource_sharing/pal_base_resources.cpp` (réduit aux constructeurs/destructeurs PIMPL)

**Étapes (UNE tâche Cx à la fois, GATE BUILD après chacune) :**

- **C1** : Extraire `impl.hpp` (juste la déclaration `class Impl` + membres, `#pragma once`, inclut les bons headers). `pal_base_resources.cpp` inclut `impl.hpp`. GATE BUILD. Bump `2.2.0-cpp`.
- **C2** : Déplacer `lifecycle.cpp`. Ajouter au CMake. GATE BUILD. Bump `2.2.1-cpp`. **IN-GAME.**
- **C3** : Déplacer `live_union.cpp`. GATE BUILD. Bump `2.2.2-cpp`. **IN-GAME.**
- **C4** : Déplacer `persistent.cpp`. GATE BUILD. Bump `2.2.3-cpp`.
- **C5** : Déplacer `hooks.cpp`. GATE BUILD. Bump `2.2.4-cpp`.

**Règle C :** chaque déplacement est **pure mécanique** (couper-coller de définitions de méthodes, ajouter l'include, ajouter au CMake). Aucune logique modifiée. Si le build casse sur un include manquant → on l'ajoute, on ne réécrit pas.

CMake à jour (Step C2) :

```cmake
add_library(CrossBaseCrafting SHARED
    cpp/dllmain.cpp
    cpp/common/function_hook_registry.cpp
    cpp/game/pal_base_camp_reflection.cpp
    cpp/base_resource_sharing/pal_base_resource_runtime.cpp
    cpp/base_resource_sharing/pal_base_resources.cpp
    cpp/base_resource_sharing/lifecycle.cpp      # C2
    cpp/base_resource_sharing/live_union.cpp     # C3
    cpp/base_resource_sharing/persistent.cpp     # C4
    cpp/base_resource_sharing/hooks.cpp          # C5
)
```

→ **FIN PHASE C : GATE IN-GAME (Smoke Test) sur `2.2.4-cpp`.** `pal_base_resources.cpp` cible ≈ 120 lignes (constructeurs PIMPL + rien d'autre).

---

# Phase D — Polish perf (🟡)

### Task D1 : `FunctionParams` → stack buffer (−allocs)

**Fichier :** `old/cpp/inc/common/game_reflection.hpp` (classe `FunctionParams`, ligne ~45).

**Problème :** `std::vector<std::byte> storage_` = 1 alloc heap **par appel réflexe** (présentence 2×/s → 6 allocs ; scan → ~25). Le param buffer d'UFunction est petit (< 512 B pour nos fonctions).

**Step 1 :** Remplacer `vector` par `alignas(16) std::byte stack_[512]` + fallback heap `unique_ptr` si `function->GetPropertiesSize() > 512`. Initialisation/Destruction struct identiques.

**Step 2 :** GATE BUILD. Bump `2.3.0-cpp`. 🔴 **IN-GAME** — touche TOUT appel réflexe. Si crash → revert cette tâche seule.

### Task D2 : TTL sur `env_flag`

**Fichier :** `cpp/base_resource_sharing/pal_base_resources.cpp` (`env_flag`, ligne ~68).

**Step 1 :** Mettre en cache le résultat de `GetFileAttributesW` par nom avec un TTL de 1 s (steady_clock) — évite un appel système par probe. Le nom est constant par call-site → cache par `std::string_view`. GATE BUILD. Bump `2.3.1-cpp`. 🟢 BUILD (mineur).

### Task D3 : Déplacer les headers `old/cpp/inc` → `cpp/inc`

**But cosmétique :** « old/ » contient les headers *vivants*, ce qui trompe.

**Files:** déplacer `old/cpp/inc/*` → `cpp/inc/*`, mettre à jour `CMakeLists.txt` `target_include_directories` (`cpp/inc`), et tout `#include <...>` reste inchangé (chemins relatifs au include-dir).

**Step 1 :** `git mv old/cpp/inc cpp/inc`. **Step 2 :** CMakeLists : `target_include_directories(... "${CMAKE_CURRENT_SOURCE_DIR}/cpp/inc")`. **Step 3 :** GATE BUILD. Bump `2.3.2-cpp`. 🟢 BUILD.

→ **FIN PHASE D : GATE IN-GAME (Smoke Test) sur `2.3.2-cpp`.**

---

# Final — v3.0.0

### Task F : Bilan & version finale

**Step 1 :** Compter les lignes, comparer au baseline :

```bash
wc -l cpp/*.cpp cpp/*/*.hpp cpp/*/*.cpp cpp/inc/*/*.hpp   # (ou old/cpp/inc si D3 non fait)
```

**Step 2 :** Bump `3.0.0-cpp` (`dllmain.cpp` + `Info.json`). GATE BUILD. **GATE IN-GAME finale** (Smoke Test complète + test FPS build-mode + confirmation bug palbox si reproduit avant).

**Step 3 :** `memory_store` : bilan LOC, gains de perf mesurés, tout point ouvert (bug palbox ghost toujours inexpliqué).

---

## Résumé des livrables

- **LOC nette :** ≈ **−300** (dont −285 = code mort, −60 = dédup réflexe, +120 = cache).
- **Perf :** scan catalog ~15 → ~6 ms ; chaîne présence ~×3 ; −100+ allocs/min (D1).
- **Maintenabilité :** god-class 1 392 L → 4 fichiers cohésifs + 120 L de PIMPL ; constantes centralisées ; boilerplate réflexe en un template.
- **Sûreté :** Aucune sémantique d'union modifiée. Chaque tâche à gate vert. Flags de bisect + `kDiagnostics` prêts si régression.
- **Reste ouvert (hors scope) :** bug palbox-ghost jamais diagnostiqué — à traiter séparément si reproduit avec logs.
