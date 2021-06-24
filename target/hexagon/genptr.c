/*
 *  Copyright(c) 2019-2021 Qualcomm Innovation Center, Inc. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "tcg/tcg-op.h"
#include "insn.h"
#include "opcodes.h"
#include "translate.h"
#define QEMU_GENERATE       /* Used internally by macros.h */
#include "macros.h"
#undef QEMU_GENERATE
#include "gen_tcg.h"
#include "genptr.h"

TCGv gen_read_reg(TCGv result, int num)
{
    tcg_gen_mov_tl(result, hex_gpr[num]);
    return result;
}

TCGv gen_read_preg(TCGv pred, uint8_t num)
{
    tcg_gen_mov_tl(pred, hex_pred[num]);
    return pred;
}

static inline void gen_log_predicated_reg_write(int rnum, TCGv val,
                                                uint32_t slot)
{
    TCGv zero = tcg_const_tl(0);
    TCGv slot_mask = tcg_temp_new();

    tcg_gen_andi_tl(slot_mask, hex_slot_cancelled, 1 << slot);
    tcg_gen_movcond_tl(TCG_COND_EQ, hex_new_value[rnum], slot_mask, zero,
                           val, hex_new_value[rnum]);
    if (HEX_DEBUG) {
        /*
         * Do this so HELPER(debug_commit_end) will know
         *
         * Note that slot_mask indicates the value is not written
         * (i.e., slot was cancelled), so we create a true/false value before
         * or'ing with hex_reg_written[rnum].
         */
        tcg_gen_setcond_tl(TCG_COND_EQ, slot_mask, slot_mask, zero);
        tcg_gen_or_tl(hex_reg_written[rnum], hex_reg_written[rnum], slot_mask);
    }

    tcg_temp_free(zero);
    tcg_temp_free(slot_mask);
}

void gen_log_reg_write(int rnum, TCGv val)
{
    tcg_gen_mov_tl(hex_new_value[rnum], val);
    if (HEX_DEBUG) {
        /* Do this so HELPER(debug_commit_end) will know */
        tcg_gen_movi_tl(hex_reg_written[rnum], 1);
    }
}

static void gen_log_predicated_reg_write_pair(int rnum, TCGv_i64 val,
                                              uint32_t slot)
{
    TCGv val32 = tcg_temp_new();
    TCGv zero = tcg_const_tl(0);
    TCGv slot_mask = tcg_temp_new();

    tcg_gen_andi_tl(slot_mask, hex_slot_cancelled, 1 << slot);
    /* Low word */
    tcg_gen_extrl_i64_i32(val32, val);
    tcg_gen_movcond_tl(TCG_COND_EQ, hex_new_value[rnum],
                       slot_mask, zero,
                       val32, hex_new_value[rnum]);
    /* High word */
    tcg_gen_extrh_i64_i32(val32, val);
    tcg_gen_movcond_tl(TCG_COND_EQ, hex_new_value[rnum + 1],
                       slot_mask, zero,
                       val32, hex_new_value[rnum + 1]);
    if (HEX_DEBUG) {
        /*
         * Do this so HELPER(debug_commit_end) will know
         *
         * Note that slot_mask indicates the value is not written
         * (i.e., slot was cancelled), so we create a true/false value before
         * or'ing with hex_reg_written[rnum].
         */
        tcg_gen_setcond_tl(TCG_COND_EQ, slot_mask, slot_mask, zero);
        tcg_gen_or_tl(hex_reg_written[rnum], hex_reg_written[rnum], slot_mask);
        tcg_gen_or_tl(hex_reg_written[rnum + 1], hex_reg_written[rnum + 1],
                      slot_mask);
    }

    tcg_temp_free(val32);
    tcg_temp_free(zero);
    tcg_temp_free(slot_mask);
}

static void gen_log_reg_write_pair(int rnum, TCGv_i64 val)
{
    /* Low word */
    tcg_gen_extrl_i64_i32(hex_new_value[rnum], val);
    if (HEX_DEBUG) {
        /* Do this so HELPER(debug_commit_end) will know */
        tcg_gen_movi_tl(hex_reg_written[rnum], 1);
    }

    /* High word */
    tcg_gen_extrh_i64_i32(hex_new_value[rnum + 1], val);
    if (HEX_DEBUG) {
        /* Do this so HELPER(debug_commit_end) will know */
        tcg_gen_movi_tl(hex_reg_written[rnum + 1], 1);
    }
}

