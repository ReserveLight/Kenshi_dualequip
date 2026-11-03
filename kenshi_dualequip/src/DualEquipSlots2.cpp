// ============================================================================
//  DualEquipSlots v2 - extra equipment slots, implemented the way the shipped
//                     "Extra Inventory Sections" plugin does it.
//
//  WHAT CHANGED FROM v1 (and why v1 failed)
//    v1 created sections from a background thread and overlapped Dark UI's
//    layout file. Both were wrong:
//      * Sections must exist when the character initialises, or the inventory
//        panel has no matching widget -> MYGUI_ASSERT -> crash on opening bag.
//        FIX: hook Character::_NV_init.
//      * Overriding Kenshi_InventoryCharacterWindow.layout collides with UI mods.
//        FIX: register a DIFFERENT layout name and redirect the request for the
//        vanilla one (hook wraps::BaseLayout::initialise). No collision possible.
//      * The engine routes equip attempts through Inventory::getSectionOfType;
//        if it hands back one of our sections the game writes out of bounds and
//        crashes. FIX: hook it and prefer the vanilla section for our slot types.
//
//  DESIGN (per the user's approach)
//    Each extra section is "cosmetic-only storage": the appearance system does
//    not know its name (proven: hasSlot("de_slot_*") = no), so items placed
//    there contribute STATS but render nothing. The vanilla section keeps
//    rendering the nice-looking item.
//        vanilla slot  -> the LOOKS  item  (renders)
//        extra slot    -> the STATS  item  (no mesh)
//
//  SAFETY
//    * Adds empty sections only; never moves/adds/removes items.
//    * Player characters only (Character::isPlayerCharacter).
//    * getSectionOfType hook only ever redirects AWAY from our sections.
//
//  Pure ASCII source - MSVC on a GBK codepage mangles non-ASCII input.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>

// MyGUI (the game ships MyGUIEngine_x64.dll; we link its generated import lib).
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_InputManager.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Widget.h>
#include <mygui/MyGUI_Window.h>

// ---------------------------------------------------------------------------
template <typename T>
struct lektor { void* allocBase; uint32_t count; uint32_t maxSize; T* stuff; };

// MSVC x64 std::string image: union@0x0, size@0x10, capacity@0x18
struct MsvcString
{
    union { char buf[16]; char* ptr; } u;
    uint64_t size;
    uint64_t capacity;
    void set(const char* s)
    {
        const size_t n = strlen(s);
        memset(this, 0, sizeof(*this));
        if (n < 16) { memcpy(u.buf, s, n); u.buf[n] = 0; size = n; capacity = 15; }
        else { char* p = (char*)malloc(n + 1); memcpy(p, s, n + 1); u.ptr = p; size = n; capacity = n; }
    }
    const char* c_str() const { return (size < 16) ? u.buf : (u.ptr ? u.ptr : ""); }
};

namespace off
{
    constexpr size_t Character_inventory = 0x2E8; // Character.h:599
    constexpr size_t Section_name        = 0x8;   // Inventory.h:87  std::string name
    constexpr size_t Section_width       = 0x30;  // Inventory.h:88
    constexpr size_t Section_height      = 0x34;  // Inventory.h:89
    constexpr size_t Section_firstItem   = 0x60;  // best-effort: first SectionItem storage
    constexpr size_t Section_totalWeight = 0xB0;  // Inventory.h: totalWeight
    constexpr size_t Section_equipped    = 0xB4;  // Inventory.h: isAnEquippedItemSection
    constexpr size_t Inventory_totalWeight = 0x90; // Inventory.h: float totalWeight
    constexpr size_t Character_stats     = 0x450; // Character.h: CharStats* stats
    constexpr size_t Character_equipList = 0x4A0; // lektor<Item*> equipped armour (count +0x4A8, stuff +0x4B0)
}

enum AttachSlot : int
{
    ATTACH_WEAPON = 0, ATTACH_BACK, ATTACH_HAIR, ATTACH_HAT, ATTACH_EYES,
    ATTACH_BODY, ATTACH_LEGS, ATTACH_NONE, ATTACH_SHIRT, ATTACH_BOOTS,
    ATTACH_GLOVES, ATTACH_NECK, ATTACH_BACKPACK, ATTACH_BEARD, ATTACH_BELT
};

// ---------------------------------------------------------------------------
// Function pointer types
// ---------------------------------------------------------------------------
static void __cdecl Detour_loadFromSerialise_Compat(void* item, void* container, void* gameData);

typedef void* (*fn_getSingleton_t)();
typedef int   (*fn_AddHook_t)(void* target, void* detour, void** original);
// KenshiLib::GetRealAddress(void*) -> the address of the REAL game function.
// Sym() alone returns KenshiLib's own forwarding stub, which nothing calls -
// that is why six hooks installed successfully but never fired.
typedef int64_t (*fn_GetRealAddress_t)(void* stub);
static fn_GetRealAddress_t p_GetRealAddress = nullptr;
typedef bool  (*fn_isPlayerChar_t)(void* character);

typedef void* (*fn_getSection_t)(void* inv, const MsvcString* name);
typedef void* (*fn_initSection_t)(void* inv, const MsvcString* name, int w, int h,
                                  int slot, bool equipCallbacks,
                                  bool isContainerSlot, bool enabled, int limit);
typedef void  (*fn_resizeSection_t)(void* inv, void* section, int w, int h, bool clearContent);
typedef void* (*fn_getSectionOfType_t)(void* inv, int slot);
typedef lektor<void*>& (*fn_getAllSections_t)(void* inv);

typedef void  (*fn_voidThis_t)(void* self);
typedef void  (*fn_baseLayoutInit_t)(void* self, const MsvcString* layout,
                                     void* parent, bool throwOnMissing,
                                     bool createFakeWidgets);

static fn_getSection_t        p_getSection        = nullptr;
static fn_initSection_t       p_initSection       = nullptr;
static fn_resizeSection_t     p_resizeSection     = nullptr;
static fn_getSectionOfType_t  p_getSectionOfType  = nullptr;
static fn_getAllSections_t    p_getAllSections    = nullptr;
static fn_isPlayerChar_t      p_isPlayerChar      = nullptr;
typedef bool (*fn_isLoadingSave_t)();
static fn_isLoadingSave_t     p_isLoadingSave     = nullptr;
static fn_AddHook_t           p_AddHook           = nullptr;

static fn_getSectionOfType_t  p_getSectOfTypeOrig = nullptr;
static fn_baseLayoutInit_t    p_baseLayoutOrig    = nullptr;

static void* g_pKenshiLib = nullptr;
static int   g_created    = 0;
static volatile LONG g_sectionsReenabled = 0;

// ---------------------------------------------------------------------------
// Mangled names - each verified with dumpbin /exports KenshiLib.lib
// ---------------------------------------------------------------------------
#define SYM_ADD_HOOK        "?AddHook@KenshiLib@@YA?AW4HookStatus@1@PEAX0PEAPEAX@Z"
#define SYM_GET_REAL_ADDR   "?GetRealAddress@KenshiLib@@YA_JPEAX@Z"
#define SYM_IS_PLAYER       "?isPlayerCharacter@Character@@QEBA_NXZ"
#define SYM_CHAR_INIT       "?_NV_init@Character@@QEAAXXZ"
// Virtual variant: builds a character's inventory sections while loading a save.
#define SYM_SETUP_INV_SECTIONS "?setupInventorySections@Character@@UEAA_NPEAVGameSaveState@@@Z"
// Inventory constructor - runs for every inventory, incl. during a save load.
#define SYM_INV_CTOR        "?_CONSTRUCTOR@Inventory@@QEAAPEAV1@PEAVRootObject@@@Z"
// InventorySection::addItem (virtual) - where deserialised items land.
#define SYM_SECTION_ADDITEM "?addItem@InventorySection@@UEAA_NPEAVItem@@H@Z"
// Inventory::getSection(name) - lookup by name; hooked to create our sections
// on demand if the save loader asks for one before it exists.
#define SYM_GET_SECTION_HOOK "?getSection@Inventory@@QEBAPEAVInventorySection@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
// Item deserialisation entry point (virtual) - fires once per item during load.
#define SYM_LOAD_FROM_SERIALISE "?_loadFromSerialise@Item@@UEAAXPEAVGameDataContainer@@PEAVGameData@@@Z"
#define SYM_SECTION_GETITEM "?getItem@InventorySection@@QEBAPEAVItem@@XZ"
#define SYM_SECTION_REMOVEITEM "?removeItem@InventorySection@@QEAA_NPEAVItem@@@Z"
#define SYM_SECTION_SETENABLED "?setEnabled@InventorySection@@QEAAX_N@Z"
#define SYM_GET_SECTION     "?getSection@Inventory@@QEBAPEAVInventorySection@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
#define SYM_NEW_SECTION     "?_NV_initialiseNewSection@Inventory@@QEAAPEAVInventorySection@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@HHW4AttachSlot@@_N22H@Z"
#define SYM_RESIZE_SECTION  "?resizeSection@Inventory@@QEAAXPEAVInventorySection@@HH_N@Z"
#define SYM_SECTION_OF_TYPE "?getSectionOfType@Inventory@@QEAAPEAVInventorySection@@W4AttachSlot@@@Z"
#define SYM_ALL_SECTIONS    "?getAllSections@Inventory@@QEAAAEAV?$lektor@PEAVInventorySection@@@@XZ"
#define SYM_BASELAYOUT_INIT "?initialise@BaseLayout@wraps@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@PEAVWidget@MyGUI@@_N2@Z"
#define SYM_OU              "?ou@@3PEAVGameWorld@@EA"
#define SYM_LOAD_ALL_PLATOONS "?loadAllPlatoons@GameWorld@@QEAAXXZ"
// Tombsone + finding (2026-10-08): CharStats::getStat(StatsEnumerated, bool)
// is hookable and DOES fire (measured ~125 calls/s), and its bool argument is
// meaningful (the same enum arrives with both 0 and 1). It was therefore a
// candidate interception point for suppressing the stats of items sitting in
// our extra slots - but the attempt was abandoned, because a CharStats* cannot
// be mapped back to its owning Character. See:
//     文档/研究_屏蔽装备数值_可行性结论.md
// Do not re-add a hook here without solving that first.
// #define SYM_GETSTAT "?getStat@CharStats@@QEBAMW4StatsEnumerated@@_N@Z"
#define SYM_IS_LOADING_SAVE  "?isLoadingFromASaveGame@GameWorld@@QEAA_NXZ"
// CharStats::getStat(StatsEnumerated, bool) -> float
//   The ONLY query point for "what is this character's value for stat X".
//   The trailing bool is probably includeMods (whether gear bonuses count).
//   Probed read-only on 2026-10-08 to decide whether "suppress the stats of
//   items sitting in our extra slots" is engineering-feasible at all.
// InventoryLayout::getWidget(const std::string& name) -> MyGUI::Widget*
//
// EVERY inventory window (character / limbs / trader / animal / generic ...)
// looks its section widgets up through this one function. When a layout has no
// widget for a section name, MyGUI raises MYGUI_ASSERT ->
// "Can't assign widget with name '...'" -> the game dies.
//
// That is the single mechanism behind BOTH user-visible crashes:
//   * "Hide your helmet" + this mod  -> crash opening the bag
//   * this mod alone                 -> crash opening the limb menu
//   * our own early build            -> crash opening the bag
// It is also why shipping extra widgets in Kenshi_LimbReplacementWindow.layout
// helped only partially: every OTHER layout has the same problem.
//
// Hooking getWidget fixes all of them at once, without having to guess which
// inventories are characters (a guess that failed repeatedly - see the version
// history in Detour_inventoryCtor).
// Character::loadFromSerialise(GameSaveState*) - per-character, during load.
#define SYM_CHAR_LOAD_FROM_SERIALISE "?loadFromSerialise@Character@@UEAAXPEAVGameSaveState@@@Z"
#define SYM_LAYOUT_GET_WIDGET "?getWidget@InventoryLayout@@QEAAPEAVWidget@MyGUI@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
#define SYM_LAYOUT_GET_WINDOW "?getWindow@InventoryLayout@@QEBAPEAVWindow@MyGUI@@XZ"
// Inventory::getTotalWeight() - sums the cached totalWeight of EVERY section.
#define SYM_GET_TOTAL_WEIGHT "?getTotalWeight@Inventory@@QEAAMXZ"
// Character::recalculateTotalEquipmentSkillBonus() - the ONE place that turns
// the character's equipped-armour list into CharStats equipment bonuses.
#define SYM_RECALC_EQUIP "?recalculateTotalEquipmentSkillBonus@Character@@QEAAXXZ"
// Armour::didIHitFlesh(GameData* bodypart) - the coverage roll / hit test used
// by the melee hit resolver. Returning true means "the hit reached flesh",
// i.e. this armour does NOT protect that body part.
#define SYM_DID_HIT_FLESH "?didIHitFlesh@Armour@@QEAA_NPEAVGameData@@@Z"
// ---- UI / per-character options (2026-10-22) ----
#define SYM_INV_GUI_SHOW    "?_NV_show@InventoryGUI@@QEAAX_N@Z"
#define SYM_WORLD_TICK      "?_NV_mainLoop_GPUSensitiveStuff@GameWorld@@QEAAXM@Z"
#define SYM_GET_ALL_SELECTED "?getAllSelectedObjects@PlayerInterface@@QEAAXAEAV?$lektor@PEAVRootObject@@@@W4itemType@@@Z"
#define SYM_GET_INSTANCE_ID "?_NV_getInstanceID@RootObject@@QEAAPEAVInstanceID@@XZ"
#define SYM_GET_LAYOUT_ID  "?_NV_getLayoutInstanceID@RootObject@@QEAAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ"
#define SYM_GET_HAND       "?getHandle@RootObjectBase@@QEBAAEBVhand@@XZ"
#define SYM_IS_PLAYER_ROOT "?_isPlayerCharacter@PlayerInterface@@QEAA_NPEAVRootObject@@@Z"
#define SYM_FACTORY_TICK    "?mainThreadUpdate@RootObjectFactory@@QEAAXXZ"

// ---------------------------------------------------------------------------
// Our extra sections - FIVE body parts, matching what the user asked for.
//
// NAME MUST MATCH the widget name in gui/layout/Kenshi_InventoryCharacterWindow.layout,
// or opening the bag asserts and crashes.
//
// SIZE = the kind of equipment stored there, in INVENTORY CELLS as
// (width x height). The user clarified: "6x4 means 6 cells TALL by 4 cells WIDE".
// Every armour item is 4 cells wide, so width is 4 for all of them. Heights are
// taken straight from the vanilla slot heights, which fit exactly:
//
//   part     vanilla slot (w x h)   cells (w x h)   cell size derived
//   shirt    0.2023 x 0.0832        4 x 2           0.0506 x 0.0416
//   head     0.2023 x 0.1275        4 x 3           0.0506 x 0.0425
//   boots    0.2023 x 0.0832        4 x 2           0.0506 x 0.0416
//   legs     0.2023 x 0.2144        4 x 5           0.0506 x 0.0429
//   armour   0.2023 x 0.2569        4 x 6           0.0506 x 0.0428
//
// That is why cells are NOT square: 0.0506 wide vs ~0.0427 tall.
// NOTE: v1 of this file used 1x1 sections for EIGHT parts (including neck/eyes/
// gloves, which are not body parts the user wants). That made the cells too small
// to hold any real equipment. Both the count and the sizes are fixed here.
// ---------------------------------------------------------------------------
struct SectionInfo { const char* name; AttachSlot slot; int w; int h; int limit; };

static const SectionInfo kSections[] =
{
    //          name              slot            w  h  limit
    { "de_slot_head",   ATTACH_HAT,    4, 3, 1 },
    { "de_slot_armour", ATTACH_BODY,   4, 6, 1 },
    { "de_slot_legs",   ATTACH_LEGS,   4, 5, 1 },
    { "de_slot_shirt",  ATTACH_SHIRT,  4, 2, 1 },
    { "de_slot_boots",  ATTACH_BOOTS,  4, 2, 1 },
};
static const int kSectionCount = (int)(sizeof(kSections) / sizeof(kSections[0]));

// ---------------------------------------------------------------------------
static void LogF(const char* fmt, ...)
{
    char buf[1200];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (FILE* f = fopen("DualEquipSlots.log", "a"))
    {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
        fclose(f);
    }
}

static FARPROC Sym(const char* n)
{
    FARPROC p = GetProcAddress((HMODULE)g_pKenshiLib, n);
    if (!p) LogF("[slots] symbol NOT FOUND: %s", n);
    return p;
}

// Resolve a symbol AND translate the KenshiLib stub to the real game function.
static void* RealSym(const char* n)
{
    void* stub = (void*)Sym(n);
    if (!stub) return nullptr;
    if (!p_GetRealAddress) return stub;
    int64_t real = p_GetRealAddress(stub);
    return real ? (void*)real : stub;
}

static bool IsOurSectionName(const char* n)
{
    for (int i = 0; i < kSectionCount; ++i)
        if (strcmp(n, kSections[i].name) == 0) return true;
    return false;
}

// ===========================================================================
//  PER-SLOT SETTINGS (2026-10-22)
//
//  DualEquip_settings.txt in the game directory (created on first run):
//
//      stats_head=1
//      stats_armour=1
//      stats_legs=0
//      stats_shirt=0
//      stats_boots=0
//
//  1 = items in that extra slot contribute stats / weight / armour coverage
//      exactly like a vanilla-slot item;
//  0 = cosmetic only (default): filtered out of the equipment bonus recalc,
//      no coverage, no weight.
//
//  Re-read every 2 s. A change triggers one recalculation per player
//  character so the panel updates without re-equipping anything.
// ===========================================================================
static volatile LONG g_statsMask = 0;          // bit i = extra section i contributes stats
static DWORD g_settingsNextMs = 0;
static bool  g_settingsTemplateWritten = false;

