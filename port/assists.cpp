#include "assists.h"
#include "guest.h"
#include "ue3_reflect.h"
#include "rl_achievements.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace {

using ue3::Elements;
using ue3::Member;
using ue3::elements;
using ue3::hook_bit;
using ue3::k_object_class;

constexpr uint32_t k_delta_seconds = 0x019B8690;

constexpr uint8_t k_start_command = 5;
constexpr uint8_t k_cmd_hitting_repeat = 1;
constexpr uint8_t k_cmd_select = 5;
constexpr uint8_t k_judge_none = 0;
constexpr uint8_t k_judge_success = 1;
constexpr uint8_t k_sync_normal = 0;
constexpr uint8_t k_sync_barrage = 1;
constexpr uint8_t k_sync_good = 1;
constexpr uint8_t k_sync_great = 2;
constexpr uint8_t k_sync_excellent = 3;
constexpr uint8_t k_story_open_flg = 1;
constexpr uint8_t k_collection_get_flg = 1;
constexpr int32_t k_max_damage = 999999;

enum HookId : uint32_t {
    hk_pc_tick = 1,
    hk_player_tick,
    hk_set_mantra_val,
    hk_set_unlim_val,
    hk_player_take_damage,
    hk_pawn_base_dmg,
    hk_enemy_base_dmg,
    hk_judge_update,
    hk_stick_update,
    hk_sync_normal,
    hk_sync_barrage,
    hk_sync_is_input,
    hk_sync_get_result,
    hk_anim_rep_pushed,
    hk_seq_fade_rep_update,
    hk_save_is_opened,
    hk_save_is_open_mission,
    hk_save_is_story_flg,
    hk_save_is_gauge_flg,
    hk_save_is_extra_flg,
};

constexpr std::pair<const char*, uint32_t> k_hooks[] = {
    { "BCGame.BCPlayerController.PlayerTick",                hk_pc_tick },
    { "BCGame.BCPawn_Player.Tick",                           hk_player_tick },
    { "BCGame.BCPawn_Player.SetMantraGaugeVal",              hk_set_mantra_val },
    { "BCGame.BCPawn_Player.SetUnlimitedModeGaugeValue",     hk_set_unlim_val },
    { "BCGame.BCPawn_Player.TakeDamage",                     hk_player_take_damage },
    { "BCGame.BCPawn.TakeDamageGetBaseDamageAmount",         hk_pawn_base_dmg },
    { "BCGame.BCPawn_Enemy.TakeDamageGetBaseDamageAmount",   hk_enemy_base_dmg },
    { "BCGame.BCInputJudgeSystem.Update",                    hk_judge_update },
    { "BCGame.BCInputJudgeSystem_StickRotation.Update",      hk_stick_update },
    { "BCGame.BCInterpTrackSynchronic.UpdateNormal",         hk_sync_normal },
    { "BCGame.BCInterpTrackSynchronic.UpdateBarrage",        hk_sync_barrage },
    { "BCGame.BCInterpTrackSynchronic.IsInputButton",        hk_sync_is_input },
    { "BCGame.BCInterpTrackSynchronic.GetResult",            hk_sync_get_result },
    { "BCGame.BCAnim_CrossFadeButtonRep.IsPushedButton",     hk_anim_rep_pushed },
    { "BCGame.BCAnim_FadeByButtonRep.IsPushedButton",        hk_anim_rep_pushed },
    { "BCGame.BCAnim_FadeRateByButtonRep.IsPushedButton",    hk_anim_rep_pushed },
    { "BCGame.BCSeqAct_AnimFadeByButtonRep.Update",          hk_seq_fade_rep_update },
    { "BCGame.BCSaveParam.IsOpened",                         hk_save_is_opened },
    { "BCGame.BCSaveParam.IsOpenMission",                    hk_save_is_open_mission },
    { "BCGame.BCSaveParam.IsStoryFlg",                       hk_save_is_story_flg },
    { "BCGame.BCSaveParam.IsGaugeFlg",                       hk_save_is_gauge_flg },
    { "BCGame.BCSaveParam.IsCGArtFlg",                       hk_save_is_extra_flg },
    { "BCGame.BCSaveParam.IsArtIllustFlg",                   hk_save_is_extra_flg },
    { "BCGame.BCSaveParam.IsMovieFlg",                       hk_save_is_extra_flg },
};

