// ============================================================================
//  DualEquip2 - hide/show appearance for the vanilla equipment slots.
//
//  STATUS: the detach/attach path IS ENABLED (DUAL_EQUIP_ENABLE_DETACH = 1
//  below) and working. The crash that forced it off on 2026-10-04 was solved on
//  2026-10-06 by the InventoryLayout::getWidget crash guard in DualEquipSlots2
//  (see the note there): MyGUI aborted the process because a layout had no
//  widget for a de_slot_* section. The "Suspects" list that used to live here
//  is obsolete - the real cause was found and is documented in the other file.
//
// ---------------------------------------------------------------------------
//  HOW THE EXTRA SLOTS RENDER  (corrected 2026-10-07 - the previous note here
//  was WRONG and cost a user report)
//
//  This header used to claim the extra sections are "invisible BY
//  CONSTRUCTION". That was true of an early prototype, which created them with
//  no attach-slot type. It is NOT true of the shipped code: DualEquipSlots2
//  creates them with REAL attach slots -
//
//      de_slot_head   -> ATTACH_HAT
//      de_slot_shirt  -> ATTACH_SHIRT
//      de_slot_legs   -> ATTACH_LEGS
//      de_slot_armour -> ATTACH_BODY
//      de_slot_boots  -> ATTACH_BOOTS
//
//  - so the appearance system DOES know them and an item placed in an extra
//  slot DOES render. (The old note cited getAttachedEntity/hasSlot being
//  negative for de_slot_*; that measurement predates the switch to real attach
//  slots and no longer holds.)
//
//  CONSEQUENCE FOR HIDING - this is the part users notice:
//  an extra slot and its vanilla counterpart share ONE socket, so hiding the
//  vanilla slot only detaches the vanilla item. If the extra slot also holds an
//  item, that item is still attached and its model becomes visible - and if it
//  is the only attachment left, it becomes THE appearance.
//
//  So "hide" does not mean "show nothing". It means "show whatever is still
//  attached", which is the extra slot's item when one is equipped.
//
//      vanilla slot -> the item you want to SEE
//      extra slot   -> the item you only want the STATS from
//
//  Following that split gives the intended result. Putting a decorative item in
//  an extra slot and then hiding the vanilla slot will display the decorative
//  item instead - which is correct behaviour, not a rendering bug.
//
//  KNOWN LIMITATION: there is currently no way to hide BOTH items of one body
//  part. The two slots share a socket name, so detaching the second by name is
//  ambiguous; an earlier attempt to detach by index removed the wrong item, so
//  it was abandoned. Revisit only with a reliable way to tell the two
//  attachments apart.
// ============================================================================

#define DUAL_EQUIP_ENABLE_DETACH 1

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

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
    constexpr size_t Section_name        = 0x8;   // Inventory.h:87
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
typedef void* (*fn_charInventory_t)(void* character);            // Character::_NV_getInventory
typedef void* (*fn_charAppearance_t)(void* character);           // Character::getAppearance
typedef void* (*fn_getSectionOfType_t)(void* inv, int slot);
typedef void* (*fn_sectionGetItem_t)(void* section);             // InventorySection::getItem
typedef void* (*fn_getAttachedEntity_t)(void* app, const MsvcString* slot);
typedef bool  (*fn_detachSlot_t)(void* app, const MsvcString* slot);
typedef bool (*fn_attachItemSlot_t)(void* app, void* item, const MsvcString* slot);



// ---------------------------------------------------------------------------
//  REVERTED 2026-10-18 - the slot-name substitution has been removed.
//
//  WHAT IT WAS FOR
//  A shirt placed in an EXTRA slot was invisible on characters who had lost a
//  limb. The engine resolves some rendering through the SLOT NAME, and our
//  sections are called de_slot_*, which it does not know, so those items took a
//  fallback path. On a complete body the fallback is fine; on an amputee the
//  already-partial body mesh collapsed and the shirt vanished.
//
//  WHAT IT DID
//  It rewrote the name on attachItem (de_slot_shirt -> shirt), which did fix the
//  amputee case. But that name is ALSO the attachment key. With both slots
//  holding something, the vanilla item and the extra item now claimed the SAME
//  key, so only one of them could exist at a time.
//
//  WHAT USERS SAW (reported within a day, four separate comments)
//      "the second outfit on head / legs / shirt / body / boots conflicts with
//       the first - rendering can only be one or the other"
//      "after restarting the game, wearing two items makes BOTH disappear"
//      "touching the extra item overwrites the basic one, and vice versa"
//
//  Simultaneous rendering of two sets is THE core feature of this mod, so a fix
//  that breaks it is not a fix. Reverted to the previous behaviour: attach and
//  detach are no longer hooked at all, every item keeps its own unique slot name,
//  and two sets render side by side again.
//
//  CONSEQUENCE: the amputee-shirt problem is BACK and is now documented as a
//  known limitation. A correct fix needs the material/socket lookup to be
//  corrected WITHOUT changing the attachment key - a separate research task,
//  noted in 文档/修复_缺肢体衬衫不可见.md. Do not re-add a name rewrite here.
// ---------------------------------------------------------------------------


typedef bool  (*fn_attachmentsLoaded_t)(void* app);
typedef void  (*fn_notifyDirty_t)(void* app);
typedef bool  (*fn_isPlayerChar_t)(void* character);

typedef void  (*fn_postUpdate_t)(void* character);
typedef void  (*fn_removeFromUpdateList_t)(void* world, void* character);
typedef int   (*fn_AddHook_t)(void* target, void* detour, void** original);

static fn_charInventory_t        p_charInventory      = nullptr;
static fn_charAppearance_t       p_charAppearance     = nullptr;
static fn_getSectionOfType_t     p_getSectionOfType   = nullptr;
static fn_sectionGetItem_t       p_sectionGetItem     = nullptr;
static fn_getAttachedEntity_t    p_getAttachedEntity  = nullptr;
static fn_detachSlot_t           p_detachSlot         = nullptr;
static fn_attachItemSlot_t       p_attachItemSlot     = nullptr;
typedef void* (*fn_getSection_t)(void* inv, const MsvcString* name);
static fn_getSection_t           p_getSection         = nullptr;
static fn_attachmentsLoaded_t    p_attachmentsLoaded  = nullptr;
static fn_notifyDirty_t          p_notifyDirty        = nullptr;
static fn_isPlayerChar_t         p_isPlayerChar       = nullptr;
static fn_AddHook_t              p_AddHook            = nullptr;

