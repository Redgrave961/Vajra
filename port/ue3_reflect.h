#pragma once

#include "guest.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace ue3 {

constexpr uint32_t k_gnames = 0x01A0B014;
constexpr uint32_t k_object_name = 0x2C;
constexpr uint32_t k_object_class = 0x34;
constexpr uint32_t k_field_next = 0x3C;
constexpr uint32_t k_struct_super = 0x40;
constexpr uint32_t k_struct_children = 0x44;
constexpr uint32_t k_property_element = 0x44;
constexpr uint32_t k_property_offset = 0x54;
constexpr uint32_t k_property_detail = 0x64;

inline constexpr uint64_t hook_bit(uint32_t fn)
{
    return 1ull << ((fn * 0x9E3779B1u) >> 26);
}

inline bool object_name_eq(uint32_t object, const char* want)
{
    if (vm_read32(object + k_object_name + 4)) return false;
    const uint32_t entry = vm_read32(vm_read32(k_gnames) + vm_read32(object + k_object_name) * 4);
    if (vm_read32(entry) & 1) return false;
    for (uint32_t at = entry + 8;; ++at, ++want) {
        const char c = (char)vm_read8(at);
        if (c != *want) return false;
        if (!c) return true;
    }
}

struct Member {
    struct Resolved {
        uint32_t offset = 0;
        uint32_t detail = 0;
        bool found = false;
    };
    struct Entry {
        std::atomic<uint32_t> owner{ 0 };
        Resolved r{};
    };
    static constexpr uint32_t k_cap = 16;

    const char* name;
    std::atomic<uint32_t> count{ 0 };
    Entry cache[k_cap]{};
    std::mutex lock{};

    Resolved bind(uint32_t cls)
    {
        const uint32_t n = count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i)
            if (cache[i].owner.load(std::memory_order_relaxed) == cls) return cache[i].r;

        std::lock_guard<std::mutex> guard(lock);
        const uint32_t n2 = count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n2; ++i)
            if (cache[i].owner.load(std::memory_order_relaxed) == cls) return cache[i].r;

        uint32_t field = 0;
        for (uint32_t c = cls; c && !field; c = vm_read32(c + k_struct_super))
            for (uint32_t f = vm_read32(c + k_struct_children); f && !field; f = vm_read32(f + k_field_next))
                if (object_name_eq(f, name)) field = f;

        const Resolved res{ field ? vm_read32(field + k_property_offset) : 0, field ? vm_read32(field + k_property_detail) : 0, field != 0 };
        if (n2 < k_cap) {
            cache[n2].r = res;
            cache[n2].owner.store(cls, std::memory_order_relaxed);
            count.store(n2 + 1, std::memory_order_release);
        }
        return res;
    }

    uint32_t at(uint32_t base, uint32_t cls, uint32_t* out_detail = nullptr)
    {
        const Resolved r = bind(cls);
        if (out_detail) *out_detail = r.detail;
        return r.found ? base + r.offset : 0;
    }

    uint32_t obj(uint32_t o) { const uint32_t a = at(o, vm_read32(o + k_object_class)); return a ? vm_read32(a) : 0; }
    uint8_t byte(uint32_t o) { const uint32_t a = at(o, vm_read32(o + k_object_class)); return a ? vm_read8(a) : 0; }
    int32_t i32(uint32_t o) { const uint32_t a = at(o, vm_read32(o + k_object_class)); return a ? (int32_t)vm_read32(a) : 0; }
    float f32(uint32_t o) { const uint32_t a = at(o, vm_read32(o + k_object_class)); return a ? from_bits(vm_read32(a)) : 0.f; }
    void set_f32(uint32_t o, float v) { if (const uint32_t a = at(o, vm_read32(o + k_object_class))) vm_write32(a, to_bits(v)); }

    bool flag(uint32_t o)
    {
        const Resolved r = bind(vm_read32(o + k_object_class));
        return r.found && (vm_read32(o + r.offset) & r.detail) != 0;
    }

    void set_flag(uint32_t o, bool v)
    {
        const Resolved r = bind(vm_read32(o + k_object_class));
        if (!r.found) return;
        const uint32_t cur = vm_read32(o + r.offset);
        const uint32_t next = v ? (cur | r.detail) : (cur & ~r.detail);
        if (next != cur) vm_write32(o + r.offset, next);
    }

    bool sflag(uint32_t base, uint32_t owner)
    {
        const Resolved r = bind(owner);
        return r.found && (vm_read32(base + r.offset) & r.detail) != 0;
    }

    void set_sflag(uint32_t base, uint32_t owner, bool v)
    {
        const Resolved r = bind(owner);
        if (!r.found) return;
        const uint32_t cur = vm_read32(base + r.offset);
        const uint32_t next = v ? (cur | r.detail) : (cur & ~r.detail);
        if (next != cur) vm_write32(base + r.offset, next);
    }
};

struct Elements {
    uint32_t data = 0, count = 0, stride = 0, type = 0;
};

inline Elements elements(Member& arr, uint32_t base, uint32_t owner)
{
    uint32_t inner = 0;
    const uint32_t at = arr.at(base, owner, &inner);
    if (!at || !inner) return {};
    return { vm_read32(at), vm_read32(at + 4), vm_read32(inner + k_property_element), vm_read32(inner + k_property_detail) };
}

constexpr uint32_t k_gobjects = 0x01A51564;
constexpr uint32_t k_object_index = 0x20;

inline bool uobject_registered(const ppu_mm& m, uint32_t obj)
{
    const uint32_t index = vm_fast_read32(m, obj + k_object_index);
    return index < vm_fast_read32(m, k_gobjects + 4) && vm_fast_read32(m, vm_fast_read32(m, k_gobjects) + 4u * index) == obj;
}

inline uint32_t find_function_probe(const uint8_t* vb, uint32_t level, uint32_t index, uint64_t name_be)
{
    auto rd32 = [](const uint8_t* p) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return __builtin_bswap32(v);
    };
    const uint32_t size = rd32(vb + level + 0xB8);
    if (!size) return 0;
    const uint32_t buckets = rd32(vb + level + 0xB4);
    const uint32_t table = buckets ? buckets : level + 0xB0;
    const uint32_t elements = rd32(vb + level + 0x80);
    for (uint32_t i = rd32(vb + table + 4u * (index & (size - 1u))); (int32_t)i != -1;) {
        const uint8_t* ep = vb + elements + 20u * i;
        uint64_t raw;
        std::memcpy(&raw, ep, 8);
        if (raw == name_be) return rd32(ep + 8);
        i = rd32(ep + 0xC);
    }
    return 0;
}

inline uint32_t find_function_walk(const uint8_t* vb, uint32_t level, uint32_t index, uint64_t name_be)
{
    for (; level;) {
        if (const uint32_t found = find_function_probe(vb, level, index, name_be)) return found;
        uint32_t next_lvl;
        std::memcpy(&next_lvl, vb + level + 0x40, 4);
        level = __builtin_bswap32(next_lvl);
    }
    return 0;
}

}
