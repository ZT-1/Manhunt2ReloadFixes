// RevolverFlickFix.asi - Manhunt 2 PC (Manhunt2.exe, image base 0x400000, no ASLR).
// Port of the Manhunt 1 fix (github.com/ZT-1/ManhuntRevolverReloadFix). Same engine bug,
// confirmed in game in MH2 as well.
//
// Bug: CPed+0x308 bit 0x40 means "partial looped reload: when the RELOAD_LOOP ends, stop the reload
// anim instead of playing its tail". It lets a partial shotgun reload skip the pump.
//   - The player reload request only writes the bit for CT_SHOTGUN (0x2E) / CT_SHOTGUN_TORCH (0x2F):
//     0x508001  cmp [ebx+64h],0  ->  or [esi+308h],40h  /  and [esi+308h],0FFFFFFBFh
//   - The other guns start their reload at 0x508025 without touching the bit. The empty-clip
//     reload at 0x5AB6FA doesn't touch it either.
//   - The torso reload state checks the bit for EVERY RELOAD_LOOP weapon on loop exit (0x510A8D)
//     and cuts the anim there. So the 6-shooter (RELOAD_LOOP 1.067, 1.267) stops before its
//     cylinder closes (6Shooter_Reload_W: open 0.28-1.90 s).
//   - The re-reload after the pump (0x510941) already clears 0x40/0x80 itself.
//
// Fix: at both non-shotgun reload starts, clear bit 0x40 before calling CPedTorsoAnimFSM::SetReload.
//   site:  8B 8E C0 02 00 00   mov ecx,[esi+2C0h]  ; torso FSM
//          E8 rel32            call 0x492B30       ; SetReload
//   ->     jmp stub, nop x6
//   stub:  and dword [esi+308h],0FFFFFFBFh ; mov ecx,[esi+2C0h] ; call 0x492B30 ; jmp site+11

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>

namespace
{
    constexpr uintptr_t EXPECTED_IMAGE_BASE = 0x400000;
    constexpr uintptr_t ADDR_SET_RELOAD = 0x492B30;   // CPedTorsoAnimFSM::SetReload
    constexpr uintptr_t SITES[] = { 0x508025, 0x5AB6FA };
    constexpr int SITE_LEN = 11;
    const unsigned char MOV_ECX_FSM[6] = { 0x8B, 0x8E, 0xC0, 0x02, 0x00, 0x00 };   // mov ecx,[esi+2C0h]
    const unsigned char AND_FLAG40[7]  = { 0x83, 0xA6, 0x08, 0x03, 0x00, 0x00, 0xBF }; // and dword [esi+308h],-41h

    void Log(const char* fmt, ...)
    {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\'))
            strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "revolverflickfix_asi.log");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "a") != 0 || !f) return;
        va_list args; va_start(args, fmt); vfprintf(f, fmt, args); va_end(args);
        fputc('\n', f); fclose(f);
    }

    int32_t Rel(uintptr_t from, uintptr_t to) { return static_cast<int32_t>(to - (from + 5)); }

    bool SiteMatches(uintptr_t site)
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(site);
        if (memcmp(p, MOV_ECX_FSM, 6) != 0 || p[6] != 0xE8) return false;
        int32_t rel; memcpy(&rel, p + 7, 4);
        return site + 11 + rel == ADDR_SET_RELOAD;
    }

    bool PatchSite(uintptr_t site, unsigned char* stub)
    {
        unsigned char* s = stub;
        memcpy(s, AND_FLAG40, 7); s += 7;
        memcpy(s, MOV_ECX_FSM, 6); s += 6;
        int32_t r = Rel(reinterpret_cast<uintptr_t>(s), ADDR_SET_RELOAD);
        *s++ = 0xE8; memcpy(s, &r, 4); s += 4;
        r = Rel(reinterpret_cast<uintptr_t>(s), site + SITE_LEN);
        *s++ = 0xE9; memcpy(s, &r, 4); s += 4;

        unsigned char buf[SITE_LEN];
        buf[0] = 0xE9;
        r = Rel(site, reinterpret_cast<uintptr_t>(stub));
        memcpy(buf + 1, &r, 4);
        memset(buf + 5, 0x90, SITE_LEN - 5);
        DWORD prot = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(site), SITE_LEN, PAGE_EXECUTE_READWRITE, &prot)) return false;
        memcpy(reinterpret_cast<void*>(site), buf, SITE_LEN);
        VirtualProtect(reinterpret_cast<void*>(site), SITE_LEN, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), SITE_LEN);
        return true;
    }

    void Install()
    {
        Log("RevolverFlickFix (MH2): loading");
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        if (base != EXPECTED_IMAGE_BASE) { Log("  ABORT: image base 0x%08X, expected 0x400000.", static_cast<unsigned>(base)); return; }
        for (uintptr_t site : SITES)
            if (!SiteMatches(site)) { Log("  ABORT: bytes at 0x%08X are not the expected reload call; nothing patched.", static_cast<unsigned>(site)); return; }

        unsigned char* mem = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!mem) { Log("  ABORT: VirtualAlloc failed."); return; }
        int i = 0;
        for (uintptr_t site : SITES)
        {
            if (PatchSite(site, mem + i * 32)) Log("  patched 0x%08X -> stub %p (clears CPed+0x308 bit 0x40 before SetReload)", static_cast<unsigned>(site), mem + i * 32);
            else Log("  FAILED to patch 0x%08X", static_cast<unsigned>(site));
            ++i;
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hModule); Install(); }
    return TRUE;
}