constexpr uint32_t k_dirty_pc_bit = 1u << 31;

std::atomic<uint32_t> g_assists_flags{ 0 };
std::atomic<uint64_t> g_hooks[std::size(k_hooks)]{};
std::atomic<uint32_t> g_damage_mult{ 1 };
std::atomic<uint32_t> g_qte_timer_pct{ 100 };
std::atomic<uint32_t> g_player_pawn{ 0 };
std::atomic<uint32_t> g_player_input{ 0 };

thread_local uint32_t t_sync_obj = 0;
thread_local float t_sync_now = 0.f;
thread_local int32_t t_sync_idx = -1;
thread_local uint8_t t_sync_type = 0;
thread_local uint64_t t_last_mash_ms = 0;

Member m_god_mode{ "bPlayerGodMode" }, m_always_hp_max{ "bAlwaysHealthMax" }, m_enemy_blow_death{ "bEnemyBlowDeath" },
    m_disable_unlim_end{ "bDisableUnlimModeForceEnd" }, m_player_input{ "PlayerInput" },
    m_mantra_val{ "MantraGaugeVal" }, m_mantra_max{ "MantraGaugeValMax" }, m_lval_parm{ "lVal" },
    m_unlim_val{ "UnlimitedModeGaugeVal" }, m_unlim_max{ "UnlimitedModeGaugeValMax" }, m_in_value_parm{ "InValue" },
    m_dmg_parm{ "Dmg" }, m_dt_parm{ "DeltaTime" }, m_flg_parm{ "Flg" }, m_clear_flg_parm{ "ClearFlg" },
    m_now_time_parm{ "NowTime" }, m_idx_parm{ "Idx" },
    m_judge_info{ "JudgeInfo" }, m_request{ "Request" }, m_input_setting{ "InputSetting" }, m_cmd_type{ "CommandType" },
    m_buttons{ "Buttons" }, m_input_info{ "InputInfo" }, m_enable_judge{ "bEnableJudge" }, m_paused{ "bPause" },
    m_shown{ "bShow" }, m_succeeded{ "bSuccess" }, m_success_num{ "SuccessNum" }, m_result{ "Result" },
    m_btn_input_list{ "ButtonInputList" }, m_btn_active{ "Active" }, m_btn_pressed{ "Pressed" },
    m_stick_judging{ "bJudge" }, m_stick_auto{ "bAutoJudge" }, m_stick_input{ "bStickInput" }, m_stick_success{ "bSuccess" },
    m_sync_tracks{ "Tracks" }, m_sync_active_idx{ "ActiveTrackIndex" }, m_sync_enable{ "bEnableInput" },
    m_sync_start{ "StartTime" }, m_sync_type{ "Type" },
    m_sync_rank_a{ "RankTimeA" }, m_sync_rank_b{ "RankTimeB" }, m_sync_rank_c{ "RankTimeC" },
    m_anim_last_push{ "bLastFramePushedButton" }, m_seq_time_cnt{ "TimeCount" }, m_seq_time_lim{ "TimeLimit" };

uint64_t now_ms()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool mash_pulse_ready()
{
    const uint64_t t = now_ms();
    if (t - t_last_mash_ms < 75u) return false;
    t_last_mash_ms = t;
    return true;
}

void pulse_held_buttons(uint32_t pi)
{
    if (!pi) return;
    const Elements list = elements(m_btn_input_list, pi, vm_read32(pi + k_object_class));
    for (uint32_t i = 0; i < list.count; ++i) {
        const uint32_t item = list.data + i * list.stride;
        if (m_btn_active.sflag(item, list.type))
            m_btn_pressed.set_sflag(item, list.type, true);
    }
}

