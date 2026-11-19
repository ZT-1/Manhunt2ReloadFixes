// ReloadFixes.asi - Manhunt 2 PC (Manhunt2.exe, image base 0x400000, no ASLR). Port of the
// Manhunt 1 ReloadFixes (github.com/ZT-1/ManhuntRevolverReloadFix): the same engine bugs at MH2's
// addresses. Each fix can be turned on or off in ReloadFixes.ini (next to the .asi). All are on
// if the .ini is missing.
//
// The revolver and shotgun-queue bugs come from one per-ped flag: CPed+0x308 bit 0x40, "this
// looped reload started with rounds still loaded, so cut the reload anim when RELOAD_LOOP ends".
// It lets a partial shotgun reload skip the pump. Only the normal shotgun reload start writes it
// (0x508001: rounds loaded -> set, empty -> clear), but the torso reload state (0x510A8D) obeys
// it for every RELOAD_LOOP weapon.
//
// [Fixes] RevolverFlick
//   Other guns start their reload at 0x508025, and the empty-clip reload at 0x5AB6FA, without
//   touching the bit, so after a partial shotgun reload the 6-shooter's reload is cut before
//   its cylinder closes. Fix: clear the bit before SetReload at both sites.
//
// [Fixes] ShotgunQueuedReload
//   Reload pressed while a shotgun shot or pump is playing is queued (CPed+0x308 bit 0x80) and
//   started when the pump ends, at 0x50E18D in TorsoReloadCallback, without writing bit 0x40. A
//   partial reload can then end with a pump that ejects a phantom shell. Fix: after that
//   SetReload, set or clear the bit from the rounds loaded, as the normal start does.
//
// [Fixes] SniperLastCasing / ShotgunLastShell
//   After the last shot, the bolt/pump state (Sniper_Cock / Shotgun_Cock) first sees the empty
//   magazine and requests the reload (0x51093B) on every frame instead of reaching its eject
//   point (SHELL_EJECT_DELAY_FIRE, eject at 0x5109A1). The sniper's last case is never ejected.
//   The shotgun's comes out late in the reload (SHELL_EJECT_DELAY_RELOAD 0.7), right before the
//   closing pump. Fix: skip that request until the bolt (0.55 s) or pump (0.35 s) has closed, and
//   run the normal bolt/pump code (0x510958) instead, which ejects the case on time. Then request
//   the reload as vanilla does but keep running that code, so the rifle/shotgun model anim stays
//   synced to Cash's hands while the reload blends in.
//   ShotgunLastShell also keeps the "shell already ejected" bit (collectable +0x1BC, 0x10) during
//   the reload instead of re-arming it (0x510BE0), so the 0.7 s reload eject only happens if the
//   shell is still in the gun. Firing clears that bit (0x52913A).
//
// Every patch checks the original bytes first and does nothing if they differ.

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>

namespace
{
    constexpr uintptr_t EXPECTED_IMAGE_BASE = 0x400000;
    constexpr uintptr_t ADDR_SET_RELOAD = 0x492B30;   // CPedTorsoAnimFSM::SetReload

    // RevolverFlick: mov ecx,[esi+2C0h] ; call SetReload  (11 bytes), esi = ped
    constexpr uintptr_t SITE_RELOAD_START = 0x508025;
    constexpr uintptr_t SITE_EMPTY_RELOAD = 0x5AB6FA;

    // ShotgunQueuedReload: call SetReload (5 bytes, ecx = torso FSM), esi = ped
    constexpr uintptr_t SITE_QUEUED_RELOAD = 0x50E18D;

    // SniperLastCasing / ShotgunLastShell: mov ecx,[esi+2C0h] ; call SetReload  (11 bytes),
    // esi = ped, ebx = held collectable. 0x510958 is where that code continues when rounds are
    // loaded; 0x510946 is the vanilla path after the call.
    constexpr uintptr_t SITE_BOLT_AUTO_RELOAD = 0x51093B;

    // ShotgunLastShell, reload-eject re-arm: and dword [ebx+1BCh],-11h  (7 bytes), ebx = held
    // collectable; followed by jmp 0x510BEB at 0x510BE7.
    constexpr uintptr_t SITE_RELOAD_EJECT_REARM = 0x510BE0;

    // The naked stubs below use literal addresses: MSVC inline asm reads a C++ constant name as a
    // memory operand, not an immediate. Keep them in sync with the SITE_ values above.

    constexpr int CT_SHOTGUN       = 0x2E;
    constexpr int CT_SHOTGUN_TORCH = 0x2F;
    constexpr int CT_SNIPER_RIFLE  = 0x32;    // also CT_SNIPER_RIFLE_SILENCED

