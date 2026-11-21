#include "button_prompts.h"
#include "game_files.h"
#include "guest.h"
#include "win32_compat.h"
#include "pc_options.h"
#include "rsx_core.h"
#include "vk/rsx_vk.h"
#include "vm_track.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <windows.h>
#include <timeapi.h>

extern "C" {
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;
extern const char* ppu_vfs_root;
extern uint32_t g_last_hle_nid;
extern const char* g_last_hle_name;
extern const char* g_hle_inflight[];
extern int g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

uint32_t ppu_load_elf(const char* path);
uint32_t ppu_load_elf_image(const uint8_t* file, uint64_t size);
void ppu_recomp_register(void);
void ps3_load_prx_modules(void);
void ppu_hle_init(void);
void ppu_sysprx_register(void);
void ppu_fs_register(void);
void lv2_init_syscalls(void);
int ppu_run(uint32_t entry_opd, uint32_t stack_top);
void ppu_report_guest_lrs(void);
void cellGame_init_boot(const char* vfs_root);
const char* cellGame_get_title(void);

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
}

namespace {

constexpr uint64_t k_vm_size = 0x100010000ull;
constexpr uint32_t k_stack_top = 0x0FF00000u;

void guest_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                  uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

char g_root[1024];

void set_vfs_root(const char* eboot)
{
    const char* env = std::getenv("PS3_VFS_ROOT");
    if (env && *env) {
        ppu_vfs_root = env;
        return;
    }
    std::snprintf(g_root, sizeof g_root, "%s", eboot);
    for (char* p = g_root; *p; p++)
        if (*p == '\\') *p = '/';
    for (int i = 0; i < 3; i++)
        if (char* s = std::strrchr(g_root, '/')) *s = 0;
    if (!g_root[0]) std::strcpy(g_root, ".");
    ppu_vfs_root = g_root;
}

struct env_slot {
    const char* volatile key;
    const char* value;
};
env_slot g_env[1024];
SRWLOCK g_env_lock = SRWLOCK_INIT;

unsigned env_hash(const char* s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s)) * 16777619u;
    return h;
}

const char* env_os(const char* name)
{
    const DWORD n = GetEnvironmentVariableA(name, nullptr, 0);
    if (!n) return nullptr;
    char* v = static_cast<char*>(std::malloc(n));
    if (!v || GetEnvironmentVariableA(name, v, n) >= n) {
        std::free(v);
        return nullptr;
    }
    return v;
}

char* __cdecl cached_getenv(const char* name)
{
    if (!name || !*name) return nullptr;
    constexpr unsigned mask = 1023;
    const unsigned h = env_hash(name);
    for (unsigned i = h & mask, n = 0; n <= mask; n++, i = (i + 1) & mask) {
        const char* k = g_env[i].key;
        MemoryBarrier();
        if (!k) break;
        if (!_stricmp(k, name)) return const_cast<char*>(g_env[i].value);
    }
    AcquireSRWLockExclusive(&g_env_lock);
    const char* value = nullptr;
    unsigned i = h & mask, n = 0;
    bool found = false;
    for (; n <= mask; n++, i = (i + 1) & mask) {
        if (!g_env[i].key) break;
        if (!_stricmp(g_env[i].key, name)) {
            value = g_env[i].value;
            found = true;
            break;
        }
    }
    if (!found) {
        value = env_os(name);
        if (n <= mask) {
            g_env[i].value = value;
            MemoryBarrier();
            g_env[i].key = _strdup(name);
        }
    }
    ReleaseSRWLockExclusive(&g_env_lock);
    return const_cast<char*>(value);
}

void crash_frame(int i, DWORD64 pc)
{
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(pc), &m);
    char path[MAX_PATH] = "?";
    if (m) GetModuleFileNameA(m, path, sizeof path);
    const char* name = std::strrchr(path, '\\');
    std::fprintf(stderr, "[crash]   #%d %s+0x%llX\n", i, name ? name + 1 : path, (unsigned long long)(pc - reinterpret_cast<DWORD64>(m)));
}