void scale_qte_dt(uint32_t flags, uint32_t locals, uint32_t function)
{
    const uint32_t pct = g_qte_timer_pct.load(std::memory_order_relaxed);
    if ((flags & ASSIST_EXT_QTE_TIMER) && pct > 100u && locals) {
        if (const uint32_t a = m_dt_parm.at(locals, function))
            vm_write32(a, to_bits(from_bits(vm_read32(a)) * (100.f / (float)pct)));
    }
}

uint8_t judge_cmd_type(uint32_t req, uint32_t req_type)
{
    uint32_t set_type = 0;
    const uint32_t setting = m_input_setting.at(req, req_type, &set_type);
    const uint32_t cmd_ea = setting ? m_cmd_type.at(setting, set_type) : 0;
    return cmd_ea ? vm_read8(cmd_ea) : 0;
}

uint32_t find_hook(uint32_t function)
{
    for (const std::atomic<uint64_t>& slot : g_hooks) {
        const uint64_t h = slot.load(std::memory_order_acquire);
        if ((uint32_t)(h >> 32) == function) return (uint32_t)h;
    }
    return 0;
}

constexpr uint32_t k_any_assist_mask = 0x7FFu;
std::atomic<bool> g_casual_reported{ false };

void report_casual_if_needed()
{
    if ((g_assists_flags.load(std::memory_order_relaxed) & k_any_assist_mask) != 0 &&
        rl_ach_connected() && !g_casual_reported.exchange(true, std::memory_order_relaxed)) {
        rl_ach_casual("cheats");
    }
}

}

std::atomic<uint64_t> g_assists_hook_mask{ 0 };

void assists_sync_casual()
{
    report_casual_if_needed();
}

void assists_set_flag(AssistFlag flag, bool on)
{
    if (on) {
        g_assists_flags.fetch_or((uint32_t)flag, std::memory_order_relaxed);
        report_casual_if_needed();
    } else {
        g_assists_flags.fetch_and(~(uint32_t)flag, std::memory_order_relaxed);
        if (flag == ASSIST_GOD_MODE || flag == ASSIST_INF_UNLIMITED || flag == ASSIST_ONE_HIT_KILL)
            g_assists_flags.fetch_or(k_dirty_pc_bit, std::memory_order_relaxed);
    }
}

void assists_set_damage_mult(uint32_t mult)
{
    g_damage_mult.store(mult ? mult : 1u, std::memory_order_relaxed);
    assists_set_flag(ASSIST_DAMAGE_MULT, mult > 1u);
}

void assists_set_qte_timer_pct(uint32_t pct)
{
    g_qte_timer_pct.store(pct ? pct : 100u, std::memory_order_relaxed);
    assists_set_flag(ASSIST_EXT_QTE_TIMER, pct > 100u);
}

void assists_link(uint32_t function, const std::string& path)
{
    for (size_t i = 0; i < std::size(k_hooks); ++i) {
        if (path == k_hooks[i].first) {
            g_hooks[i].store((uint64_t)function << 32 | k_hooks[i].second, std::memory_order_release);
            g_assists_hook_mask.fetch_or(hook_bit(function), std::memory_order_release);
        }
    }
}

