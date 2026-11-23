#include "button_prompts.h"
#include "game_files.h"
#include "guest.h"
#include "pc_options.h"
#include "rl_achievements.h"
#include "rsx_core.h"
#include "vk/rsx_vk.h"
#include "vm_track.h"
#include "../ps3recomp/libs/network/sceNpTrophy.h"
#include "../ps3recomp/libs/system/cellSaveData.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#else
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <algorithm>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unwind.h>
#endif

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

#ifdef _WIN32
struct env_slot {
    std::atomic<const char*> key{ nullptr };
    std::atomic<const char*> value{ nullptr };
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

void env_cache_update(const char* name)
{
    if (!name || !*name) return;
    constexpr unsigned mask = 1023;
    const unsigned h = env_hash(name);
    AcquireSRWLockExclusive(&g_env_lock);
    for (unsigned i = h & mask, n = 0; n <= mask; n++, i = (i + 1) & mask) {
        const char* k = g_env[i].key.load(std::memory_order_relaxed);
        if (!k) break;
        if (!_stricmp(k, name)) {
            g_env[i].value.store(env_os(name), std::memory_order_release);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_env_lock);
}

char* __cdecl cached_getenv(const char* name)
{
    if (!name || !*name) return nullptr;
    constexpr unsigned mask = 1023;
    const unsigned h = env_hash(name);
    for (unsigned i = h & mask, n = 0; n <= mask; n++, i = (i + 1) & mask) {
        const char* k = g_env[i].key.load(std::memory_order_acquire);
        if (!k) break;
        if (!_stricmp(k, name)) return const_cast<char*>(g_env[i].value.load(std::memory_order_acquire));
    }
    AcquireSRWLockExclusive(&g_env_lock);
    const char* value = nullptr;
    unsigned i = h & mask, n = 0;
    bool found = false;
    for (; n <= mask; n++, i = (i + 1) & mask) {
        const char* k = g_env[i].key.load(std::memory_order_relaxed);
        if (!k) break;
        if (!_stricmp(k, name)) {
            value = g_env[i].value.load(std::memory_order_relaxed);
            found = true;
            break;
        }
    }
    if (!found) {
        value = env_os(name);
        if (n <= mask) {
            g_env[i].value.store(value, std::memory_order_relaxed);
            g_env[i].key.store(_strdup(name), std::memory_order_release);
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

void install_crash_report()
{
    SetUnhandledExceptionFilter(crash_report);
    ULONG guarantee = 256 * 1024;
    SetThreadStackGuarantee(&guarantee);
    vm_track_install();
    AddVectoredExceptionHandler(0, crash_fatal);
}
#else
constexpr size_t k_alt_stack = 256 * 1024;

struct alt_stack {
    void* base = nullptr;
    ~alt_stack()
    {
        if (!base) return;
        const stack_t off{ nullptr, SS_DISABLE, 0 };
        sigaltstack(&off, nullptr);
        munmap(base, k_alt_stack);
    }
};

thread_local alt_stack t_alt;
thread_local uintptr_t t_stack_lo;
thread_local size_t t_stack_guard;

void guard_thread()
{
    void* base = mmap(nullptr, k_alt_stack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (base == MAP_FAILED) return;
    t_alt.base = base;
    const stack_t ss{ base, 0, k_alt_stack };
    sigaltstack(&ss, nullptr);
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a)) return;
    void* lo;
    size_t size;
    pthread_attr_getstack(&a, &lo, &size);
    pthread_attr_getguardsize(&a, &t_stack_guard);
    pthread_attr_destroy(&a);
    t_stack_lo = reinterpret_cast<uintptr_t>(lo);
}

struct thread_start {
    void* (*fn)(void*);
    void* arg;
};

void* guarded_start(void* p)
{
    const thread_start s = *static_cast<thread_start*>(p);
    delete static_cast<thread_start*>(p);
    guard_thread();
    return s.fn(s.arg);
}

struct crash_walk {
    uintptr_t from;
    int i;
};

_Unwind_Reason_Code crash_frame(_Unwind_Context* uc, void* arg)
{
    crash_walk& w = *static_cast<crash_walk*>(arg);
    const uintptr_t pc = _Unwind_GetIP(uc);
    if (!pc || w.i >= 48) return _URC_END_OF_STACK;
    if (w.i < 0 && pc != w.from) return _URC_NO_REASON;
    Dl_info info{};
    const char* name = dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname ? info.dli_fname : "?";
    const char* base = std::strrchr(name, '/');
    w.i = w.i < 0 ? 0 : w.i;
    std::fprintf(stderr, "[crash]   #%d %s+0x%llX\n", w.i++, base ? base + 1 : name, (unsigned long long)(pc - reinterpret_cast<uintptr_t>(info.dli_fbase)));
    return _URC_NO_REASON;
}

void crash_signal(int sig, siginfo_t* si, void* ctx)
{
    const ucontext_t* uc = static_cast<const ucontext_t*>(ctx);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(si->si_addr), guard = std::max<size_t>(t_stack_guard, (size_t)getpagesize());
    const bool fault = sig == SIGSEGV || sig == SIGBUS;
    if (fault && t_stack_lo && addr < t_stack_lo && addr >= t_stack_lo - guard)
        std::fprintf(stderr, "\n[crash] stack overflow, thread %ld, last HLE 0x%08X (%s)\n", (long)gettid(), g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    else
        std::fprintf(stderr, "\n[crash] signal %d (%s), thread %ld, last HLE 0x%08X (%s)\n", sig, strsignal(sig), (long)gettid(), g_last_hle_nid,
                     g_last_hle_name ? g_last_hle_name : "");
    if (fault)
        std::fprintf(stderr, "[crash] %s of 0x%llX at 0x%llX\n", uc->uc_mcontext.gregs[REG_ERR] & 2 ? "write" : "read", (unsigned long long)addr,
                     (unsigned long long)uc->uc_mcontext.gregs[REG_RIP]);
    crash_walk w{ (uintptr_t)uc->uc_mcontext.gregs[REG_RIP], -1 };
    _Unwind_Backtrace(crash_frame, &w);
    std::fflush(stderr);
    raise(sig);
}

void install_crash_report()
{
    guard_thread();
    struct sigaction sa {};
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (const int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT }) sigaction(sig, &sa, nullptr);
}
#endif

void debug_console(const char* path)
{
    char out_path[1024];
    std::snprintf(out_path, sizeof out_path, "%s.out", path);
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
            std::fprintf(o, "  uptime   %.1f s\n  flips    %u\n  last HLE 0x%08X (%s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
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

#if defined(_WIN32) && !defined(ASURA_PGO_GEN)
extern "C" char* getenv(const char* name) { return cached_getenv(name); }
#endif

#ifndef _WIN32
extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg);

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg)
{
    thread_start* start = new thread_start{ fn, arg };
    const int rc = __real_pthread_create(thread, attr, guarded_start, start);
    if (rc) delete start;
    return rc;
}
#endif

namespace {

#ifdef _WIN32
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
    env_cache_update(name);
}

void attach_log(const std::filesystem::path& exe)
{
    const char* log_env = std::getenv("VAJRA_LOG");
    if (!log_env || !*log_env || !std::strcmp(log_env, "0")) return;
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) return;
    const std::string log = (exe / "asura.log").string();
    if (FILE* f = std::fopen(log.c_str(), "wb")) std::fclose(f);
    std::freopen(log.c_str(), "a", stderr);
    std::freopen(log.c_str(), "a", stdout);
}

void precise_timers() { timeBeginPeriod(1); }

uint8_t* reserve_guest_memory() { return static_cast<uint8_t*>(VirtualAlloc(nullptr, k_vm_size, MEM_RESERVE, PAGE_READWRITE)); }

std::filesystem::path desktop_identity() { return {}; }
#else
std::filesystem::path executable_dir()
{
    const char* image = std::getenv("APPIMAGE");
    return std::filesystem::path(image && *image ? image : std::filesystem::read_symlink("/proc/self/exe")).parent_path();
}

void default_env(const char* name, const std::filesystem::path& value) { setenv(name, value.c_str(), 0); }

void attach_log(const std::filesystem::path& exe)
{
    const char* log_env = std::getenv("VAJRA_LOG");
    if (!log_env || !*log_env || !std::strcmp(log_env, "0")) return;
    struct stat st;
    if (isatty(STDERR_FILENO) || (!fstat(STDERR_FILENO, &st) && (S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode)))) return;
    const std::string log = (exe / "asura.log").string();
    if (FILE* f = std::fopen(log.c_str(), "wb")) std::fclose(f);
    std::freopen(log.c_str(), "a", stderr);
    std::freopen(log.c_str(), "a", stdout);
}

void precise_timers() { prctl(PR_SET_TIMERSLACK, 1ul); }

uint8_t* reserve_guest_memory()
{
    void* p = mmap(nullptr, k_vm_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return nullptr;
#ifdef MADV_HUGEPAGE
    madvise(p, k_vm_size, MADV_HUGEPAGE);
#endif
    return static_cast<uint8_t*>(p);
}

std::filesystem::path desktop_identity()
{
    SDL_SetAppMetadata("Vajra", "1.0.0.0", "Vajra");
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
    return std::filesystem::read_symlink("/proc/self/exe").parent_path() / "Vajra.png";
}
#endif

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

void on_trophy_register(int id, const char* name, const char* detail, char grade, int unlocked)
{
    const uint32_t pts = grade == 'P' ? 180u : grade == 'G' ? 90u : grade == 'S' ? 30u : 15u;
    rl_ach_native_define((uint32_t)id, name, detail, pts);
    if (unlocked) rl_ach_native_earlier((uint32_t)id);
}

void on_trophy_unlock(int id) { rl_ach_native((uint32_t)id); }
int is_trophy_quiet(void) { return rl_ach_quiet_toasts(); }
void on_trophy_frame(void) { rl_ach_frame(); }
void on_save_loaded(void) { rl_ach_save_loaded(); }

void init_recomp_achievements()
{
    rl_ach_set_info("vajra", "1.0.0");
    if (rl_ach_init("ps3", nullptr, nullptr)) {
        ps3_trophy_set_callbacks(on_trophy_register, on_trophy_unlock, is_trophy_quiet, on_trophy_frame);
        cellSaveData_set_load_callback(on_save_loaded);
    }
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

int main(int argc, char** argv)
{
    const std::filesystem::path exe = executable_dir();
    std::filesystem::path data_dir;
    launch_options opts;
    const char* elf = nullptr;

    auto path_arg = [](const char* s) {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s)));
    };
    for (int i = 1; i < argc; i++) {
        const std::string_view a = argv[i];
        auto take = [&](std::string_view flag, std::filesystem::path& out) -> bool {
            if (a == flag && i + 1 < argc) { out = path_arg(argv[++i]); return true; }
            if (a.size() > flag.size() + 1 && a.starts_with(flag) && a[flag.size()] == '=') {
                out = path_arg(argv[i] + flag.size() + 1);
                return true;
            }
            return false;
        };
        std::filesystem::path dlc_item;
        if (a == "--prepare") opts.prepare = true;
        else if (take("--data", data_dir)) {}
        else if (take("--game", opts.game)) {}
        else if (take("--update", opts.update)) {}
        else if (take("--dlc", dlc_item)) { opts.dlcs.push_back(std::move(dlc_item)); }
        else if (!a.starts_with("-") && !elf) elf = argv[i];
    }

    if (data_dir.empty()) {
        if (const char* env_data = std::getenv("RECOMPLAUNCHER_DATA"); env_data && *env_data)
            data_dir = path_arg(env_data);
        else
            data_dir = exe;
    }
    {
        std::error_code data_ec;
        std::filesystem::create_directories(data_dir, data_ec);
    }

    if (opts.prepare) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        return prepare_game_files(data_dir, opts);
    }

    attach_log(data_dir);
    const std::filesystem::path icon = desktop_identity();
    game_files files;
    if (!load_game_files(data_dir, icon, files, opts)) return 2;
    use_game_files(files);
    pc_options_boot(data_dir);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    precise_timers();
    install_crash_report();
    vm_base = reserve_guest_memory();
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
    std::error_code cache_ec;
    const std::filesystem::path cache_dir = data_dir / "cache";
    std::filesystem::create_directories(cache_dir, cache_ec);
    rsx_core_set_renderer(rsx_vk_renderer(cellGame_get_title(), (cache_dir / "vk_pipelines.bin").string().c_str(), icon.string().c_str()));
    use_button_prompts();

    ppu_recomp_register();
    ps3_load_prx_modules();
    ppu_hle_init();
    ppu_sysprx_register();
    ppu_fs_register();
    lv2_init_syscalls();
    g_ps3_guest_caller = guest_caller;

    if (const char* dbg = std::getenv("PS3_DEBUG"); dbg && *dbg) std::thread(debug_console, dbg).detach();

    init_recomp_achievements();
    ppu_run(entry, k_stack_top);
    rl_ach_shutdown();
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
