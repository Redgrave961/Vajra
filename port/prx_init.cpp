#include <cstdint>
#include <cstdio>

#include "prx_loader.h"

#ifdef PORT_HAVE_LIBSRE
struct ppu_context;

extern "C" const prx_module* libsre_module(void);
extern "C" void libsre_spu_register(void);
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

static void register_lifted(uint32_t addr, void (*host)(void*))
{
    ppu_register_function(addr, reinterpret_cast<void (*)(ppu_context*)>(host));
}
#endif

extern "C" void ps3_load_prx_modules(void)
{
#ifdef PORT_HAVE_LIBSRE
    const prx_load_result r = prx_load_module(libsre_module(), register_lifted);
    if (!r.ok) {
        std::fprintf(stderr, "[prx] libsre failed to load\n");
        return;
    }
    libsre_spu_register();
#endif
}