    // Sniper_Cock_T: bolt back 0.267 s, forward 0.491 s. Shotgun_Cock_T: pump back 0.169 s,
    // forward 0.313 s. (The Nostalgia Modpack uses the Manhunt 1 player anims.)
    constexpr float BOLT_CLOSED_TIME = 0.55f;
    constexpr float PUMP_CLOSED_TIME = 0.35f;

    bool g_log = true;
    bool g_sniperLastCasing = true;
    bool g_shotgunLastShell = true;
    char g_dir[MAX_PATH] = {};

    void Log(bool always, const char* fmt, ...)
    {
        if (!g_log && !always) return;
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\'))
            strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "reloadfixes_asi.log");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "a") != 0 || !f) return;
        va_list args; va_start(args, fmt); vfprintf(f, fmt, args); va_end(args);
        fputc('\n', f); fclose(f);
    }

    // ped -> held collectable (slot +0x1C0, inventory [[+0x1BC]+0xC]) and its ammo data (+0x204).
    // Same as the game's own getter at 0x4F66C0.
    bool HeldWeapon(uintptr_t ped, uintptr_t& coll, uintptr_t& weapon)
    {
        const int slot = *reinterpret_cast<int*>(ped + 0x1C0);
        if (slot < 0 || slot > 64) return false;
        const uintptr_t inv = *reinterpret_cast<uintptr_t*>(ped + 0x1BC);
        if (!inv) return false;
        const uintptr_t items = *reinterpret_cast<uintptr_t*>(inv + 0xC);
        if (!items) return false;
        coll = *reinterpret_cast<uintptr_t*>(items + slot * 4);
        if (!coll) return false;
        weapon = *reinterpret_cast<uintptr_t*>(coll + 0x204);
        return weapon != 0;
    }

    // ---- ShotgunQueuedReload: set bit 0x40 from the held weapon's loaded rounds ----
    void __cdecl SetPartialReloadFlag(uintptr_t ped)
    {
        __try
        {
            uintptr_t coll, weapon;
            if (!HeldWeapon(ped, coll, weapon)) return;
            const uint32_t loaded = *reinterpret_cast<uint32_t*>(weapon + 0x64);
            uint32_t& flags = *reinterpret_cast<uint32_t*>(ped + 0x308);
            if (loaded != 0) flags |= 0x40; else flags &= ~0x40u;   // same rule as 0x508001
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ---- SniperLastCasing / ShotgunLastShell ----
    // Returns 0: vanilla (SetReload, return); 1: skip SetReload and run the bolt/pump code;
    // 2: SetReload, clear CPed+0x308 bits 0x40/0x80 as vanilla does, then run the bolt/pump code.
    int __cdecl DeferLastRoundAutoReload(uintptr_t ped)
    {
        __try
        {
            uintptr_t coll, weapon;
            if (!HeldWeapon(ped, coll, weapon)) return 0;
            const uintptr_t typeData = *reinterpret_cast<uintptr_t*>(coll + 0x6C);
            if (!typeData) return 0;
            const int ct = *reinterpret_cast<int*>(typeData + 0x1F4);
            float closedTime;
            if (ct == CT_SNIPER_RIFLE && g_sniperLastCasing) closedTime = BOLT_CLOSED_TIME;
            else if ((ct == CT_SHOTGUN || ct == CT_SHOTGUN_TORCH) && g_shotgunLastShell) closedTime = PUMP_CLOSED_TIME;
            else return 0;
            if (*reinterpret_cast<float*>(typeData + 0x560) <= 0.0f) return 0;     // no bolt/pump eject time
            const uintptr_t fsm = *reinterpret_cast<uintptr_t*>(ped + 0x2C0);
            if (!fsm) return 0;
            const uintptr_t anim = *reinterpret_cast<uintptr_t*>(fsm + 0xA0);
            if (!anim) return 0;
            return *reinterpret_cast<float*>(anim + 0x20) < closedTime ? 1 : 2;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // 0x51093B. ebx (held collectable) and edi (ammo data) are live in the code at 0x510958, so
    // the state is kept on the stack.
    __declspec(naked) void StubBoltAutoReload()
    {
        __asm {
            push esi                        // ped
            call DeferLastRoundAutoReload
            add esp, 4
            cmp eax, 1
            je bolt
            push eax
            mov ecx, dword ptr [esi + 0x2C0]
            mov eax, 0x492B30               // CPedTorsoAnimFSM::SetReload
            call eax
            pop eax
            test eax, eax
            jnz release
            push 0x510946                   // vanilla: SITE_BOLT_AUTO_RELOAD + 11
            ret
        release:
            and dword ptr [esi + 0x308], 0xFFFFFF3F    // as vanilla does after this SetReload
        bolt:
            push 0x510958                   // bolt/pump code, as when rounds are loaded
            ret
        }
    }

    // ---- ShotgunLastShell: reload-eject re-arm (0x510BE0) ----
    //   Keep the "shell already ejected" bit for the pump shotguns; re-arm for everything else.
    __declspec(naked) void StubReloadEjectRearm()
    {
        __asm {
            push eax
            mov eax, dword ptr [ebx + 0x6C]     // type data
            test eax, eax
            jz rearm
            mov eax, dword ptr [eax + 0x1F4]    // collectable type
            cmp eax, 0x2E                       // CT_SHOTGUN
            je keep
            cmp eax, 0x2F                       // CT_SHOTGUN_TORCH
            je keep
        rearm:
            and dword ptr [ebx + 0x1BC], 0xFFFFFFEF
        keep:
            pop eax
            push 0x510BE7                       // SITE_RELOAD_EJECT_REARM + 7 (jmp 0x510BEB)
            ret
        }
    }

    // ---- ShotgunQueuedReload (0x50E18D): replaces call SetReload, ecx = torso FSM ----
    __declspec(naked) void StubQueuedReload()
    {
        __asm {
            mov eax, 0x492B30               // CPedTorsoAnimFSM::SetReload
            call eax
            push esi                        // ped
            call SetPartialReloadFlag
            add esp, 4
            ret                             // back to 0x50E192
        }
    }

    // ---- RevolverFlick: clear bit 0x40 before SetReload ----
    __declspec(naked) void StubReloadStart()
    {
        __asm {
            and dword ptr [esi + 0x308], 0xFFFFFFBF
            mov ecx, dword ptr [esi + 0x2C0]
            mov eax, 0x492B30               // CPedTorsoAnimFSM::SetReload
            call eax
            push 0x508030                   // SITE_RELOAD_START + 11
            ret
        }
    }

    __declspec(naked) void StubEmptyReload()
    {
        __asm {
            and dword ptr [esi + 0x308], 0xFFFFFFBF
            mov ecx, dword ptr [esi + 0x2C0]
            mov eax, 0x492B30               // CPedTorsoAnimFSM::SetReload
            call eax
            push 0x5AB705                   // SITE_EMPTY_RELOAD + 11
            ret
        }
    }

    // ---- patching helpers ----
    bool CallsSetReload(uintptr_t callInsn)
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(callInsn);
        if (p[0] != 0xE8) return false;
        int32_t rel; memcpy(&rel, p + 1, 4);
        return callInsn + 5 + rel == ADDR_SET_RELOAD;
    }

    bool MatchMovEcxFsm(uintptr_t site)        // mov ecx,[esi+2C0h] ; call SetReload
    {
        static const unsigned char MOV[6] = { 0x8B, 0x8E, 0xC0, 0x02, 0x00, 0x00 };
        return memcmp(reinterpret_cast<const void*>(site), MOV, 6) == 0 && CallsSetReload(site + 6);
    }

    bool MatchReloadEjectRearm(uintptr_t site) // and dword [ebx+1BCh],-11h ; jmp short +2
    {
        static const unsigned char CODE[9] = { 0x83, 0xA3, 0xBC, 0x01, 0x00, 0x00, 0xEF, 0xEB, 0x02 };
        return memcmp(reinterpret_cast<const void*>(site), CODE, 9) == 0;
    }

    bool WriteBranch(uintptr_t site, int len, void* target, unsigned char opcode)   // E9 jmp / E8 call
    {
        unsigned char buf[16];
        buf[0] = opcode;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
        memcpy(buf + 1, &rel, 4);
        memset(buf + 5, 0x90, len - 5);
        DWORD prot = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(site), len, PAGE_EXECUTE_READWRITE, &prot)) return false;
        memcpy(reinterpret_cast<void*>(site), buf, len);
        VirtualProtect(reinterpret_cast<void*>(site), len, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), len);
        return true;
    }

    bool ReadOption(const char* key, bool def)
    {
        char ini[MAX_PATH];
        sprintf_s(ini, "%sReloadFixes.ini", g_dir);
        return GetPrivateProfileIntA(key[0] == 'L' ? "General" : "Fixes", key, def ? 1 : 0, ini) != 0;
    }

    void Install()
    {
        g_log = ReadOption("Log", true);
        const bool revolver = ReadOption("RevolverFlick", true);
        const bool queued   = ReadOption("ShotgunQueuedReload", true);
        g_sniperLastCasing  = ReadOption("SniperLastCasing", true);
        g_shotgunLastShell  = ReadOption("ShotgunLastShell", true);
        Log(false, "ReloadFixes (MH2): loading (RevolverFlick=%d, ShotgunQueuedReload=%d, SniperLastCasing=%d, ShotgunLastShell=%d)",
            revolver, queued, g_sniperLastCasing, g_shotgunLastShell);

        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        if (base != EXPECTED_IMAGE_BASE)
        {
            Log(true, "ReloadFixes (MH2): ABORT: image base 0x%08X, expected 0x400000. Nothing patched.", static_cast<unsigned>(base));
            return;
        }

        if (revolver)
        {
            if (!MatchMovEcxFsm(SITE_RELOAD_START) || !MatchMovEcxFsm(SITE_EMPTY_RELOAD))
                Log(true, "ReloadFixes (MH2): RevolverFlick ABORT: bytes at 0x%08X/0x%08X are not the expected reload call; not patched.",
                    static_cast<unsigned>(SITE_RELOAD_START), static_cast<unsigned>(SITE_EMPTY_RELOAD));
            else if (WriteBranch(SITE_RELOAD_START, 11, StubReloadStart, 0xE9) && WriteBranch(SITE_EMPTY_RELOAD, 11, StubEmptyReload, 0xE9))
                Log(false, "  RevolverFlick: patched 0x%08X and 0x%08X", static_cast<unsigned>(SITE_RELOAD_START), static_cast<unsigned>(SITE_EMPTY_RELOAD));
            else
                Log(true, "ReloadFixes (MH2): RevolverFlick FAILED to write the patch.");
        }

        if (queued)
        {
            if (!CallsSetReload(SITE_QUEUED_RELOAD))
                Log(true, "ReloadFixes (MH2): ShotgunQueuedReload ABORT: bytes at 0x%08X are not the expected reload call; not patched.",
                    static_cast<unsigned>(SITE_QUEUED_RELOAD));
            else if (WriteBranch(SITE_QUEUED_RELOAD, 5, StubQueuedReload, 0xE8))
                Log(false, "  ShotgunQueuedReload: patched 0x%08X", static_cast<unsigned>(SITE_QUEUED_RELOAD));
            else
                Log(true, "ReloadFixes (MH2): ShotgunQueuedReload FAILED to write the patch.");
        }

        // SniperLastCasing and ShotgunLastShell share the auto-reload hook; each is checked at run
        // time in DeferLastRoundAutoReload.
        if (g_sniperLastCasing || g_shotgunLastShell)
        {
            if (!MatchMovEcxFsm(SITE_BOLT_AUTO_RELOAD))
                Log(true, "ReloadFixes (MH2): SniperLastCasing/ShotgunLastShell ABORT: bytes at 0x%08X are not the expected reload call; not patched.",
                    static_cast<unsigned>(SITE_BOLT_AUTO_RELOAD));
            else if (WriteBranch(SITE_BOLT_AUTO_RELOAD, 11, StubBoltAutoReload, 0xE9))
                Log(false, "  SniperLastCasing/ShotgunLastShell: patched 0x%08X", static_cast<unsigned>(SITE_BOLT_AUTO_RELOAD));
            else
                Log(true, "ReloadFixes (MH2): SniperLastCasing/ShotgunLastShell FAILED to write the auto reload patch.");
        }

        if (g_shotgunLastShell)
        {
            if (!MatchReloadEjectRearm(SITE_RELOAD_EJECT_REARM))
                Log(true, "ReloadFixes (MH2): ShotgunLastShell ABORT: bytes at 0x%08X are not the expected re-arm; not patched.",
                    static_cast<unsigned>(SITE_RELOAD_EJECT_REARM));
            else if (WriteBranch(SITE_RELOAD_EJECT_REARM, 7, StubReloadEjectRearm, 0xE9))
                Log(false, "  ShotgunLastShell: patched 0x%08X", static_cast<unsigned>(SITE_RELOAD_EJECT_REARM));
            else
                Log(true, "ReloadFixes (MH2): ShotgunLastShell FAILED to write the re-arm patch.");
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        GetModuleFileNameA(hModule, g_dir, MAX_PATH);
        if (char* slash = strrchr(g_dir, '\\')) slash[1] = '\0';
        Install();
    }
    return TRUE;
}