static fn_postUpdate_t           p_postUpdateOriginal = nullptr;
static fn_removeFromUpdateList_t p_removeListOriginal = nullptr;

static void* g_pKenshiLib = nullptr;
static bool  g_hidden     = false;

// ---------------------------------------------------------------------------
#define SYM_ADD_HOOK         "?AddHook@KenshiLib@@YA?AW4HookStatus@1@PEAX0PEAPEAX@Z"
#define SYM_IS_PLAYER        "?isPlayerCharacter@Character@@QEBA_NXZ"
#define SYM_CHAR_INV         "?_NV_getInventory@Character@@QEBAPEAVInventory@@XZ"
#define SYM_CHAR_APP         "?getAppearance@Character@@QEAAPEAVAppearanceBase@@XZ"
#define SYM_SECTION_OF_TYPE  "?getSectionOfType@Inventory@@QEAAPEAVInventorySection@@W4AttachSlot@@@Z"
#define SYM_SECTION_GETITEM  "?getItem@InventorySection@@QEBAPEAVItem@@XZ"
#define SYM_GET_ATTACHED     "?getAttachedEntity@AppearanceBase@@QEBAPEAVAttachedEntity@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
// PROBE 2026-10-21 (third shirt attempt): does the build pass see our items?
#define SYM_DETACH_SLOT      "?detachItem@AppearanceBase@@QEAA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
// SHIRT FIX v2: the position query (bone/offset), hooked so our slot names can be
// answered with the vanilla bone without touching any attachment key.
// Needed to install hooks: Sym() returns KenshiLib's forwarding stub, and hooking
// the stub installs a detour that never fires (learned the hard way).
// detachItem(Item*) - see the note next to fn_detachItem_t.
#define SYM_ATTACH_ITEM      "?attachItem@AppearanceBase@@QEAA_NPEAVItem@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
#define SYM_ATTACH_LOADED    "?getAttachmentsLoaded@AppearanceBase@@QEBA_NXZ"
#define SYM_NOTIFY_DIRTY     "?notifyDirty@AppearanceBase@@QEAAXXZ"
#define SYM_POST_UPDATE      "?_NV_postUpdate@Character@@QEAAXXZ"
#define SYM_REMOVE_LIST      "?removeFromUpdateListMain@GameWorld@@QEAAXPEAVCharacter@@@Z"
#define SYM_OU               "?ou@@3PEAVGameWorld@@EA"
// Currently selected objects - used by the single-character hotkey.
#define SYM_GET_ALL_SELECTED "?getAllSelectedObjects@PlayerInterface@@QEAAXAEAV?$lektor@PEAVRootObject@@@@W4itemType@@@Z"
// Inventory::getSection(name) - used by the SHIRT PROBE to reach our extra sections.
#define SYM_GET_SECTION      "?getSection@Inventory@@QEBAPEAVInventorySection@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"

// The body parts we manage, paired with their vanilla section names.
struct SlotMap { int attachSlot; const char* vanillaSection; };
static const SlotMap kSlots[] =
{
    { ATTACH_HAT,    "head"   },
    { ATTACH_SHIRT,  "shirt"  },
    { ATTACH_LEGS,   "legs"   },
    { ATTACH_BODY,   "armour" },
    { ATTACH_BOOTS,  "boots"  },
    { ATTACH_BELT,   "belt"   },
};
static const int kSlotCount = (int)(sizeof(kSlots) / sizeof(kSlots[0]));

// ---------------------------------------------------------------------------
static void LogF(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (FILE* f = fopen("DualEquip.log", "a"))
    {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
        fclose(f);
    }
}



static FARPROC Sym(const char* n)
{
    FARPROC p = GetProcAddress((HMODULE)g_pKenshiLib, n);
    if (!p) LogF("[hide] symbol NOT FOUND: %s", n);
    return p;
}



// ---------------------------------------------------------------------------
// Throttle: bounded fixed cache keyed by character pointer, same idea as the
// reference mod. A collision costs one extra check, never a skipped action.
// ---------------------------------------------------------------------------
static const int kThrottleSize = 1024;
struct ThrottleEntry { void* ch; DWORD nextMs; };
static ThrottleEntry g_throttle[kThrottleSize];

static bool Throttled(void* ch, DWORD now, DWORD delayMs)
{
    size_t idx = ((uintptr_t)ch >> 4) & (kThrottleSize - 1);
    ThrottleEntry& e = g_throttle[idx];
    if (e.ch == ch && now < e.nextMs) return true;
    e.ch = ch;
    e.nextMs = now + delayMs;
    return false;
}

static void ClearThrottleFor(void* ch)
{
    size_t idx = ((uintptr_t)ch >> 4) & (kThrottleSize - 1);
    if (g_throttle[idx].ch == ch) { g_throttle[idx].ch = nullptr; g_throttle[idx].nextMs = 0; }
}


// ---------------------------------------------------------------------------
//  EXTRA-SHIRT FIX (2026-10-22)
//
//  Measured root cause: an extra-slot shirt is attached under the section
//  name "de_slot_shirt". The appearance system looks attachment data up by
//  that name, finds nothing, and creates the AttachedEntity without the Ogre
//  attach objects the vanilla "shirt" attachment has (live dump: +0x30 and
//  +0x38 are null). On a character missing a limb the body mesh differs and
//  the fallback transform collapses the shirt inward.
//
//  The 1.0.18 attempt (attach the extra shirt under the vanilla name "shirt")
//  rendered correctly but collided with the vanilla shirt: one key can only
//  hold one entity, so the two shirts ate each other.
//
//  This version uses the vanilla key ONLY while it is free:
//      * no vanilla shirt worn, or
//      * the player has hidden the vanilla set (its attachment is detached).
//  In both cases the extra shirt is attached as "shirt" and gets the same
//  attachment data as a vanilla shirt. When both shirts must be visible the
//  vanilla key is taken, so the extra shirt keeps its own name (old
//  behaviour - both render, amputee case still imperfect).
//
//  Everything goes through the engine's own name-keyed calls; no map surgery,
//  no item fields touched.
// ---------------------------------------------------------------------------
struct ShirtFixState { void* ch; void* item; void* entity; bool asVanilla; };
static ShirtFixState g_shirtFix[256];