void crash_dump(const EXCEPTION_POINTERS* ep, const char* kind)
{
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return;
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    std::fprintf(stderr, "\n[crash] %s 0x%08lX, thread %lu, last HLE 0x%08X (%s)\n", kind, (unsigned long)er->ExceptionCode, GetCurrentThreadId(),
                 g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        std::fprintf(stderr, "[crash] %s of 0x%llX\n", er->ExceptionInformation[0] ? "write" : "read", (unsigned long long)er->ExceptionInformation[1]);
    CONTEXT ctx = *ep->ContextRecord;
    for (int i = 0; i < 48 && ctx.Rip; i++) {
        crash_frame(i, ctx.Rip);
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
        if (!f) {
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* data;
        DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &data, &frame, nullptr);
    }
    std::fflush(stderr);
}

LONG WINAPI crash_fatal(EXCEPTION_POINTERS* ep)
{
    switch (ep->ExceptionRecord->ExceptionCode) {
    case 0xC0000374u:
    case 0xC0000409u:
    case 0xC0000420u:
    case 0xC0000421u:
    case 0xC00000FDu:
    case 0xC0000417u:
        crash_dump(ep, "fatal");
        break;
    default:
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI crash_report(EXCEPTION_POINTERS* ep)
{
    crash_dump(ep, "unhandled");
    return EXCEPTION_EXECUTE_HANDLER;
}
DWORD WINAPI debug_console(LPVOID param)
{
    const char* path = static_cast<const char*>(param);
    char out_path[1024];
    std::snprintf(out_path, sizeof out_path, "%s.out", path);
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        Sleep(100);
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        char cmd[256] = {};
        const bool got = std::fgets(cmd, sizeof cmd, f) != nullptr;
        std::fclose(f);
        if (!got) continue;
        cmd[std::strcspn(cmd, "\r\n")] = 0;
        if (!cmd[0]) continue;
        if (FILE* t = std::fopen(path, "wb")) std::fclose(t);
        FILE* o = std::fopen(out_path, "ab");
        if (!o) continue;
        unsigned a = 0, b = 0;
        std::fprintf(o, "> %s\n", cmd);
        if (!std::strcmp(cmd, "stat")) {
            std::fprintf(o, "  uptime   %.1f s\n  flips    %u\n  last HLE 0x%08X (%s)\n", (GetTickCount64() - t0) / 1000.0,
                         rsx_core_frames_presented(), g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
        } else if (!std::strcmp(cmd, "threads")) {
            for (unsigned t = 0; t < 64u; t++)
                if (g_hle_inflight[t]) std::fprintf(o, "  guest tid %u in %s\n", t, g_hle_inflight[t]);
            std::fclose(o);
            o = nullptr;
            ppu_report_guest_lrs();
        } else if (std::sscanf(cmd, "mem %x %u", &a, &b) >= 1) {
            if (b == 0 || b > 4096) b = 64;
            for (unsigned off = 0; off < b; off += 16) {
                std::fprintf(o, "  0x%08X", a + off);
                for (unsigned i = 0; i < 16 && off + i < b; i++) std::fprintf(o, " %02X", vm_base[(size_t)a + off + i]);
                std::fprintf(o, "\n");
            }
        } else {
            std::fprintf(o, "  commands: stat, threads, mem <hex> [len]\n");
        }
        if (o) std::fclose(o);
    }
}

}

#ifndef ASURA_PGO_GEN
extern "C" char* getenv(const char* name) { return cached_getenv(name); }
#endif

namespace {

std::filesystem::path executable_dir()
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(std::wstring(path, n)).parent_path();
}

void default_env(const char* name, const std::filesystem::path& value)
{
    if (GetEnvironmentVariableA(name, nullptr, 0)) return;
    _wputenv_s(std::filesystem::path(name).c_str(), value.c_str());
}

bool skipped_intro(uint32_t name)
{
    static const char* const k_intros[] = {
        "00_AUTOSAVE",     "00_AUTOSAVE_DEU", "00_AUTOSAVE_ESN", "00_AUTOSAVE_FRA",  "00_AUTOSAVE_ITA",
        "00_AUTOSAVE_JPN", "00_EXAMINATION",  "20_BINKLOGO",     "20_SPEEDTREELOGO", "20_UE3LOGO",
        "30_GORE",         "40_CAPCOMLOGO",   "40_CC2LOGO",
    };
    for (const char* intro : k_intros) {
        uint32_t i = 0;
        for (uint16_t c; intro[i] && (c = vm_read16(name + 2 * i)) && (c >= 'a' && c <= 'z' ? c - 32 : c) == (uint8_t)intro[i]; i++) {}
        if (!intro[i] && !vm_read16(name + 2 * i)) return true;
    }
    return false;
}

void use_game_files(const game_files& files)
{
    default_env("PS3_VFS_ROOT", files.bdvd);
    default_env("PS3_HDD0_ROOT", files.hdd0);
    default_env("PS3_HDD1_ROOT", files.hdd1);
    default_env("PS3_GAMEDATA_ROOT", files.user);
    default_env("PS3_DEV_FLASH", files.flash);
    default_env("PS3_VERBOSE", "0");
}

void attach_log(const std::filesystem::path& exe)
{
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) return;
    const std::string log = (exe / "asura.log").string();
    std::freopen(log.c_str(), "w", stderr);
    std::freopen(log.c_str(), "a", stdout);
}

}