static bool StatsEnabled(int i) { return (g_statsMask & (1L << i)) != 0; }

// defined further down; used to refresh characters when the mask changes
static void __cdecl Detour_recalcEquip(void* character);

static void TrimInPlace(char* s)
{
    char* p = s;
    while (*p == ' ' || *p == '\t') ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

// Returns true when the mask changed (caller should refresh characters).
static bool LoadSettings()
{
    const DWORD now = GetTickCount();
    if (now < g_settingsNextMs) return false;
    g_settingsNextMs = now + 2000;

    static const char* kKeys[kSectionCount] =
        { "stats_head", "stats_armour", "stats_legs", "stats_shirt", "stats_boots" };

    LONG mask = 0;
    FILE* f = fopen("DualEquip_settings.txt", "r");
    if (f)
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            char* hash = strchr(line, '#'); if (hash) *hash = 0;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char key[64];
            strncpy_s(key, sizeof(key), line, _TRUNCATE);
            TrimInPlace(key);
            if (key[0] == 0) continue;
            const int val = atoi(eq + 1);
            for (int i = 0; i < kSectionCount; ++i)
            {
                if (_stricmp(key, kKeys[i]) == 0)
                {
                    if (val != 0) mask |= (1L << i);
                    break;
                }
            }
        }
        fclose(f);
    }
    else if (!g_settingsTemplateWritten)
    {
        g_settingsTemplateWritten = true;
        if (FILE* t = fopen("DualEquip_settings.txt", "w"))
        {
            fprintf(t, "# DualEquip per-slot settings\r\n");
            fprintf(t, "# 1 = the item in this extra slot contributes stats, weight and armour coverage\r\n");
            fprintf(t, "# 0 = cosmetic only (default)\r\n");
            fprintf(t, "# Re-read every 2 seconds, no restart needed.\r\n");
            for (int i = 0; i < kSectionCount; ++i) fprintf(t, "%s=0\r\n", kKeys[i]);
            fclose(t);
            LogF("[settings] created default DualEquip_settings.txt (all cosmetic)");
        }
    }

    const LONG oldMask = g_statsMask;
    if (mask != oldMask)
    {
        InterlockedExchange(&g_statsMask, mask);
        LogF("[settings] stats mask %#lx -> %#lx (head=%d armour=%d legs=%d shirt=%d boots=%d)",
             oldMask, mask,
             (mask & 1) ? 1 : 0, (mask & 2) ? 1 : 0, (mask & 4) ? 1 : 0,
             (mask & 8) ? 1 : 0, (mask & 16) ? 1 : 0);
        return true;
    }
    return false;
}

// ===========================================================================
//  PER-CHARACTER OPTIONS (2026-10-22)
//
//  bits:
//    2*i + 0 : slot i option 1 - the extra item contributes stats/weight/coverage
//    2*i + 1 : slot i option 2 - slot disabled: hide mode keeps the vanilla item
//    10      : backpack appearance hidden
//
//  Persisted per character in DualEquip_optionmap.txt (game dir), keyed by the
//  character's InstanceID.uid (the save's stable instance id).
// ===========================================================================
#define DE_OPT_BAG 10
#define DE_OPT_HIDDEN 11   // Insert / [I]: 1 = this character's equipment is hidden
static CRITICAL_SECTION g_optLock;
static bool g_optLockReady = false;

struct CharOptEntry { void* ch; char uid[96]; unsigned short bits; bool used; };
static CharOptEntry g_charOpts[256];
static std::map<std::string, unsigned short> g_uidBits;
static volatile LONG g_optDirty = 0;

typedef void* (*fn_getInstanceID_t)(void* rootObject);
static fn_getInstanceID_t p_getInstanceID = nullptr;
typedef const MsvcString* (*fn_getLayoutID_t)(void* rootObject);
static fn_getLayoutID_t p_getLayoutInstanceID = nullptr;
typedef const void* (*fn_getHandle_t)(const void* obj);
static fn_getHandle_t p_getHandle = nullptr;
typedef bool (*fn_isPlayerRoot_t)(void* playerInterface, void* rootObject);
static fn_isPlayerRoot_t p_isPlayerRoot = nullptr;

static bool IsReadableRange(const void* p, size_t bytes);

static void ReadCharacterUid(void* ch, char* out, size_t outSize)
{
    out[0] = 0;
    if (!ch) return;

    void* id = nullptr;
    if (p_getInstanceID) id = p_getInstanceID(ch);
    const char* u = nullptr;
    if (id)
    {
        const MsvcString* s = (const MsvcString*)id;   // InstanceID::uid at +0
        u = s->c_str();
    }

    if (!u || !*u)
    {
        // getInstanceID can be null for objects that were never saved; the
        // layout instance string is stable per object and is what the game
        // uses to prefix this object's UI widgets.
        if (p_getLayoutInstanceID)
        {
            const MsvcString* ls = p_getLayoutInstanceID(ch);
            if (ls) u = ls->c_str();
        }
    }

    if (!u || !*u)
    {
        // Stable key: ask the game for the character's own hand object, then
        // read its fields (type/container/containerSerial/index/serial).
        // No string/CRT calls - just a getter and plain memory reads.
        const void* hand = nullptr;
        if (p_getHandle)
        {
            __try { hand = p_getHandle(ch); }
            __except (EXCEPTION_EXECUTE_HANDLER) { hand = nullptr; }
        }
        if (!hand) hand = (const uint8_t*)ch + 0x58;
        if (IsReadableRange(hand, 0x20))
        {
            const unsigned int f0 = *(const unsigned int*)((const uint8_t*)hand + 0x08);
            const unsigned int f1 = *(const unsigned int*)((const uint8_t*)hand + 0x0C);
            const unsigned int f2 = *(const unsigned int*)((const uint8_t*)hand + 0x10);
            const unsigned int f3 = *(const unsigned int*)((const uint8_t*)hand + 0x14);
            const unsigned int f4 = *(const unsigned int*)((const uint8_t*)hand + 0x18);
            if (f0 || f1 || f2 || f3 || f4)
            {
                sprintf_s(out, outSize, "H%u_%u_%u_%u_%u", f0, f1, f2, f3, f4);
                static LONG s_hfLog = 0;
                if (InterlockedIncrement(&s_hfLog) <= 10)
                    LogF("[opts] hand ch=%p hand=%p -> '%s'", ch, hand, out);
                return;
            }
        }
    }

    static LONG s_uidLog = 0;
    if (InterlockedIncrement(&s_uidLog) <= 12)
        LogF("[opts] uid ch=%p id=%p str='%s'", ch, id, (u && *u) ? u : "");
    if (!u || !*u) return;
    strncpy_s(out, outSize, u, _TRUNCATE);
}

static unsigned short DefaultBits()
{
    // Default for a character with no stored entry: every extra slot is
    // COSMETIC ([1] bright = no stats). The old global mask in
    // DualEquip_settings.txt no longer drives defaults; [1] is per character.
    return 0;
}

static CharOptEntry* FindOrCreateOpt(void* ch)
{
    if (!ch) return nullptr;
    if (g_optLockReady) EnterCriticalSection(&g_optLock);
    CharOptEntry* found = nullptr;
    const size_t idx = ((uintptr_t)ch >> 4) & 255;
    for (size_t p = 0; p < 16; ++p)
    {
        CharOptEntry& e = g_charOpts[(idx + p) & 255];
        if (e.used && e.ch == ch)
        {
            // The uid may not have existed when this character was first seen
            // (recalc runs during load). Pick it up as soon as it appears.
            if (e.uid[0] == 0)
            {
                ReadCharacterUid(ch, e.uid, sizeof(e.uid));
                if (e.uid[0])
                {
                    std::map<std::string, unsigned short>::const_iterator it = g_uidBits.find(e.uid);
                    if (it != g_uidBits.end()) e.bits = it->second;
                }
            }
            found = &e; break;
        }
        if (!e.used)
        {
            e.used = true; e.ch = ch; e.bits = 0;
            ReadCharacterUid(ch, e.uid, sizeof(e.uid));
            unsigned short bits = DefaultBits();
            if (e.uid[0])
            {
                std::map<std::string, unsigned short>::const_iterator it = g_uidBits.find(e.uid);
                if (it != g_uidBits.end()) bits = it->second;
            }
            e.bits = bits;
            found = &e;
            break;
        }
    }
    if (g_optLockReady) LeaveCriticalSection(&g_optLock);
    return found;
}

static unsigned short GetBits(void* ch)
{
    CharOptEntry* e = FindOrCreateOpt(ch);
    return e ? e->bits : DefaultBits();
}

static bool CharSlotStats(void* ch, int slot)
{
    if (slot < 0 || slot >= kSectionCount) return true;
    return (GetBits(ch) >> (2 * slot)) & 1;
}

static bool CharSlotDisabled(void* ch, int slot)
{
    if (slot < 0 || slot >= kSectionCount) return false;
    return (GetBits(ch) >> (2 * slot + 1)) & 1;
}

static void SetBits(void* ch, unsigned short bits)
{
    CharOptEntry* e = FindOrCreateOpt(ch);
    if (!e || e->bits == bits) return;
    if (g_optLockReady) EnterCriticalSection(&g_optLock);
    e->bits = bits;
    if (e->uid[0]) g_uidBits[e->uid] = bits;
    if (g_optLockReady) LeaveCriticalSection(&g_optLock);
    InterlockedExchange(&g_optDirty, 1);
}

static void LoadOptionMap()
{
    g_uidBits.clear();
    FILE* f = fopen("DualEquip_optionmap.txt", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char* hash = strchr(line, '#'); if (hash) *hash = 0;
        char uid[96] = { 0 }; int bits = 0;
        if (sscanf_s(line, " %95s %d", uid, (unsigned)sizeof(uid), &bits) == 2)
        {
            if (uid[0]) g_uidBits[uid] = (unsigned short)bits;
        }
    }
    fclose(f);
    LogF("[opts] loaded %u per-character entries", (unsigned)g_uidBits.size());
}

static void FlushOptionMap()
{
    if (!InterlockedExchange(&g_optDirty, 0)) return;
    if (g_optLockReady) EnterCriticalSection(&g_optLock);
    LogF("[opts] flush %u entries", (unsigned)g_uidBits.size());
    FILE* f = fopen("DualEquip_optionmap.txt.tmp", "w");
    if (f)
    {
        fprintf(f, "# DualEquip per-character options: <instance uid> <bits>\n");
        for (std::map<std::string, unsigned short>::const_iterator it = g_uidBits.begin();
             it != g_uidBits.end(); ++it)
            fprintf(f, "%s %u\n", it->first.c_str(), (unsigned)it->second);
        fclose(f);
        MoveFileExA("DualEquip_optionmap.txt.tmp", "DualEquip_optionmap.txt", MOVEFILE_REPLACE_EXISTING);
    }
    if (g_optLockReady) LeaveCriticalSection(&g_optLock);
}

extern "C" __declspec(dllexport) int DE_GetOptionBits(void* character)
{
    return (int)GetBits(character);
}

// Called by DualEquip2 whenever Insert / Right Shift / the [I] button changes
// the hide state. Keeping it in the same bit field as [1]/[2]/[X] means the
// sidecar persists it and the panel can show it bright/dim.
extern "C" __declspec(dllexport) void DE_SetCharHidden(void* character, int hidden)
{
    if (!character) return;
    unsigned short b = GetBits(character);
    if (hidden) b |= (unsigned short)(1u << DE_OPT_HIDDEN);
    else        b &= (unsigned short)~(1u << DE_OPT_HIDDEN);
    SetBits(character, b);
}

// ===========================================================================
//  WHICH HOOKS ARE LIVE (2026-10-05 final state)
//
//  REGISTERED - each proven to fire and each needed:
//    Inventory::Inventory          - creates our slots on EVERY inventory the
//                                    game builds, incl. during a save load, and
//                                    always BEFORE items are placed. This is what
//                                    makes items in our slots survive a load.
//    Inventory::getSection         - creates a section on demand if something
//                                    looks it up by name before it exists.
//    Inventory::getSectionOfType   - MANDATORY guard. Without it the engine hands
//                                    out one of our sections for a vanilla slot
//                                    type and equipping crashes the game.
//    wraps::BaseLayout::initialise - pure PASS-THROUGH. We ship our layout under
//                                    the vanilla file name, so no redirect is
//                                    needed. A redirect here crashes the game
//                                    ("No root widget", measured 2026-10-05).
//
//  NOT REGISTERED (kept as functions only so the history stays readable):
//    Character::_NV_init                  - installed OK but NEVER fired
//    Character::setupInventorySections    - never fired before GetRealAddress fix,
//                                           superseded by the ctor hook
//    InventorySection::addItem            - diagnostic only
//    Item::_loadFromSerialise             - diagnostic only
//    vtable patching experiments          - superseded by GetRealAddress
//
//  REMOVING A HOOK FROM startPlugin() IS THE ONLY THING THAT MATTERS; leaving the
//  function defined costs nothing and keeps the record of what was measured.
// ===========================================================================

// ===========================================================================
//  Section creation - runs from Character::_NV_init so the widgets exist
// ===========================================================================
static void EnsureExtraSections(void* inv, bool enableExisting = false)
{
    if (!inv || !p_getSection || !p_initSection) return;

    for (int i = 0; i < kSectionCount; ++i)
    {
        const SectionInfo& s = kSections[i];
        MsvcString name; name.set(s.name);

        // Idempotent: getSection(name) is non-null once we made it. Our names are
        // unique to this mod, so a hit always means "already created".
        void* existing = p_getSection(inv, &name);
        if (existing)
        {
            // The NPC cleanup disables extra sections. If this inventory now
            // belongs to a player character (e.g. the NPC was recruited), turn
            // its sections back on so the player can actually use them.
            if (enableExisting)
            {
                bool wasEnabled = *(bool*)((uint8_t*)existing + 0xD0);
                *(bool*)((uint8_t*)existing + 0xD0) = true;   // enabled
                *(bool*)((uint8_t*)existing + 0xB4) = true;   // isAnEquippedItemSection
                if (!wasEnabled)
                {
                    InterlockedExchange(&g_sectionsReenabled, 1);
                    static LONG s_reLog = 0;
                    if (InterlockedIncrement(&s_reLog) <= 12)
                        LogF("[slots] re-enabled player extra sections inv=%p", inv);
                }
            }
            continue;
        }

        void* sec = p_initSection(inv, &name, s.w, s.h, s.slot,
                                  /*equipCallbacks*/ true,
                                  /*isContainerSlot*/ false,
                                  /*enabled*/ true,
                                  /*limit*/ s.limit);
        if (!sec)
        {
            LogF("[slots] FAILED to create section '%s'", s.name);
            continue;
        }

        // Guard against the constructor using swapped dims (the reference plugin
        // saw this; a wrong size makes later Array2d access go out of range).
        int gotW = *(int*)((uint8_t*)sec + off::Section_width);
        int gotH = *(int*)((uint8_t*)sec + off::Section_height);
        if ((gotW != s.w || gotH != s.h) && p_resizeSection)
        {
            LogF("[slots] section '%s' wrong size %dx%d, resizing to %dx%d",
                 s.name, gotW, gotH, s.w, s.h);
            p_resizeSection(inv, sec, s.w, s.h, true);
        }
        ++g_created;
    }
}



// ===========================================================================
//  SAVE/LOAD SECTION SETUP - the fix for "items in extra slots vanish on load"
//
//  Symptom (measured 2026-10-04/05): items placed in de_slot_* ARE written to
//  the save (the .platoon file contains
//      "company sid....section....de_slot_head....base data sid...."
//  records), but after loading they are GONE from the world entirely.
//
//  Cause: the game deserialises inventory items during load and looks up the
//  section named in each record. Our sections did not exist yet at that moment -
//  they were only created later by the 500 ms polling thread - so the items had
//  nowhere to go and were discarded.
//
//  Fix: hook Character::setupInventorySections(GameSaveState*). This VIRTUAL
//  function is what builds a character's sections while a save is being loaded.
//  We call it first (so the vanilla sections exist), then add ours, so the
//  extra sections are present BEFORE items are placed.
//
//  NOTE: we hook the VIRTUAL variant (?setupInventorySections@Character@@UEAA_).
//  The non-virtual ?_NV_ variant of other Character methods never fired (they
//  are inlined), so the virtual one is the one that actually runs.
// ===========================================================================
typedef bool (*fn_setupInvSections_t)(void* character, void* saveState);
static fn_setupInvSections_t p_setupInvSectionsOrig = nullptr;
static bool g_setupFired = false;

static bool __cdecl Detour_setupInventorySections(void* character, void* saveState)
{
    // 1) Let the game build its own sections FIRST.
    bool r = true;
    if (p_setupInvSectionsOrig) r = p_setupInvSectionsOrig(character, saveState);

    // 2) Now add ours, so the deserialiser can find them.
    //    Player characters only - this is the reliable per-character creation path.
    if (character && (!p_isPlayerChar || p_isPlayerChar(character)))
    {
        void* inv = *(void**)((uint8_t*)character + off::Character_inventory);
        int before = g_created;
        EnsureExtraSections(inv, true);
        if (!g_setupFired)
        {
            g_setupFired = true;
            LogF("[slots] setupInventorySections fired; +%d sections, total %d",
                 g_created - before, g_created);
        }
    }
    return r;
}

