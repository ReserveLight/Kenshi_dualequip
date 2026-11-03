# Technical notes

This is a short map of the code for anyone reading or modifying it. The source files contain much more detail in their comments.

## Two DLLs

| DLL | Source | Responsibility |
|---|---|---|
| `DualEquipSlots2.dll` | `src/DualEquipSlots2.cpp` | Extra sections, layouts, guards, stats/weight/coverage handling, in-game UI, NPC cleanup |
| `DualEquip2.dll` | `src/DualEquip2.cpp` | Hide/show appearance, `Insert` / `Right Shift` hotkeys, shirt attachment fix |

`DualEquip2` never hooks engine functions; it calls `DualEquipSlots2` exports (`DE_GetOptionBits`, `DE_SetCharHidden`, `DE_ResetCharStates`) where needed.

## Engine interface

- The mod does **not** link `KenshiLib.lib` and does not include KenshiLib headers.
- Every engine function is resolved at runtime by its MSVC mangled name from `KenshiLib.dll`.
- Hooking must use `RealSym()` (real address), not `Sym()` (forwarding stub): `Sym()` hooks install but never fire.
- Field offsets come from the KenshiLib reference headers for Kenshi 1.0.65 x64.
- MyGUI is linked against an import library generated from the game's `MyGUIEngine_x64.dll` (see `BUILDING.md`). The mod is built `/MT`; it must never let MyGUI/C++ objects cross the game's CRT. For that reason there are **no MyGUI event delegates**: clicks are polled with `MyGUI::InputManager::getMousePosition()` + `GetAsyncKeyState(VK_LBUTTON)`, and UI ticks run on the game thread from `RootObjectFactory::mainThreadUpdate`.

## Extra sections

Names and attach slots (`src/DualEquipSlots2.cpp`, `kSections`):

```cpp
de_slot_head    // head
de_slot_armour  // body armour
de_slot_legs    // legs
de_slot_shirt   // shirt
de_slot_boots   // boots
```

Each section is `4 x 2` or `4 x 3` cells (matching the vanilla slot), with the real `AttachSlot` so its item renders. The character inventory layout is the original `Kenshi_InventoryCharacterWindow.layout` plus a separate `DualEquipWindow` (`EXTRA SET`) and five `de_slot_*` widgets; the Animal and Limb layouts contain hidden zero-size `de_slot_*` widgets as layout crash guards.

## Hooks (main)

| Hook | Purpose |
|---|---|
| `Inventory::Inventory` | Create extra sections in player-character inventories during normal play |
| `Inventory::getSection` | On-demand creation; **player characters only** |
| `Character::setupInventorySections` | Save-load section creation |
| `Character::loadFromSerialise` | Per-character section creation during load; queues characters for NPC cleanup |
| `InventoryLayout::getWidget` | Crash guard: returns the layout root when a section has no widget |
| `Inventory::getSectionOfType` | Equip-routing guard: if the engine selects an extra section, redirect to the vanilla section |
| `Inventory::getTotalWeight` | Zero the weight contribution of cosmetic extra slots |
| `Character::recalculateTotalEquipmentSkillBonus` | Exclude disabled extra-slot items from skill bonuses |
| `Armour::didIHitFlesh` | Extra-slot items give no coverage/resistance when disabled |
| `InventoryGUI::show` | Ensure player sections before opening; refresh section widgets |
| `RootObjectFactory::mainThreadUpdate` | Main-thread UI tick and deferred NPC cleanup |

There are also diagnostic probes (`Inventory::addItem`, `Inventory::transferMouseItem`, `Inventory::equipItem`, `Character::equipItem`, `Item::_loadFromSerialise`). They only log; they do not change behavior.

## Per-character options and sidecar

Each character has a 16-bit option value stored in `DualEquip_optionmap.txt` in the game root (one line per character, keyed by a handle-derived string). The file is a **sidecar**, not part of the `.save`.

| Bit | Meaning |
|---|---|
| `2*i` | `[1]` for extra slot `i`: 1 = stats ON, 0 = no stats (default) |
| `2*i+1` | `[2]` for extra slot `i`: 1 = keep vanilla visible while hiding, 0 = hide vanilla too |
| 10 | `[X]` backpack appearance hidden |
| 11 | `[I]` character appearance hidden (also set by Insert) |

Current UI semantics (selected = bright in the game's button skin):

- `[1]` bright = no stats (default), dark = stats ON
- `[2]` bright = keep the vanilla slot while hiding, dark = hide it too
- `[I]` bright = hidden, dark = shown
- `[X]` bright = backpack hidden, dark = shown

## NPC extra slots

During a save load the player list is empty, so the load hook creates sections for every character. After loading, the main-thread tick runs `CleanupNpcExtraSections()`:

1. Characters are skipped if they are in `PlayerInterface::playerCharacters` **or** `Character::isPlayerCharacter()` returns true.
2. For each remaining NPC extra section, items are moved to the matching vanilla section if it is empty, otherwise to any normal (non-`de_slot_*`) section. If no target can take an item, it is put back; items are never silently deleted.
3. The NPC section is then disabled (`enabled=false`, `isAnEquippedItemSection=false`) — the panel shows locked/empty slots.
4. Every second `EnsureSectionsOnPlayerInventories()` calls `EnsureExtraSections(inv, true)`, so if the NPC is recruited the sections are re-enabled. `InventoryGUI::show` also ensures and refreshes the section widgets on open.

## DualEquip2

- A hide thread polls every 500 ms and detaches/attaches the **vanilla** appearance sections (`head`, `armour`, `legs`, `shirt`, `boots`, `belt`); extra sections are never hidden.
- `Insert` toggles the selected character; `Right Shift` toggles all player characters.
- The shirt fix handles the case where the extra shirt and vanilla shirt would fight over the same attachment key.
- `DualEquipSlots2` exports `DE_GetOptionBits`; `DualEquip2` exports `DE_SetCharHidden` / `DE_ResetCharStates`.

## Known issues / import

See [`docs/known-issues.md`](docs/known-issues.md). The important player-facing one:

- **Kenshi's Import Game does not carry items stored in modded extra sections.** Put them back into a normal backpack before importing. Normal save loading is unaffected.
- **Dark UI load order**: this mod must be **after** Dark UI.
- `Extra Inventory Sections Plugin` (Workshop 3681682317) is not compatible: both hook `Inventory::getSection` and require different character layouts.