void gen_log_pred_write(DisasContext *ctx, int pnum, TCGv val)
{
    TCGv zero = tcg_const_tl(0);
    TCGv base_val = tcg_temp_new();
    TCGv and_val = tcg_temp_new();
    TCGv pred_written = tcg_temp_new();

    tcg_gen_andi_tl(base_val, val, 0xff);

    /*
     * Section 6.1.3 of the Hexagon V67 Programmer's Reference Manual
     *
     * Multiple writes to the same preg are and'ed together
     * If this is the first predicate write in the packet, do a
     * straight assignment.  Otherwise, do an and.
     */
    if (!test_bit(pnum, ctx->pregs_written)) {
        tcg_gen_mov_tl(hex_new_pred_value[pnum], base_val);
    } else {
        tcg_gen_and_tl(hex_new_pred_value[pnum],
                       hex_new_pred_value[pnum], base_val);
    }
    tcg_gen_ori_tl(hex_pred_written, hex_pred_written, 1 << pnum);

    tcg_temp_free(zero);
    tcg_temp_free(base_val);
    tcg_temp_free(and_val);
    tcg_temp_free(pred_written);
}

static inline void gen_read_p3_0(TCGv control_reg)
{
    tcg_gen_movi_tl(control_reg, 0);
    for (int i = 0; i < NUM_PREGS; i++) {
        tcg_gen_deposit_tl(control_reg, control_reg, hex_pred[i], i * 8, 8);
    }
}

/*
 * Certain control registers require special handling on read
 *     HEX_REG_P3_0          aliased to the predicate registers
 *                           -> concat the 4 predicate registers together
 *     HEX_REG_PC            actual value stored in DisasContext
 *                           -> assign from ctx->base.pc_next
 *     HEX_REG_QEMU_*_CNT    changes in current TB in DisasContext
 *                           -> add current TB changes to existing reg value
 */
static inline void gen_read_ctrl_reg(DisasContext *ctx, const int reg_num,
                                     TCGv dest)
{
    if (reg_num == HEX_REG_P3_0) {
        gen_read_p3_0(dest);
    } else if (reg_num == HEX_REG_PC) {
        tcg_gen_movi_tl(dest, ctx->base.pc_next);
    } else if (reg_num == HEX_REG_QEMU_PKT_CNT) {
        tcg_gen_addi_tl(dest, hex_gpr[HEX_REG_QEMU_PKT_CNT],
                        ctx->num_packets);
    } else if (reg_num == HEX_REG_QEMU_INSN_CNT) {
        tcg_gen_addi_tl(dest, hex_gpr[HEX_REG_QEMU_INSN_CNT],
                        ctx->num_insns);
    } else {
        tcg_gen_mov_tl(dest, hex_gpr[reg_num]);
    }
}

static inline void gen_read_ctrl_reg_pair(DisasContext *ctx, const int reg_num,
                                          TCGv_i64 dest)
{
    if (reg_num == HEX_REG_P3_0) {
        TCGv p3_0 = tcg_temp_new();
        gen_read_p3_0(p3_0);
        tcg_gen_concat_i32_i64(dest, p3_0, hex_gpr[reg_num + 1]);
        tcg_temp_free(p3_0);
    } else if (reg_num == HEX_REG_PC - 1) {
        TCGv pc = tcg_const_tl(ctx->base.pc_next);
        tcg_gen_concat_i32_i64(dest, hex_gpr[reg_num], pc);
        tcg_temp_free(pc);
    } else if (reg_num == HEX_REG_QEMU_PKT_CNT) {
        TCGv pkt_cnt = tcg_temp_new();
        TCGv insn_cnt = tcg_temp_new();
        tcg_gen_addi_tl(pkt_cnt, hex_gpr[HEX_REG_QEMU_PKT_CNT],
                        ctx->num_packets);
        tcg_gen_addi_tl(insn_cnt, hex_gpr[HEX_REG_QEMU_INSN_CNT],
                        ctx->num_insns);
        tcg_gen_concat_i32_i64(dest, pkt_cnt, insn_cnt);
        tcg_temp_free(pkt_cnt);
        tcg_temp_free(insn_cnt);
    } else {
        tcg_gen_concat_i32_i64(dest,
            hex_gpr[reg_num],
            hex_gpr[reg_num + 1]);
    }
}

