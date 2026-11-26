#include "guest.h"

#include <cmath>
#include <cstring>

namespace {

inline float dot3(float ax, float ay, float az, const ppu_mm& m, uint32_t ea)
{
    const float x = vm_read_f32_mm(m, ea);
    const float y = vm_read_f32_mm(m, ea + 4u);
    const float z = vm_read_f32_mm(m, ea + 8u);
    return std::fmaf(az, z, std::fmaf(ax, x, ay * y));
}

inline float dot3_scaled(float ax, float ay, float az, const ppu_mm& m, uint32_t ea, float s)
{
    const float x = vm_read_f32_mm(m, ea) * s;
    const float y = vm_read_f32_mm(m, ea + 4u) * s;
    const float z = vm_read_f32_mm(m, ea + 8u) * s;
    return std::fmaf(az, z, std::fmaf(ax, x, ay * y));
}

inline float box_radius(float ax, float ay, float az, const ppu_mm& m,
                        uint32_t box_ax, uint32_t box_ay, uint32_t box_az, uint32_t box_ext)
{
    const float bx = std::fabs(dot3(ax, ay, az, m, box_ax));
    const float by = std::fabs(dot3(ax, ay, az, m, box_ay));
    const float bz = std::fabs(dot3(ax, ay, az, m, box_az));
    return std::fmaf(vm_read_f32_mm(m, box_ext + 8u), bz,
                     std::fmaf(vm_read_f32_mm(m, box_ext), bx, vm_read_f32_mm(m, box_ext + 4u) * by));
}

inline void write_hit(const ppu_mm& m, uint32_t ea, float nx, float ny, float nz)
{
    vm_write32_mm(&m, ea, 1u);
    vm_write_f32_mm(m, ea + 4u, nx);
    vm_write_f32_mm(m, ea + 8u, ny);
    vm_write_f32_mm(m, ea + 12u, nz);
}

inline void write_vec3(const ppu_mm& m, uint32_t ea, float x, float y, float z)
{
    vm_write_f32_mm(m, ea, x);
    vm_write_f32_mm(m, ea + 4u, y);
    vm_write_f32_mm(m, ea + 8u, z);
}

inline uint32_t clip_slab_axis(const ppu_mm& m, uint32_t hit_out, uint32_t p_t0, uint32_t p_t1,
                               uint32_t out_n0, uint32_t out_n1, float ax, float ay, float az,
                               float p0, float p1, float min_v, float max_v,
                               float c0, float eps, double eps_d, float one)
{
    const float dp = p1 - p0;
    const float adp = std::fabs(dp);
    if (!(adp >= eps)) {
        if (p0 < min_v && !(p0 <= min_v - eps))
            write_hit(m, hit_out, -ax, -ay, -az);
        if (!(p0 <= max_v) && p0 < max_v + eps)
            write_hit(m, hit_out, ax, ay, az);
        if (!((double)adp >= eps_d))
            return (p0 < min_v || p0 > max_v) ? 0u : 1u;
    }

    const float inv_dp = one / dp;
    const float t_a = (min_v - p0) * inv_dp;
    const float t_b = (max_v - p0) * inv_dp;
    float t_enter, t_exit, nx, ny, nz;
    if (dp > c0) {
        t_enter = t_a;
        t_exit = t_b;
        nx = -ax; ny = -ay; nz = -az;
    } else {
        t_enter = t_b;
        t_exit = t_a;
        nx = ax; ny = ay; nz = az;
    }

    if (!(t_enter <= vm_read_f32_mm(m, p_t0))) {
        vm_write_f32_mm(m, p_t0, t_enter);
        write_vec3(m, out_n0, nx, ny, nz);
    }
    float cur_t1 = vm_read_f32_mm(m, p_t1);
    if (t_exit < cur_t1) {
        vm_write_f32_mm(m, p_t1, t_exit);
        write_vec3(m, out_n1, -nx, -ny, -nz);
        cur_t1 = t_exit;
    }
    const float cur_t0 = vm_read_f32_mm(m, p_t0);
    return (cur_t1 >= cur_t0 && !(cur_t1 < c0)) ? 1u : 0u;
}

}

