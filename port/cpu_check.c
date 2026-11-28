#include <cpuid.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

enum { k_unsupported_cpu = 8 };

struct feature {
    const char* name;
    unsigned present;
};

static uint64_t enabled_state(void)
{
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (uint64_t)hi << 32 | lo;
}

static void append(char* out, size_t cap, size_t* len, const char* text)
{
    while (*text && *len + 1 < cap) out[(*len)++] = *text++;
    out[*len] = 0;
}

__attribute__((used)) static void cpu_check(void)
{
    unsigned a = 0, b = 0, c = 0, d = 0, basic = 0, leaf7 = 0, extended = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) basic = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) leaf7 = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) extended = c;
    const unsigned osxsave = basic >> 27 & 1;
    const struct feature needed[] = {
        { "SSE3", basic & 1 },
        { "SSSE3", basic >> 9 & 1 },
        { "SSE4.1", basic >> 19 & 1 },
        { "SSE4.2", basic >> 20 & 1 },
        { "POPCNT", basic >> 23 & 1 },
        { "CMPXCHG16B", basic >> 13 & 1 },
        { "LAHF/SAHF", extended & 1 },
        { "AVX", basic >> 28 & 1 },
        { "AVX2", leaf7 >> 5 & 1 },
        { "FMA", basic >> 12 & 1 },
        { "BMI1", leaf7 >> 3 & 1 },
        { "BMI2", leaf7 >> 8 & 1 },
        { "LZCNT", extended >> 5 & 1 },
        { "MOVBE", basic >> 22 & 1 },
        { "F16C", basic >> 29 & 1 },
        { "OSXSAVE", osxsave },
        { "AVX enabled by the operating system", osxsave && (enabled_state() & 6) == 6 },
    };
    char text[768];
    size_t len = 0;
    append(text, sizeof text, &len, "Vajra needs a CPU with AVX2 (Intel Haswell / AMD Excavator or newer).\n\nThis PC is missing:");
    const size_t head = len;
    for (size_t i = 0; i < sizeof needed / sizeof needed[0]; i++) {
        if (needed[i].present) continue;
        append(text, sizeof text, &len, len == head ? " " : ", ");
        append(text, sizeof text, &len, needed[i].name);
    }
    if (len == head) return;
    append(text, sizeof text, &len, "\n");
#ifdef _WIN32
    wchar_t wide[sizeof text];
    for (size_t i = 0; i <= len; i++) wide[i] = (unsigned char)text[i];
    MessageBoxW(NULL, wide, L"Vajra", MB_OK | MB_ICONERROR);
    ExitProcess(k_unsupported_cpu);
#else
    for (size_t done = 0; done < len;) {
        const ssize_t n = write(STDERR_FILENO, text + done, len - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    _exit(k_unsupported_cpu);
#endif
}

#ifdef _WIN32
int WinMainCRTStartup(void);

int vajra_entry(void)
{
    cpu_check();
    return WinMainCRTStartup();
}
#else
__attribute__((naked, noreturn)) void vajra_entry(void)
{
    __asm__("mov %rdx, %r12\n\t"
            "call cpu_check\n\t"
            "mov %r12, %rdx\n\t"
            "jmp _start");
}
#endif