static inline void gen_write_p3_0(TCGv control_reg)
{
    for (int i = 0; i < NUM_PREGS; i++) {
        tcg_gen_extract_tl(hex_pred[i], control_reg, i * 8, 8);
    }
}

/*
 * Certain control registers require special handling on write
 *     HEX_REG_P3_0          aliased to the predicate registers
 *                           -> break the value across 4 predicate registers
 *     HEX_REG_QEMU_*_CNT    changes in current TB in DisasContext
 *                            -> clear the changes
 */
static inline void gen_write_ctrl_reg(DisasContext *ctx, int reg_num,
                                      TCGv val)
{
    if (reg_num == HEX_REG_P3_0) {
        gen_write_p3_0(val);
    } else {
        gen_log_reg_write(reg_num, val);
        ctx_log_reg_write(ctx, reg_num);
        if (reg_num == HEX_REG_QEMU_PKT_CNT) {
            ctx->num_packets = 0;
        }
        if (reg_num == HEX_REG_QEMU_INSN_CNT) {
            ctx->num_insns = 0;
        }
    }
}

static inline void gen_write_ctrl_reg_pair(DisasContext *ctx, int reg_num,
                                           TCGv_i64 val)
{
    if (reg_num == HEX_REG_P3_0) {
        TCGv val32 = tcg_temp_new();
        tcg_gen_extrl_i64_i32(val32, val);
        gen_write_p3_0(val32);
        tcg_gen_extrh_i64_i32(val32, val);
        gen_log_reg_write(reg_num + 1, val32);
        tcg_temp_free(val32);
        ctx_log_reg_write(ctx, reg_num + 1);
    } else {
        gen_log_reg_write_pair(reg_num, val);
        ctx_log_reg_write_pair(ctx, reg_num);
        if (reg_num == HEX_REG_QEMU_PKT_CNT) {
            ctx->num_packets = 0;
            ctx->num_insns = 0;
        }
    }
}

static inline void gen_load_locked4u(TCGv dest, TCGv vaddr, int mem_index)
{
    tcg_gen_qemu_ld32u(dest, vaddr, mem_index);
    tcg_gen_mov_tl(hex_llsc_addr, vaddr);
    tcg_gen_mov_tl(hex_llsc_val, dest);
}

static inline void gen_load_locked8u(TCGv_i64 dest, TCGv vaddr, int mem_index)
{
    tcg_gen_qemu_ld64(dest, vaddr, mem_index);
    tcg_gen_mov_tl(hex_llsc_addr, vaddr);
    tcg_gen_mov_i64(hex_llsc_val_i64, dest);
}

static inline void gen_store_conditional4(CPUHexagonState *env,
                                          DisasContext *ctx, int prednum,
                                          TCGv pred, TCGv vaddr, TCGv src)
{
    TCGLabel *fail = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv one, zero, tmp;

    tcg_gen_brcond_tl(TCG_COND_NE, vaddr, hex_llsc_addr, fail);

    one = tcg_const_tl(0xff);
    zero = tcg_const_tl(0);
    tmp = tcg_temp_new();
    tcg_gen_atomic_cmpxchg_tl(tmp, hex_llsc_addr, hex_llsc_val, src,
                              ctx->mem_idx, MO_32);
    tcg_gen_movcond_tl(TCG_COND_EQ, hex_pred[prednum], tmp, hex_llsc_val,
                       one, zero);
    tcg_temp_free(one);
    tcg_temp_free(zero);
    tcg_temp_free(tmp);
    tcg_gen_br(done);

    gen_set_label(fail);
    tcg_gen_movi_tl(pred, 0);

    gen_set_label(done);
    tcg_gen_movi_tl(hex_llsc_addr, ~0);
}