void func_0013A02C(ppu_context* ctx)
{
    const uint32_t player = (uint32_t)ctx->gpr[3];
    if (!skipped_intro((uint32_t)ctx->gpr[6])) return run(func_0013A02C_lifted, ctx);
    const uint32_t started = vm_read32(player + 0xC);
    ctx->gpr[3] = started;
    guest_call(ctx, vm_read32(vm_read32(started) + 0xC));
    ctx->gpr[3] = player;
    ctx->gpr[4] = 1;
    run(func_0013B2E0, ctx);
    ctx->gpr[3] = 1;
}

void unlock_frame_rate()
{
    const char* args = env_os("PS3_ARGV");
    char line[512];
    std::snprintf(line, sizeof line, "%s%s-novsync", args ? args : "", args && *args ? ";" : "");
    _putenv_s("PS3_ARGV", line);
    _putenv_s("PS3_FREE_FLIPS", "1");
}
int main(int argc, char** argv)
{
    const std::filesystem::path exe = executable_dir();
    attach_log(exe);
    game_files files;
    if (!load_game_files(exe, files)) return 2;
    use_game_files(files);
    if (pc_options_boot()) unlock_frame_rate();
    const char* elf = argc > 1 ? argv[1] : nullptr;
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    timeBeginPeriod(1);
    SetUnhandledExceptionFilter(crash_report);
    ULONG guarantee = 256 * 1024;
    SetThreadStackGuarantee(&guarantee);
    vm_track_install();
    AddVectoredExceptionHandler(0, crash_fatal);
    vm_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, k_vm_size, MEM_RESERVE, PAGE_READWRITE));
    ppu_vm_size = 0;
    if (!vm_base) {
        std::printf("guest memory reservation failed\n");
        return 1;
    }

    const uint32_t entry = elf ? ppu_load_elf(elf) : ppu_load_elf_image(files.elf.data(), files.elf.size());
    std::vector<uint8_t>().swap(files.elf);
    if (!entry) {
        std::printf("cannot load %s\n", elf ? elf : "the update's EBOOT");
        return 1;
    }
    set_vfs_root(elf ? elf : "");
    cellGame_init_boot(ppu_vfs_root);
    rsx_core_set_renderer(rsx_vk_renderer(cellGame_get_title()));
    use_button_prompts();

    ppu_recomp_register();
    ps3_load_prx_modules();
    ppu_hle_init();
    ppu_sysprx_register();
    ppu_fs_register();
    lv2_init_syscalls();
    g_ps3_guest_caller = guest_caller;

    if (const char* dbg = std::getenv("PS3_DEBUG"); dbg && *dbg) CreateThread(nullptr, 0, debug_console, const_cast<char*>(dbg), 0, nullptr);

    ppu_run(entry, k_stack_top);
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