static ShirtFixState& ShirtFixFor(void* ch)
{
    const size_t idx = ((uintptr_t)ch >> 4) & 255;
    for (size_t p = 0; p < 8; ++p)
    {
        ShirtFixState& e = g_shirtFix[(idx + p) & 255];
        if (e.ch == ch) return e;
        if (e.ch == nullptr)
        {
            e.ch = ch; e.item = nullptr; e.entity = nullptr; e.asVanilla = false;
            return e;
        }
    }
    return g_shirtFix[idx & 255];
}

static void ManageExtraShirt(void* ch, void* inv, void* app, bool hide, ShirtFixState& sf)
{
    if (!inv || !app || !p_getSection || !p_sectionGetItem) return;

    MsvcString vanName;   vanName.set("shirt");
    MsvcString extraName; extraName.set("de_slot_shirt");

    void* vanSec    = p_getSection(inv, &vanName);
    void* extraSec  = p_getSection(inv, &extraName);
    void* vanItem   = vanSec   ? p_sectionGetItem(vanSec)   : nullptr;
    void* extraItem = extraSec ? p_sectionGetItem(extraSec) : nullptr;

    // The vanilla key is usable when no vanilla shirt is worn, or while the
    // vanilla appearance is hidden.
    const bool wantAsVanilla = (extraItem != nullptr) && (hide || vanItem == nullptr);

    if (wantAsVanilla)
    {
        void* cur = p_getAttachedEntity ? p_getAttachedEntity(app, &vanName) : nullptr;
        if (!sf.asVanilla || sf.item != extraItem || (sf.entity && cur != sf.entity))
        {
            if (cur && p_detachSlot) p_detachSlot(app, &vanName);
            void* broken = p_getAttachedEntity ? p_getAttachedEntity(app, &extraName) : nullptr;
            if (broken && p_detachSlot) p_detachSlot(app, &extraName);

            const bool ok = p_attachItemSlot ? p_attachItemSlot(app, extraItem, &vanName) : false;
            if (p_notifyDirty) p_notifyDirty(app);

            sf.entity = p_getAttachedEntity ? p_getAttachedEntity(app, &vanName) : nullptr;
            sf.item   = extraItem;
            sf.asVanilla = true;

            static LONG s_log = 0;
            if (InterlockedIncrement(&s_log) <= 24)
                LogF("[shirt] extra -> vanilla key (hide=%d ok=%d item=%p ent=%p)",
                     (int)hide, (int)ok, extraItem, sf.entity);
        }
        else
        {
            // The engine may re-create the broken attachment under our name;
            // drop it so only the correctly-shaped entity survives.
            void* broken = p_getAttachedEntity ? p_getAttachedEntity(app, &extraName) : nullptr;
            if (broken && p_detachSlot) p_detachSlot(app, &extraName);
        }
    }
    else if (sf.asVanilla)
    {
        if (p_getAttachedEntity && p_getAttachedEntity(app, &vanName) && p_detachSlot)
            p_detachSlot(app, &vanName);
        // Only restore the vanilla appearance when we are not in hidden mode.
        if (!hide && vanItem && p_attachItemSlot) p_attachItemSlot(app, vanItem, &vanName);
        if (extraItem && p_attachItemSlot) p_attachItemSlot(app, extraItem, &extraName);
        if (p_notifyDirty) p_notifyDirty(app);

        static LONG s_log2 = 0;
        if (InterlockedIncrement(&s_log2) <= 24)
            LogF("[shirt] extra -> own key (hide=%d vanItem=%p extraItem=%p)",
                 (int)hide, vanItem, extraItem);

        sf.asVanilla = false; sf.item = nullptr; sf.entity = nullptr;
    }
}



// ---------------------------------------------------------------------------
//  Per-character options live in DualEquipSlots2.dll (it owns the store and the
//  save-side persistence). We only consume the bits here:
//    2*i+1 : slot i option 2 - hide mode keeps that vanilla slot visible
//    10    : backpack appearance hidden
// ---------------------------------------------------------------------------
typedef int (*fn_getOpts_t)(void* character);
static fn_getOpts_t p_getOpts = nullptr;

static int GetOptionBits(void* character)
{
    if (!p_getOpts)
    {
        HMODULE m = GetModuleHandleA("DualEquipSlots2.dll");
        if (m) p_getOpts = (fn_getOpts_t)GetProcAddress(m, "DE_GetOptionBits");
    }
    return (p_getOpts && character) ? p_getOpts(character) : 0;
}

typedef void (*fn_setHidden_t)(void* character, int hidden);
static fn_setHidden_t p_setHidden = nullptr;

static void ResolveSetHidden()
{
    if (p_setHidden) return;
    HMODULE m = GetModuleHandleA("DualEquipSlots2.dll");
    if (m) p_setHidden = (fn_setHidden_t)GetProcAddress(m, "DE_SetCharHidden");
}

// kSlots[] order: head(0), shirt(1), legs(2), armour(3), boots(4), belt(5)
// kSections[] order: head(0), armour(1), legs(2), shirt(3), boots(4)
static int ExtraIndexForSlot(int slotIndex)
{
    switch (slotIndex)
    {
        case 0: return 0;   // head
        case 1: return 3;   // shirt
        case 2: return 2;   // legs
        case 3: return 1;   // armour
        case 4: return 4;   // boots
        default: return -1; // belt has no extra slot
    }
}