uint32_t assists_pre_slow(uint32_t self, uint32_t function, uint32_t locals, uint32_t result)
{
    const uint32_t hk = find_hook(function);
    if (!hk) return 0;
    const uint32_t flags = g_assists_flags.load(std::memory_order_relaxed);

    switch (hk) {
    case hk_pc_tick: {
        const uint32_t pi = m_player_input.obj(self);
        if (pi) g_player_input.store(pi, std::memory_order_relaxed);
        const bool god = (flags & ASSIST_GOD_MODE) != 0;
        const bool unlim = (flags & ASSIST_INF_UNLIMITED) != 0;
        const bool ohk = (flags & ASSIST_ONE_HIT_KILL) != 0;
        if (god || unlim || ohk || (flags & k_dirty_pc_bit)) {
            m_god_mode.set_flag(self, god);
            m_always_hp_max.set_flag(self, god);
            m_disable_unlim_end.set_flag(self, unlim);
            m_enemy_blow_death.set_flag(self, ohk);
            if (flags & k_dirty_pc_bit)
                g_assists_flags.fetch_and(~k_dirty_pc_bit, std::memory_order_relaxed);
        }
        return 0;
    }

    case hk_player_tick: {
        g_player_pawn.store(self, std::memory_order_relaxed);
        return 0;
    }

    case hk_set_mantra_val: {
        if ((flags & ASSIST_INF_BURST) && locals) {
            const float mx = m_mantra_max.f32(self);
            const uint32_t a = m_lval_parm.at(locals, function);
            if (mx > 0.f && a && from_bits(vm_read32(a)) > m_mantra_val.f32(self))
                vm_write32(a, to_bits(mx));
        }
        return 0;
    }

    case hk_set_unlim_val: {
        if ((flags & ASSIST_INF_UNLIMITED) && locals) {
            const float mx = m_unlim_max.f32(self);
            const uint32_t a = m_in_value_parm.at(locals, function);
            if (mx > 0.f && a && from_bits(vm_read32(a)) > m_unlim_val.f32(self))
                vm_write32(a, to_bits(mx));
        }
        return 0;
    }

    case hk_player_take_damage: {
        if ((flags & ASSIST_GOD_MODE) && locals) {
            if (const uint32_t a = m_dmg_parm.at(locals, function))
                vm_write32(a, 0u);
        }
        return 0;
    }

    case hk_pawn_base_dmg: {
        if (!(flags & (ASSIST_ONE_HIT_KILL | ASSIST_DAMAGE_MULT))) return 0;
        if (self == g_player_pawn.load(std::memory_order_relaxed)) return 0;
        return (hk << 4) | ASSIST_ACT_POST;
    }

    case hk_enemy_base_dmg: {
        if (!(flags & (ASSIST_ONE_HIT_KILL | ASSIST_DAMAGE_MULT))) return 0;
        return (hk << 4) | ASSIST_ACT_POST;
    }

    case hk_judge_update: {
        scale_qte_dt(flags, locals, function);
        if (flags & (ASSIST_AUTO_QTE | ASSIST_HOLD_MASH)) {
            const uint32_t cls = vm_read32(self + k_object_class);
            const Elements judges = elements(m_judge_info, self, cls);
            bool need_mash_pulse = false;
            for (uint32_t j = 0; j < judges.count; ++j) {
                const uint32_t judge = judges.data + j * judges.stride;
                if (!m_enable_judge.sflag(judge, judges.type) || m_paused.sflag(judge, judges.type)) continue;
                const uint32_t res_ea = m_result.at(judge, judges.type);
                if (!res_ea || vm_read8(res_ea) != k_judge_none) continue;
                uint32_t req_type = 0;
                const uint32_t req = m_request.at(judge, judges.type, &req_type);
                if (!req) continue;
                const Elements btns = elements(m_buttons, req, req_type);
                if (!btns.count) continue;
                if (vm_read8(btns.data) == k_start_command) continue;

                const uint8_t cmd_type = judge_cmd_type(req, req_type);
                if (flags & ASSIST_AUTO_QTE) {
                    const Elements infos = elements(m_input_info, judge, judges.type);
                    if (cmd_type == k_cmd_select && infos.count > 0) {
                        for (uint32_t b = 0; b < infos.count; ++b) {
                            m_shown.set_sflag(infos.data + b * infos.stride, infos.type, false);
                            m_succeeded.set_sflag(infos.data + b * infos.stride, infos.type, b == 0);
                        }
                        if (const uint32_t sn = m_success_num.at(judge, judges.type))
                            vm_write32(sn, 1u);
                    } else {
                        for (uint32_t b = 0; b < infos.count; ++b) {
                            m_shown.set_sflag(infos.data + b * infos.stride, infos.type, false);
                            m_succeeded.set_sflag(infos.data + b * infos.stride, infos.type, true);
                        }
                        if (const uint32_t sn = m_success_num.at(judge, judges.type))
                            vm_write32(sn, btns.count);
                    }
                    vm_write8(res_ea, k_judge_success);
                    m_enable_judge.set_sflag(judge, judges.type, false);
                } else if ((flags & ASSIST_HOLD_MASH) && cmd_type == k_cmd_hitting_repeat) {
                    need_mash_pulse = true;
                }
            }
            if (need_mash_pulse && mash_pulse_ready()) {
                uint32_t pi = m_player_input.obj(self);
                if (!pi) pi = g_player_input.load(std::memory_order_relaxed);
                pulse_held_buttons(pi);
            }
        }
        return 0;
    }

    case hk_stick_update: {
        scale_qte_dt(flags, locals, function);
        if ((flags & ASSIST_AUTO_QTE) && m_stick_judging.flag(self)) {
            m_stick_auto.set_flag(self, true);
            m_stick_input.set_flag(self, true);
            m_stick_success.set_flag(self, true);
            if (result) vm_write32(result, 1u);
            return (hk << 4) | ASSIST_ACT_SKIP;
        }
        return 0;
    }

    case hk_sync_normal:
    case hk_sync_barrage: {
        t_sync_obj = self;
        t_sync_type = (hk == hk_sync_barrage) ? k_sync_barrage : k_sync_normal;
        t_sync_idx = -1;
        if (locals) {
            if (const uint32_t a = m_idx_parm.at(locals, function))
                t_sync_idx = (int32_t)vm_read32(a);
            if (const uint32_t a = m_now_time_parm.at(locals, function))
                t_sync_now = from_bits(vm_read32(a));
        }
        if (hk == hk_sync_barrage && (flags & ASSIST_HOLD_MASH) && !(flags & ASSIST_AUTO_QTE) && mash_pulse_ready())
            pulse_held_buttons(g_player_input.load(std::memory_order_relaxed));
        if (hk == hk_sync_normal && (flags & ASSIST_EXT_QTE_TIMER)) {
            const float scale = (float)g_qte_timer_pct.load(std::memory_order_relaxed) / 100.f;
            if (scale > 1.f) {
                const float ra = m_sync_rank_a.f32(self);
                const float rb = m_sync_rank_b.f32(self);
                const float rc = m_sync_rank_c.f32(self);
                if (ra > 0.f && ra < 0.8f) m_sync_rank_a.set_f32(self, std::min(ra * scale, 0.95f));
                if (rb > 0.f && rb < 0.9f) m_sync_rank_b.set_f32(self, std::min(rb * scale, 0.98f));
                if (rc > 0.f && rc < 0.95f) m_sync_rank_c.set_f32(self, std::min(rc * scale, 0.99f));
            }
        }
        return 0;
    }

    case hk_sync_is_input: {
        if (!(flags & ASSIST_AUTO_QTE)) return 0;
        if (!m_sync_enable.flag(self)) return 0;
        if (t_sync_obj == self && t_sync_type == k_sync_barrage) {
            if (result) vm_write32(result, 1u);
            return (hk << 4) | ASSIST_ACT_SKIP;
        }
        const int32_t idx = (t_sync_obj == self && t_sync_idx >= 0) ? t_sync_idx : m_sync_active_idx.i32(self);
        const Elements tracks = elements(m_sync_tracks, self, vm_read32(self + k_object_class));
        if (idx >= 0 && (uint32_t)idx < tracks.count) {
            const uint32_t trk = tracks.data + (uint32_t)idx * tracks.stride;
            const uint32_t st_ea = m_sync_start.at(trk, tracks.type);
            const uint32_t ty_ea = m_sync_type.at(trk, tracks.type);
            const uint8_t ty = ty_ea ? vm_read8(ty_ea) : k_sync_normal;
            if (ty == k_sync_barrage) {
                if (result) vm_write32(result, 1u);
                return (hk << 4) | ASSIST_ACT_SKIP;
            }
            const float st = st_ea ? from_bits(vm_read32(st_ea)) : 0.f;
            if (t_sync_obj != self || t_sync_now >= st) {
                m_sync_enable.set_flag(self, false);
                if (result) vm_write32(result, 1u);
                return (hk << 4) | ASSIST_ACT_SKIP;
            }
        }
        return 0;
    }

    case hk_sync_get_result: {
        if (flags & ASSIST_AUTO_QTE)
            return (hk << 4) | ASSIST_ACT_POST;
        return 0;
    }

    case hk_anim_rep_pushed: {
        if (flags & ASSIST_AUTO_QTE) {
            m_anim_last_push.set_flag(self, false);
            if (result) vm_write32(result, 1u);
            return (hk << 4) | ASSIST_ACT_SKIP;
        }
        if ((flags & ASSIST_HOLD_MASH) && mash_pulse_ready())
            m_anim_last_push.set_flag(self, false);
        return 0;
    }

    case hk_seq_fade_rep_update: {
        const uint32_t pct = g_qte_timer_pct.load(std::memory_order_relaxed);
        if ((flags & ASSIST_EXT_QTE_TIMER) && pct > 100u)
            return (hk << 4) | ASSIST_ACT_POST;
        return 0;
    }

    case hk_save_is_opened:
    case hk_save_is_open_mission: {
        if (flags & ASSIST_UNLOCK_EPISODES) {
            if (result) vm_write32(result, 1u);
            return (hk << 4) | ASSIST_ACT_SKIP;
        }
        return 0;
    }

    case hk_save_is_story_flg: {
        if ((flags & ASSIST_UNLOCK_EPISODES) && locals) {
            const uint32_t a = m_clear_flg_parm.at(locals, function);
            if (a && vm_read8(a) == k_story_open_flg) {
                if (result) vm_write32(result, 1u);
                return (hk << 4) | ASSIST_ACT_SKIP;
            }
        }
        return 0;
    }

    case hk_save_is_gauge_flg:
    case hk_save_is_extra_flg: {
        const uint32_t need = (hk == hk_save_is_gauge_flg) ? ASSIST_UNLOCK_GAUGES : ASSIST_UNLOCK_EXTRAS;
        if ((flags & need) && locals) {
            const uint32_t a = m_flg_parm.at(locals, function);
            if (a && vm_read8(a) == k_collection_get_flg) {
                if (result) vm_write32(result, 1u);
                return (hk << 4) | ASSIST_ACT_SKIP;
            }
        }
        return 0;
    }
    }
    return 0;
}