void func_01271980(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    const uint32_t hit_out = (uint32_t)ctx->gpr[3];
    const uint32_t box = (uint32_t)ctx->gpr[4];
    const uint32_t other = (uint32_t)ctx->gpr[5];
    const uint32_t axis = (uint32_t)ctx->gpr[6];
    const uint32_t start = (uint32_t)ctx->gpr[7];
    const uint32_t end = (uint32_t)ctx->gpr[8];
    const uint32_t p_t0 = (uint32_t)ctx->gpr[9];
    const uint32_t p_t1 = (uint32_t)ctx->gpr[10];
    const uint32_t out_n0 = vm_read32_mm(&m, sp + 0x74u);
    const uint32_t out_n1 = vm_read32_mm(&m, sp + 0x7Cu);

    const float ax = vm_read_f32_mm(m, axis);
    const float ay = vm_read_f32_mm(m, axis + 4u);
    const float az = vm_read_f32_mm(m, axis + 8u);

    const float o30 = vm_read_f32_mm(m, other + 0x30u);
    const float o34 = vm_read_f32_mm(m, other + 0x34u);
    const float o38 = vm_read_f32_mm(m, other + 0x38u);

    const float c_proj = dot3(ax, ay, az, m, other);
    const float u_proj = dot3_scaled(ax, ay, az, m, other + 0x0Cu, o30);
    const float v_proj = dot3_scaled(ax, ay, az, m, other + 0x18u, o34);
    const float w_proj = dot3_scaled(ax, ay, az, m, other + 0x24u, o38);

    const float c0 = vm_read_f32_mm(m, 0x01271968u);
    const float tbl[2] = { vm_read_f32_mm(m, 0x016A4B00u), vm_read_f32_mm(m, 0x016A4B04u) };

    float min_v = c0, max_v = c0;
    bool first = true;
    for (int i = 0; i < 2; ++i) {
        const float ti = std::fmaf(tbl[i], u_proj, c_proj);
        for (int j = 0; j < 2; ++j) {
            const float tj = std::fmaf(tbl[j], v_proj, ti);
            for (int k = 0; k < 2; ++k) {
                const float val = std::fmaf(tbl[k], w_proj, tj);
                if (first) {
                    min_v = val;
                    max_v = val;
                    first = false;
                } else {
                    min_v = ((double)val - (double)min_v >= 0.0) ? min_v : val;
                    if (val > max_v) max_v = val;
                }
            }
        }
    }

    const float rad = box_radius(ax, ay, az, m, box + 0x0Cu, box + 0x18u, box + 0x24u, box + 0x30u);
    min_v -= rad;
    max_v += rad;

    const float p0 = dot3(ax, ay, az, m, start);
    const float p1 = dot3(ax, ay, az, m, end);
    ctx->gpr[3] = clip_slab_axis(m, hit_out, p_t0, p_t1, out_n0, out_n1, ax, ay, az, p0, p1, min_v, max_v,
                                 c0, vm_read_f32_mm(m, 0x0127196Cu), vm_read_f64_mm(m, 0x01271970u),
                                 vm_read_f32_mm(m, 0x01271978u));
}

void func_01272748(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    const uint32_t hit_out = (uint32_t)ctx->gpr[3];
    const uint32_t verts_arr = (uint32_t)ctx->gpr[4];
    const uint32_t axis = (uint32_t)ctx->gpr[6];
    const uint32_t start = (uint32_t)ctx->gpr[7];
    const uint32_t end = (uint32_t)ctx->gpr[8];
    const uint32_t box_ax = (uint32_t)ctx->gpr[9];
    const uint32_t box_ay = (uint32_t)ctx->gpr[10];
    const uint32_t box_az = vm_fast_read32(m, sp + 0x74u);
    const uint32_t box_ext = vm_fast_read32(m, sp + 0x7Cu);
    const uint32_t p_t0 = vm_fast_read32(m, sp + 0x84u);
    const uint32_t p_t1 = vm_fast_read32(m, sp + 0x8Cu);
    const uint32_t out_n0 = vm_fast_read32(m, sp + 0x94u);
    const uint32_t out_n1 = vm_fast_read32(m, sp + 0x9Cu);

    const float ax = vm_read_f32_mm(m, axis);
    const float ay = vm_read_f32_mm(m, axis + 4u);
    const float az = vm_read_f32_mm(m, axis + 8u);
    const float len2 = std::fmaf(az, az, std::fmaf(ax, ax, ay * ay));
    if (!((double)len2 >= vm_read_f64_mm(m, 0x01272720u))) {
        ctx->gpr[3] = 1u;
        return;
    }

    const float rad = box_radius(ax, ay, az, m, box_ax, box_ay, box_az, box_ext);
    const float p0 = dot3(ax, ay, az, m, start);
    const float p1 = dot3(ax, ay, az, m, end);

    float min_v = vm_read_f32_mm(m, 0x01272728u);
    float max_v = vm_read_f32_mm(m, 0x0127272Cu);
    const int32_t count = (int32_t)vm_fast_read32(m, verts_arr + 4u);
    uint32_t vp = vm_fast_read32(m, verts_arr);
    if (__builtin_expect(count > 0 && vm_mm_ok(&m, vp, (uint32_t)count * 12u), 1)) {
        const uint8_t* ptr = m.vb + vp;
        for (int32_t i = 0; i < count; ++i, ptr += 12) {
            uint32_t ux, uy, uz;
            std::memcpy(&ux, ptr, 4);
            std::memcpy(&uy, ptr + 4, 4);
            std::memcpy(&uz, ptr + 8, 4);
            const float x = from_bits(__builtin_bswap32(ux));
            const float y = from_bits(__builtin_bswap32(uy));
            const float z = from_bits(__builtin_bswap32(uz));
            const float d = std::fmaf(az, z, std::fmaf(ax, x, ay * y));
            min_v = (d < min_v) ? d : min_v;
            max_v = (d > max_v) ? d : max_v;
        }
    } else {
        for (int32_t i = 0; i < count; ++i, vp += 12u) {
            const float d = dot3(ax, ay, az, m, vp);
            min_v = (d - min_v >= 0.0f) ? min_v : d;
            max_v = (d - max_v >= 0.0f) ? d : max_v;
        }
    }

    min_v -= rad;
    max_v += rad;

    ctx->gpr[3] = clip_slab_axis(m, hit_out, p_t0, p_t1, out_n0, out_n1, ax, ay, az, p0, p1, min_v, max_v,
                                 vm_read_f32_mm(m, 0x01272734u), vm_read_f32_mm(m, 0x01272730u),
                                 vm_read_f64_mm(m, 0x01272738u), vm_read_f32_mm(m, 0x01272740u));
}