static inline void gen_store_conditional8(CPUHexagonState *env,
                                          DisasContext *ctx, int prednum,
                                          TCGv pred, TCGv vaddr, TCGv_i64 src)
{
    TCGLabel *fail = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i64 one, zero, tmp;

    tcg_gen_brcond_tl(TCG_COND_NE, vaddr, hex_llsc_addr, fail);

    one = tcg_const_i64(0xff);
    zero = tcg_const_i64(0);
    tmp = tcg_temp_new_i64();
    tcg_gen_atomic_cmpxchg_i64(tmp, hex_llsc_addr, hex_llsc_val_i64, src,
                               ctx->mem_idx, MO_64);
    tcg_gen_movcond_i64(TCG_COND_EQ, tmp, tmp, hex_llsc_val_i64,
                        one, zero);
    tcg_gen_extrl_i64_i32(hex_pred[prednum], tmp);
    tcg_temp_free_i64(one);
    tcg_temp_free_i64(zero);
    tcg_temp_free_i64(tmp);
    tcg_gen_br(done);

    gen_set_label(fail);
    tcg_gen_movi_tl(pred, 0);

    gen_set_label(done);
    tcg_gen_movi_tl(hex_llsc_addr, ~0);
}

void gen_store32(DisasContext *ctx, TCGv vaddr, TCGv src, tcg_target_long width,
                 uint32_t slot)
{
    tcg_gen_mov_tl(hex_store_addr[slot], vaddr);
    tcg_gen_movi_tl(hex_store_width[slot], width);
    tcg_gen_mov_tl(hex_store_val32[slot], src);
    ctx->store_width[slot] = width;
}

void gen_store1(TCGv_env cpu_env, TCGv vaddr, TCGv src, DisasContext *ctx,
                uint32_t slot)
{
    gen_store32(ctx, vaddr, src, 1, slot);
}

static inline void gen_store1i(TCGv_env cpu_env, TCGv vaddr, int32_t src,
                               DisasContext *ctx, uint32_t slot)
{
    TCGv tmp = tcg_const_tl(src);
    gen_store1(cpu_env, vaddr, tmp, ctx, slot);
    tcg_temp_free(tmp);
}

void gen_store2(TCGv_env cpu_env, TCGv vaddr, TCGv src, DisasContext *ctx,
                uint32_t slot)
{
    gen_store32(ctx, vaddr, src, 2, slot);
}

static inline void gen_store2i(TCGv_env cpu_env, TCGv vaddr, int32_t src,
                               DisasContext *ctx, uint32_t slot)
{
    TCGv tmp = tcg_const_tl(src);
    gen_store2(cpu_env, vaddr, tmp, ctx, slot);
    tcg_temp_free(tmp);
}

void gen_store4(TCGv_env cpu_env, TCGv vaddr, TCGv src, DisasContext *ctx,
                uint32_t slot)
{
    gen_store32(ctx, vaddr, src, 4, slot);
}

static inline void gen_store4i(TCGv_env cpu_env, TCGv vaddr, int32_t src,
                               DisasContext *ctx, uint32_t slot)
{
    TCGv tmp = tcg_const_tl(src);
    gen_store4(cpu_env, vaddr, tmp, ctx, slot);
    tcg_temp_free(tmp);
}

void gen_store8(TCGv_env cpu_env, TCGv vaddr, TCGv_i64 src, DisasContext *ctx,
                uint32_t slot)
{
    tcg_gen_mov_tl(hex_store_addr[slot], vaddr);
    tcg_gen_movi_tl(hex_store_width[slot], 8);
    tcg_gen_mov_i64(hex_store_val64[slot], src);
    ctx->store_width[slot] = 8;
}

static inline void gen_store8i(TCGv_env cpu_env, TCGv vaddr, int64_t src,
                               DisasContext *ctx, uint32_t slot)
{
    TCGv_i64 tmp = tcg_const_i64(src);
    gen_store8(cpu_env, vaddr, tmp, ctx, slot);
    tcg_temp_free_i64(tmp);
}

void gen_set_usr_field(int field, TCGv val)
{
    tcg_gen_deposit_tl(hex_gpr[HEX_REG_USR], hex_gpr[HEX_REG_USR], val,
                       reg_field_info[field].offset,
                       reg_field_info[field].width);
}