// ===========================================================================
//  INVENTORY CONSTRUCTOR HOOK - create sections the moment an Inventory exists.
//
//  Rationale: both Character::_NV_init and Character::setupInventorySections
//  installed fine but never fired, and the polling thread is always too late
//  for a save load (items are deserialised before it notices the world).
//
//  Inventory::Inventory(RootObject*) runs for EVERY inventory the game builds,
//  including the ones created while loading a save, and always BEFORE items are
//  placed into it. Calling the original first leaves the object fully
//  constructed, so adding our sections afterwards is safe.
// ===========================================================================
typedef void* (*fn_invCtor_t)(void* self, void* owner);
static fn_invCtor_t p_invCtorOrig = nullptr;

// ---------------------------------------------------------------------------
//  Is this inventory one of a player character's?
//
//  WHY THIS EXISTS (root-caused 2026-10-06, confirmed by probe logs):
//  the ctor hook used to add our 5 sections to EVERY inventory in the game -
//  shops and storage buildings included. Probe output proved shop inventories
//  really did contain de_slot_* sections, and the user's symptom was a shop
//  whose item grid had far too few cells (and, earlier, empty stock).
//  Shop UI code sizes its grid from the section list, so extra sections on a
//  shop inventory corrupt it.
//
//  Fix: only ever create sections on inventories owned by a player character.
//  The world pointer / player interface are resolved lazily because the ctor
//  hook can fire before the world exists.
// ---------------------------------------------------------------------------
static void** g_ppOu = nullptr;                  // address of the game's ?ou global

static bool IsPlayerCharacterInventory(void* inv)
{
    if (!inv || !g_ppOu) return false;

    void* gw = *g_ppOu;                          // GameWorld::ou
    if (!gw) return false;
    void* piface = *(void**)((uint8_t*)gw + 0x580);   // GameWorld::player
    if (!piface) return false;

    lektor<void*>& players = *(lektor<void*>*)((uint8_t*)piface + 0x2B0);
    //                  ^ PlayerInterface::playerCharacters

    for (uint32_t i = 0; i < players.count; ++i)
    {
        void* ch = players.stuff[i];
        if (!ch) continue;
        if (p_isPlayerChar && !p_isPlayerChar(ch)) continue;
        if (*(void**)((uint8_t*)ch + off::Character_inventory) == inv) return true;
    }
    return false;
}

// InventorySection::addItem, VIRTUAL override. Unused now - kept because the
// symbol was verified to resolve, which is useful if the section-content
// question ever comes up again (see 文档/研究_屏蔽装备数值_可行性结论.md).
typedef bool (*fn_sectionAddItemVirt_t)(void* section, void* item, int a);
static fn_sectionAddItemVirt_t p_sectionAddItemVirtOrig = nullptr;

static void* __cdecl Detour_inventoryCtor(void* self, void* owner)
{
    static LONG s_calls = 0;
    LONG n = InterlockedIncrement(&s_calls);

    void* r = nullptr;
    if (p_invCtorOrig) r = p_invCtorOrig(self, owner);
    else                r = self;

    void* inv = r ? r : self;
    if (!inv) return r;

    // ------------------------------------------------------------------------
    //  WHO GETS SECTIONS?  (v5, 2026-10-07)
    //
    //  The rule is deliberately split by PHASE, because one rule cannot satisfy
    //  both and every single-rule attempt broke something:
    //
    //    DURING A SAVE LOAD -> give sections to everything.
    //      The save loader resolves de_slot_* records by name before the player
    //      character list exists, so it must find our sections already there.
    //      Gating here is exactly what made items vanish (v4, v2).
    //
    //    DURING NORMAL PLAY -> give sections only to player characters.
    //      Extra sections on shops / storage / UI inventories corrupt the UIs
    //      that size themselves from the section list (shop grid, limb window).
    //      Creating them there is exactly what caused the shop bug (v1) and the
    //      limb duplication (v3).
    //
    //  isPlayerCharacterInventory is only meaningful in the second phase; during
    //  a load the player list is empty, hence the phase test comes FIRST.
    //
    //  History, so this does not get re-broken:
    //    v1 unconditional      -> shops got sections; shop grid corrupted
    //    v2 player-list lookup -> save/load broke (list is EMPTY during load)
    //    v3 owner guess        -> garbage bool on non-Character owners; limb
    //                             duplication + 580->1035 section inflation
    //    v4 list lookup only   -> limb bug gone, save/load broke again
    //    v5 phase split (this) -> load: all; play: player characters only
    // ------------------------------------------------------------------------
    // v6: UNCONDITIONAL again.
    //
    // This is the only configuration ever proven to keep BOTH save/load and the
    // limb window working (it is v1's behaviour). The shop-grid damage it causes
    // is neutralised separately - see the getWidget hook, which stops ANY layout
    // from crashing on a section it has no widget for.
    //
    // Why every "smart" gate failed (all four measured, not guessed):
    //   * During a save load, PlayerInterface::playerCharacters is EMPTY - the
    //     diagnostic on 2026-10-07 showed players=0 for every one of the first
    //     60 inventories and 335 inventories total. So no lookup can tell a
    //     character inventory from a shop inventory at that moment.
    //   * Asking the game "is this owner a Character?" returns a garbage bool
    //     for non-Character owners (580 -> 1035 sections in 39 s).
    //   * Restricting to the (empty) list breaks save/load, because the save
    //     loader resolves de_slot_* records by name before characters exist.
    bool give = IsPlayerCharacterInventory(inv);

    // Permanent (cheap) verification counter. "NPCs no longer get slots" rests on
    // a code-level argument rather than a measurement, so this records what the
    // runtime gate actually does - one line per threshold, nothing per call.
    if (give)
    {
        static LONG s_playGiven = 0;
        LONG g = InterlockedIncrement(&s_playGiven);
        if (g == 1 || g == 50 || g == 200)
            LogF("[slots] play-phase: %ld player-character inventories given slots (sec=%d)",
                 g, g_created);
    }

    if (give)
    {
        int before = g_created;
        EnsureExtraSections(inv, true);
        if (g_created != before && n <= 6)
        {
            LogF("[slots] Inventory ctor #%ld inv=%p -> +%d sections (total %d)",
                 n, inv, g_created - before, g_created);
        }
    }
    return r;
}

// ===========================================================================
//  THREAD FALLBACK
//  On 2026-10-04 the Character::_NV_init hook installed with HookStatus=0 but
//  produced NO log lines even with a world demonstrably loaded (kenshi.log was
//  streaming mesh loads and save.log was 77 MB yet kCreated stayed 0). Rather
//  than depend on a hook we cannot confirm fires, poll for player characters
//  from a thread as well. Creating sections from a thread is known-safe here:
//  the earlier synthetic test created and registered a section off-thread with
//  no crash, and EnsureExtraSections is idempotent per inventory.
//  It does NOT create MyGUI widgets, so the open-bag crash cause does not apply.
// ===========================================================================
static void*  g_lastInventory = nullptr;
static bool   g_threadSawWorld = false;

// Defined with the coverage hook further down; keeps the hit registry fresh.
static void RefreshExtraItemRegistry();

static void PollForWorld()
{
    // ?ou is only populated once a world exists. We resolved the ADDRESS of the
    // global capsule at startPlugin time; re-read it every pass because it is
    // null before a world loads (treating it as a precondition caused a
    // deadlock-style bug in an earlier revision).
    if (!g_ppOu || !*g_ppOu) return;
    uint8_t* gw = (uint8_t*)*g_ppOu;

    void* piface = *(void**)(gw + 0x580);      // GameWorld::player  (GameWorld.h:137)
    if (!piface) return;

    lektor<void*>& players = *(lektor<void*>*)(
        (uint8_t*)piface + 0x2B0);             // PlayerInterface::playerCharacters
    if (!players.stuff || players.count == 0) return;

    if (!g_threadSawWorld)
    {
        g_threadSawWorld = true;
        LogF("[slots] thread fallback: world detected, players=%u", players.count);
    }

    RefreshExtraItemRegistry();          // hit-time coverage registry (1 s throttle)
    FlushOptionMap();                    // per-character options (only when dirty)

    // Per-slot stats switches (hot reload). A change must be pushed into every
    // character's CharStats, otherwise the panel would only update on the next
    // equipment change.
    if (LoadSettings())
    {
        int refreshed = 0;
        for (uint32_t i = 0; i < players.count; ++i)
        {
            void* ch = players.stuff[i];
            if (!ch) continue;
            if (p_isPlayerChar && !p_isPlayerChar(ch)) continue;
            Detour_recalcEquip(ch);
            ++refreshed;
        }
        LogF("[settings] refreshed %d player characters after stats-mask change", refreshed);
    }

    // Process a few characters per pass so we never stall the thread.
    for (uint32_t i = 0; i < players.count && i < 4; ++i)
    {
        void* ch = players.stuff[i];
        if (!ch) continue;
        if (p_isPlayerChar && !p_isPlayerChar(ch)) continue;

        void* inv = *(void**)((uint8_t*)ch + off::Character_inventory);
        if (!inv || inv == g_lastInventory) continue;

        int before = g_created;
        EnsureExtraSections(inv, true);
        g_lastInventory = inv;

        if (g_created > before)
            LogF("[slots] thread fallback created %d sections on inventory %p (total %d)",
                 g_created - before, inv, g_created);
    }
}

static DWORD WINAPI SlotsThread(LPVOID)
{
    for (;;)                            // no iteration cap: the old 2400-loop
    {                                   // stopped flushing options after ~20 min
        Sleep(500);
        __try { PollForWorld(); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogF("[slots] guarded exception in thread fallback - stopping poll");
            return 0;
        }
    }
    return 0;
}

// ===========================================================================
//  getSectionOfType guard - MUST exist or equipping crashes the game.
//  The engine asks "which section holds an item of slot type X". If it gets one
//  of our 1x1 sections it will try to place items there and go out of bounds.
//  We always prefer the vanilla section for any slot type we mirror.
// ===========================================================================
static void* FindVanillaForSlot(void* inv, int slot)
{
    if (!inv || !p_getSection) return nullptr;
    const char* want = nullptr;
    switch (slot)
    {
        case ATTACH_HAT:    want = "head";   break;
        case ATTACH_SHIRT:  want = "shirt";  break;
        case ATTACH_LEGS:   want = "legs";   break;
        case ATTACH_BODY:   want = "armour"; break;
        case ATTACH_BOOTS:  want = "boots";  break;
        case ATTACH_BELT:   want = "belt";   break;
        default: return nullptr;             // eyes/neck/gloves are NOT mirrored
    }
    MsvcString n; n.set(want);
    return p_getSection(inv, &n);
}

// ===========================================================================
//  Inventory::getSection(name) hook - the on-demand section creator.
//
//  Why here: this is the function the game itself uses to look a section up BY
//  NAME, and our FindVanillaForSlot proves it is a live, non-inlined call (the
//  reference plugin relies on it too). If the save loader asks for
//  "de_slot_head" while restoring an equipped item, hooking here lets us create
//  the section on the spot instead of letting the lookup fail and the item be
//  dropped.
//
//  Measured context: items ARE written to the save (the .platoon contains
//  "section....de_slot_head....base data sid...." records) but vanish on load,
//  because the section did not exist at that moment - our polling thread only
//  creates them ~60 s after the world appears.
// ===========================================================================
typedef void* (*fn_getSection_t)(void* inv, const MsvcString* name);
static fn_getSection_t p_getSectionOrig = nullptr;
static LONG g_getSectionCalls = 0;
static LONG g_getSectionOurName = 0;

static void UiTickOnce(const char* source);