// ---------------------------------------------------------------------------
// Hide or show every managed vanilla section on one character.
// ---------------------------------------------------------------------------
static void ApplyOne(void* character, bool hide)
{
    // ---- one-shot trace of which early-exit we hit (diagnostic) -----------
    static bool s_traced = false;
    const bool trace = !s_traced;

    if (!character) { if (trace) { s_traced = true; LogF("[hide] TRACE exit: null character"); } return; }
    if (p_isPlayerChar && !p_isPlayerChar(character))
    { if (trace) { s_traced = true; LogF("[hide] TRACE exit: not a player character"); } return; }

    void* inv = p_charInventory ? p_charInventory(character) : nullptr;
    if (!inv) { if (trace) { s_traced = true; LogF("[hide] TRACE exit: no inventory"); } return; }
    void* app = p_charAppearance ? p_charAppearance(character) : nullptr;
    if (!app) { if (trace) { s_traced = true; LogF("[hide] TRACE exit: no appearance"); } return; }
    if (p_attachmentsLoaded && !p_attachmentsLoaded(app))
    { if (trace) { s_traced = true; LogF("[hide] TRACE exit: attachments not loaded yet"); } return; }

    if (trace)
    {
        s_traced = true;
        LogF("[hide] TRACE passed all guards (char=%p inv=%p app=%p)", character, inv, app);
    }

    ShirtFixState& sf = ShirtFixFor(character);
    const int optionBits = GetOptionBits(character);

    for (int i = 0; i < kSlotCount; ++i)
    {
        // Slot 1 is "shirt". While the extra-shirt fix owns the vanilla key we
        // must not detach/attach it from here - ManageExtraShirt() below does.
        if (i == 1 && sf.asVanilla) continue;

        void* sec = p_getSectionOfType(inv, kSlots[i].attachSlot);
        if (!sec) { if (trace) LogF("[hide] TRACE  [%d] type=%d -> no section", i, kSlots[i].attachSlot); continue; }

        const MsvcString* secName = (const MsvcString*)((uint8_t*)sec + off::Section_name);
        if (!secName || !secName->c_str() || secName->size == 0)
        { if (trace) LogF("[hide] TRACE  [%d] type=%d -> section has no name", i, kSlots[i].attachSlot); continue; }
        MsvcString slot; slot.set(secName->c_str());

        void* item = p_sectionGetItem ? p_sectionGetItem(sec) : nullptr;
        if (!item)
        { if (trace) LogF("[hide] TRACE  [%d] section='%s' -> EMPTY (no item)", i, secName->c_str()); continue; }

        void* attached = p_getAttachedEntity ? p_getAttachedEntity(app, &slot) : nullptr;

        if (trace)
            LogF("[hide] TRACE  [%d] section='%s' item=%p attached=%p",
                 i, secName->c_str(), item, attached);

        if (hide)
        {
            const int xi = ExtraIndexForSlot(i);
            const bool slotDisabled = (xi >= 0) && ((optionBits >> (2 * xi + 1)) & 1);
            if (slotDisabled)
            {
                // Per-character option 2: this extra slot is off, so hiding must
                // NOT hide the vanilla item - re-attach it if it was detached.
                if (!attached && p_attachItemSlot) p_attachItemSlot(app, item, &slot);
                continue;
            }
            if (!attached) continue;

            // REVERTED 2026-10-20 (later the same day): detaching by ITEM pointer
            // does not work here - hiding stopped working entirely.
            //
            // Why it fails is not established, but two candidates:
            //   * the AttachedEntity's item may not be the same pointer that
            //     InventorySection::getItem() returns, so detachItem(item) finds
            //     nothing to remove and silently returns false;
            //   * detachItem(Item*) may require the item to be attached already
            //     and reject our call for another reason.
            // The name-keyed call below is the behaviour that shipped and worked
            // for months, so it stays. Do not swap it again without a test that
            // proves hiding still works.
            if (p_detachSlot) p_detachSlot(app, &slot);
        }
        else
        {
            if (attached) continue;                       // already shown
            if (p_attachItemSlot)
            {
                if (!p_attachItemSlot(app, item, &slot) && p_notifyDirty) p_notifyDirty(app);
            }
        }
    }

    // Backpack appearance follows per-character option bit 10 (the "X" button).
    if (p_getSectionOfType)
    {
        void* bpSec = p_getSectionOfType(inv, ATTACH_BACKPACK);
        if (bpSec)
        {
            const MsvcString* bpn = (const MsvcString*)((uint8_t*)bpSec + off::Section_name);
            if (bpn && bpn->c_str() && bpn->size > 0)
            {
                MsvcString bpName; bpName.set(bpn->c_str());
                const bool bpHidden = ((optionBits >> 10) & 1) != 0;
                if (bpHidden)
                {
                    void* bpAttached = p_getAttachedEntity ? p_getAttachedEntity(app, &bpName) : nullptr;
                    if (bpAttached && p_detachSlot) p_detachSlot(app, &bpName);
                }
                else
                {
                    void* bpItem = p_sectionGetItem ? p_sectionGetItem(bpSec) : nullptr;
                    void* bpAttached = p_getAttachedEntity ? p_getAttachedEntity(app, &bpName) : nullptr;
                    if (bpItem && !bpAttached && p_attachItemSlot)
                        p_attachItemSlot(app, bpItem, &bpName);
                }
            }
        }
    }

    ManageExtraShirt(character, inv, app, hide, sf);
}