void gen_set_usr_fieldi(int field, int x)
{
    TCGv val = tcg_const_tl(x);
    gen_set_usr_field(field, val);
    tcg_temp_free(val);
}

void gen_write_new_pc(TCGv addr)
{
    /* If there are multiple branches in a packet, ignore the second one */
    TCGv zero = tcg_const_tl(0);
    tcg_gen_movcond_tl(TCG_COND_NE, hex_next_PC, hex_branch_taken, zero,
                       hex_next_PC, addr);
    tcg_gen_movi_tl(hex_branch_taken, 1);
    tcg_temp_free(zero);
}

void gen_sat_i32(TCGv dest, TCGv source, int width)
{
    TCGv max_val = tcg_const_tl((1 << (width - 1)) - 1);
    TCGv min_val = tcg_const_tl(-(1 << (width - 1)));
    tcg_gen_smin_tl(dest, source, max_val);
    tcg_gen_smax_tl(dest, dest, min_val);
    tcg_temp_free(max_val);
    tcg_temp_free(min_val);
}

void gen_sat_i32_ovfl(TCGv ovfl, TCGv dest, TCGv source, int width)
{
    gen_sat_i32(dest, source, width);
    tcg_gen_setcond_tl(TCG_COND_NE, ovfl, source, dest);
}

void gen_satu_i32(TCGv dest, TCGv source, int width)
{
    TCGv max_val = tcg_const_tl((1 << width) - 1);
    tcg_gen_movcond_tl(TCG_COND_GTU, dest, source, max_val, max_val, source);
    TCGv zero = tcg_const_tl(0);
    tcg_gen_movcond_tl(TCG_COND_LT, dest, source, zero, zero, dest);
    tcg_temp_free(max_val);
    tcg_temp_free(zero);
}

void gen_satu_i32_ovfl(TCGv ovfl, TCGv dest, TCGv source, int width)
{
    gen_satu_i32(dest, source, width);
    tcg_gen_setcond_tl(TCG_COND_NE, ovfl, source, dest);
}

void gen_sat_i64(TCGv_i64 dest, TCGv_i64 source, int width)
{
    TCGv_i64 max_val = tcg_const_i64((1 << (width - 1)) - 1);
    TCGv_i64 min_val = tcg_const_i64(-(1 << (width - 1)));
    tcg_gen_smin_i64(dest, source, max_val);
    tcg_gen_smax_i64(dest, dest, min_val);
    tcg_temp_free_i64(max_val);
    tcg_temp_free_i64(min_val);
}

void gen_sat_i64_ovfl(TCGv ovfl, TCGv_i64 dest, TCGv_i64 source, int width)
{
    gen_sat_i64(dest, source, width);
    TCGv_i64 ovfl_64 = tcg_temp_new_i64();
    tcg_gen_setcond_i64(TCG_COND_NE, ovfl_64, dest, source);
    tcg_gen_trunc_i64_tl(ovfl, ovfl_64);
    tcg_temp_free_i64(ovfl_64);
}

void gen_satu_i64(TCGv_i64 dest, TCGv_i64 source, int width)
{
    TCGv_i64 max_val = tcg_const_i64((1 << width) - 1);
    tcg_gen_movcond_i64(TCG_COND_GTU, dest, source, max_val, max_val, source);
    TCGv_i64 zero = tcg_const_i64(0);
    tcg_gen_movcond_i64(TCG_COND_LT, dest, source, zero, zero, dest);
    tcg_temp_free_i64(max_val);
    tcg_temp_free_i64(zero);
}

void gen_satu_i64_ovfl(TCGv ovfl, TCGv_i64 dest, TCGv_i64 source, int width)
{
    gen_sat_i64(dest, source, width);
    TCGv_i64 ovfl_64 = tcg_temp_new_i64();
    tcg_gen_setcond_i64(TCG_COND_NE, ovfl_64, dest, source);
    tcg_gen_trunc_i64_tl(ovfl, ovfl_64);
    tcg_temp_free_i64(ovfl_64);
}

#include "tcg_funcs_generated.c.inc"
#include "tcg_func_table_generated.c.inc"
