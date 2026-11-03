# Hide / Show DualEquip

**Kenshi 1.0.65 x64 · RE_Kenshi plugin · five extra equipment slots per player character**

[Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3813933161) · [中文](#中文) · [English](#english)

> Unofficial mod. Not affiliated with Lo-Fi Games.

---

## 中文

### 简介

给每个玩家角色增加 **5 个额外装备槽**（头 / 铠甲 / 裤子 / 衬衫 / 靴子），并按角色控制额外槽装备的**外观、数值、隐藏行为**。

默认情况下，额外槽装备**只显示外观、不提供数值**；你可以按槽开启数值，也可以让隐藏只作用于部分原版槽。额外槽那一套始终显示，Insert / 右 Shift 隐藏的是**原版槽**的外观。

### 功能

| 操作 | 作用 |
|---|---|
| `Insert` | 隐藏 / 显示**当前选中角色**的原版外观 |
| `Right Shift` | 隐藏 / 显示**全队**的原版外观 |
| 【1】 | 额外槽数值开关：**亮 = 无数值（默认）**，暗 = 参与技能加成 / 重量 / 护甲覆盖与抗性 |
| 【2】 | 隐藏时是否保留对应原版槽：亮 = 保留，暗 = 连原版槽一起隐藏 |
| 【X】 | 隐藏 / 显示背包外观（不影响背包功能） |
| 【I】 | 等同 Insert；**亮 = 该角色已隐藏** |
| 【R】 | 等同右 Shift |
| 【A】 | 把当前角色的【1】【2】复制给所有选中角色（不改 X / I） |
| 【?】 | 打开 / 关闭游戏内帮助 |

- **两套外观**：额外槽和原版槽可以同时穿戴，互不覆盖；不隐藏时两套都会渲染。
- **每槽独立**：五个额外槽各有自己的【1】【2】，可以只让某一件装备生效。
- **每角色保存**：设置写入游戏根目录 `DualEquip_optionmap.txt`（独立 sidecar，不写存档）。
- **额外槽数值**：开启后，额外槽与原版槽的数值叠加（技能加成、重量、护甲覆盖 / 抗性）。

### 安装

1. 安装 **RE_Kenshi 0.3.5+**（自带 KenshiLib）：  
   https://www.nexusmods.com/kenshi/mods/847
2. **工坊订阅**，或把本仓库 `mod/` 里的内容复制到：
   ```
   Kenshi/mods/Hide_Show_DualEquip/
   ```
   需要包含 `Hide_Show_DualEquip.mod`、`RE_Kenshi.json`、`DualEquip_help.txt`、`gui/layout/*.layout`、`Plugins/*.dll`。
3. 在游戏 mod 列表启用 `Hide_Show_DualEquip`。
4. **如果使用 Dark UI**：本 mod 必须排在 Dark UI **之后**（列表更靠下）。
5. 启动游戏，打开角色装备窗口即可看到 `EXTRA SET` 面板。

### 说明

- **导入存档（Import Game）不会保留额外槽里的装备。**  
  这是 Kenshi 导入机制的限制：它只搬运原版/背包数据。导入前请把额外装备放回普通背包；普通**读档（Load）**不受影响。
- **NPC 额外槽会被清理并锁定**：读档后 NPC 额外槽里的装备会移回对应原版槽（空）或普通库存，然后区格禁用；NPC 被招募进队伍后会自动恢复可用。详见 [`docs/known-issues.md`](docs/known-issues.md)。
- **Dark UI**：当前顺序（Dark UI 在前、本 mod 在后）安全；如果本 mod 排在 Dark UI 之前，Dark UI 的库存布局会覆盖本 mod 的 `DualEquipWindow` / `de_slot_*`，打开装备面板会崩溃。当前顺序下 Dark UI 的**库存窗口布局**会被本 mod 覆盖（其他皮肤/菜单仍生效）。
- **设置 sidecar 不随 `.save` 移动**：只把存档拷到另一台电脑不会带走每角色设置。

### 构建

完整步骤见 [`BUILDING.md`](BUILDING.md)。

- Visual Studio 2022（v143），x64，`/MT` 静态 CRT；
- 需要 MyGUI 头文件（`third_party/MyGUI/Include`）；
- 用 `tools/gen_mygui_lib.py` 从游戏的 `MyGUIEngine_x64.dll` 生成 `third_party/MyGUIEngine_x64.lib`；
- 复制 `build_all.example.bat` 为 `build_all.bat`，修改 `VSDIR` / `MYGUI_INC` / `MYGUI_LIB`，在仓库根目录运行。

源码：

| 文件 | 作用 |
|---|---|
| `src/DualEquipSlots2.cpp` | 额外槽、布局、装备路由、数值/重量/覆盖、UI、NPC 清理 |
| `src/DualEquip2.cpp` | 隐藏/显示、Insert / 右 Shift 热键、衬衫挂载修复 |

实现细节见 [`TECHNICAL.md`](TECHNICAL.md)。仓库不包含 Kenshi 游戏 DLL、KenshiLib 头文件或 MyGUIEngine DLL。

### 仓库结构

```
src/                 两个 DLL 的源码
mod/                 可直接放入 Kenshi/mods/Hide_Show_DualEquip 的文件
build_all.example.bat 构建模板
tools/               生成 MyGUI import lib 的脚本
docs/                截图、已知问题记录
BUILDING.md          构建说明
TECHNICAL.md         实现 / hook / 数据结构说明
LICENSE              MIT（原创代码；第三方组件不在仓库内）
```

### 常见问题

- **额外槽装备没有数值？** 默认【1】亮 = 无数值；点暗才参与加成。
- **隐藏后只看到一套，另一套呢？** Insert / 右 Shift 只隐藏原版槽；额外槽始终显示。若某个原版槽想保留，点该槽的【2】。
- **导入存档后额外装备不见了？** 导入机制不搬运额外槽；导入前放回背包。
- **NPC 的额外槽为什么是锁的？** 防止 NPC 获得额外数值；招募后自动恢复。
- **设置换电脑后没了？** 设置保存在 sidecar，不在 `.save` 里。
- **和 Dark UI 一起用？** 保持本 mod 排在 Dark UI 之后。

### 致谢 / 许可

- **Kenshi** © Lo-Fi Games.
- **RE_Kenshi / KenshiLib** by KenshiReclaimer — https://github.com/KenshiReclaimer/KenshiLib
- **MyGUI** — https://github.com/MyGUI/mygui

本仓库中的原创代码与 mod 文件采用 **MIT License** 。第三方组件（Kenshi、RE_Kenshi、KenshiLib、MyGUI）的版权与许可归各自作者。

---

## English

### About

Adds **five extra equipment slots** per player character (head / armour / legs / shirt / boots) and gives you per-character control over the **appearance, stats and hiding behaviour** of the extra set.

By default, extra-slot items are **cosmetic only** — they show their appearance but give no stats. You can enable stats per slot, choose which vanilla slots stay visible while hiding, and hide the vanilla set without touching the extra set.

### Features

| Control | What it does |
|---|---|
| `Insert` | Hide / show the **selected character's** vanilla appearance |
| `Right Shift` | Hide / show **the whole squad's** vanilla appearance |
| [1] | Extra-slot stats: **bright = no stats (default)**, dark = counts towards skill bonuses, weight and armour coverage / resistance |
| [2] | While hiding: bright = keep the matching vanilla slot visible, dark = hide it too |
| [X] | Hide / show the backpack appearance (no effect on backpack function) |
| [I] | Same as Insert; **bright = this character is hidden** |
| [R] | Same as Right Shift |
| [A] | Copy this character's [1]/[2] settings to all selected characters (X / I are not changed) |
| [?] | Open / close in-game help (9 game languages) |

- **Two sets**: extra and vanilla slots can be worn at the same time; both appearances render unless you hide the vanilla set.
- **Per slot**: each of the five extra slots has its own [1]/[2] switches.
- **Per character**: settings are stored in `DualEquip_optionmap.txt` in the game root (a sidecar, not part of the `.save`).
- **Optional stats**: when enabled, extra and vanilla stats stack (skill bonuses, weight, armour coverage / resistance).

### Installation

1. Install **RE_Kenshi 0.3.5+** (bundles KenshiLib):  
   https://www.nexusmods.com/kenshi/mods/847
2. Subscribe on the Workshop, or copy the contents of `mod/` into:
   ```
   Kenshi/mods/Hide_Show_DualEquip/
   ```
   This includes `Hide_Show_DualEquip.mod`, `RE_Kenshi.json`, `DualEquip_help.txt`, `gui/layout/*.layout` and `Plugins/*.dll`.
3. Enable `Hide_Show_DualEquip` in the mod list.
4. **If you use Dark UI**: keep this mod **below** Dark UI in the load order.
5. Start the game and open a character's equipment window; the `EXTRA SET` panel appears.

### notes

- **Kenshi's Import Game does not carry items stored in mod-added extra slots.**  
  This is an import limitation: only vanilla/backpack data is carried. Put extra-slot items back into a normal backpack before importing. Normal **Load** is unaffected.
- **NPC extra slots are cleaned and locked**: after loading, any items in NPC extra slots are moved to the matching vanilla slot (if empty) or to normal inventory, and the sections are disabled. If the NPC joins the player's squad, the sections are re-enabled automatically. See [`docs/known-issues.md`](docs/known-issues.md).
- **Dark UI**: the current order (Dark UI first, this mod later) is safe. If this mod is placed **before** Dark UI, Dark UI's inventory layout overrides `DualEquipWindow` / `de_slot_*` and opening the equipment panel can crash. In the safe order, this mod overrides Dark UI's **inventory layout** (other Dark UI skins/menus still apply).
- **The sidecar does not travel with the `.save`**: copying only the save to another PC does not carry per-character settings.

### Building

See [`BUILDING.md`](BUILDING.md).

- Visual Studio 2022 (v143), x64, `/MT` static CRT.
- MyGUI headers under `third_party/MyGUI/Include`.
- Generate `third_party/MyGUIEngine_x64.lib` with `tools/gen_mygui_lib.py` from the game's `MyGUIEngine_x64.dll`.
- Copy `build_all.example.bat` to `build_all.bat`, adjust `VSDIR` / `MYGUI_INC` / `MYGUI_LIB`, and run it from the repository root.

Source:

| File | Purpose |
|---|---|
| `src/DualEquipSlots2.cpp` | Extra sections, layouts, equip routing, stats/weight/coverage, UI, NPC cleanup |
| `src/DualEquip2.cpp` | Hide/show, Insert / Right Shift hotkeys, shirt attachment fix |

Implementation notes: [`TECHNICAL.md`](TECHNICAL.md). The repository does not contain Kenshi game DLLs, KenshiLib headers or the MyGUIEngine DLL.

### Repository layout

```
src/                 Source for the two DLLs
mod/                 Ready-to-install mod files
build_all.example.bat Build template
tools/               MyGUI import-lib generator
docs/                Screenshot and known-issues notes
BUILDING.md          Build instructions
TECHNICAL.md         Hooks / data structures / implementation notes
LICENSE              MIT (original code; third-party components are not included)
```

### FAQ

- **Why do extra-slot items give no stats?** [1] is bright by default (no stats); make it dark to enable.
- **Why do I only see one set?** Insert / Right Shift hide the vanilla set only; the extra set is always visible. Use [2] if you want a vanilla slot to stay visible.
- **Why did my extra equipment disappear after Import Game?** Import does not carry extra-slot items; put them in the backpack first.
- **Why are NPC extra slots locked?** To prevent NPCs from gaining extra stats; they are re-enabled when the NPC joins your squad.
- **Why did my settings disappear on another PC?** They live in a sidecar, not in the `.save`.
- **Can I use it with Dark UI?** Yes, but keep this mod below Dark UI.

### Credits / license

- **Kenshi** © Lo-Fi Games.
- **RE_Kenshi / KenshiLib** by KenshiReclaimer — https://github.com/KenshiReclaimer/KenshiLib
- **MyGUI** — https://github.com/MyGUI/mygui

Original code and mod files in this repository are released under the **MIT License** . Third-party components (Kenshi, RE_Kenshi, KenshiLib, MyGUI) remain under their respective licenses.