// SEH guard, kept free of C++ objects that need unwinding.
__declspec(noinline) static void ApplyGuarded(void* character, bool hide)
{
    __try { ApplyOne(character, hide); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogF("[hide] guarded exception while applying visual state");
    }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static void __cdecl Detour_postUpdate(void* character)
{
    // Prove the hook actually fires (one-shot).
    static bool s_fired = false;
    if (!s_fired) { s_fired = true; LogF("[hide] HOOK FIRED: _NV_postUpdate reached (char=%p)", character); }

    if (p_postUpdateOriginal) p_postUpdateOriginal(character);

#if DUAL_EQUIP_ENABLE_DETACH
    const DWORD now = GetTickCount();
    if (!g_hidden)
    {
        if (Throttled(character, now, 1000)) return;
        ApplyGuarded(character, false);
        return;
    }
    if (Throttled(character, now, 200)) return;   // re-hide aggressively
    ApplyGuarded(character, true);
#else
    (void)character;
#endif
}

static void __cdecl Detour_removeFromUpdateList(void* world, void* character)
{
    // 2026-10-04 crash suspicion REMOVED: this hook no longer runs the apply.
    // Reason: it fires on characters that are being torn down, and it was the
    // prime suspect for the crash. We only clear the throttle cache here.
    (void)world;
    ClearThrottleFor(character);

    // Always forward: the game's own update-list bookkeeping must still happen.
    if (p_removeListOriginal) p_removeListOriginal(world, character);
}

// ---------------------------------------------------------------------------
//  APPLY DRIVER - background thread, NOT a hook.
//
//  Why not a hook: Character::_NV_postUpdate was hooked successfully
//  (HookStatus=0) but NEVER FIRED - proven 2026-10-04 with a one-shot
//  "HOOK FIRED" log that never appeared across two sessions. Same trap as
//  Character::_NV_init earlier: the _NV_ (non-virtual) functions are inlined by
//  the engine, so the standalone exported copy is dead code.
//
//  So the apply is driven from our own thread, polling ?ou -> GameWorld::player
//  -> PlayerInterface::playerCharacters. That is exactly how DualEquipSlots2
//  creates its sections, and that path is proven to work in game.
// ---------------------------------------------------------------------------
static void** g_ppOu = nullptr;          // address of the ?ou GameWorld global
static bool  g_threadAnnounced = false;
static bool  g_threadSawWorld = false;

// ---------------------------------------------------------------------------
//  Per-character state + throttle.
//
//  BUG FIXED 2026-10-05: the first version of this loop had two defects that
//  together limited hiding to the first few squad members:
//      for (i = 0; i < players.count && i < 4; ++i)      // only 4 characters
//      if (g_lastChar == ch && g_lastState == g_hidden)  // cache held ONE char
//  So at most 4 characters were ever touched, and only the last one was
//  remembered. Selecting a later squad member and pressing the hotkey did
//  nothing.
//
//  Now every character is processed and each one carries its own applied-state
//  and its own cooldown, so any squad size behaves identically.
// ---------------------------------------------------------------------------
struct CharState
{
    void* ch;
    int   applied;      // last state we pushed to this character (-1 = never)
    DWORD nextMs;       // throttle: do not re-apply before this tick
};

static const size_t kCharSlots = 512;
static CharState    g_charState[kCharSlots];

// What this character should be, given the global toggle and its own override.
static CharState& StateFor(void* ch)
{
    size_t idx = ((uintptr_t)ch >> 4) & (kCharSlots - 1);
    for (size_t probe = 0; probe < 8; ++probe)
    {
        CharState& e = g_charState[(idx + probe) & (kCharSlots - 1)];
        if (e.ch == ch) return e;                       // existing entry
        if (e.ch == nullptr)                            // empty -> claim it
        {
            e.ch = ch; e.applied = -1; e.nextMs = 0;
            return e;
        }
    }
    return g_charState[idx & (kCharSlots - 1)];          // saturated: degrade
}

// ---------------------------------------------------------------------------
//  PlayerInterface::getAllSelectedObjects(lektor<RootObject*>&, itemType)
//  Used by the SINGLE-character hotkey to learn who is currently selected.
// ---------------------------------------------------------------------------
typedef void (*fn_getAllSelected_t)(void* playerInterface, lektor<void*>& out, int itemType);
static fn_getAllSelected_t p_getAllSelected = nullptr;

// ---------------------------------------------------------------------------
//  SHIRT PROBE (2026-10-21, temporary research build)
//
//  The amputee-only "extra-slot shirt shrinks inward" bug cannot be diagnosed
//  from the outside: the engine attaches our item through exactly the same
//  call as a vanilla item, only with our section name as the key. That name
//  is also what getSlotPosition() looks up in AppearanceBase::slots. To learn
//  what actually differs, dump the two AttachedEntity objects (vanilla slot
//  vs extra slot) byte-for-byte and let the next session diff them offline.
//
//  Cost: bounded (one probe per character every 5 s, logging only when the
//  item/entity signature changes) and read-only.
// ---------------------------------------------------------------------------
struct ExtraSlotName { const char* section; const char* vanilla; };
static const ExtraSlotName kExtraSlots[] =
{
    { "de_slot_head",   "head"   },
    { "de_slot_armour", "armour" },
    { "de_slot_legs",   "legs"   },
    { "de_slot_shirt",  "shirt"  },
    { "de_slot_boots",  "boots"  },
};

struct ProbeState { void* ch; unsigned long long sig; void* entVanilla; void* entExtra; DWORD nextMs; };
static ProbeState g_probe[512];
static bool g_probeEnabled = false;   // off by default; create DualEquip_probe_on to enable
static LONG g_probeDumps = 0;

static void DumpBytes(const char* tag, void* p, int bytes)
{
    if (!p) { LogF("[probe] %s: <null>", tag); return; }
    const unsigned char* b = (const unsigned char*)p;
    for (int off = 0; off < bytes; off += 16)
    {
        char hex[16 * 3 + 2];
        int n = 0;
        for (int i = 0; i < 16 && off + i < bytes; ++i)
            n += sprintf_s(hex + n, sizeof(hex) - n, "%02X ", b[off + i]);
        hex[n] = 0;
        LogF("[probe] %s %p+%02X: %s", tag, p, off, hex);
    }
}

static void ProbeExtraSections(void* ch, void* inv, void* app)
{
    if (!g_probeEnabled) return;
    if (!ch || !inv || !app || !p_getSection || !p_sectionGetItem || !p_getAttachedEntity) return;

    ProbeState& st = g_probe[((uintptr_t)ch >> 4) & 511];
    if (st.ch != ch) { st.ch = ch; st.sig = 0; st.entVanilla = st.entExtra = nullptr; st.nextMs = 0; }

    const DWORD now = GetTickCount();
    if (now < st.nextMs) return;
    st.nextMs = now + 5000;

    unsigned long long sig = 1469598103934665603ull;
    void* show[5][4];
    for (int i = 0; i < 5; ++i)
    {
        MsvcString en; en.set(kExtraSlots[i].section);
        MsvcString vn; vn.set(kExtraSlots[i].vanilla);
        void* esec = p_getSection(inv, &en);
        void* vsec = p_getSection(inv, &vn);
        void* eitem = esec ? p_sectionGetItem(esec) : nullptr;
        void* vitem = vsec ? p_sectionGetItem(vsec) : nullptr;
        void* vent  = p_getAttachedEntity(app, &vn);
        void* eent  = p_getAttachedEntity(app, &en);
        show[i][0] = vitem; show[i][1] = vent; show[i][2] = eitem; show[i][3] = eent;
        const uintptr_t vals[4] = { (uintptr_t)vitem, (uintptr_t)vent, (uintptr_t)eitem, (uintptr_t)eent };
        for (int k = 0; k < 4; ++k) { sig ^= vals[k]; sig *= 1099511628211ull; }
    }
    if (sig == st.sig) return;                    // nothing changed since last probe
    st.sig = sig;

    // Only characters that actually hold something in an extra slot are
    // interesting - otherwise this logs every character in the squad.
    bool anyExtra = false;
    for (int i = 0; i < 5; ++i)
        if (show[i][2] || show[i][3]) { anyExtra = true; break; }
    if (!anyExtra) return;

    for (int i = 0; i < 5; ++i)
        if (show[i][2] || show[i][3])
            LogF("[probe] %-14s van(item=%p ent=%p) extra(item=%p ent=%p)",
                 kExtraSlots[i].section, show[i][0], show[i][1], show[i][2], show[i][3]);

    const int SHIRT = 3;
    st.entVanilla = show[SHIRT][1];
    st.entExtra   = show[SHIRT][3];
    if (st.entExtra && InterlockedIncrement(&g_probeDumps) <= 20)
    {
        LogF("[probe] SHIRT DUMP char=%p vanillaEnt=%p extraEnt=%p", ch, st.entVanilla, st.entExtra);
        DumpBytes("vanillaShirt", st.entVanilla, 128);
        DumpBytes("extraShirt",   st.entExtra,   128);
    }
}

// itemType value that means "characters" for PlayerInterface::getAllSelectedObjects.
// Calibrated in game 2026-10-05: itemType=1 returned the selected character(s).
// (The enum is not documented anywhere, so this was found by trying each value;
//  0 returned nothing, 1 returned the current selection.)
static const int kItemTypeCharacter = 1;

// Force the CURRENTLY SELECTED characters to the opposite of their current state.
// Returns how many characters were toggled (0 if nothing is selected).
static int ToggleSelectedCharacters()
{
    if (!p_getAllSelected) return 0;
    if (!g_ppOu || !*g_ppOu) return 0;
    uint8_t* gw = (uint8_t*)*g_ppOu;
    void* piface = *(void**)(gw + 0x580);            // GameWorld::player
    if (!piface) return 0;

    lektor<void*> sel = {};                          // zero-init: count 0
    p_getAllSelected(piface, sel, kItemTypeCharacter);
    if (!sel.stuff || sel.count == 0) return 0;

    int n = 0;
    for (uint32_t i = 0; i < sel.count; ++i)
    {
        void* ch = sel.stuff[i];
        if (!ch) continue;
        // Only player characters are managed by this mod.
        if (p_isPlayerChar && !p_isPlayerChar(ch)) continue;

        ResolveSetHidden();
        const int cur = (GetOptionBits(ch) >> 11) & 1;   // bit 11 = hidden
        if (p_setHidden) p_setHidden(ch, cur ? 0 : 1);
        CharState& st = StateFor(ch);
        st.applied = -1;
        st.nextMs  = 0;                              // apply immediately
        ++n;
    }
    return n;
}

// Right Shift / the R button: if anyone is visible, hide everyone; otherwise show
// everyone. The result is written back into each character's bit 11, so it is
// persisted exactly like [1]/[2]/[X].
static int ToggleAllCharacters()
{
    if (!g_ppOu || !*g_ppOu) return 0;
    uint8_t* gw = (uint8_t*)*g_ppOu;
    void* piface = *(void**)(gw + 0x580);
    if (!piface || !p_getAllSelected) return 0;

    lektor<void*>& players = *(lektor<void*>*)((uint8_t*)piface + 0x2B0);
    if (!players.stuff || players.count == 0) return 0;

    ResolveSetHidden();
    if (!p_setHidden) return 0;

    bool anyVisible = false;
    for (uint32_t i = 0; i < players.count && !anyVisible; ++i)
    {
        void* ch = players.stuff[i];
        if (!ch) continue;
        if (p_isPlayerChar && !p_isPlayerChar(ch)) continue;
        if (((GetOptionBits(ch) >> 11) & 1) == 0) anyVisible = true;
    }
    const int newVal = anyVisible ? 1 : 0;

    int n = 0;
    for (uint32_t i = 0; i < players.count; ++i)
    {
        void* ch = players.stuff[i];
        if (!ch) continue;
        if (p_isPlayerChar && !p_isPlayerChar(ch)) continue;
        p_setHidden(ch, newVal);
        CharState& st = StateFor(ch);
        st.applied = -1;
        st.nextMs  = 0;
        ++n;
    }
    g_hidden = newVal ? 1 : 0;
    return n;
}

// / toggles EVERYONE: clear all per-character overrides so the whole squad follows.
static void ResetAllOverrides()
{
    for (size_t k = 0; k < kCharSlots; ++k)
    {
        g_charState[k].nextMs    = 0;
    }
}

static void PollAndApply()
{
    if (!g_ppOu || !*g_ppOu) return;                 // no world yet
    uint8_t* gw = (uint8_t*)*g_ppOu;

    void* piface = *(void**)(gw + 0x580);            // GameWorld::player
    if (!piface) return;

    lektor<void*>& players = *(lektor<void*>*)((uint8_t*)piface + 0x2B0);
    if (!players.stuff || players.count == 0) return;

    if (!g_threadSawWorld)
    {
        g_threadSawWorld = true;
        LogF("[hide] thread driver: world detected, players=%u, selected-query %s",
             players.count, p_getAllSelected ? "available" : "MISSING");
    }

    const DWORD now = GetTickCount();

    int touched = 0;
    for (uint32_t i = 0; i < players.count; ++i)     // ALL characters, no cap
    {
        void* ch = players.stuff[i];
        if (!ch) continue;

        CharState& st = StateFor(ch);
        const int want = (GetOptionBits(ch) >> 11) & 1;   // persisted hidden flag
        // Re-hiding must be aggressive (the appearance system can re-attach);
        // showing again is a one-off, so it can be checked far less often.
        const DWORD delay = want ? 200u : 1000u;
        if (st.applied == want && now < st.nextMs) continue;

        ApplyGuarded(ch, want != 0);

        // Temporary research build: capture the shirt attachment state.
        ProbeExtraSections(ch,
                           p_charInventory ? p_charInventory(ch) : nullptr,
                           p_charAppearance ? p_charAppearance(ch) : nullptr);

        st.applied = want;
        st.nextMs  = now + delay;
        ++touched;
    }

    (void)touched;
}

// ===========================================================================
//  ApplyThread - re-applies the hide state, forever.
//
//  BUG FIXED 2026-10-10 (reported on the Workshop: "after loading a save the two
//  outfits sometimes render at the same time and the hotkeys stop responding
//  until I restart the game").
//
//  This loop used to be  for (int i = 0; i < 7200; ++i)  - roughly one hour at
//  500 ms - after which the thread simply RETURNED. Same story for HotkeyThread,
//  which gave up after ~33 minutes. So an hour into a session the mod silently
//  stopped working: nothing re-applied the hide state (both outfits visible) and
//  no key was polled any more (hotkeys dead). Restarting the game revived both,
//  which is exactly what the report described.
//
//  Both threads now run until the process exits, which is what they were always
//  meant to do. The bounds were a leftover guard against runaway threads; they
//  caused the core feature to die on a timer instead.
// ===========================================================================
static DWORD WINAPI ApplyThread(LPVOID)
{
    for (;;)
    {
        Sleep(500);
        __try { PollAndApply(); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogF("[hide] guarded exception in apply thread - stopping");
            return 0;
        }
    }
}

// ---------------------------------------------------------------------------
//  HOTKEY THREAD - the '/' key
//  Deliberately NOT a game-input hook:
//    * the user has F1-F8 and F12 bound to other things, and F9-F11 are risky
//      (Kenshi itself and other mods use F-keys);
//    * Ctrl+H was tried first but did not register reliably, so a single key
//      was requested instead;
//    * polling GetAsyncKeyState from our own thread cannot break game input,
//      which is why the reference mod (GlobalHideHelmets) does it this way.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Are we allowed to act on a hotkey right now?
//
//  Problem (reported 2026-10-05): the '/' and 'L' hotkeys also fired while the
//  user was typing - both in the game's own text boxes (rename / name-entry)
//  and while a Chinese IME candidate window was up.
//
//  The clean fix would be "is a MyGUI EditBox focused?", but that is not
//  reachable: KenshiLib exports no MyGUI interface, and although the PDB proves
//  MyGUI::InputManager exists, it does NOT expose its member/method names, so
//  there is no way to query the focus widget.
//
//  What IS reliably detectable is the FOREGROUND WINDOW:
//    * an IME candidate / composition window is a DIFFERENT top-level window, so
//      it owns the foreground while it is up -> we must stay quiet;
//    * likewise any other application the user alt-tabbed to.
//  So the rule is: only act when OUR OWN process owns the foreground window.
//
//  EXCEPTION added 2026-10-10: the Steam overlay is a separate process
//  (steamwebhelper.exe), so with the overlay focused we used to go quiet and the
//  hotkeys looked dead. The overlay is still the game as far as the player is
//  concerned, so it now counts as "us".
//
//  This solves the IME case completely. It does NOT solve the game's own text
//  boxes (those are inside our window) - see the PAUSE FILE below for that.
// ---------------------------------------------------------------------------
static bool IsSteamProcess(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    char buf[MAX_PATH] = { 0 };
    DWORD n = MAX_PATH;
    bool steam = false;
    if (QueryFullProcessImageNameA(h, 0, buf, &n))
    {
        const char* base = buf;
        for (const char* q = buf; *q; ++q) if (*q == '\\') base = q + 1;
        if (_stricmp(base, "steam.exe") == 0 || _stricmp(base, "steamwebhelper.exe") == 0)
            steam = true;
    }
    CloseHandle(h);
    return steam;
}

static bool OwnsForeground()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return true;
    return IsSteamProcess(pid);          // Steam overlay counts as the game
}