void assists_post_slow(uint32_t token, uint32_t self, uint32_t function, uint32_t locals, uint32_t result)
{
    (void)function;
    (void)locals;
    const uint32_t hk = token >> 4;
    const uint32_t flags = g_assists_flags.load(std::memory_order_relaxed);

    switch (hk) {
    case hk_pawn_base_dmg:
    case hk_enemy_base_dmg: {
        if (!result) break;
        const int32_t dmg = (int32_t)vm_read32(result);
        if (dmg <= 0) break;
        if (flags & ASSIST_ONE_HIT_KILL) {
            vm_write32(result, (uint32_t)k_max_damage);
        } else if (flags & ASSIST_DAMAGE_MULT) {
            const uint32_t mult = g_damage_mult.load(std::memory_order_relaxed);
            const int64_t scaled = (int64_t)dmg * (int64_t)mult;
            vm_write32(result, (uint32_t)(scaled > k_max_damage ? k_max_damage : scaled));
        }
        break;
    }

    case hk_sync_get_result: {
        if (!result) break;
        const uint8_t r = vm_read8(result);
        if (r == k_sync_good || r == k_sync_great || r == 0)
            vm_write8(result, k_sync_excellent);
        break;
    }

    case hk_seq_fade_rep_update: {
        const uint32_t pct = g_qte_timer_pct.load(std::memory_order_relaxed);
        const float lim = m_seq_time_lim.f32(self);
        if (pct > 100u && lim > 0.f) {
            const float cur = m_seq_time_cnt.f32(self);
            const float dt = (float)from_bits64(vm_read64(k_delta_seconds));
            const float refund = dt * (1.f - 100.f / (float)pct);
            if (dt > 0.f && cur > refund) m_seq_time_cnt.set_f32(self, cur - refund);
        }
        break;
    }
    }
}