// A backpack/container/trade inventory is owned by the item or trader, not by
// a Character. During play the on-demand getSection hook used to create our
// 1x1 sections in THOSE inventories too; the right-click equip then picked the
// 1x1 section and destroyed the item. Ask the owner object's virtual
// getDataType() (RootObjectBase vtable offset 0x20) - itemType CHARACTER == 1.
static bool IsCharacterOwnedInventory(void* inv)
{
    if (!inv) return false;
    void* owner = nullptr;
    __try { owner = *(void**)((uint8_t*)inv + 0x88); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!owner) return false;
    __try
    {
        void** vt = *(void***)owner;
        if (!vt) return false;
        typedef int (*fn_getType_t)(void*);
        fn_getType_t f = (fn_getType_t)vt[4];      // vtable offset 0x20
        if (f) return f(owner) == 1;               // itemType::CHARACTER
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    return false;
}

static void* __cdecl Detour_getSection(void* inv, const MsvcString* name)
{
    InterlockedIncrement(&g_getSectionCalls);

    const char* nm = (name && name->c_str()) ? name->c_str() : "?";
    const bool ours = IsOurSectionName(nm);
    if (ours) InterlockedIncrement(&g_getSectionOurName);

    void* sec = p_getSectionOrig ? p_getSectionOrig(inv, name) : nullptr;


    // On-demand creation: if anyone (including the save loader) asks for one of
    // our section names and it does not exist yet, build it right now.
    // Only log the FIRST time per section name - this runs many times per second.
    if (!sec && ours && inv && p_initSection &&
        IsPlayerCharacterInventory(inv))
    {
        for (int i = 0; i < kSectionCount; ++i)
        {
            if (strcmp(kSections[i].name, nm) != 0) continue;
            MsvcString want; want.set(kSections[i].name);
            sec = p_initSection(inv, &want, kSections[i].w, kSections[i].h,
                                kSections[i].slot, true, false, true, kSections[i].limit);
            if (sec)
            {
                ++g_created;
                static bool s_logged[8] = { false };
                if (i < 8 && !s_logged[i])
                {
                    s_logged[i] = true;
                    LogF("[slots] ON-DEMAND created '%s' (getSection miss) - total %d",
                         kSections[i].name, g_created);
                }
            }
            break;
        }
    }

    return sec;
}

// ===========================================================================
//  Character::loadFromSerialise - per-character section creation (v7).
//
//  THE REAL FIX for "extra slots on non-character inventories".
//
//  Every previous attempt tried to answer "is this inventory a character's?"
//  from an Inventory* alone, and every one failed:
//    * during a save load PlayerInterface::playerCharacters is EMPTY
//      (measured 2026-10-07: players=0 for all of the first 60 inventories),
//    * asking the owner "are you a Character?" returns garbage for other types,
//    * the player list, the layout list and the section list are all empty or
//      wrong at exactly the moment the answer is needed.
//
//  This hook removes the question entirely. It is called once per character
//  while a save is being read and hands us the Character directly, so the
//  inventory we must extend is unambiguous - the same guarantee that makes
//  Character::setupInventorySections reliable.
//
//  Shops, storage buildings and UI inventories are not characters and are never
//  passed through here, so they no longer receive de_slot_* sections. That is
//  what fixes the limb duplication and the missing shop stock.
// ===========================================================================
static volatile LONG g_uiResetRequest = 0;   // set by the save-load hook, consumed by the UI tick

typedef void (*fn_charLoadFromSerialise_t)(void* character, void* saveState);
static fn_charLoadFromSerialise_t p_charLoadFromSerialiseOrig = nullptr;
static LONG g_charLoadRuns = 0;
#define DE_MAX_PENDING 2048
static void* g_pendingChars[DE_MAX_PENDING];
static volatile LONG g_pendingCount = 0;

static bool GameIsLoadingSave()
{
    if (!p_isLoadingSave || !g_ppOu || !*g_ppOu) return false;
    __try { return ((bool (__cdecl*)(void*))p_isLoadingSave)(*g_ppOu); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void __cdecl Detour_charLoadFromSerialise(void* character, void* saveState)
{
    // IMPORTANT: this hook also fires for characters spawned at runtime
    // (squads entering the area). Only a REAL save load may wipe the
    // pointer-keyed tables - otherwise the per-character Insert/Right Shift
    // overrides are cleared a few seconds after the player pressed the key.
    const bool realLoad = GameIsLoadingSave();

    static DWORD s_windowStart = 0;
    static int   s_windowCount = 0;
    const DWORD nowWin = GetTickCount();
    if (nowWin - s_windowStart > 4000) { s_windowStart = nowWin; s_windowCount = 0; }
    ++s_windowCount;
    const bool burstLoad = (s_windowCount >= 60);      // fallback: full save burst
    const bool doReset = realLoad || burstLoad;

    static LONG s_loadLog = 0;
    if (InterlockedIncrement(&s_loadLog) <= 24)
        LogF("[opts] loadFromSerialise char=%p isLoadingSave=%d burst=%d",
             character, (int)realLoad, s_windowCount);

    if (!doReset)
    {
        if (p_charLoadFromSerialiseOrig) p_charLoadFromSerialiseOrig(character, saveState);
        return;
    }

    InterlockedExchange(&g_uiResetRequest, 1);   // save load: rebuild UI bindings

    static DWORD s_lastTableClear = 0;
    const DWORD nowClear = GetTickCount();
    if (nowClear - s_lastTableClear > 2000)
    {
        s_lastTableClear = nowClear;
        if (g_optLockReady) EnterCriticalSection(&g_optLock);
        memset(g_charOpts, 0, sizeof(g_charOpts));
        if (g_optLockReady) LeaveCriticalSection(&g_optLock);
        LogF("[opts] table cleared for save load");

        typedef void (*fn_resetStates_t)();
        static fn_resetStates_t s_resetStates = nullptr;
        if (!s_resetStates)
        {
            HMODULE hm = GetModuleHandleA("DualEquip2.dll");
            if (hm) s_resetStates = (fn_resetStates_t)GetProcAddress(hm, "DE_ResetCharStates");
        }
        if (s_resetStates) s_resetStates();
    }
    if (p_charLoadFromSerialiseOrig) p_charLoadFromSerialiseOrig(character, saveState);

    if (!character) return;

    void* inv = *(void**)((uint8_t*)character + off::Character_inventory);
    int before = g_created;
    EnsureExtraSections(inv);

    // Queue this character for the post-load NPC cleanup. Player characters
    // are skipped by the cleanup; NPCs get their extra-slot items moved out
    // and their sections disabled.
    {
        const LONG pslot = InterlockedIncrement(&g_pendingCount);
        if (pslot <= DE_MAX_PENDING) g_pendingChars[pslot - 1] = character;
        else InterlockedDecrement(&g_pendingCount);
    }

    LONG run = InterlockedIncrement(&g_charLoadRuns);
    if (run <= 6)
        LogF("[slots] Character::loadFromSerialise #%ld char=%p inv=%p +%d (total %d)",
             run, character, inv, g_created - before, g_created);
}






// ===========================================================================
//  InventoryLayout::getWidget - crash guard (kept from v6).
//
//  THE PROBLEM
//  Every inventory window builds one GUI object per inventory section, and it
//  finds that section's widget by asking the layout for a widget with the
//  section's name. We add five sections (de_slot_*) to inventories that some
//  layouts have no widget for:
//
//      Kenshi_LimbReplacementWindow.layout   -> no de_slot_* widgets
//      Kenshi_InventoryTraderWindow.layout   -> no de_slot_* widgets
//      ... and every other layout we cannot ship an override for
//
//  MyGUI answers a missing widget with MYGUI_ASSERT, which aborts the process:
//      Widget with name 'de_slot_head' not found.
//      Critical | Can't assign widget with name 'de_slot_head'.
//
//  THE FIX
//  Intercept the lookup. For OUR section names only, when the layout genuinely
//  has no such widget, hand back the layout's own root widget instead of letting
//  MyGUI assert. That widget is a real, live MyGUI::Widget, so the caller has
//  something valid to attach to, and the extra slot simply draws nowhere in
//  windows that do not know about it. In the character window - which DOES ship
//  de_slot_* widgets - the normal widget is returned and everything behaves as
//  before.
//
//  Why this is better than gating section creation: it needs no knowledge of
//  which inventory belongs to whom, so it cannot regress save/load, shops or
//  limbs the way the five previous attempts did.
// ===========================================================================
typedef void* (*fn_layoutGetWidget_t)(void* layout, const MsvcString* name);
static fn_layoutGetWidget_t p_layoutGetWidget = nullptr;
typedef void* (*fn_layoutGetWindow_t)(void* layout);
static fn_layoutGetWindow_t p_layoutGetWindow = nullptr;

// Captured MyGUI widgets for our five sections (filled by the getWidget hook).
// The EXTRA SET panel is a SEPARATE top-level window in the layout, so walking
// down from InventoryLayout::getWindow() never reaches it - we take the widget
// pointers straight from the layout manager instead.
static MyGUI::Widget* g_slotWidgets[8] = { nullptr };

static void* __cdecl Detour_layoutGetWidget(void* layout, const MsvcString* name)
{
    void* w = p_layoutGetWidget ? p_layoutGetWidget(layout, name) : nullptr;
    if (w || !layout || !name) return w;

    // Only stand in for OUR section names; anything else keeps vanilla behaviour.
    const char* nm = name->c_str();
    if (!nm || !IsOurSectionName(nm)) return w;

    void* root = p_layoutGetWindow ? p_layoutGetWindow(layout) : nullptr;

    static LONG s_log = 0;
    if (InterlockedIncrement(&s_log) <= 12)
        LogF("[slots] layout has no widget for '%s' -> substitute %s",
             nm, root ? "root widget" : "null (no root!)");

    return root;                     // may be null; preferable to an abort
}


//
//  The engine routes equip attempts through Inventory::getSectionOfType(slot).
//  Because our extra sections carry the SAME AttachSlot as their vanilla
//  counterpart (de_slot_head is ATTACH_HAT, etc.), the engine can pick one of
//  ours when it means to touch the real equipment slot. If it then writes
//  through that wrong section the game goes out of bounds.
//
//  So: whenever the engine picks one of our sections, hand back the VANILLA
//  section for that slot instead.
//
//  Verified correct on 2026-10-06 with a probe that logged the value actually
//  returned (not the raw engine result, which is misleading): over a full
//  session every single redirection hit "engine gets VANILLA" - zero leaks.
//
//  ---------------------------------------------------------------------------
//  LIMB SECTIONS EXCEPTION (added 2026-10-06, after a user bug report)
//
//  Reported: "if I try to equip a robotic limb it gets deleted", and "if I load
//  a save with a limb replacement already in the slot and try to remove it, it
//  gets duplicated".
//
//  The limb-replacement window uses four special sections whose names end in
//  "_replace" (left_arm_replace, right_arm_replace, left_leg_replace,
//  right_leg_replace - the names are present verbatim in Kenshi_x64.exe).
//  Those sections apparently share an AttachSlot enum value with a vanilla
//  equipment slot, so this guard was redirecting limb lookups to the vanilla
//  armour section. The limb UI then held a section that does not own the limb
//  item: equipping lost it, removing duplicated it.
//
//  Fix: never redirect a section whose name ends in "_replace". Identify them
//  by NAME, not by slot number, because we do not know which slot value the
//  engine uses for limbs and must not guess.
// ===========================================================================
static bool IsLimbReplaceSection(const char* n)
{
    if (!n) return false;
    const size_t len = strlen(n);
    const char* suffix = "_replace";
    const size_t slen = strlen(suffix);
    if (len < slen) return false;
    return strcmp(n + (len - slen), suffix) == 0;
}

typedef void (*fn_charEquipItem_t)(void* ch, const MsvcString* slot, void* item);
static fn_charEquipItem_t p_charEquipItemOrig = nullptr;

static void __cdecl Detour_charEquipItem(void* ch, const MsvcString* slot, void* item)
{
    const char* nm = (slot && slot->c_str()) ? slot->c_str() : "";
    const bool loading = GameIsLoadingSave();
    static LONG s_eq2Log = 0;
    if (!loading && nm[0] && InterlockedIncrement(&s_eq2Log) <= 60)
        LogF("[equip2] ch=%p slot='%s' item=%p", ch, nm, item);

    // Do NOT rewrite de_slot_* slot names: manual equipping into the extra
    // slots and the save loader both use them legitimately. The backpack item
    // loss was fixed by not creating our sections in container inventories.
    if (p_charEquipItemOrig) p_charEquipItemOrig(ch, slot, item);
}

typedef bool (*fn_invAddItem_t)(void* inv, void* item, int qty, bool dropOnFail, bool destroyOnFail);
static fn_invAddItem_t p_invAddItemOrig = nullptr;

static bool __cdecl Detour_invAddItem(void* inv, void* item, int qty, bool drop, bool destroy)
{
    static LONG s_iaLog = 0;
    const bool log = (InterlockedIncrement(&s_iaLog) <= 60) != 0;
    if (log) LogF("[invAdd] inv=%p item=%p qty=%d drop=%d destroy=%d", inv, item, qty, (int)drop, (int)destroy);
    const bool r = p_invAddItemOrig ? p_invAddItemOrig(inv, item, qty, drop, destroy) : false;
    if (log) LogF("[invAdd] done inv=%p item=%p -> %d", inv, item, (int)r);
    return r;
}

typedef bool (*fn_transferMouse_t)(void* inv, void* item);
static fn_transferMouse_t p_transferMouseOrig = nullptr;

static bool __cdecl Detour_transferMouseItem(void* inv, void* item)
{
    static LONG s_tmLog = 0;
    if (InterlockedIncrement(&s_tmLog) <= 40) LogF("[transfer] inv=%p item=%p", inv, item);
    return p_transferMouseOrig ? p_transferMouseOrig(inv, item) : false;
}

typedef bool (*fn_invEquipItem_t)(void* inv, void* item);
static fn_invEquipItem_t p_invEquipItemOrig = nullptr;

static bool __cdecl Detour_invEquipItem(void* inv, void* item)
{
    // The right-click "equip" path uses this function. Our extra sections carry
    // the same AttachSlot as vanilla, so the engine can pick one of them; the
    // item then vanishes while only the appearance is attached. Temporarily
    // mark our sections as non-equipped so this call resolves to the vanilla
    // section, then restore the flag (the extra slots must stay usable).
    void* secs[kSectionCount] = { nullptr };
    unsigned char oldFlag[kSectionCount] = { 0 };
    if (p_getSection)
    {
        for (int i = 0; i < kSectionCount; ++i)
        {
            MsvcString nm; nm.set(kSections[i].name);
            void* sec = p_getSection(inv, &nm);
            if (!sec) continue;
            secs[i] = sec;
            oldFlag[i] = *(unsigned char*)((uint8_t*)sec + off::Section_equipped);
            *(unsigned char*)((uint8_t*)sec + off::Section_equipped) = 0;
        }
    }

    static LONG s_eqLog = 0;
    if (InterlockedIncrement(&s_eqLog) <= 30) LogF("[equip] inv=%p item=%p", inv, item);
    const bool r = p_invEquipItemOrig ? p_invEquipItemOrig(inv, item) : false;
    if (s_eqLog <= 30) LogF("[equip] done inv=%p item=%p -> %d", inv, item, (int)r);

    for (int i = 0; i < kSectionCount; ++i)
        if (secs[i]) *(unsigned char*)((uint8_t*)secs[i] + off::Section_equipped) = oldFlag[i];
    return r;
}

static void* __cdecl Detour_getSectionOfType(void* inv, int slot)
{
    void* sec = p_getSectOfTypeOrig ? p_getSectOfTypeOrig(inv, slot) : nullptr;

    // Diagnostic: the trade window uses temporary inventories; items placed in
    // their sections are destroyed with the window. Log every query that does
    // not target the owner's main inventory, plus every redirect of our section.
    if (inv)
    {
        void* owner = nullptr;
        void* mainInv = nullptr;
        __try
        {
            owner = *(void**)((uint8_t*)inv + 0x88);
            if (owner) mainInv = *(void**)((uint8_t*)owner + 0x2E8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { owner = nullptr; mainInv = nullptr; }

        if (owner && mainInv && inv != mainInv)
        {
            static LONG s_tempLog = 0;
            if (InterlockedIncrement(&s_tempLog) <= 40)
                LogF("[route] TEMP inv=%p owner=%p main=%p slot=%d", inv, owner, mainInv, slot);
        }
    }

    // Never redirect limb-replacement sections - see the note above.
    if (sec)
    {
        const MsvcString* nm = (const MsvcString*)((uint8_t*)sec + off::Section_name);
        if (nm && nm->c_str() && IsLimbReplaceSection(nm->c_str()))
        {
            static LONG s_limbLog = 0;
            if (InterlockedIncrement(&s_limbLog) <= 8)
                LogF("[slots] limb section '%s' passed through (slot=%d, no redirect)",
                     nm->c_str(), slot);
            return sec;
        }
    }

    // If the engine picked one of our sections, redirect to the vanilla one.
    if (sec)
    {
        const MsvcString* nm = (const MsvcString*)((uint8_t*)sec + off::Section_name);
        if (nm && nm->c_str() && IsOurSectionName(nm->c_str()))
        {
            void* vanilla = FindVanillaForSlot(inv, slot);
            static LONG s_redirLog = 0;
            if (InterlockedIncrement(&s_redirLog) <= 30)
                LogF("[route] redirect inv=%p slot=%d ours='%s' -> %p",
                     inv, slot, nm->c_str(), vanilla);
            if (vanilla) return vanilla;
            // The queried inventory (e.g. a backpack container) has no vanilla
            // section for this slot. Returning OUR section here made the
            // right-click equip add the item to a 1x1 container section, fail,
            // and destroy it. Null lets the caller use its normal fallback.
            return nullptr;
        }
        return sec;
    }

    // Engine found nothing (e.g. ATTACH_EYES/NECK/GLOVES with no vanilla section):
    // that is fine, leave it alone so nothing new becomes equippable.
    return sec;
}

// ===========================================================================
//  InventorySection::addItem hook - the deserialisation probe.
//
//  If save-loaded items reach a section at all, they come through here. Logging
//  the section name tells us exactly what the loader does with a record whose
//  "section" is de_slot_head: does it find our section, or does it look for a
//  section that does not exist yet and drop the item?
// ===========================================================================


// ===========================================================================
//  Item::_loadFromSerialise hook - THE deserialisation entry point.
//
//  Found by dumping KenshiLib exports:
//      ?_loadFromSerialise@Item@@UEAAXPEAVGameDataContainer@@PEAVGameData@@@Z
//  This VIRTUAL function is called once per item while a save is being read.
//  The second argument is the GameData record, which is exactly where the save
//  stores "section....de_slot_head" (verified by inspecting the .platoon file).
//
//  So this is the first point where we can see an item that BELONGS to one of
//  our sections, and make sure that section exists before the item is placed.
//  Everything we tried before either never fired (inlined) or fired only later
//  (our own polling thread, long after the load finished).
// ===========================================================================
typedef void (*fn_loadFromSerialise_t)(void* item, void* container, void* gameData);
typedef void* (*fn_sectionGetItem_t)(void* section);
typedef bool (*fn_sectionRemoveItem_t)(void* section, void* item);
typedef void (*fn_sectionSetEnabled_t)(void* section, bool enabled);
static fn_loadFromSerialise_t p_loadFromSerialiseOrig = nullptr;
static fn_sectionGetItem_t    p_sectionGetItem          = nullptr;
static fn_sectionRemoveItem_t p_sectionRemoveItem       = nullptr;
static fn_sectionSetEnabled_t p_sectionSetEnabled       = nullptr;
static LONG g_loadItemCalls = 0;

// Scan a bounded window of an object for one of our literal section names.
// Our names are unique to this mod, so a false positive is essentially impossible.
static bool ScanObjectForOurName(void* obj, size_t window)
{
    if (!obj) return false;
    const unsigned char* p = (const unsigned char*)obj;
    for (size_t off = 0; off + 8 < window; ++off)
    {
        if (p[off] != 'd' || p[off + 1] != 'e' || p[off + 2] != '_') continue;
        if (strncmp((const char*)(p + off), "de_slot_", 8) == 0) return true;
    }
    return false;
}

// Dump printable runs of an object (diagnostic only).
static void DumpObjectStrings(const char* tag, void* obj, size_t window)
{
    if (!obj) { LogF("[slots] %s: <null>", tag); return; }
    const unsigned char* p = (const unsigned char*)obj;
    char buf[700];
    int  n = 0;
    for (size_t off = 0; off < window && n < (int)sizeof(buf) - 4; ++off)
    {
        unsigned char c = p[off];
        if (c >= 0x20 && c < 0x7F) buf[n++] = (char)c;
        else if (n > 0 && buf[n - 1] != '|') buf[n++] = '|';
    }
    buf[n] = 0;
    LogF("[slots] %s: %s", tag, buf);
}

static bool RecordMentionsOurSection(void* gameData)
{
    return ScanObjectForOurName(gameData, 0x400);
}

// ---------------------------------------------------------------------------
// Route 2 helper: make sure every player inventory has our sections, called
// while a save is being deserialised. Idempotent and cheap.
// ---------------------------------------------------------------------------
static LONG g_loadEnsureRuns = 0;

// ===========================================================================
//  SAVE LOAD - GameWorld::loadAllPlatoons hook.
//
//  This is the piece the player-character-only ctor hook broke (measured
//  2026-10-06): once the ctor stopped creating sections on non-character
//  inventories, items in extra slots vanished again after a load.
//
//  Why this hook: loadAllPlatoons runs once while a save is being read, AFTER
//  the platoons and their characters exist but BEFORE their inventories are
//  populated from the save records. That is exactly the window in which the
//  extra sections must already exist.
//
//  It is also the SAFE place to do it - we walk the player's own character
//  list, so shops and storage buildings are never touched (that was the bug
//  fixed on the same day: extra sections on shop inventories corrupted the
//  shop item grid).
// ===========================================================================
typedef void (*fn_loadAllPlatoons_t)();
static fn_loadAllPlatoons_t p_loadAllPlatoonsOrig = nullptr;
static LONG g_loadAllPlatoonsRuns = 0;

// Create our sections on every player character's inventory.
// No artificial caps: the user's squad can exceed 16 (measured 90 entries in
// PlayerInterface::playerCharacters), and the old i<16 limit silently skipped
// most of them.
static int EnsureSectionsOnPlayerInventories()
{
    if (!g_ppOu || !*g_ppOu) return 0;
    uint8_t* gw = (uint8_t*)*g_ppOu;
    void* piface = *(void**)(gw + 0x580);            // GameWorld::player
    if (!piface) return 0;
    lektor<void*>& players = *(lektor<void*>*)((uint8_t*)piface + 0x2B0);
    if (!players.stuff) return 0;

    int scanned = 0;
    for (uint32_t i = 0; i < players.count; ++i)
    {
        void* ch = players.stuff[i];
        if (!ch) continue;
        if (p_isPlayerChar && !p_isPlayerChar(ch)) continue;
        void* inv = *(void**)((uint8_t*)ch + off::Character_inventory);
        if (inv) { EnsureExtraSections(inv, true); ++scanned; }
    }
    return scanned;
}

static void __cdecl Detour_loadAllPlatoons()
{
    if (p_loadAllPlatoonsOrig) p_loadAllPlatoonsOrig();

    // Sections must exist before the save loader starts placing items.
    int before  = g_created;
    int scanned = EnsureSectionsOnPlayerInventories();

    LONG run = InterlockedIncrement(&g_loadAllPlatoonsRuns);
    if (run <= 8)
        LogF("[slots] loadAllPlatoons #%ld: scanned %d player inventories, created %d (total %d)",
             run, scanned, g_created - before, g_created);
}

static void EnsureSectionsOnAllPlayerInventories()
{
    if (!g_ppOu || !*g_ppOu) return;

    // Throttle: running this for every deserialised item would be wasteful, but a
    // short cooldown still guarantees it runs many times during a load.
    static DWORD s_next = 0;
    DWORD now = GetTickCount();
    if (now < s_next) return;
    s_next = now + 100;

    uint8_t* gw = (uint8_t*)*g_ppOu;
    void* piface = *(void**)(gw + 0x580);           // GameWorld::player
    if (!piface) return;
    lektor<void*>& players = *(lektor<void*>*)((uint8_t*)piface + 0x2B0);
    if (!players.stuff) return;

    int before  = g_created;
    int scanned = 0;
    for (uint32_t i = 0; i < players.count && i < 16; ++i)
    {
        void* ch = players.stuff[i];
        if (!ch) continue;
        if (p_isPlayerChar && !p_isPlayerChar(ch)) continue;
        void* inv = *(void**)((uint8_t*)ch + off::Character_inventory);
        if (inv) { EnsureExtraSections(inv); ++scanned; }
    }

    // Log the first few RUNS unconditionally (not only when something was created),
    // otherwise an existing-section case looks identical to "never ran".
    LONG run = InterlockedIncrement(&g_loadEnsureRuns);
    if (run <= 5)
        LogF("[slots] LOAD-ENSURE run #%ld: scanned=%d inventories, created=%d, total=%d",
             run, scanned, g_created - before, g_created);
}

static void __cdecl Detour_loadFromSerialise_Compat(void* item, void* container, void* gameData)
{
    LONG n = InterlockedIncrement(&g_loadItemCalls);

    // Unconditional proof-of-life for the first few calls.
    if (n <= 5)
        LogF("[slots] LOAD-FIRED #%ld item=%p container=%p gameData=%p", n, item, container, gameData);

    // Diagnostic: is the "section" record inside EITHER object?
    static LONG s_dumps = 0;
    if (InterlockedIncrement(&s_dumps) <= 2)
    {
        DumpObjectStrings("DUMP gameData", gameData, 0x300);
        DumpObjectStrings("DUMP container", container, 0x300);
    }

    // Route 2: unconditionally make sure our sections exist on every player
    // inventory while a save is being read. EnsureExtraSections is idempotent, so
    // this is cheap, and it does NOT depend on decoding the opaque record.
    EnsureSectionsOnAllPlayerInventories();

    // Route 1: name-based detection (either object may carry the record).
    const bool ours = RecordMentionsOurSection(gameData) ||
                      ScanObjectForOurName(container, 0x400);
    if (ours)
    {
        static LONG s_hits = 0;
        if (InterlockedIncrement(&s_hits) <= 10)
            LogF("[slots] LOAD: item %p belongs to an extra slot (call #%ld)", item, n);
    }

    if (p_loadFromSerialiseOrig) p_loadFromSerialiseOrig(item, container, gameData);
}

// ===========================================================================
typedef void (*fn_void_any_t)();

// ===========================================================================
//  Layout injection - ask for the vanilla inventory layout, get ours instead.
//  A distinct layout NAME means no other mod can collide with it.
// ===========================================================================
// OBSOLETE - kept only as a pure pass-through.
//
// History: this hook used to redirect "Kenshi_InventoryCharacterWindow.layout" to
// "Custom_InventoryCharacterWindow.layout". That approach was abandoned (we ship
// the layout under the vanilla file name instead), and the redirect looked
// harmless for a long time ONLY because Sym() never resolved to a real address,
// so the hook never fired.
//
// Once GetRealAddress() was fixed the redirect came alive and crashed the game
// the moment the bag was opened:
//     ResourceManager : 'Custom_InventoryCharacterWindow.layout', not found
//     Critical | No root widget. ['Custom_InventoryCharacterWindow.layout]
//
// It now forwards everything unchanged. Do NOT reintroduce a redirect here.
static void __cdecl Detour_baseLayoutInit(void* self, const MsvcString* layout,
                                          void* parent, bool throwOnMissing,
                                          bool createFakeWidgets)
{
    if (p_baseLayoutOrig)
        p_baseLayoutOrig(self, layout, parent, throwOnMissing, createFakeWidgets);
}
// ===========================================================================
//  WEIGHT ZEROING FOR THE EXTRA SECTIONS (2026-10-21)
//
//  Research result (static disassembly of the shipped 1.0.65 image, see
//  文档\研究_额外槽数值_2026-10-21.md):
//
//    Inventory::getEquippedArmour() - the engine's "which armour is equipped"
//    query - looks items up under the FIVE VANILLA SECTION NAMES:
//        "armour", "legs", "head", "boots", "shirt"
//    (readable as ASCII in the function body). Our de_slot_* sections are
//    never in that list, so extra-slot armour contributes NO equipment skill
//    bonuses and NO armour resistance. The old "stats stack" claim was wrong.
//
//  The ONE numeric thing an extra-slot item still contributes is WEIGHT:
//  Inventory::getTotalWeight() walks every section and sums its cached
//  totalWeight (section + 0xB0) - ours included. That is what this hook
//  removes. Per-section weight is maintained by the engine, so the number is
//  exact and the hook only costs five section lookups.
// ===========================================================================
static void UiTickOnce(const char* source);

typedef float (*fn_getTotalWeight_t)(void* inv);
static fn_getTotalWeight_t p_getTotalWeightOrig = nullptr;

static bool OwnerIsPlayerCharacter(void* owner)
{
    if (!owner || !p_isPlayerRoot || !g_ppOu || !*g_ppOu) return false;
    void* piface = *(void**)((uint8_t*)*g_ppOu + 0x580);
    if (!piface) return false;
    __try { return p_isPlayerRoot(piface, owner); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static float ExtraSectionsWeight(void* inv)
{
    if (!inv || !p_getSection) return 0.0f;
    void* owner = *(void**)((uint8_t*)inv + 0x88);   // Inventory::owner
    // Only player characters have per-character options; querying NPC/shop
    // inventories would create junk keys in the sidecar (they are not
    // characters at all, so the handle fields are meaningless).
    const bool player = OwnerIsPlayerCharacter(owner);
    float sum = 0.0f;
    for (int i = 0; i < kSectionCount; ++i)
    {
        // Per-character option 1: enabled slots keep their weight.
        if (player && CharSlotStats(owner, i)) continue;
        MsvcString n; n.set(kSections[i].name);
        void* sec = p_getSection(inv, &n);
        if (!sec) continue;
        const float w = *(float*)((uint8_t*)sec + off::Section_totalWeight);
        if (w > 0.0f) sum += w;
    }
    return sum;
}

static float __cdecl Detour_getTotalWeight(void* inv)
{
    const float total = p_getTotalWeightOrig ? p_getTotalWeightOrig(inv) : 0.0f;
    const float ours  = ExtraSectionsWeight(inv);
    if (ours <= 0.0f) return total;

    static LONG s_weightLogged = 0;
    if (InterlockedIncrement(&s_weightLogged) <= 8)
        LogF("[slots] WEIGHT: inv=%p engine=%.2f extra-sections=%.2f -> %.2f",
             inv, total, ours, total - ours);

    const float adjusted = total - ours;
    const float result   = adjusted > 0.0f ? adjusted : 0.0f;
    // Keep the cached field consistent with what we return: the original calls
    // above already stored the unadjusted sum there, and some UI code reads the
    // field instead of the return value.
    *(float*)((uint8_t*)inv + off::Inventory_totalWeight) = result;
    return result;
}


// ===========================================================================
//  EXTRA-SLOT STATS ZEROING (2026-10-21, round 2)
//
//  In-game measurement (user, 22:21): moving a metal jacket out of the extra
//  armour slot changed the panel from Defence 27 (+2) / Dodge 2 (-5) to
//  31 (+6) / 3 (-4). So the extra slot DOES feed the equipment-bonus list.
//  The earlier "getEquippedArmour excludes it" finding only covers the
//  hit/coverage queries - not this aggregation.
//
//  Character::recalculateTotalEquipmentSkillBonus (RVA 0x886160 in 1.0.65) is
//  the function that converts the equipped-armour list (Character+0x4A0,
//  a lektor<Item*>: count +0x4A8, stuff +0x4B0) into CharStats bonuses. It only
//  READS that list (verified by disassembly: the only accesses are cmp/mov).
//
//  So: for the duration of that call, replace the list with the same list minus
//  the items sitting in our extra sections, then restore it. The result is
//  exactly "the bonuses the character would have without the extra items" -
//  no item fields are touched, nothing is written to a save, and the vanilla
//  slots keep their full contribution.
//
//  SAFETY: the list is only touched when one of OUR exact item pointers is
//  found inside it, after validating the count and that the array is committed
//  readable memory. If the layout were ever wrong, nothing matches and the
//  hook is a pure pass-through.
// ===========================================================================
typedef void (*fn_recalcEquip_t)(void* character);
static fn_recalcEquip_t p_recalcEquipOrig = nullptr;

static bool IsReadableRange(const void* p, size_t bytes)
{
    if (!p || bytes == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    return (const uint8_t*)p + bytes <= (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
}

static void __cdecl Detour_recalcEquip(void* character)
{
    if (!character || !p_recalcEquipOrig)
    {
        if (p_recalcEquipOrig) p_recalcEquipOrig(character);
        return;
    }
    // Only player characters have our sections/options; skipping NPCs also
    // keeps the per-character sidecar from filling up with NPC entries.
    if (p_isPlayerChar && !p_isPlayerChar(character))
    {
        p_recalcEquipOrig(character);
        return;
    }

    uint8_t* c = (uint8_t*)character;
    void*    savedStuff = *(void**)(c + off::Character_equipList + 0x10);
    uint32_t savedCount = *(uint32_t*)(c + off::Character_equipList + 0x08);
    uint32_t savedMax   = *(uint32_t*)(c + off::Character_equipList + 0x0C);

    if (!savedStuff || savedCount == 0 || savedCount > 64 ||
        !IsReadableRange(savedStuff, (size_t)savedCount * sizeof(void*)))
    {
        p_recalcEquipOrig(character);
        return;
    }

    // Collect the item pointers currently sitting in our five extra sections.
    void* our[8] = { 0 };
    int   ourCount = 0;
    void* inv = *(void**)(c + off::Character_inventory);
    if (inv && p_getSection && p_sectionGetItem)
    {
        for (int i = 0; i < kSectionCount && ourCount < 8; ++i)
        {
            // Per-character option 1: enabled slots keep their stats.
            if (CharSlotStats(character, i)) continue;
            MsvcString n; n.set(kSections[i].name);
            void* sec = p_getSection(inv, &n);
            if (!sec) continue;
            void* it = p_sectionGetItem(sec);
            if (it) our[ourCount++] = it;
        }
    }
    if (ourCount == 0)
    {
        p_recalcEquipOrig(character);
        return;
    }

    void*    filtered[64];
    uint32_t fcount  = 0;
    int      removed = 0;
    for (uint32_t k = 0; k < savedCount; ++k)
    {
        void* it = ((void**)savedStuff)[k];
        bool ours = false;
        for (int i = 0; i < ourCount; ++i) { if (it == our[i]) { ours = true; break; } }
        if (ours) { ++removed; continue; }
        filtered[fcount++] = it;
    }
    if (removed == 0)
    {
        p_recalcEquipOrig(character);
        return;
    }

    *(void**)(c + off::Character_equipList + 0x10) = filtered;
    *(uint32_t*)(c + off::Character_equipList + 0x08) = fcount;

    p_recalcEquipOrig(character);

    *(void**)(c + off::Character_equipList + 0x10) = savedStuff;
    *(uint32_t*)(c + off::Character_equipList + 0x08) = savedCount;
    *(uint32_t*)(c + off::Character_equipList + 0x0C) = savedMax;

    static LONG s_statsLogged = 0;
    if (InterlockedIncrement(&s_statsLogged) <= 12)
    {
        void* cs = *(void**)(c + off::Character_stats);
        int   def = 0, att = 0;
        float dodge = 0.0f, ath = 0.0f;
        if (cs)
        {
            def   = *(int*)((uint8_t*)cs + 0x24);   // _skillBonusDefence
            att   = *(int*)((uint8_t*)cs + 0x20);   // _skillBonusAttack
            dodge = *(float*)((uint8_t*)cs + 0x38); // skillMultDodge
            ath   = *(float*)((uint8_t*)cs + 0x18); // athleticsMultiplier
        }
        LogF("[stats] recalc char=%p list %u -> %u (removed %d extra-slot item(s)); def=%d att=%d dodgeMult=%.3f athMult=%.3f",
             character, savedCount, fcount, removed, def, att, dodge, ath);
    }
}


// ===========================================================================
//  HIT-TIME COVERAGE ZEROING (2026-10-21, round 3)
//
//  The melee hit resolver (1.0.65 function 0x64fe40) walks the SAME character
//  equipment list (Character+0x4A0) and, per body part, calls
//  Armour::didIHitFlesh(bodypart). Disassembly of the caller shows:
//
//      if (didIHitFlesh(bodypart))  -> skip this armour
//      else                         -> add cutResistance/pierceResistance/cutToStun
//
//  i.e. "true" means the attack reached flesh and this armour gives no
//  protection. Our extra-slot armour therefore still reduced damage even
//  though the panel stats were already zeroed.
//
//  Fix: keep a small registry of the item pointers sitting in our five extra
//  sections, and for those only, force didIHitFlesh() to return true. The
//  vanilla slots are untouched; no item fields are written.
// ===========================================================================
typedef bool (*fn_didHitFlesh_t)(void* armour, void* bodyPart);
static fn_didHitFlesh_t p_didHitFleshOrig = nullptr;

static void* g_extraItemsA[256];
static void* g_extraItemsB[256];
static volatile LONG g_extraCountA = 0;
static volatile LONG g_extraCountB = 0;
static volatile LONG g_extraActive = 0;      // 0 = A is readable, 1 = B

static bool IsExtraSlotItem(void* item)
{
    if (!item) return false;
    const LONG active = g_extraActive;
    void** arr = active ? g_extraItemsB : g_extraItemsA;
    const LONG n = active ? g_extraCountB : g_extraCountA;
    for (LONG i = 0; i < n; ++i)
        if (arr[i] == item) return true;
    return false;
}

static bool __cdecl Detour_didHitFlesh(void* armour, void* bodyPart)
{
    if (armour && IsExtraSlotItem(armour))
    {
        static LONG s_covLogged = 0;
        if (InterlockedIncrement(&s_covLogged) <= 12)
            LogF("[coverage] extra-slot armour %p treated as unprotected (hit resolution)", armour);
        return true;                          // hit flesh: no resistance from this item
    }
    return p_didHitFleshOrig ? p_didHitFleshOrig(armour, bodyPart) : false;
}

// Rebuild the registry from every player character's five extra sections.
// Double-buffered so the hit hook can read it without a lock. Called from the
// slots polling thread, at most once per second.
static void RefreshExtraItemRegistry()
{
    static DWORD s_next = 0;
    const DWORD now = GetTickCount();
    if (now < s_next) return;
    s_next = now + 1000;

    if (!g_ppOu || !*g_ppOu) return;
    uint8_t* gw = (uint8_t*)*g_ppOu;
    void* piface = *(void**)(gw + 0x580);
    if (!piface) return;
    lektor<void*>& players = *(lektor<void*>*)((uint8_t*)piface + 0x2B0);
    if (!players.stuff) return;

    const LONG active = g_extraActive;
    void** dst = active ? g_extraItemsA : g_extraItemsB;
    LONG n = 0;

    for (uint32_t p = 0; p < players.count && n < 256; ++p)
    {
        void* ch = players.stuff[p];
        if (!ch) continue;
        if (p_isPlayerChar && !p_isPlayerChar(ch)) continue;
        void* inv = *(void**)((uint8_t*)ch + off::Character_inventory);
        if (!inv) continue;
        for (int i = 0; i < kSectionCount && n < 256; ++i)
        {
            // Only cosmetic slots are forced to "no coverage"; enabled slots
            // keep their normal armour coverage.
            if (CharSlotStats(ch, i)) continue;
            MsvcString name; name.set(kSections[i].name);
            void* sec = p_getSection ? p_getSection(inv, &name) : nullptr;
            if (!sec || !p_sectionGetItem) continue;
            void* item = p_sectionGetItem(sec);
            if (item) dst[n++] = item;
        }
    }

    if (active) g_extraCountA = n; else g_extraCountB = n;
    InterlockedExchange(&g_extraActive, active ? 0 : 1);
}


// ===========================================================================
//  IN-GAME UI (2026-10-22, v2)
//
//  Two option buttons per extra slot (1 = stats, 2 = hide-mode keeps the
//  vanilla item) plus a vertical column on the right of the EXTRA SET panel.
//
//  The panel is a SEPARATE top-level window in the layout, so we do NOT walk
//  down from InventoryLayout::getWindow(). The five section widgets are
//  captured directly in the getWidget hook (g_slotWidgets) and every button is
//  a child of their common parent (the PanelEmpty inside DualEquipWindow).
//
//  IMPORTANT: this DLL is /MT (msvcp140) while the game's MyGUIEngine is
//  msvcp100, so we never allocate MyGUI objects (no event delegates). Clicks
//  are detected by polling the mouse against the buttons' absolute rects.
// ===========================================================================
typedef void (*fn_invGuiShow_t)(void* gui, bool show);
static fn_invGuiShow_t p_invGuiShowOrig = nullptr;
typedef void* (*fn_getGuiInventory_t)(void* gui);
typedef void (*fn_refreshGuiSections_t)(void* gui);
static fn_getGuiInventory_t     p_getGuiInventory     = nullptr;
static fn_refreshGuiSections_t  p_refreshGuiSections  = nullptr;
typedef void (*fn_worldTick_t)(void* world, float time);
static fn_worldTick_t p_worldTickOrig = nullptr;
typedef void (*fn_factoryTick_t)();
static fn_factoryTick_t p_factoryTickOrig = nullptr;
typedef void (*fn_getAllSelected_t)(void* playerInterface, lektor<void*>& out, int itemType);
static fn_getAllSelected_t p_getAllSelected = nullptr;

static void*           g_uiChar     = nullptr;
static void*           g_uiGui      = nullptr;
static MyGUI::TextBox* g_helpBox    = nullptr;
static MyGUI::Window*  g_helpWindow = nullptr;
static MyGUI::Widget*  g_uiBuiltPanel = nullptr;

struct UiButtonRect { int x, y, w, h, id; };
static UiButtonRect g_btnRects[16];
static int g_btnRectCount = 0;

typedef void (*fn_void_t)();
static fn_void_t p_deInsert = nullptr;
static fn_void_t p_deAll    = nullptr;

static void ResolveHideApi()
{
    if (p_deInsert && p_deAll) return;
    HMODULE m = GetModuleHandleA("DualEquip2.dll");
    if (!m) return;
    if (!p_deInsert) p_deInsert = (fn_void_t)GetProcAddress(m, "DE_UiInsert");
    if (!p_deAll)    p_deAll    = (fn_void_t)GetProcAddress(m, "DE_UiAll");
}

static MyGUI::Widget* GetPanel()
{
    for (int i = 0; i < kSectionCount; ++i)
    {
        MyGUI::Widget* sw = g_slotWidgets[i];
        if (!sw) continue;
        MyGUI::Widget* p = sw->getParent();
        if (!p) continue;
        // Only the EXTRA SET window's container is a valid parent. This
        // rejects stale/foreign widgets that the old getWidget capture used
        // to inject (the "1 2 buttons on the vanilla slots" bug).
        for (MyGUI::Widget* a = p; a; a = a->getParent())
        {
            const char* nm = a->getName().c_str();
            if (nm && strstr(nm, "DualEquipWindow")) return p;
        }
    }
    return nullptr;
}

static bool HasDualEquipAncestor(MyGUI::Widget* w)
{
    for (int depth = 0; w && depth < 16; ++depth)
    {
        const char* nm = w->getName().c_str();
        if (nm && strstr(nm, "DualEquipWindow")) return true;
        w = w->getParent();
    }
    return false;
}

static MyGUI::Widget* g_slotCandidate = nullptr;

static void FindSlotRecursive(MyGUI::Widget* node, const char* name)
{
    if (!node || g_slotCandidate) return;
    const char* nm = node->getName().c_str();
    if (nm && *nm)
    {
        const size_t n = strlen(nm), s = strlen(name);
        const bool exact = (n == s && strcmp(nm, name) == 0);
        const bool suffix = (n >= s && strcmp(nm + n - s, name) == 0);
        if ((exact || suffix) && node->getWidth() > 0 && node->getHeight() > 0 &&
            HasDualEquipAncestor(node))
        {
            g_slotCandidate = node;
            return;
        }
    }
    const size_t count = node->getChildCount();
    for (size_t i = 0; i < count; ++i)
    {
        FindSlotRecursive(node->getChildAt(i), name);
        if (g_slotCandidate) return;
    }
}

static MyGUI::Widget* FindUiWidget(const char* name)
{
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui || !name) return nullptr;
    g_slotCandidate = nullptr;
    MyGUI::EnumeratorWidgetPtr e = gui->getEnumerator();
    while (e.next())
    {
        FindSlotRecursive(e.current(), name);
        if (g_slotCandidate) break;
    }
    return g_slotCandidate;
}

static void CollectOrphanDeq(MyGUI::Widget* node, MyGUI::Widget* panel,
                             MyGUI::Widget** out, int& n, int max)
{
    if (!node) return;
    const char* nm = node->getName().c_str();
    if (nm && strncmp(nm, "DEQ_", 4) == 0 && (strcmp(nm, "DEQ_HelpBox") != 0 && strcmp(nm, "DEQ_HelpWindow") != 0))
    {
        bool under = false;
        for (MyGUI::Widget* p = node; p; p = p->getParent())
            if (p == panel) { under = true; break; }
        if (!under && n < max) out[n++] = node;
    }
    const size_t count = node->getChildCount();
    for (size_t i = 0; i < count; ++i)
        CollectOrphanDeq(node->getChildAt(i), panel, out, n, max);
}

// The layout is destroyed/recreated on fast backpack toggles and on save
// loads. Buttons created for an old panel stay alive in the GUI tree and
// render over whatever window happens to be at those coordinates (the
// "buttons on top of the vanilla slots" bug). Destroy every DEQ_* widget that
// is not a child of the panel we are building on now.
static void DestroyOrphanDeqWidgets(MyGUI::Widget* panel)
{
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) return;
    MyGUI::Widget* list[96];
    int n = 0;
    MyGUI::EnumeratorWidgetPtr e = gui->getEnumerator();
    while (e.next()) CollectOrphanDeq(e.current(), panel, list, n, 96);
    for (int i = 0; i < n; ++i)
    {
        if (list[i] == g_helpBox) g_helpBox = nullptr;
        // Never destroy from the tick: MyGUI may still hold the widget in its
        // render/input queues and crash during the next frame (0xC0000005 in
        // MyGUIEngine on 2026-10-26). Hiding is enough - the old panel is
        // hidden anyway, and MakeUiButton re-shows the widgets if it returns.
        __try { list[i]->setVisible(false); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    if (n) LogF("[ui] hid %d orphan DEQ widget(s)", n);
}

static bool UiWindowVisible()
{
    MyGUI::Widget* panel = GetPanel();
    if (!panel) return false;
    MyGUI::Widget* win = panel->getParent();
    return win ? win->getVisible() : false;
}

static void* FirstSelectedCharacter()
{
    if (!g_ppOu || !*g_ppOu || !p_getAllSelected) return nullptr;
    void* piface = *(void**)((uint8_t*)*g_ppOu + 0x580);
    if (!piface) return nullptr;
    lektor<void*> sel = {};
    p_getAllSelected(piface, sel, 1);
    static LONG s_selLog = 0;
    if (InterlockedIncrement(&s_selLog) <= 20)
        LogF("[ui] selection count=%u piface=%p", (unsigned)sel.count, piface);
    return (sel.count > 0 && sel.stuff) ? sel.stuff[0] : nullptr;
}

static void DoUiAction(int id);

static MyGUI::Button* MakeUiButton(MyGUI::Widget* parent, const char* name,
                                   const MyGUI::IntCoord& coord, const char* caption)
{
    if (!parent || !name) return nullptr;
    MyGUI::Widget* existing = parent->findWidget(name);
    MyGUI::Button* b = nullptr;
    if (existing) b = static_cast<MyGUI::Button*>(existing);
    if (b) b->setVisible(true);
    if (!b)
    {
        b = parent->createWidget<MyGUI::Button>(
            "Kenshi_Button1", coord, MyGUI::Align::Left | MyGUI::Align::Top, name);
        if (!b) return nullptr;
        b->setNeedMouseFocus(true);
    }
    b->setCoord(coord);
    b->setCaption(caption);
    return b;
}

static std::string g_helpText;

static std::string DetectGameLanguage()
{
    FILE* f = fopen("settings.cfg", "r");
    if (f)
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            if (strncmp(line, "language=", 9) == 0)
            {
                std::string v = line + 9;
                while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ' || v.back() == '\t'))
                    v.pop_back();
                fclose(f);
                if (!v.empty()) return v;
                break;
            }
        }
        fclose(f);
    }
    const LANGID lid = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(lid))
    {
        case LANG_CHINESE:    return "zh_CN";
        case LANG_RUSSIAN:    return "ru_RU";
        case LANG_JAPANESE:   return "ja_JP";
        case LANG_KOREAN:     return "ko_KR";
        case LANG_GERMAN:     return "de_DE";
        case LANG_FRENCH:     return "fr_FR";
        case LANG_SPANISH:    return "es_ES";
        case LANG_PORTUGUESE: return "pt_BR";
        default:              return "en_GB";
    }
}

static size_t Utf8CharLen(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static std::string WrapUtf8(const std::string& in, size_t maxChars)
{
    std::string out;
    size_t col = 0;
    for (size_t i = 0; i < in.size(); )
    {
        if (in[i] == '\n') { out += '\n'; col = 0; ++i; continue; }
        if (in[i] == '\r') { ++i; continue; }
        const size_t l = Utf8CharLen((unsigned char)in[i]);
        if (col >= maxChars) { out += '\n'; col = 0; }
        out.append(in, i, l);
        ++col;
        i += l;
    }
    return out;
}

static bool ReadWholeFile(const char* path, std::string& out)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return !out.empty();
}

static std::string ExtractSection(const std::string& all, const std::string& tag)
{
    const std::string header = "[" + tag + "]";
    size_t p = all.find(header);
    if (p == std::string::npos) return "";
    p = all.find('\n', p);
    if (p == std::string::npos) return "";
    const size_t start = p + 1;

    // Stop at the next SECTION header line ([de_DE], [en_GB], ...). The old
    // code searched for "\n[" which also matched the "[1]" line at the very
    // start of a section, so a multi-language file returned everything from
    // that point to EOF (all languages concatenated).
    size_t lineStart = start;
    while (lineStart < all.size())
    {
        size_t lineEnd = all.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = all.size();
        size_t e = lineEnd;
        if (e > lineStart && all[e - 1] == '\r') --e;
        if (e > lineStart + 2 && all[lineStart] == '[' && all[e - 1] == ']')
        {
            bool tagLike = true;
            for (size_t k = lineStart + 1; k + 1 < e; ++k)
            {
                const char c = all[k];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
                { tagLike = false; break; }
            }
            if (tagLike) return all.substr(start, lineStart - start);
        }
        if (lineEnd == all.size()) break;
        lineStart = lineEnd + 1;
    }
    return all.substr(start);
}

static bool ReadWholeFileW(const wchar_t* path, std::string& out)
{
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return !out.empty();
}

// Works for both mods\<name>\Plugins and the Steam workshop content folder.
// Uses the WIDE module path so non-ASCII Steam library paths still work.
static bool ReadHelpFromModuleDir(std::string& out)
{
    HMODULE hm = GetModuleHandleA("DualEquipSlots2.dll");
    if (!hm) return false;
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(hm, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::wstring p = path;
    size_t s1 = p.find_last_of(L"\\/");
    if (s1 == std::wstring::npos) return false;
    p.resize(s1);                       // ...\Plugins
    size_t s2 = p.find_last_of(L"\\/");
    if (s2 == std::wstring::npos) return false;
    p.resize(s2);                       // mod root
    p += L"\\DualEquip_help.txt";
    return ReadWholeFileW(p.c_str(), out);
}

static void LoadHelpText()
{
    const std::string lang = DetectGameLanguage();
    std::string all;
    // 1) user override in the game root
    // 2) next to the DLL (works for both mods\... and Steam workshop content)
    // 3) legacy author path
    if (!ReadWholeFile("DualEquip_help.txt", all) &&
        !ReadHelpFromModuleDir(all))
        ReadWholeFile("mods\\Hide_Show_DualEquip\\DualEquip_help.txt", all);

    std::string t;
    if (!all.empty())
    {
        t = ExtractSection(all, lang);
        if (t.empty() && lang.size() >= 2) t = ExtractSection(all, lang.substr(0, 2));
        if (t.empty()) t = ExtractSection(all, "en_GB");
        if (t.empty()) t = ExtractSection(all, "en");
    }

    const bool cjk = (lang.size() >= 2 && (lang[0] == 'z' || lang[0] == 'j' || lang[0] == 'k'));
    const bool cyr = (lang.size() >= 2 && lang[0] == 'r' && lang[1] == 'u');
    const size_t wrapChars = cjk ? 20 : (cyr ? 34 : 42);
    if (!t.empty())
    {
        g_helpText = WrapUtf8(t, wrapChars);
        LogF("[ui] help text loaded: language=%s (%u bytes)", lang.c_str(), (unsigned)g_helpText.size());
    }
    else
    {
        g_helpText = WrapUtf8(
            "[1] Extra-slot stats for this slot (bright = on, dim = off)\n"
            "[2] Keep the matching vanilla item visible while hiding\n"
            "[X] Hide / show the backpack appearance\n"
            "[I] Same as the Insert hotkey (selected character)\n"
            "[R] Same as the Right Shift hotkey (all characters)\n"
            "[A] Copy this character's [1]/[2] settings to all selected characters\n"
            "Click [?] again to close this help.\n"
            "Sorry, languages not supported by the game can only be shown in English.\n",
            wrapChars);
        LogF("[ui] help text: built-in fallback (language=%s)", lang.c_str());
    }
}

static void EnsureUi()
{
    MyGUI::Widget* panel = GetPanel();
    if (!panel) return;

    static MyGUI::Widget* s_lastPanel = nullptr;
    if (panel != s_lastPanel)
    {
        s_lastPanel = panel;
        DestroyOrphanDeqWidgets(panel);
        LogF("[ui] panel=%p name='%s'", panel, panel->getName().c_str());
    }

    for (int i = 0; i < kSectionCount; ++i)
    {
        MyGUI::Widget* sw = g_slotWidgets[i];
        if (!sw) continue;
        const MyGUI::IntCoord c = sw->getCoord();
        const MyGUI::IntCoord c1(c.left + c.width + 4, c.top + 4, 54, 54);
        const MyGUI::IntCoord c2(c1.left + 58, c1.top, 54, 54);
        char n1[32], n2[32];
        sprintf_s(n1, sizeof(n1), "DEQ_s%d_1", i);
        sprintf_s(n2, sizeof(n2), "DEQ_s%d_2", i);
        MakeUiButton(panel, n1, c1, "1");
        MakeUiButton(panel, n2, c2, "2");
    }

    const int right = panel->getWidth() - 58;
    if (right > 4)
    {
        static const char* kNames[5] = { "DEQ_help", "DEQ_bag", "DEQ_ins", "DEQ_rshift", "DEQ_apply" };
        static const char* kCaps[5]  = { "?", "X", "I", "R", "A" };
        for (int k = 0; k < 5; ++k)
            MakeUiButton(panel, kNames[k], MyGUI::IntCoord(right, 4 + k * 60, 54, 54), kCaps[k]);
    }

    // True top-level popup: create it through the Gui on the "Window" layer.
    // MyGUI top-level windows have getParent() == nullptr, so climbing from the
    // panel stopped at the EXTRA SET window and the popup was clipped inside
    // it. Gui-created widgets live on the layer root and are not clipped.
    {
        MyGUI::Gui* mgui = MyGUI::Gui::getInstancePtr();
        if (mgui)
        {
            const int hw = 520, hh = 560;
            const int hx = panel->getAbsoluteLeft() + panel->getWidth() - hw + 4;
            const int hy = panel->getAbsoluteTop() + 4;

            MyGUI::Widget* existing = mgui->findWidgetT("DEQ_HelpWindow", false);
            bool created = false;
            if (!existing)
            {
                existing = mgui->createWidget<MyGUI::Window>("Kenshi_WindowCXP",
                    MyGUI::IntCoord(hx, hy, hw, hh),
                    MyGUI::Align::Left | MyGUI::Align::Top, "Popup", "DEQ_HelpWindow");
                created = true;
            }
            if (existing)
            {
                g_helpWindow = static_cast<MyGUI::Window*>(existing);
                if (created)
                {
                    g_helpWindow->setNeedMouseFocus(false);
                    g_helpWindow->setCaption("HELP");
                    g_helpWindow->setVisible(false);
                }
                MyGUI::Widget* tbw = g_helpWindow->findWidget("DEQ_HelpBox");
                if (!tbw)
                    tbw = g_helpWindow->createWidget<MyGUI::TextBox>("Kenshi_TextboxPaintedText",
                        MyGUI::IntCoord(10, 34, hw - 20, hh - 44),
                        MyGUI::Align::Left | MyGUI::Align::Top, "DEQ_HelpBox");
                if (tbw)
                {
                    g_helpBox = static_cast<MyGUI::TextBox*>(tbw);
                    g_helpBox->setNeedMouseFocus(false);
                    g_helpBox->setCaption(g_helpText.c_str());
                }

                // Fit the frame to the actual rendered text (languages differ a
                // lot in length). Measure the TextBox, fall back to a line-count
                // estimate, clamp to the screen.
                static MyGUI::Widget* s_helpPanel = nullptr;
                static bool s_helpSized = false;
                if (created || s_helpPanel != panel || !s_helpSized)
                {
                    s_helpPanel = panel;
                    s_helpSized = true;
                    MyGUI::IntSize ts = MyGUI::IntSize(0, 0);
                    if (g_helpBox) ts = g_helpBox->getTextSize();
                    int fh = g_helpBox ? g_helpBox->getFontHeight() : 20;
                    if (fh <= 0) fh = 20;
                    int lines = 1;
                    for (const char* cp = g_helpText.c_str(); *cp; ++cp)
                        if (*cp == '\n') ++lines;
                    int textH = ts.height;
                    if (textH < lines * 8) textH = lines * (fh + 3);
                    int fh2 = textH + 70;
                    if (fh2 < 320) fh2 = 320;
                    const int screenH = GetSystemMetrics(SM_CYSCREEN);
                    if (fh2 > screenH - 80) fh2 = screenH - 80;
                    if (fh2 < 200) fh2 = 200;
                    int hy2 = hy;
                    if (hy2 + fh2 > screenH - 20) hy2 = screenH - 20 - fh2;
                    if (hy2 < 10) hy2 = 10;
                    g_helpWindow->setCoord(MyGUI::IntCoord(hx, hy2, hw, fh2));
                    if (g_helpBox) g_helpBox->setCoord(MyGUI::IntCoord(10, 34, hw - 20, fh2 - 44));
                }
            }
        }
    }
}

static void RefreshUi()
{
    MyGUI::Widget* panel = GetPanel();
    if (!panel) return;
    const unsigned short bits = g_uiChar ? GetBits(g_uiChar) : 0;
    for (int i = 0; i < kSectionCount; ++i)
    {
        char n1[32], n2[32];
        sprintf_s(n1, sizeof(n1), "DEQ_s%d_1", i);
        sprintf_s(n2, sizeof(n2), "DEQ_s%d_2", i);
        MyGUI::Widget* w1 = panel->findWidget(n1);
        MyGUI::Widget* w2 = panel->findWidget(n2);
        // Selected state renders BRIGHT in this skin: bright = off, dark = on.
        // [1] bright = no stats (default), dark = stats ON.
        if (w1) static_cast<MyGUI::Button*>(w1)->setStateSelected(((bits >> (2 * i)) & 1) == 0);
        // [2] bit 1 means "keep vanilla visible": bright = keep, dark = hide.
        if (w2) static_cast<MyGUI::Button*>(w2)->setStateSelected(((bits >> (2 * i + 1)) & 1) != 0);
    }
    MyGUI::Widget* wb = panel->findWidget("DEQ_bag");
    if (wb) static_cast<MyGUI::Button*>(wb)->setStateSelected(((bits >> DE_OPT_BAG) & 1) != 0);
    MyGUI::Widget* wi = panel->findWidget("DEQ_ins");
    // bright = hidden, dark = shown
    if (wi) static_cast<MyGUI::Button*>(wi)->setStateSelected(((bits >> DE_OPT_HIDDEN) & 1) != 0);
}

static void RebuildUiRects()
{
    g_btnRectCount = 0;
    MyGUI::Widget* panel = GetPanel();
    if (!panel) return;
    for (int i = 0; i < kSectionCount; ++i)
    {
        for (int k = 0; k < 2 && g_btnRectCount < 16; ++k)
        {
            char n[32]; sprintf_s(n, sizeof(n), "DEQ_s%d_%d", i, k + 1);
            MyGUI::Widget* w = panel->findWidget(n);
            if (!w) continue;
            UiButtonRect& r = g_btnRects[g_btnRectCount++];
            r.x = w->getAbsoluteLeft(); r.y = w->getAbsoluteTop();
            r.w = w->getWidth();        r.h = w->getHeight();
            r.id = i * 2 + k;
        }
    }
    static const char* kNames[5] = { "DEQ_help", "DEQ_bag", "DEQ_ins", "DEQ_rshift", "DEQ_apply" };
    for (int k = 0; k < 5 && g_btnRectCount < 16; ++k)
    {
        MyGUI::Widget* w = panel->findWidget(kNames[k]);
        if (!w) continue;
        UiButtonRect& r = g_btnRects[g_btnRectCount++];
        r.x = w->getAbsoluteLeft(); r.y = w->getAbsoluteTop();
        r.w = w->getWidth();        r.h = w->getHeight();
        r.id = 10 + k;
    }

    static LONG s_rectLog = 0;
    if (g_btnRectCount >= 15 && InterlockedIncrement(&s_rectLog) <= 4)
        LogF("[ui] rects s0=(%d,%d,%d,%d) s1=(%d,%d,%d,%d) s4=(%d,%d,%d,%d) v0=(%d,%d,%d,%d)",
             g_btnRects[0].x, g_btnRects[0].y, g_btnRects[0].w, g_btnRects[0].h,
             g_btnRects[1].x, g_btnRects[1].y, g_btnRects[1].w, g_btnRects[1].h,
             g_btnRects[4].x, g_btnRects[4].y, g_btnRects[4].w, g_btnRects[4].h,
             g_btnRects[10].x, g_btnRects[10].y, g_btnRects[10].w, g_btnRects[10].h);
}

static void ApplyBitsToSelected()
{
    if (!g_uiChar || !g_ppOu || !*g_ppOu || !p_getAllSelected) return;
    void* piface = *(void**)((uint8_t*)*g_ppOu + 0x580);
    if (!piface) return;
    const unsigned short bits = GetBits(g_uiChar);
    const unsigned short slotMask = 0x03FF;          // [1]/[2] of the five slots
    lektor<void*> sel = {};
    p_getAllSelected(piface, sel, 1);
    int n = 0;
    for (uint32_t i = 0; i < sel.count; ++i)
    {
        void* ch = sel.stuff[i];
        if (!ch) continue;
        const unsigned short dst = GetBits(ch);
        const unsigned short merged = (unsigned short)((dst & ~slotMask) | (bits & slotMask));
        SetBits(ch, merged);
        Detour_recalcEquip(ch);
        ++n;
    }
    LogF("[ui] applied slot bits=0x%X to %d selected character(s)", (unsigned)(bits & slotMask), n);
}

static void DoUiAction(int id)
{
    static LONG s_actLog = 0;
    if (InterlockedIncrement(&s_actLog) <= 16)
        LogF("[ui] action id=%d char=%p", id, g_uiChar);
    if (id >= 10)
    {
        switch (id)
        {
            case 10:
            {
                MyGUI::Gui* mgui = MyGUI::Gui::getInstancePtr();
                MyGUI::Widget* hw = mgui ? mgui->findWidgetT("DEQ_HelpWindow", false) : nullptr;
                if (!hw) { EnsureUi(); hw = mgui ? mgui->findWidgetT("DEQ_HelpWindow", false) : nullptr; }
                if (hw)
                {
                    const bool v = hw->getVisible();
                    LogF("[ui] help toggle: window=%p visible=%d", hw, (int)v);
                    hw->setVisible(!v);
                    LogF("[ui] help toggle done");
                }
                else LogF("[ui] help toggle: no help window");
                return;
            }
            case 11:
                if (g_uiChar)
                {
                    unsigned short b = GetBits(g_uiChar);
                    b ^= (unsigned short)(1u << DE_OPT_BAG);
                    SetBits(g_uiChar, b);
                    RefreshUi();
                }
                return;
            case 12: ResolveHideApi(); if (p_deInsert) p_deInsert(); RefreshUi(); return;
            case 13: ResolveHideApi(); if (p_deAll)    p_deAll();    RefreshUi(); return;
            case 14: ApplyBitsToSelected(); return;
            default: return;
        }
    }

    const int slot = id / 2;
    const int opt  = id % 2;          // 0 = option 1, 1 = option 2
    if (!g_uiChar)
    {
        static LONG s_noCharLog = 0;
        if (InterlockedIncrement(&s_noCharLog) <= 10) LogF("[ui] action id=%d ignored: no character", id);
        return;
    }
    if (slot < 0 || slot >= kSectionCount) return;

    unsigned short bits = GetBits(g_uiChar);
    bits ^= (unsigned short)(1u << (2 * slot + opt));
    SetBits(g_uiChar, bits);
    Detour_recalcEquip(g_uiChar);
    RefreshUi();
}

static void PollUiClicks()
{
    static bool s_prevDown = false;
    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool edge = down && !s_prevDown;
    s_prevDown = down;
    if (!edge || g_btnRectCount == 0) return;

    MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr();
    if (!im) return;
    const MyGUI::IntPoint& mp = im->getMousePosition();
    const int mx = mp.left, my = mp.top;

    static LONG s_clickLog = 0;
    if (InterlockedIncrement(&s_clickLog) <= 60)
    {
        MyGUI::Widget* focus = im->getMouseFocusWidget();
        const char* fname = focus ? focus->getName().c_str() : "<none>";
        LogF("[ui] LMB gui=%d,%d focus='%s' rects=%d", mx, my, fname ? fname : "", g_btnRectCount);
    }

    for (int i = 0; i < g_btnRectCount; ++i)
    {
        const UiButtonRect& r = g_btnRects[i];
        if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h)
        {
            static LONG s_hitLog = 0;
            if (InterlockedIncrement(&s_hitLog) <= 16)
                LogF("[ui] click hit id=%d rect=%d,%d,%d,%d", r.id, r.x, r.y, r.w, r.h);
            DoUiAction(r.id);
            return;
        }
    }
}

static void OnInventoryShown(void* gui)
{
    if (!gui) return;
    g_uiGui  = gui;
    g_uiChar = *(void**)((uint8_t*)gui + 0x50);           // InventoryGUI::callbackObject
    EnsureUi();
    RefreshUi();
    RebuildUiRects();

    static LONG s_uiLog = 0;
    if (InterlockedIncrement(&s_uiLog) <= 8)
        LogF("[ui] show: char=%p panel=%p w=%d visible=%d",
             g_uiChar, GetPanel(), GetPanel() ? GetPanel()->getWidth() : 0, (int)UiWindowVisible());
}

static void OnInventoryShownGuarded(void* gui)
{
    __try { OnInventoryShown(gui); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LogF("[ui] exception while building inventory buttons"); }
}

static void __cdecl Detour_invGuiShow(void* gui, bool show)
{
    void* inv = nullptr;
    if (show && gui && p_getGuiInventory)
    {
        __try
        {
            inv = p_getGuiInventory(gui);
            if (inv && IsPlayerCharacterInventory(inv))
                EnsureExtraSections(inv, true);   // before the layout is shown
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }

    if (p_invGuiShowOrig) p_invGuiShowOrig(gui, show);

    if (show)
    {
        __try
        {
            if (inv && IsPlayerCharacterInventory(inv) && p_refreshGuiSections)
                p_refreshGuiSections(gui);        // refresh already-built section widgets
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
        InterlockedExchange(&g_sectionsReenabled, 0);
        OnInventoryShownGuarded(gui);
    }
    else if (gui == g_uiGui) { g_uiGui = nullptr; g_uiChar = nullptr; }
}

static volatile LONG g_uiReenter = 0;

// ===========================================================================
//  NPC CLEANUP (2026-11-01)
//
//  The per-character load hook has to build sections for every Character
//  because the player list is empty during a save load. That also gives NPCs
//  empty de_slot_* sections (and, in existing saves, possibly items). After the
//  load is finished the player list is available: move any NPC extra-slot
//  items back to the matching vanilla slot (or a normal section) and disable
//  the NPC extra sections so they cannot affect stats/weight.
// ===========================================================================
static bool SectionIsOurs(void* sec)
{
    if (!sec) return false;
    const MsvcString* sn = (const MsvcString*)((uint8_t*)sec + off::Section_name);
    return sn && sn->c_str() && strncmp(sn->c_str(), "de_slot_", 8) == 0;
}

static bool SectionAddItemRaw(void* sec, void* item, int qty)
{
    if (!sec || !item) return false;
    void** vt = *(void***)sec;
    if (!vt) return false;
    typedef bool (*fn_add_t)(void*, void*, int);
    fn_add_t f = (fn_add_t)vt[2];          // InventorySection::addItem (vtable 0x10)
    return f ? f(sec, item, qty) : false;
}

static void CleanupNpcExtraSections()
{
    if (g_pendingCount <= 0) return;

    // Give the game a moment after loading to finish populating the player
    // list / NPC spawns before deciding who is an NPC.
    static DWORD s_cleanupAt = 0;
    if (GameIsLoadingSave())
    {
        s_cleanupAt = GetTickCount() + 2000;
        return;
    }
    if (s_cleanupAt == 0) s_cleanupAt = GetTickCount() + 1500;
    if (GetTickCount() < s_cleanupAt) return;

    void* piface = nullptr;
    if (g_ppOu && *g_ppOu)
        piface = *(void**)((uint8_t*)*g_ppOu + 0x580);
    if (!piface) return;
    lektor<void*>& players = *(lektor<void*>*)((uint8_t*)piface + 0x2B0);
    if (!players.stuff || players.count == 0) return;   // wait until the player list is ready

    const LONG n = InterlockedExchange(&g_pendingCount, 0);
    if (n <= 0) return;

    int npc = 0, moved = 0, failed = 0;
    for (LONG k = 0; k < n && k < DE_MAX_PENDING; ++k)
    {
        void* ch = g_pendingChars[k];
        g_pendingChars[k] = nullptr;
        if (!ch) continue;
        __try
        {
            void* inv = *(void**)((uint8_t*)ch + off::Character_inventory);
            if (!inv) continue;
            if (IsPlayerCharacterInventory(inv)) continue;   // player: keep everything
            if (p_isPlayerChar && p_isPlayerChar(ch)) continue; // double check
            ++npc;

            for (int i = 0; i < kSectionCount; ++i)
            {
                MsvcString nm; nm.set(kSections[i].name);
                void* sec = p_getSection ? p_getSection(inv, &nm) : nullptr;
                if (!sec) continue;

                MsvcString vn; vn.set(kSections[i].name + 8);   // de_slot_head -> head
                void* vsec = p_getSection ? p_getSection(inv, &vn) : nullptr;

                for (int guard = 0; guard < 64; ++guard)
                {
                    void* item = p_sectionGetItem ? p_sectionGetItem(sec) : nullptr;
                    if (!item) break;
                    if (!p_sectionRemoveItem) { ++failed; break; }
                    if (!p_sectionRemoveItem(sec, item)) { ++failed; break; }

                    bool added = false;
                    if (vsec && p_sectionGetItem(vsec) == nullptr)
                        added = SectionAddItemRaw(vsec, item, 1);
                    if (!added && p_getAllSections)
                    {
                        lektor<void*>& all = p_getAllSections(inv);
                        for (uint32_t a = 0; a < all.count; ++a)
                        {
                            void* t = all.stuff[a];
                            if (!t || t == sec || SectionIsOurs(t)) continue;
                            if (SectionAddItemRaw(t, item, 1)) { added = true; break; }
                        }
                    }
                    if (!added)
                    {
                        SectionAddItemRaw(sec, item, 1);   // put it back, never lose it
                        ++failed;
                        break;
                    }
                    ++moved;
                }

                if (p_sectionSetEnabled) p_sectionSetEnabled(sec, false);
                else *(bool*)((uint8_t*)sec + 0xD0) = false;   // enabled
                *(bool*)((uint8_t*)sec + 0xB4) = false;        // isAnEquippedItemSection
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ++failed; }
    }
    s_cleanupAt = 0;
    LogF("[cleanup] npc=%d moved=%d failed=%d pending_left=%ld",
         npc, moved, failed, (long)g_pendingCount);
}


static void UiTickOnce(const char* source)
{
    if (InterlockedExchange(&g_uiReenter, 1)) return;

    static DWORD s_tickNext = 0;
    const DWORD nowMs = GetTickCount();
    if (nowMs < s_tickNext) { InterlockedExchange(&g_uiReenter, 0); return; }
    s_tickNext = nowMs + 30;

    __try
    {
        if (InterlockedExchange(&g_uiResetRequest, 0))
        {
            for (int i = 0; i < kSectionCount; ++i) g_slotWidgets[i] = nullptr;
            g_uiBuiltPanel = nullptr;
            g_btnRectCount = 0;
            g_uiChar = nullptr;
            g_uiGui = nullptr;
            LogF("[ui] reset after save load");
        }

        // Runs on the main thread, after the load has finished and the player
        // list exists. No-op unless the load hook queued characters.
        CleanupNpcExtraSections();

        // Re-enable extra sections for characters that became player characters
        // after the load (recruited NPCs). 1 s is enough and keeps the scan cheap.
        static DWORD s_playerEnsureNext = 0;
        const DWORD nowPE = GetTickCount();
        if (nowPE >= s_playerEnsureNext)
        {
            s_playerEnsureNext = nowPE + 1000;
            EnsureSectionsOnPlayerInventories();
            if (g_uiGui && p_getGuiInventory && p_refreshGuiSections &&
                InterlockedExchange(&g_sectionsReenabled, 0))
            {
                __try
                {
                    void* curInv = p_getGuiInventory(g_uiGui);
                    if (curInv && IsPlayerCharacterInventory(curInv))
                        p_refreshGuiSections(g_uiGui);
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { }
            }
        }

        static LONG s_srcLog = 0;
        if (InterlockedIncrement(&s_srcLog) <= 8) LogF("[ui] tick via %s", source);

        static DWORD s_recheckNext = 0;
        const DWORD nowTick = GetTickCount();
        if (nowTick >= s_recheckNext)
        {
            s_recheckNext = nowTick + 1000;
            MyGUI::Gui* mgui = MyGUI::Gui::getInstancePtr();
            MyGUI::Widget* head = FindUiWidget(kSections[0].name);
            static LONG s_rcLog = 0;
            if (InterlockedIncrement(&s_rcLog) <= 6)
                LogF("[ui] recheck head=%p gui=%p", head, (void*)mgui);
            if (!head)
            {
                for (int i = 0; i < kSectionCount; ++i) g_slotWidgets[i] = nullptr;
                g_uiBuiltPanel = nullptr;
                g_btnRectCount = 0;
            }
            else if (head != g_slotWidgets[0])
            {
                for (int i = 0; i < kSectionCount; ++i) g_slotWidgets[i] = FindUiWidget(kSections[i].name);
                g_uiBuiltPanel = nullptr;
                static LONG s_foundLog = 0;
                if (InterlockedIncrement(&s_foundLog) <= 4)
                    LogF("[ui] found widgets by name: head=%p panel=%p", g_slotWidgets[0], GetPanel());
            }
        }

        MyGUI::Widget* panel = GetPanel();
        if (!panel) { InterlockedExchange(&g_uiReenter, 0); return; }

        if (panel != g_uiBuiltPanel)
        {
            EnsureUi();
            RebuildUiRects();
            g_uiBuiltPanel = panel;
            static LONG s_builtLog = 0;
            if (InterlockedIncrement(&s_builtLog) <= 4)
                LogF("[ui] built from captured widgets: panel=%p w=%d visible=%d",
                     panel, panel->getWidth(), (int)UiWindowVisible());
        }

        if (g_uiGui)
        {
            if (!*(uint8_t*)((uint8_t*)g_uiGui + 0x59))
            {
                if (g_helpWindow) g_helpWindow->setVisible(false); else if (g_helpBox) g_helpBox->setVisible(false);
                InterlockedExchange(&g_uiReenter, 0);
                return;
            }
        }
        else if (!UiWindowVisible())
        {
            if (g_helpWindow) g_helpWindow->setVisible(false); else if (g_helpBox) g_helpBox->setVisible(false);
            InterlockedExchange(&g_uiReenter, 0);
            return;
        }
        { void* sel = FirstSelectedCharacter(); if (sel) g_uiChar = sel; }
        PollUiClicks();

        static DWORD s_next = 0;
        const DWORD now = GetTickCount();
        if (now >= s_next)
        {
            s_next = now + 250;
            EnsureUi();
            RefreshUi();
            RebuildUiRects();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        for (int i = 0; i < kSectionCount; ++i) g_slotWidgets[i] = nullptr;
        g_uiBuiltPanel = nullptr;
        g_btnRectCount = 0;
        static LONG s_err = 0;
        if (InterlockedIncrement(&s_err) <= 3) LogF("[ui] exception in UI tick");
    }
    InterlockedExchange(&g_uiReenter, 0);
}

static void __cdecl Detour_worldTick(void* world, float time)
{
    if (p_worldTickOrig) p_worldTickOrig(world, time);
    UiTickOnce("world-tick");
}

static void __cdecl Detour_factoryTick()
{
    if (p_factoryTickOrig) p_factoryTickOrig();
    UiTickOnce("factory");
}

__declspec(dllexport) void startPlugin()
{
    LogF("[slots] ===== DualEquipSlots v2 loading =====");

    LoadSettings();                      // per-slot stats switches (creates the file)
    InitializeCriticalSection(&g_optLock);
    g_optLockReady = true;
    LoadOptionMap();                     // per-character options (sidecar, keyed by InstanceID)
    LoadHelpText();                      // help window text (per game language)

    HMODULE klib = GetModuleHandleA("KenshiLib.dll");
    if (!klib) klib = LoadLibraryA("KenshiLib.dll");
    if (!klib) { LogF("[slots] KenshiLib.dll missing"); return; }
    g_pKenshiLib = (void*)klib;

    p_AddHook         = (fn_AddHook_t)        Sym(SYM_ADD_HOOK);
    p_GetRealAddress  = (fn_GetRealAddress_t) Sym(SYM_GET_REAL_ADDR);
    if (p_GetRealAddress) LogF("[slots] GetRealAddress resolved - hooks will use REAL game addresses");
    else                  LogF("[slots] WARNING: GetRealAddress missing - hooks may never fire");
    p_isPlayerChar    = (fn_isPlayerChar_t)   Sym(SYM_IS_PLAYER);
    p_isLoadingSave   = (fn_isLoadingSave_t)  Sym(SYM_IS_LOADING_SAVE);
    LogF("[slots] isLoadingFromASaveGame = %p", p_isLoadingSave);
    p_getSection      = (fn_getSection_t)     Sym(SYM_GET_SECTION);
    p_sectionGetItem  = (fn_sectionGetItem_t) Sym(SYM_SECTION_GETITEM);
    p_sectionRemoveItem = (fn_sectionRemoveItem_t)Sym(SYM_SECTION_REMOVEITEM);
    p_sectionSetEnabled = (fn_sectionSetEnabled_t)Sym(SYM_SECTION_SETENABLED);
    if (!p_sectionGetItem) LogF("[slots] WARNING: InventorySection::getItem unresolved (vtable hook needs it)");
    p_initSection     = (fn_initSection_t)    Sym(SYM_NEW_SECTION);
    p_resizeSection   = (fn_resizeSection_t)  Sym(SYM_RESIZE_SECTION);
    p_getSectionOfType= (fn_getSectionOfType_t)Sym(SYM_SECTION_OF_TYPE);
    p_getAllSections  = (fn_getAllSections_t) Sym(SYM_ALL_SECTIONS);
    p_getInstanceID   = (fn_getInstanceID_t)  Sym(SYM_GET_INSTANCE_ID);
    p_getLayoutInstanceID = (fn_getLayoutID_t) Sym(SYM_GET_LAYOUT_ID);
    p_getHandle = (fn_getHandle_t) Sym(SYM_GET_HAND);
    p_isPlayerRoot = (fn_isPlayerRoot_t) Sym(SYM_IS_PLAYER_ROOT);
    if (!p_isPlayerRoot) LogF("[slots] WARNING: PlayerInterface::_isPlayerCharacter unresolved - weight zeroing disabled for extra slots");
    p_getAllSelected  = (fn_getAllSelected_t) Sym(SYM_GET_ALL_SELECTED);
    LogF("[slots] getInstanceID = %p  getLayoutInstanceID = %p  getHandle = %p  isPlayerRoot = %p", (void*)p_getInstanceID, (void*)p_getLayoutInstanceID, (void*)p_getHandle, (void*)p_isPlayerRoot);
    if (!p_getInstanceID) LogF("[slots] WARNING: getInstanceID unresolved - per-character options cannot persist");
    if (!p_getAllSelected) LogF("[slots] WARNING: getAllSelectedObjects unresolved - apply-to-selection disabled");

    if (!p_AddHook) { LogF("[slots] AddHook missing - disabled"); return; }

    // -----------------------------------------------------------------------
    //  FOUR HOOKS ONLY. Everything else was measured and removed - see the
    //  "WHICH HOOKS ARE LIVE" comment further up this file for the full table.
    //
    //  Reminder: use RealSym(), never Sym(). Sym() returns KenshiLib's forwarding
    //  stub, and a hook installed on that silently never fires.
    // -----------------------------------------------------------------------

    // 1) THE SAVE/LOAD FIX: fires for EVERY Inventory the game constructs,
    //    including the ones built while reading a save, and always BEFORE items
    //    are placed into it.
    void* t1 = (void*)RealSym(SYM_INV_CTOR);
    void* o1 = nullptr;
    if (t1 && p_AddHook(t1, (void*)&Detour_inventoryCtor, &o1) == 0)
    {
        p_invCtorOrig = (fn_invCtor_t)o1;
        LogF("[slots] hook OK: Inventory::Inventory  (slots on every inventory)");
    }
    else LogF("[slots] FAILED to hook Inventory::Inventory");

    // 2) Create a section on demand if anything looks it up by name first.
    void* t2 = (void*)RealSym(SYM_GET_SECTION_HOOK);
    void* o2 = nullptr;
    if (t2 && p_AddHook(t2, (void*)&Detour_getSection, &o2) == 0)
    {
        p_getSectionOrig = (fn_getSection_t)o2;
        LogF("[slots] hook OK: Inventory::getSection  (on-demand creation)");
    }
    else LogF("[slots] FAILED to hook Inventory::getSection");

    // 2b) SAVE LOAD (PRIMARY): Character::setupInventorySections is called once
    //     per character while a save is being deserialised, and it hands us the
    //     Character directly - so no lookup is needed and it is inherently
    //     "player characters only". This is the hook that actually makes items in
    //     extra slots survive a load.
    //
    //     Its registration was accidentally dropped during a cleanup pass on
    //     2026-10-06; without it, items in de_slot_* vanished after loading
    //     (measured: loadAllPlatoons fires with an EMPTY player list, and the
    //     polling thread is ~17 s too late).
    void* t2a = (void*)RealSym(SYM_SETUP_INV_SECTIONS);
    void* o2a = nullptr;
    if (t2a && p_AddHook(t2a, (void*)&Detour_setupInventorySections, &o2a) == 0)
    {
        p_setupInvSectionsOrig = (fn_setupInvSections_t)o2a;
        LogF("[slots] hook OK: Character::setupInventorySections  (save load)");
    }
    else LogF("[slots] FAILED to hook Character::setupInventorySections");

    // 2c) REMOVED 2026-10-20: the GameWorld::loadAllPlatoons fallback.
    //
    //  It was measured doing nothing useful - every run logged
    //      "scanned 0 player inventories, created 0"
    //  because PlayerInterface::playerCharacters is still empty at that moment.
    //  The per-character hook below (2d) is the real mechanism, so this hook was
    //  pure risk with no benefit. Kept only as a note; the function body above
    //  is dead but harmless.
    //
    //  Same reasoning for wraps::BaseLayout::initialise (removed at 4): since we
    //  ship our layout under the VANILLA file name, nothing needs redirecting, so
    //  that hook was a pure pass-through - again pure risk.

    // 2d) THE ROOT FIX: per-character section creation while a save is read.
    //     Gives us the Character directly, so character inventories get sections
    //     and nothing else does - no guessing, no player list.
    void* t2c = (void*)RealSym(SYM_CHAR_LOAD_FROM_SERIALISE);
    void* o2c = nullptr;
    if (t2c && p_AddHook(t2c, (void*)&Detour_charLoadFromSerialise, &o2c) == 0)
    {
        p_charLoadFromSerialiseOrig = (fn_charLoadFromSerialise_t)o2c;
        LogF("[slots] hook OK: Character::loadFromSerialise  (per-character load)");
    }
    else LogF("[slots] FAILED to hook Character::loadFromSerialise");
    // 2d) THE CRASH FIX: never let a layout assert on a section widget it does
    //     not have. Covers the limb window, the trader window and every other
    //     layout we cannot ship an override for.
    // Character::equipItem(slot, item) - the slot-name equip path
    void* tEq2 = (void*)RealSym("?equipItem@Character@@UEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@PEAVItem@@@Z");
    void* oEq2 = nullptr;
    if (tEq2 && p_AddHook(tEq2, (void*)&Detour_charEquipItem, &oEq2) == 0)
    {
        p_charEquipItemOrig = (fn_charEquipItem_t)oEq2;
        LogF("[slots] hook OK: Character::equipItem  (slot-name equip probe)");
    }
    else LogF("[slots] FAILED to hook Character::equipItem");

    // Inventory::addItem(Item*,int,bool,bool) - the destroyOnFail path
    void* tIA = (void*)RealSym("?addItem@Inventory@@UEAA_NPEAVItem@@H_N1@Z");
    void* oIA = nullptr;
    if (tIA && p_AddHook(tIA, (void*)&Detour_invAddItem, &oIA) == 0)
    {
        p_invAddItemOrig = (fn_invAddItem_t)oIA;
        LogF("[slots] hook OK: Inventory::addItem  (destroyOnFail probe)");
    }
    else LogF("[slots] FAILED to hook Inventory::addItem");

    // Inventory::transferMouseItem(Item*) - item dragged/right-clicked with the mouse
    void* tTM = (void*)RealSym("?transferMouseItem@Inventory@@QEAA_NPEAVItem@@@Z");
    void* oTM = nullptr;
    if (tTM && p_AddHook(tTM, (void*)&Detour_transferMouseItem, &oTM) == 0)
    {
        p_transferMouseOrig = (fn_transferMouse_t)oTM;
        LogF("[slots] hook OK: Inventory::transferMouseItem  (mouse transfer probe)");
    }
    else LogF("[slots] FAILED to hook Inventory::transferMouseItem");

    // Inventory::equipItem(Item*) - right-click equip path
    void* tEq = (void*)RealSym("?equipItem@Inventory@@QEAA_NPEAVItem@@@Z");
    void* oEq = nullptr;
    if (tEq && p_AddHook(tEq, (void*)&Detour_invEquipItem, &oEq) == 0)
    {
        p_invEquipItemOrig = (fn_invEquipItem_t)oEq;
        LogF("[slots] hook OK: Inventory::equipItem  (right-click guard)");
    }
    else LogF("[slots] FAILED to hook Inventory::equipItem");

    void* t2d = (void*)RealSym(SYM_LAYOUT_GET_WIDGET);
    void* o2d = nullptr;
    if (t2d && p_AddHook(t2d, (void*)&Detour_layoutGetWidget, &o2d) == 0)
    {
        p_layoutGetWidget = (fn_layoutGetWidget_t)o2d;
        LogF("[slots] hook OK: InventoryLayout::getWidget  (crash guard)");
    }
    else LogF("[slots] FAILED to hook InventoryLayout::getWidget");

    void* t2e = (void*)RealSym(SYM_LAYOUT_GET_WINDOW);
    p_layoutGetWindow = (fn_layoutGetWindow_t)t2e;
    if (p_layoutGetWindow) LogF("[slots] getWindow resolved (root-widget fallback ready)");
    else LogF("[slots] WARNING: getWindow unresolved - fallback will return null");


    // 3) MANDATORY equip-routing guard: without it, equipping crashes the game.
    void* t3 = (void*)RealSym(SYM_SECTION_OF_TYPE);
    void* o3 = nullptr;
    if (t3 && p_AddHook(t3, (void*)&Detour_getSectionOfType, &o3) == 0)
    {
        p_getSectOfTypeOrig = (fn_getSectionOfType_t)o3;
        LogF("[slots] hook OK: Inventory::getSectionOfType  (equip-routing guard)");
    }
    else LogF("[slots] FAILED to hook Inventory::getSectionOfType");

    // 3b) NUMERIC ZEROING: extra-slot items must not add carried weight.
    //     Stats are already excluded by the engine (getEquippedArmour only knows
    //     the vanilla section names); weight is the one number that still leaks.
    void* t3b = (void*)RealSym(SYM_GET_TOTAL_WEIGHT);
    void* o3b = nullptr;
    if (t3b && p_AddHook(t3b, (void*)&Detour_getTotalWeight, &o3b) == 0)
    {
        p_getTotalWeightOrig = (fn_getTotalWeight_t)o3b;
        LogF("[slots] hook OK: Inventory::getTotalWeight  (extra slots carry no weight)");
    }
    else LogF("[slots] FAILED to hook Inventory::getTotalWeight");

    // 3c) STATS ZEROING: keep extra-slot items out of the equipment-bonus list
    //     for the duration of the one recalculation that builds CharStats.
    void* t3c = (void*)RealSym(SYM_RECALC_EQUIP);
    void* o3c = nullptr;
    if (t3c && p_AddHook(t3c, (void*)&Detour_recalcEquip, &o3c) == 0)
    {
        p_recalcEquipOrig = (fn_recalcEquip_t)o3c;
        LogF("[slots] hook OK: Character::recalculateTotalEquipmentSkillBonus  (extra slots give no stats)");
    }
    else LogF("[slots] FAILED to hook Character::recalculateTotalEquipmentSkillBonus");

    // 3d) HIT-TIME COVERAGE: extra-slot armour must never absorb a hit.
    void* t3d = (void*)RealSym(SYM_DID_HIT_FLESH);
    void* o3d = nullptr;
    if (t3d && p_AddHook(t3d, (void*)&Detour_didHitFlesh, &o3d) == 0)
    {
        p_didHitFleshOrig = (fn_didHitFlesh_t)o3d;
        LogF("[slots] hook OK: Armour::didIHitFlesh  (extra slots give no coverage/resistance)");
    }
    else LogF("[slots] FAILED to hook Armour::didIHitFlesh");

    // 3e) IN-GAME UI (2026-10-22): buttons inside the EXTRA SET panel.
    void* tUi = (void*)RealSym(SYM_INV_GUI_SHOW);
    void* oUi = nullptr;
    if (tUi && p_AddHook(tUi, (void*)&Detour_invGuiShow, &oUi) == 0)
    {
        p_invGuiShowOrig = (fn_invGuiShow_t)oUi;
        p_getGuiInventory = (fn_getGuiInventory_t)Sym("?getInventory@InventoryGUI@@UEAAPEAVInventory@@XZ");
        p_refreshGuiSections = (fn_refreshGuiSections_t)Sym("?refreshSection@InventoryGUI@@QEAAXXZ");
        LogF("[slots] hook OK: InventoryGUI::show  (extra-slot buttons)");
    }
    else LogF("[slots] FAILED to hook InventoryGUI::show");

    void* tTick = (void*)RealSym(SYM_WORLD_TICK);
    void* oTick = nullptr;
    if (tTick && p_AddHook(tTick, (void*)&Detour_worldTick, &oTick) == 0)
    {
        p_worldTickOrig = (fn_worldTick_t)oTick;
        LogF("[slots] hook OK: GameWorld::mainLoop_GPUSensitiveStuff  (UI refresh)");
    }
    else LogF("[slots] FAILED to hook GameWorld tick");

    void* tFac = (void*)RealSym(SYM_FACTORY_TICK);
    void* oFac = nullptr;
    if (tFac && p_AddHook(tFac, (void*)&Detour_factoryTick, &oFac) == 0)
    {
        p_factoryTickOrig = (fn_factoryTick_t)oFac;
        LogF("[slots] hook OK: RootObjectFactory::mainThreadUpdate  (UI tick)");
    }
    else LogF("[slots] FAILED to hook RootObjectFactory::mainThreadUpdate");

    // 4) REMOVED 2026-10-20: wraps::BaseLayout::initialise is no longer hooked.
    //
    //  It was a pure PASS-THROUGH: we ship our layout under the VANILLA file name,
    //  so there is nothing to redirect. A redirect here once caused
    //  "No root widget" + a crash on opening the bag (measured 2026-10-05), and a
    //  pass-through hook buys nothing while still putting our detour in the path
    //  of every layout load. If a redirect is ever needed again, re-add it here -
    //  but only WITH a reason, not as a leftover.


    LogF("[slots] loaded; %d extra section definitions ready", kSectionCount);

    // Resolve the ADDRESS of the ?ou global once (for the thread fallback).
    g_ppOu = (void**)Sym(SYM_OU);
    if (!g_ppOu) LogF("[slots] WARNING: ?ou unresolved - thread fallback cannot find the world");

    // Belt-and-braces: _NV_init did not demonstrably fire on 2026-10-04, so also
    // poll for the world from a thread. Idempotent and SEH-guarded.
    HANDLE th = CreateThread(nullptr, 0, SlotsThread, nullptr, 0, nullptr);
    if (th) { CloseHandle(th); LogF("[slots] thread fallback started"); }
    else      LogF("[slots] thread fallback FAILED to start");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID)
{
    if (r == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