// ---------------------------------------------------------------------------
//  PAUSE FILE - manual escape hatch.
//
//  Presence of "DualEquip_hotkeys_off" (relative to the game's working
//  directory) disables both hotkeys. Polled at most once per second.
//  Deliberately a file rather than another key: any key we could pick might be
//  taken by another mod, which is exactly the problem this mod already hit.
// ---------------------------------------------------------------------------
static bool HotkeysPaused()
{
    static DWORD s_next = 0;
    static bool  s_paused = false;
    DWORD now = GetTickCount();
    if (now >= s_next)
    {
        s_next = now + 1000;
        static const char* kPath = "DualEquip_hotkeys_off";
        DWORD attr = GetFileAttributesA(kPath);
        bool paused = (attr != INVALID_FILE_ATTRIBUTES);
        if (paused != s_paused)
        {
            s_paused = paused;
            LogF("[hide] hotkeys %s (pause file %s)",
                 paused ? "PAUSED" : "RESUMED", kPath);
        }
    }
    return s_paused;
}

static DWORD WINAPI HotkeyThread(LPVOID)
{
    bool prevAll = false;
    bool prevOne = false;
    DWORD lastToggle = 0;
    for (;;)                              // see the note on ApplyThread: this
    {                                     // used to stop after ~33 minutes
        Sleep(20);

        //  RIGHT SHIFT  -> toggle the WHOLE squad
        //  INSERT       -> toggle ONLY the currently selected character(s)
        //
        // Why these two (settled 2026-10-05 after many failed attempts):
        //   * Every F-key is taken by other mods installed on this system, the
        //     backslash is claimed by an AI-chat mod, and this keyboard has no
        //     numpad - so the obvious "safe" keys were all unavailable.
        //   * Ctrl and Alt are both used by the game itself (mouse_rotate =
        //     Left Control, highlight = Left Alt) and are swallowed during IME
        //     composition, which is why Ctrl+H never registered.
        //   * Insert is unbound in controls.cfg and is not a character anyone
        //     types, so it cannot be hit by accident while writing text.
        //   * RIGHT Shift is likewise unbound (the game only binds Shift+F12 and
        //     Ctrl+Shift+F11), and only the RIGHT one is used - the left Shift
        //     still works normally for the user.
        //
        // NOTE: an earlier attempt tried to detect IME composition via imm32
        // (ImmGetContext/ImmGetCompositionStringW). It was removed: Microsoft
        // Pinyin and WeChat IME both go through TSF, so every IMM32 query
        // returned 0 and the guard never triggered.
        const bool allDown = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
        const bool oneDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        const DWORD now = GetTickCount();

        // Suppress the hotkeys when the user is not looking at our window
        // (alt-tabbed away) or when the pause file exists.
        const bool active = OwnsForeground() && !HotkeysPaused();

        // Debounce: key-repeat must not toggle repeatedly (a held key used to
        // produce alternating toggles ~1/s).
        if (active && allDown && !prevAll && (now - lastToggle) > 800)
        {
            lastToggle = now;
            const int n = ToggleAllCharacters();
            LogF("[hide] RIGHT SHIFT -> %s  (%d characters)",
                 g_hidden ? "HIDDEN" : "SHOWN", n);
        }
        if (active && oneDown && !prevOne && (now - lastToggle) > 800)
        {
            lastToggle = now;
            const int n = ToggleSelectedCharacters();
            LogF("[hide] INSERT -> toggled %d selected character(s)%s",
                 n, n == 0 ? "  [nothing selected]" : "");
        }
        prevAll = allDown;
        prevOne = oneDown;

        if (!g_threadAnnounced && g_ppOu && *g_ppOu)
        {
            g_threadAnnounced = true;
            LogF("[hide] hotkey thread: world detected; Right Shift = all, Insert = selected");
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  UI entry points - called by the buttons in DualEquipSlots2.dll
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void DE_ResetCharStates()
{
    for (size_t k = 0; k < kCharSlots; ++k)
    {
        g_charState[k].ch = nullptr;
        g_charState[k].applied = -1;
        g_charState[k].nextMs = 0;
    }
    for (size_t k = 0; k < 256; ++k)
    {
        g_shirtFix[k].ch = nullptr;
        g_shirtFix[k].item = nullptr;
        g_shirtFix[k].entity = nullptr;
        g_shirtFix[k].asVanilla = false;
    }
    LogF("[hide] per-character states reset (save load)");
}

extern "C" __declspec(dllexport) void DE_UiInsert()
{
    const int n = ToggleSelectedCharacters();
    LogF("[ui] Insert button -> toggled %d selected character(s)", n);
}

extern "C" __declspec(dllexport) void DE_UiAll()
{
    const int n = ToggleAllCharacters();
    LogF("[ui] Right Shift button -> %s (%d characters)", g_hidden ? "HIDDEN" : "SHOWN", n);
}


__declspec(dllexport) void startPlugin()
{
#if !DUAL_EQUIP_ENABLE_DETACH
    LogF("[hide] ===== DualEquip v2 loading (detach DISABLED) =====");
    return;
#else
    LogF("[hide] ===== DualEquip v2 loading (detach ENABLED) =====");
    g_probeEnabled = (GetFileAttributesA("DualEquip_probe_on") != INVALID_FILE_ATTRIBUTES);
    LogF("[hide] probe %s", g_probeEnabled ? "ON" : "off");

    HMODULE klib = GetModuleHandleA("KenshiLib.dll");
    if (!klib) klib = LoadLibraryA("KenshiLib.dll");
    if (!klib) { LogF("[hide] KenshiLib.dll missing"); return; }
    g_pKenshiLib = (void*)klib;

    p_AddHook            = (fn_AddHook_t)            Sym(SYM_ADD_HOOK);
    p_isPlayerChar       = (fn_isPlayerChar_t)       Sym(SYM_IS_PLAYER);
    p_charInventory      = (fn_charInventory_t)      Sym(SYM_CHAR_INV);
    p_charAppearance     = (fn_charAppearance_t)     Sym(SYM_CHAR_APP);
    p_getSectionOfType   = (fn_getSectionOfType_t)   Sym(SYM_SECTION_OF_TYPE);
    p_sectionGetItem     = (fn_sectionGetItem_t)     Sym(SYM_SECTION_GETITEM);
    p_getAttachedEntity  = (fn_getAttachedEntity_t)  Sym(SYM_GET_ATTACHED);
    p_detachSlot         = (fn_detachSlot_t)         Sym(SYM_DETACH_SLOT);
    p_attachItemSlot     = (fn_attachItemSlot_t)     Sym(SYM_ATTACH_ITEM);
    p_attachmentsLoaded  = (fn_attachmentsLoaded_t)  Sym(SYM_ATTACH_LOADED);
    p_notifyDirty        = (fn_notifyDirty_t)        Sym(SYM_NOTIFY_DIRTY);
    p_getSection         = (fn_getSection_t)         Sym(SYM_GET_SECTION);

    // Needed by the 'L' hotkey (toggle only the currently selected characters).
    // Non-fatal if missing: '/' still works and 'L' just reports 0 characters.
    p_getAllSelected     = (fn_getAllSelected_t)     Sym(SYM_GET_ALL_SELECTED);
    if (!p_getAllSelected)
        LogF("[hide] WARNING: PlayerInterface::getAllSelectedObjects unresolved - 'L' disabled");

    if (!p_AddHook || !p_detachSlot || !p_getSectionOfType || !p_charAppearance)
    {
        LogF("[hide] required symbols missing - disabled");
        return;
    }

    // NOTE: Character::_NV_postUpdate is deliberately NOT hooked any more.
    // It installed fine but never fired (proven by the one-shot HOOK FIRED log
    // never appearing). The apply is driven by ApplyThread instead.
    // GameWorld::removeFromUpdateListMain is still hooked, but ONLY to clear the
    // throttle cache - it must be forwarded, or the game's update list breaks.
    void* t2 = (void*)Sym(SYM_REMOVE_LIST);
    void* o2 = nullptr;
    if (t2 && p_AddHook(t2, (void*)&Detour_removeFromUpdateList, &o2) == 0)
        p_removeListOriginal = (fn_removeFromUpdateList_t)o2;
    else LogF("[hide] FAILED to hook GameWorld::removeFromUpdateListMain (non-fatal)");

    // Resolve the address of the ?ou global so the apply thread can find the world.
    g_ppOu = (void**)Sym(SYM_OU);

    // NOTE (2026-10-18): attachItem / detachItem used to be hooked here so the
    // engine could be handed the vanilla slot name for extra-slot items. That
    // fixed the amputee-shirt case but broke simultaneous rendering, because the
    // slot name doubles as the attachment key. Both hooks are gone; see the
    // REVERTED note near the top of this file before adding them back.
    //


    // Apply driver: own thread polling the player list (hooks never fire here).
    HANDLE ta = CreateThread(nullptr, 0, ApplyThread, nullptr, 0, nullptr);
    if (ta) { CloseHandle(ta); LogF("[hide] apply thread started (polls every 500 ms)"); }
    else      LogF("[hide] apply thread FAILED to start");

    // Hotkey via own thread - no game input hook, so input cannot be broken.
    HANDLE th = CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
    if (th) { CloseHandle(th); LogF("[hide] hotkey thread started (Right Shift = all, Insert = selected)"); }
    else      LogF("[hide] hotkey thread FAILED to start");

    LogF("[hide] loaded; %d vanilla sections managed, / toggles", kSlotCount);
#endif
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID)
{
    if (r == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}

