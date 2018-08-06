#include "dr_api.h"
#include "drmgr.h"
#include "drreg.h"
#include "drutil.h"

#include "umbra.h"
#include "drsyscall.h"
#include "drtaint.h"
#include "drtaint_shadow.h"
#include "drtaint_helper.h"

static dr_emit_flags_t
event_app_instruction(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where,
                      bool for_trace, bool translating, void *user_data);

static bool
event_pre_syscall(void *drcontext, int sysnum);

static void
event_post_syscall(void *drcontext, int sysnum);

static bool
instr_affects_on_flags(int opcode);

static bool
instr_predicate_is_true(instr_t *where, uint flags);

static void
what_are_srcs(instr_t *where);

static void
what_are_dsts(instr_t *where);

extern bool
instr_is_simd(instr_t *where);

static int drtaint_init_count;

static client_id_t client_id;

bool drtaint_init(client_id_t id)
{
    drreg_options_t drreg_ops = {sizeof(drreg_ops), 4, false};
    drsys_options_t drsys_ops = {sizeof(drsys_ops), 0};
    drmgr_priority_t pri = {sizeof(pri),
                            DRMGR_PRIORITY_NAME_DRTAINT, NULL, NULL,
                            DRMGR_PRIORITY_INSERT_DRTAINT};

    int count = dr_atomic_add32_return_sum(&drtaint_init_count, 1);
    if (count > 1)
        return true;

    client_id = id;
    drmgr_init();
    if (!drtaint_shadow_init(id) ||
        drreg_init(&drreg_ops) != DRREG_SUCCESS ||
        drsys_init(id, &drsys_ops) != DRMF_SUCCESS)
        return false;
    drsys_filter_all_syscalls();
    if (!drmgr_register_bb_instrumentation_event(NULL,
                                                 event_app_instruction,
                                                 &pri) ||
        !drmgr_register_pre_syscall_event(event_pre_syscall) ||
        !drmgr_register_post_syscall_event(event_post_syscall))
        return false;

    return true;
}

void drtaint_exit(void)
{
    int count = dr_atomic_add32_return_sum(&drtaint_init_count, -1);
    if (count != 0)
        return;
    drmgr_unregister_pre_syscall_event(event_pre_syscall);
    drmgr_unregister_post_syscall_event(event_post_syscall);
    drtaint_shadow_exit();
    drmgr_exit();
    drreg_exit();
    drsys_exit();
}

bool drtaint_insert_app_to_taint(void *drcontext, instrlist_t *ilist, instr_t *where,
                                 reg_id_t reg_addr, reg_id_t scratch)
{
    return drtaint_shadow_insert_app_to_shadow(drcontext, ilist, where,
                                               reg_addr, scratch);
}

bool drtaint_insert_reg_to_taint(void *drcontext, instrlist_t *ilist, instr_t *where,
                                 reg_id_t shadow, reg_id_t regaddr)
{
    return drtaint_shadow_insert_reg_to_shadow(drcontext, ilist, where,
                                               shadow, regaddr);
}

bool drtaint_insert_reg_to_taint_load(void *drcontext, instrlist_t *ilist, instr_t *where,
                                      reg_id_t shadow, reg_id_t regaddr)
{
    return drtaint_shadow_insert_reg_to_shadow_load(drcontext, ilist, where,
                                                    shadow, regaddr);
}

bool drtaint_get_reg_taint(void *drcontext, reg_id_t reg, byte *result)
{
    return drtaint_shadow_get_reg_taint(drcontext, reg, result);
}

bool drtaint_set_reg_taint(void *drcontext, reg_id_t reg, byte value)
{
    return drtaint_shadow_set_reg_taint(drcontext, reg, value);
}

bool drtaint_get_app_taint(void *drcontext, app_pc app, byte *result)
{
    return drtaint_shadow_get_app_taint(drcontext, app, result);
}

bool drtaint_set_app_taint(void *drcontext, app_pc app, byte result)
{
    return drtaint_shadow_set_app_taint(drcontext, app, result);
}

void drtaint_save_instr(void *drcontext, int opcode)
{
    drtaint_shadow_save_instr(drcontext, opcode);
}

int drtaint_get_prev_instr(void *drcontext)
{
    return drtaint_shadow_get_prev_instr(drcontext);
}

void drtaint_update_cpsr(void *drcontext, uint new_flags)
{
    drtaint_shadow_update_cpsr(drcontext, new_flags);
}

uint drtaint_get_cpsr(void *drcontext)
{
    return drtaint_shadow_get_cpsr(drcontext);
}

/* ======================================================================================
 * main implementation, taint propagation step
 * ==================================================================================== */

static void
propagate_ldr(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    ldr reg1, [mem2]

    We need to save the tag value stored at
    [mem2] shadow address to shadow register of reg1
*/
{
    opnd_t mem2 = instr_get_src(where, 0);
    if (opnd_is_base_disp(mem2))
    {
        auto sreg1 = drreg_reservation{ilist, where};
        auto sapp2 = drreg_reservation{ilist, where};
        reg_id_t reg1 = opnd_get_reg(instr_get_dst(where, 0));

        // dereference the memory address at mem2 and store the result to %sapp2% register
        drutil_insert_get_mem_addr(drcontext, ilist, where, mem2, sapp2, sreg1);

        // get shadow memory addresses of reg1 and [mem2] and place them to %sreg1% and %sapp2%
        drtaint_insert_app_to_taint(drcontext, ilist, where, sapp2, sreg1);
        drtaint_insert_reg_to_taint(drcontext, ilist, where, reg1, sreg1);

        // place to %sapp2% the value placed at [mem2] shadow address
        instrlist_meta_preinsert(ilist, where, XINST_CREATE_load_1byte // ldr sapp2, [sapp2]
                                 (drcontext,
                                  opnd_create_reg(sapp2),       // dst: sapp2
                                  OPND_CREATE_MEM8(sapp2, 0))); // src: sapp2

        // propagate 3d policy: ldr r0, [r1, r2].
        // If r2 is tainted then r0 is tainted too
        if (opnd_num_regs_used(mem2) == 2)
        {
            reg_id_t reg_ind = opnd_get_index(mem2);
            auto sreg_ind = drreg_reservation{ilist, where};

            // get value of %reg_ind% shadow register and place it to %sreg_ind%
            drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg_ind, sreg_ind);

            instrlist_meta_preinsert(ilist, where, INSTR_CREATE_orr // sapp2 |= sreg_ind
                                     (drcontext, opnd_create_reg(sapp2), opnd_create_reg(sapp2), opnd_create_reg(sreg_ind)));
        }

        // save the value of %sapp2% to shadow register of reg1
        instrlist_meta_preinsert_xl8(ilist, where, XINST_CREATE_store_1byte  // str sapp2, [sreg1]
                                     (drcontext, OPND_CREATE_MEM8(sreg1, 0), // dst_mem: sreg1
                                      opnd_create_reg(sapp2)));              // src_reg: sapp2
    }
}

static void
propagate_ldrd(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    ldrd reg1, reg2, [mem2]

    We need to save the tag value stored at
    [mem2] shadow address to shadow registers reg1, reg2
*/
{
    opnd_t mem2 = instr_get_src(where, 0);
    if (opnd_is_base_disp(mem2))
    {
        auto sreg1 = drreg_reservation{ilist, where};
        auto sapp2 = drreg_reservation{ilist, where};
        auto sapp2n = drreg_reservation{ilist, where};

        reg_id_t reg1 = opnd_get_reg(instr_get_dst(where, 0));
        reg_id_t reg2 = opnd_get_reg(instr_get_dst(where, 1));
        reg_id_t sreg2 = sreg1;

        // dereference the memory address at mem2 and store the result to %sapp2% register
        drutil_insert_get_mem_addr(drcontext, ilist, where, mem2, sapp2, sreg1);

        // get [mem2 + 4] address
        instrlist_meta_preinsert(ilist, where, XINST_CREATE_add_2src // add sapp2n, sapp2, #4
                                 (drcontext, opnd_create_reg(sapp2n),
                                  opnd_create_reg(sapp2), 
                                  OPND_CREATE_INT8(4)));  

        // get shadow memory addresses of reg1 and [mem2] and place them to %sreg1% and %sapp2%
        drtaint_insert_app_to_taint(drcontext, ilist, where, sapp2, sreg1);
        drtaint_insert_reg_to_taint(drcontext, ilist, where, reg1, sreg1);

        // place to %sapp2% the value placed at [mem2] shadow address
        instrlist_meta_preinsert(ilist, where, XINST_CREATE_load_1byte // ldr sapp2, [sapp2]
                                 (drcontext, opnd_create_reg(sapp2), OPND_CREATE_MEM8(sapp2, 0)));

        // save the value of %sapp2% to shadow register of reg1
        instrlist_meta_preinsert_xl8(ilist, where, XINST_CREATE_store_1byte // str sapp2, [sreg1]
                                     (drcontext, OPND_CREATE_MEM8(sreg1, 0), opnd_create_reg(sapp2)));

        // get shadow memory addresses of reg2 and [mem2 + 4] and place them to %sreg2% and %sapp2n%
        drtaint_insert_app_to_taint(drcontext, ilist, where, sapp2n, sreg2);
        drtaint_insert_reg_to_taint(drcontext, ilist, where, reg2, sreg2);

        // place to %sapp2n% the value placed at [mem2 + 4] shadow address
        instrlist_meta_preinsert(ilist, where, XINST_CREATE_load_1byte // ldr sapp2n, [sapp2n]
                                 (drcontext, opnd_create_reg(sapp2n), OPND_CREATE_MEM8(sapp2n, 0)));

        // save the value of %sapp2n% to shadow register of reg2
        instrlist_meta_preinsert_xl8(ilist, where, XINST_CREATE_store_1byte // str sapp2n, [sreg2]
                                     (drcontext, OPND_CREATE_MEM8(sreg2, 0), opnd_create_reg(sapp2n)));
    }
}

static void
propagate_str(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    str reg1, [mem2]  

    We need to save the tag value stored in 
    shadow register of reg1 to shadow address of [mem2] 
*/
{
    opnd_t mem2 = instr_get_dst(where, 0);
    if (opnd_is_base_disp(mem2))
    {
        auto sreg1 = drreg_reservation{ilist, where};
        auto sapp2 = drreg_reservation{ilist, where};

        reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 0));

        // dereference the memory address at mem2 and store the result to %sapp2% register
        drutil_insert_get_mem_addr(drcontext, ilist, where, mem2, sapp2, sreg1);

        // get shadow memory address of [mem2] and place it to %sapp2%
        drtaint_insert_app_to_taint(drcontext, ilist, where, sapp2, sreg1);

        // get value of shadow register of reg1 and place it to %sreg1%
        drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);

        // write the value of reg1 to [mem2] shadow address
        instrlist_meta_preinsert_xl8(ilist, where, XINST_CREATE_store_1byte  // str sreg1, [sapp2]
                                     (drcontext, OPND_CREATE_MEM8(sapp2, 0), // dst_mem: sapp2
                                      opnd_create_reg(sreg1)));              // src_reg: sreg1

        // if it's a strex then we have to untaint return status register
        int opcode = instr_get_opcode(where);
        if (opcode == OP_strex ||
            opcode == OP_strexb ||
            opcode == OP_strexh)
        {
            reg_id_t rd = opnd_get_reg(instr_get_dst(where, 1));
            reg_id_t srd = sreg1;
            reg_id_t nullreg = sapp2;

            // nullreg = 0
            instrlist_meta_preinsert(ilist, where, XINST_CREATE_move(drcontext, opnd_create_reg(nullreg), OPND_CREATE_INT8(0)));

            // get shadow register address of %rd% and place it to %srd%
            drtaint_insert_reg_to_taint(drcontext, ilist, where, rd, srd);

            // write 0 to shadow value of %rd%
            instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte // str nullreg, [strd]
                                     (drcontext, OPND_CREATE_MEM8(srd, 0), opnd_create_reg(nullreg)));
        }
    }
}

static void
propagate_strd(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    strd reg1, reg2, [mem2]  

    We need to save the tag value stored in 
    shadow registers of reg2 and reg3 to shadow addresses of [mem2] and [mem2 + 4] 
*/
{
    opnd_t mem2 = instr_get_dst(where, 0);
    if (opnd_is_base_disp(mem2))
    {
        auto sreg1 = drreg_reservation{ilist, where};
        auto sapp2 = drreg_reservation{ilist, where};
        auto sapp2n = drreg_reservation{ilist, where};

        reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 0));
        reg_id_t reg2 = opnd_get_reg(instr_get_src(where, 1));

        // dereference the memory address at mem2 and store the result to %sapp2% register
        drutil_insert_get_mem_addr(drcontext, ilist, where, mem2, sapp2, sreg1);

        // get next 4 bytes after [mem2]
        instrlist_meta_preinsert(ilist, where, XINST_CREATE_add_2src // sapp2n = sapp2 + 4
                                 (drcontext, opnd_create_reg(sapp2n), opnd_create_reg(sapp2), OPND_CREATE_INT8(4)));

        // get shadow memory address of [mem2] and place it to %sapp2%
        drtaint_insert_app_to_taint(drcontext, ilist, where, sapp2, sreg1);

        // get value of shadow register of reg1 and place it to %sreg1%
        drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);

        // write the value of reg1 to [mem2] shadow address
        instrlist_meta_preinsert_xl8(ilist, where, XINST_CREATE_store_1byte // str sreg1, [sapp2]
                                     (drcontext, OPND_CREATE_MEM8(sapp2, 0), opnd_create_reg(sreg1)));

        // get shadow memory address of [mem2 + 4] and place it to %sapp2n%
        drtaint_insert_app_to_taint(drcontext, ilist, where, sapp2n, sapp2);

        // get value of shadow register of reg2 and place it to %sreg1%
        drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg2, sreg1);

        // write the shadow value of reg2 to [mem2 + 4] shadow address
        instrlist_meta_preinsert_xl8(ilist, where, XINST_CREATE_store_1byte // str sreg1, [sapp2n]
                                     (drcontext, OPND_CREATE_MEM8(sapp2n, 0), opnd_create_reg(sreg1)));

        // if it's a strexd then we have to untaint return status register
        if (instr_get_opcode(where) == OP_strexd)
        {
            reg_id_t rd = opnd_get_reg(instr_get_dst(where, 1));
            reg_id_t srd = sreg1;
            reg_id_t nullreg = sapp2;

            // nullreg = 0
            instrlist_meta_preinsert(ilist, where, XINST_CREATE_move(drcontext, opnd_create_reg(nullreg), OPND_CREATE_INT8(0)));

            // get shadow register address of %rd% and place it to %srd%
            drtaint_insert_reg_to_taint(drcontext, ilist, where, rd, srd);

            // write 0 to shadow value of %rd%
            instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte // str nullreg, [strd]
                                     (drcontext, OPND_CREATE_MEM8(srd, 0), opnd_create_reg(nullreg)));
        }
    }
}

static void
propagate_mov_regs(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where,
                   reg_id_t reg1, reg_id_t reg2)
/*
    mov reg2, reg1

    Need to save the tag value of reg1's 
    shadow register to reg2's shadow register
*/
{
    auto sreg2 = drreg_reservation{ilist, where};
    auto sreg1 = drreg_reservation{ilist, where};

    // get value of shadow register of reg1 and place it to %sreg1%
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);

    // get shadow register address of reg2 and place it to %sreg2%
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg2, sreg2);

    // write shadow value of reg1 to shadow value of reg2
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte  // str sreg1, [sreg2]
                             (drcontext, OPND_CREATE_MEM8(sreg2, 0), // dst_mem: sreg2
                              opnd_create_reg(sreg1)));              // src_reg: sreg1
}

static void
propagate_mov_reg_src(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
{
    /* mov reg2, reg1 */
    reg_id_t reg2 = opnd_get_reg(instr_get_dst(where, 0));
    reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 0));
    propagate_mov_regs(drcontext, tag, ilist, where, reg1, reg2);
}

static void
propagate_mov_imm_src(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    mov reg2, imm1

    Saves the value of 0 to the shadow register 
    of reg2 because moving constant to reg2 untaints reg2
*/
{
    auto sreg2 = drreg_reservation{ilist, where};
    auto simm2 = drreg_reservation{ilist, where};
    reg_id_t reg2 = opnd_get_reg(instr_get_dst(where, 0));

    // get shadow register address of reg2 and place it to %sreg2%
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg2, sreg2);

    // place constant imm to register %simm2%
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_move // mov simm2, 0
                             (drcontext,
                              opnd_create_reg(simm2),             // dst: simm2
                              opnd_create_immed_int(0, OPSZ_1))); // src: 0

    // move to shadow register of reg2 the value of imm1
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte  // str simm2, [sreg2]
                             (drcontext, OPND_CREATE_MEM8(sreg2, 0), // dst_mem: sreg2
                              opnd_create_reg(simm2)));              // src_reg: simm2
}

static void
propagate_arith_imm_reg(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    [add | sub | ... ] reg2, imm, reg1

    Need to mark reg2 tainted
*/
{
    auto sreg2 = drreg_reservation{ilist, where};
    auto sreg1 = drreg_reservation{ilist, where};
    reg_id_t reg2 = opnd_get_reg(instr_get_dst(where, 0));
    reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 1));

    // get value of shadow register of reg1 and place it to %sreg1%
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);

    // get shadow register address of reg2 and place it to %sreg2%
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg2, sreg2);

    // write the result to shadow register of reg2
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte  // str sreg1, [sreg2]
                             (drcontext, OPND_CREATE_MEM8(sreg2, 0), // dst_mem: sreg2
                              opnd_create_reg(sreg1)));              // src_reg: sreg1
}

static void
propagate_arith_reg_imm(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    [add | sub | ... ] reg2, reg1, imm

    Need to mark reg2 tainted
*/
{
    auto sreg2 = drreg_reservation{ilist, where};
    auto sreg1 = drreg_reservation{ilist, where};
    reg_id_t reg2 = opnd_get_reg(instr_get_dst(where, 0));
    reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 0));

    // get value of shadow register of reg1 and place it to %sreg1%
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);

    // get shadow register address of reg2 and place it to %sreg2%
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg2, sreg2);

    // write the result to shadow register of reg2
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte  // str sreg1, [sreg2]
                             (drcontext, OPND_CREATE_MEM8(sreg2, 0), // dst_mem: sreg2
                              opnd_create_reg(sreg1)));              // src_reg: sreg1
}

static void
propagate_mla(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/* 
    mla reg4, reg3, reg2, reg1 

    Need to mark reg4 tainted. 
    Because its value depends on values of reg3, reg2, reg1, 
    we use OR to combine their impacts to reg4
*/
{

    auto sreg1 = drreg_reservation{ilist, where};
    auto sreg2 = drreg_reservation{ilist, where};
    reg_id_t sreg3 = sreg2;
    reg_id_t sreg4 = sreg3; /* we reuse a register for this */

    reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 2));
    reg_id_t reg2 = opnd_get_reg(instr_get_src(where, 1));
    reg_id_t reg3 = opnd_get_reg(instr_get_src(where, 0));
    reg_id_t reg4 = opnd_get_reg(instr_get_dst(where, 0));

    // get value of shadow registers of reg1, reg2 and place it to %sreg1%, %sreg2%
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg2, sreg2);

    instrlist_meta_preinsert(ilist, where, INSTR_CREATE_orr // sreg1 = sreg1 || sreg2
                             (drcontext,
                              opnd_create_reg(sreg1),   // dst:  sreg1
                              opnd_create_reg(sreg2),   // src1: sreg2
                              opnd_create_reg(sreg1))); // src2: sreg1

    // get value of shadow register of reg3 and place it to %sreg3%
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg3, sreg3);

    instrlist_meta_preinsert(ilist, where, INSTR_CREATE_orr // sreg1 = sreg3 || sreg1
                             (drcontext,
                              opnd_create_reg(sreg1),   // dst:  sreg1
                              opnd_create_reg(sreg3),   // src1: sreg3
                              opnd_create_reg(sreg1))); // src2: sreg1

    // get address of shadow register of reg4 and place it to %sreg4%
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg4, sreg4);

    // save the result to shadow register of reg4
    instrlist_meta_preinsert_xl8(ilist, where, XINST_CREATE_store_1byte  // str sreg1, [sreg4]
                                 (drcontext, OPND_CREATE_MEM8(sreg4, 0), // dst_mem: sreg4
                                  opnd_create_reg(sreg1)));              // src_reg: sreg1
}

static void
propagate_umull(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/* 
    umull reg4, reg3, reg2, reg1 

    Need to mark reg3 and reg4 tainted. 
    Because their values depend on values of reg2, reg1, 
    we use OR to combine their impacts to reg3 and reg4
*/
{
    auto sreg1 = drreg_reservation{ilist, where};
    auto sreg2 = drreg_reservation{ilist, where};
    auto sreg3 = drreg_reservation{ilist, where};
    reg_id_t sreg4 = sreg3; /* we reuse a register for this */

    reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 0));
    reg_id_t reg2 = opnd_get_reg(instr_get_src(where, 1));
    reg_id_t reg3 = opnd_get_reg(instr_get_dst(where, 0));
    reg_id_t reg4 = opnd_get_reg(instr_get_dst(where, 1));

    // get value of shadow registers of reg1, reg2 and place it to %sreg1%, %sreg2%
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg2, sreg2);

    instrlist_meta_preinsert(ilist, where, INSTR_CREATE_orr // sreg1 = sreg2 || sreg1
                             (drcontext,
                              opnd_create_reg(sreg1),   // dst:  sreg1
                              opnd_create_reg(sreg2),   // src1: sreg2
                              opnd_create_reg(sreg1))); // src2: sreg1

    // get address of shadow register of reg3 and place it to %sreg3%
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg3, sreg3);

    // save the higher part of result to shadow register of reg3
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte // str sreg1, [sreg3]
                             (drcontext, OPND_CREATE_MEM8(sreg3, 0), opnd_create_reg(sreg1)));

    // get address of shadow register of reg4 and place it to %sreg4%
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg4, sreg4);

    // save the the lower part of result to shadow register of reg4
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte // str sreg1, [sreg4]
                             (drcontext, OPND_CREATE_MEM8(sreg4, 0), opnd_create_reg(sreg1)));
}

static void
propagate_arith_reg_reg(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    [add | sub | ... ] reg3, reg2, reg1

    Need to mark reg3 tainted. 
    Because its value depends on values of reg2, reg1, 
    we use OR to combine their impacts to reg3
*/
{
    auto sreg2 = drreg_reservation{ilist, where};
    auto sreg1 = drreg_reservation{ilist, where};
    reg_id_t sreg3 = sreg2; /* we reuse a register for this */
    reg_id_t reg3 = opnd_get_reg(instr_get_dst(where, 0));
    reg_id_t reg2 = opnd_get_reg(instr_get_src(where, 0));
    reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 1));

    // get value of shadow registers of reg1, reg2 and place it to %sreg1%, %sreg2%
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg2, sreg2);

    instrlist_meta_preinsert(ilist, where, INSTR_CREATE_orr // sreg1 |= sreg2
                             (drcontext, opnd_create_reg(sreg1), opnd_create_reg(sreg2), opnd_create_reg(sreg1)));

    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg3, sreg3);
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte // str sreg1, [sreg3]
                             (drcontext, OPND_CREATE_MEM8(sreg3, 0), opnd_create_reg(sreg1)));
}

// decrement before, increment after, decrement after, increment before
typedef enum
{
    DB,
    IA,
    DA,
    IB
} stack_dir_t;

template <stack_dir_t T>
app_pc
calculate_addr(instr_t *instr, void *base, int i, int top)
{
    DR_ASSERT_MSG(false, "Unreachable");
}

template <>
app_pc
calculate_addr<DB>(instr_t *instr, void *base, int i, int top)
{
    return (app_pc)base - 4 * (top - i);
}

template <>
app_pc
calculate_addr<IA>(instr_t *instr, void *base, int i, int top)
{
    return (app_pc)base + 4 * i;
}
//////////////////////////
template <>
app_pc
calculate_addr<DA>(instr_t *instr, void *base, int i, int top)
{
    return (app_pc)base - 4 * (top - i - 1);
}

template <>
app_pc
calculate_addr<IB>(instr_t *instr, void *base, int i, int top)
{
    return (app_pc)base + 4 * (i + 1);
}

template <stack_dir_t c>
void propagate_ldm_cc_template(void *pc, void *base, bool writeback)
/*
    ldm r, { regs }

    When handling a ldm command we have to save all values of registers
    that will be popped from a stack to their shadow registers
*/
{
    void *drcontext = dr_get_current_drcontext();
    instr_t *instr = instr_create(drcontext);
    decode(drcontext, (byte *)pc, instr);

    int num_dsts = instr_num_dsts(instr);
    if (writeback)
        num_dsts--;

    for (int i = 0; i < num_dsts; ++i)
    {
        bool ok;

        // ? why 1 instead of 0
        // ? why do we need this
        if (writeback &&
            (opnd_get_reg(instr_get_dst(instr, i)) ==
             opnd_get_reg(instr_get_src(instr, 1))))
            break;

        // set taint from stack to the appropriate register
        byte res;
        ok = drtaint_get_app_taint(drcontext, calculate_addr<c>(instr, base, i, num_dsts), &res);
        DR_ASSERT(ok);
        ok = drtaint_set_reg_taint(drcontext, opnd_get_reg(instr_get_dst(instr, i)), res);
        DR_ASSERT(ok);
    }

    instr_destroy(drcontext, instr);
}

template <stack_dir_t c>
void propagate_stm_cc_template(void *pc, void *base, bool writeback)
/*
    stm r, { regs }
    
    When handling a stm command we have to set all memory 
    in the stack where the register values will be written tainted
*/
{
    void *drcontext = dr_get_current_drcontext();
    instr_t *instr = instr_create(drcontext);
    decode(drcontext, (byte *)pc, instr);

    int num_srcs = instr_num_srcs(instr);
    if (writeback)
        num_srcs--;

    for (int i = 0; i < num_srcs; ++i)
    {
        bool ok;

        // ? why 1 instead of 0
        // ? why do we need this
        if (writeback &&
            (opnd_get_reg(instr_get_src(instr, i)) ==
             opnd_get_reg(instr_get_dst(instr, 1))))
            break;

        // set taint from registers to the stack
        byte res;
        ok = drtaint_get_reg_taint(drcontext, opnd_get_reg(instr_get_src(instr, i)), &res);
        DR_ASSERT(ok);
        ok = drtaint_set_app_taint(drcontext, calculate_addr<c>(instr, base, i, num_srcs), res);
        DR_ASSERT(ok);
    }
    instr_destroy(drcontext, instr);
}

static bool
instr_handle_constant_func(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
{
    short opcode = instr_get_opcode(where);

    if (opcode == OP_eor ||
        opcode == OP_eors ||
        opcode == OP_sub ||
        opcode == OP_subs ||
        opcode == OP_sbc ||
        opcode == OP_sbcs)
    {
        // xor r1, r0, r0 causes r1 to be untainted
        if (!opnd_is_reg(instr_get_src(where, 0)))
            return false;

        if (!opnd_is_reg(instr_get_src(where, 1)))
            return false;

        if (opnd_get_reg(instr_get_src(where, 0)) !=
            opnd_get_reg(instr_get_src(where, 1)))
            return false;

        // mov r1, imm
        propagate_mov_imm_src(drcontext, tag, ilist, where);
        return true;
    }
    return false;
}

static dr_emit_flags_t
event_app_instruction(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where,
                      bool for_trace, bool translating, void *user_data)
{
    if (instr_is_meta(where))
        return DR_EMIT_DEFAULT;

    int opcode = instr_get_opcode(where);

    // get opcode of previsous instruction
    // if it changes cpsr then update shadow cpsr
    if (instr_affects_on_flags(drtaint_get_prev_instr(drcontext)))
    {
        uint flags = instr_get_arith_flags(where, DR_QUERY_INCLUDE_ALL);
        drtaint_update_cpsr(drcontext, flags);
    }

    // save current opcode
    drtaint_save_instr(drcontext, opcode);

    // check if instruction is conditionally executed
    // if predication is false we don't need to continue
    if (instr_is_predicated(where))
    {
        uint flags = drtaint_get_cpsr(drcontext);
        if (!instr_predicate_is_true(where, flags))
            return DR_EMIT_DEFAULT;
    }

    // untaint stack area when allocating a new frame
    if (opcode == OP_sub || opcode == OP_subs)
    {
        if (opnd_get_reg(instr_get_dst(where, 0)) == DR_REG_SP &&
            opnd_get_reg(instr_get_src(where, 0)) == DR_REG_SP &&
            opnd_is_immed(instr_get_src(where, 1)))
        {
            bool ok;
            dr_mcontext_t mcontext = {sizeof(dr_mcontext_t), DR_MC_CONTROL};
            ok = dr_get_mcontext(drcontext, &mcontext);
            DR_ASSERT(ok);

            ptr_int_t imm = opnd_get_immed_int(instr_get_src(where, 1));
            app_pc sp_val = (app_pc)reg_get_value(DR_REG_SP, &mcontext);

            for (int i = 1; i < imm + 2; i++)
            {
                ok = drtaint_set_app_taint(drcontext, &sp_val[-i], 0);
                DR_ASSERT(ok);
            }
        }
    }

    // no support for simd instructions
    if (instr_is_simd(where))
    {
        unimplemented_opcode(where);
        return DR_EMIT_DEFAULT;
    }

    if (instr_handle_constant_func(drcontext, tag, ilist, where))
        return DR_EMIT_DEFAULT;

        /* We define a routine to make it easier to call drreg_restore_app_value() in
     * the case that we have to swap a register out to make space for the stolen
     * reg.
     */
#define DRREG_RESTORE_APP_VALUE(drcontext, ilist, where, opnd, cb)      \
    do                                                                  \
    {                                                                   \
        reg_id_t swap = DR_REG_NULL;                                    \
        drreg_restore_app_values(drcontext, ilist, where, opnd, &swap); \
        cb;                                                             \
        if (swap != DR_REG_NULL)                                        \
            drreg_unreserve_register(drcontext, ilist, where, swap);    \
    } while (false);

    switch (opcode)
    {
    case OP_ldmia:
        DRREG_RESTORE_APP_VALUE(drcontext, ilist, where, instr_get_src(where, 0), {
            dr_insert_clean_call(drcontext, ilist, where, (void *)propagate_ldm_cc_template<IA>,
                                 false, 3, OPND_CREATE_INTPTR(instr_get_app_pc(where)),
                                 opnd_create_reg(opnd_get_base(instr_get_src(where, 0))),
                                 /* writeback */
                                 OPND_CREATE_INT8(instr_num_srcs(where) > 1));
        });
        break;
    case OP_ldmdb:
        DRREG_RESTORE_APP_VALUE(drcontext, ilist, where, instr_get_src(where, 0), {
            dr_insert_clean_call(drcontext, ilist, where, (void *)propagate_ldm_cc_template<DB>,
                                 false, 3, OPND_CREATE_INTPTR(instr_get_app_pc(where)),
                                 opnd_create_reg(opnd_get_base(instr_get_src(where, 0))),
                                 /* writeback */
                                 OPND_CREATE_INT8(instr_num_srcs(where) > 1));
        });
        break;
    case OP_ldmib:
        DRREG_RESTORE_APP_VALUE(drcontext, ilist, where, instr_get_src(where, 0), {
            dr_insert_clean_call(drcontext, ilist, where, (void *)propagate_ldm_cc_template<IB>,
                                 false, 3, OPND_CREATE_INTPTR(instr_get_app_pc(where)),
                                 opnd_create_reg(opnd_get_base(instr_get_src(where, 0))),
                                 /* writeback */
                                 OPND_CREATE_INT8(instr_num_srcs(where) > 1));
        });
        break;
    case OP_ldmda:
        DRREG_RESTORE_APP_VALUE(drcontext, ilist, where, instr_get_src(where, 0), {
            dr_insert_clean_call(drcontext, ilist, where, (void *)propagate_ldm_cc_template<DA>,
                                 false, 3, OPND_CREATE_INTPTR(instr_get_app_pc(where)),
                                 opnd_create_reg(opnd_get_base(instr_get_src(where, 0))),
                                 /* writeback */
                                 OPND_CREATE_INT8(instr_num_srcs(where) > 1));
        });
        break;
    case OP_stmia:
        DRREG_RESTORE_APP_VALUE(drcontext, ilist, where, instr_get_dst(where, 0), {
            dr_insert_clean_call(drcontext, ilist, where, (void *)propagate_stm_cc_template<IA>,
                                 false, 3, OPND_CREATE_INTPTR(instr_get_app_pc(where)),
                                 opnd_create_reg(opnd_get_base(instr_get_dst(where, 0))),
                                 /* writeback */
                                 OPND_CREATE_INT8(instr_num_dsts(where) > 1));
        });
        break;
    case OP_stmdb:
        DRREG_RESTORE_APP_VALUE(drcontext, ilist, where, instr_get_dst(where, 0), {
            dr_insert_clean_call(drcontext, ilist, where, (void *)propagate_stm_cc_template<DB>,
                                 false, 3, OPND_CREATE_INTPTR(instr_get_app_pc(where)),
                                 opnd_create_reg(opnd_get_base(instr_get_dst(where, 0))),
                                 /* writeback */
                                 OPND_CREATE_INT8(instr_num_dsts(where) > 1));
        });
        break;
    case OP_stmib:
        DRREG_RESTORE_APP_VALUE(drcontext, ilist, where, instr_get_dst(where, 0), {
            dr_insert_clean_call(drcontext, ilist, where, (void *)propagate_stm_cc_template<IB>,
                                 false, 3, OPND_CREATE_INTPTR(instr_get_app_pc(where)),
                                 opnd_create_reg(opnd_get_base(instr_get_dst(where, 0))),
                                 /* writeback */
                                 OPND_CREATE_INT8(instr_num_dsts(where) > 1));
        });
        break;
    case OP_stmda:
        DRREG_RESTORE_APP_VALUE(drcontext, ilist, where, instr_get_dst(where, 0), {
            dr_insert_clean_call(drcontext, ilist, where, (void *)propagate_stm_cc_template<DA>,
                                 false, 3, OPND_CREATE_INTPTR(instr_get_app_pc(where)),
                                 opnd_create_reg(opnd_get_base(instr_get_dst(where, 0))),
                                 /* writeback */
                                 OPND_CREATE_INT8(instr_num_dsts(where) > 1));
        });
        break;

    case OP_ldr:
    case OP_ldrb:
    case OP_ldrh:
    case OP_ldrsh:
    case OP_ldrsb:

    // exclusive ldr
    case OP_ldrex:
    case OP_ldrexb:
    case OP_ldrexh:

    // thumb mode
    case OP_ldrt:
    case OP_ldrbt:
    case OP_ldrht:
    case OP_ldrsbt:
    case OP_ldrsht:

        propagate_ldr(drcontext, tag, ilist, where);
        break;

    case OP_ldrd:
    case OP_ldrexd:

        propagate_ldrd(drcontext, tag, ilist, where);
        break;

    case OP_str:
    case OP_strb:
    case OP_strh:
    case OP_strex:
    case OP_strexb:
    case OP_strexh:

    // thumb
    case OP_strt:
    case OP_strbt:
    case OP_strht:

        propagate_str(drcontext, tag, ilist, where);
        break;

    case OP_strd:
    case OP_strexd:

        propagate_strd(drcontext, tag, ilist, where);
        break;

    case OP_mov:
    case OP_mvn:
    case OP_mvns:
    case OP_movw:
    case OP_movt:
    case OP_movs:
    case OP_rrx:
    case OP_rrxs:

        if (opnd_is_reg(instr_get_src(where, 0)))
            propagate_mov_reg_src(drcontext, tag, ilist, where);
        else
            propagate_mov_imm_src(drcontext, tag, ilist, where);
        break;

    case OP_sbfx:
    case OP_ubfx:
    case OP_uxtb:
    case OP_uxth:
    case OP_sxtb:
    case OP_sxth:
    case OP_rev:
    case OP_rev16:

        /* These aren't mov's per se, but they only accept 1
         * reg source and 1 reg dest.
         */
        propagate_mov_reg_src(drcontext, tag, ilist, where);
        break;

    case OP_sel:
    case OP_clz:

        /* These aren't mov's per se, but they only accept 1
         * reg source and 1 dest.
         */
        if (opnd_is_reg(instr_get_src(where, 0)))
            propagate_mov_reg_src(drcontext, tag, ilist, where);
        else
            propagate_mov_imm_src(drcontext, tag, ilist, where);
        break;

    case OP_adc:
    case OP_adcs:
    case OP_add:
    case OP_adds:
    case OP_addw:
    case OP_rsb:
    case OP_rsbs:
    case OP_rsc:
    case OP_sbc:
    case OP_sbcs:
    case OP_sub:
    case OP_subw:
    case OP_subs:
    case OP_and:
    case OP_ands:
    case OP_bic:
    case OP_bics:
    case OP_eor:
    case OP_eors:
    case OP_mul:
    case OP_muls:
    case OP_orr:
    case OP_ror:
    case OP_orrs:
    case OP_lsl:
    case OP_lsls:
    case OP_lsr:
    case OP_lsrs:
    case OP_asr:
    case OP_asrs:
    case OP_orn:
    case OP_uadd8:
    case OP_uqsub8:

        DR_ASSERT(instr_num_srcs(where) == 2 || instr_num_srcs(where) == 4);
        DR_ASSERT(instr_num_dsts(where) == 1);
        if (opnd_is_reg(instr_get_src(where, 0)))
        {
            if (opnd_is_reg(instr_get_src(where, 1)))
                propagate_arith_reg_reg(drcontext, tag, ilist, where);
            else
                propagate_arith_reg_imm(drcontext, tag, ilist, where);
        }
        else if (opnd_is_reg(instr_get_src(where, 1)))
            propagate_arith_imm_reg(drcontext, tag, ilist, where);
        else
            DR_ASSERT(false); /* add reg, imm, imm does not make sense */
        break;

    case OP_smull:
    case OP_umull:
        propagate_umull(drcontext, tag, ilist, where);
        break;

    case OP_mla:
    case OP_mls:
        propagate_mla(drcontext, tag, ilist, where);
        break;

    case OP_bl:
    case OP_blx:
    case OP_blx_ind:
        propagate_mov_regs(drcontext, tag, ilist, where,
                           DR_REG_PC, DR_REG_LR);
        /* fallthrough, we could have a register dest */
    case OP_bxj:
    case OP_bx:
    case OP_b:
    case OP_b_short:
        /* could have register destination */
        if (opnd_is_reg(instr_get_src(where, 0)))
        {
            propagate_mov_regs(drcontext, tag, ilist, where,
                               opnd_get_reg(instr_get_src(where, 0)),
                               DR_REG_PC);
        }
        /* we don't have to do anything for immediates */
        break;

    default:
        unimplemented_opcode(where);
        break;
    }

    return DR_EMIT_DEFAULT;
}

/* ======================================================================================
 * system call clearing and handling routines
 * ==================================================================================== */

static bool
drsys_iter_cb(drsys_arg_t *arg, void *drcontext)
/*
    Set syscall output parameters untainted 
*/
{
    if (!arg->valid)
        return true;

    if (arg->pre)
        return true;

#define TEST(mask, var) (((mask) & (var)) != 0)

    if (TEST(arg->mode, DRSYS_PARAM_OUT))
    {
        char *buffer = (char *)arg->start_addr;
        for (int i = 0; i < arg->size; ++i)
        {
            if (!drtaint_set_app_taint(drcontext,
                                       (app_pc)buffer + i, 0))
                DR_ASSERT(false);
        }
    }
#undef TEST
    return true;
}

static bool
event_pre_syscall(void *drcontext, int sysnum)
{
    if (drsys_iterate_memargs(drcontext, drsys_iter_cb, drcontext) !=
        DRMF_SUCCESS)
        DR_ASSERT(false);
    return true;
}

static void
event_post_syscall(void *drcontext, int sysnum)
{
    dr_syscall_result_info_t info = {
        sizeof(info),
    };
    dr_syscall_get_result_ex(drcontext, &info);

    /* all syscalls untaint rax */
    drtaint_set_reg_taint(drcontext, DR_REG_R0, 0);

    if (!info.succeeded)
    {
        /* We only care about tainting if the syscall
         * succeeded.
         */
        return;
    }

    /* clear taint for system calls with an OUT memarg param */
    if (drsys_iterate_memargs(drcontext, drsys_iter_cb, drcontext) !=
        DRMF_SUCCESS)
        DR_ASSERT(false);
}

static bool
instr_affects_on_flags(int opcode)
{
    switch (opcode)
    {
    // these four instruction always update flags
    case OP_cmp:
    case OP_cmn:
    case OP_tst:
    case OP_teq:

    // other instructions must have {S} suffix
    case OP_adcs:
    case OP_adds:
    case OP_ands:
    case OP_asrs:
    case OP_bics:
    case OP_eors:
    case OP_lsls:
    case OP_lsrs:
    case OP_mlas:
    case OP_mls:
    case OP_movs:
    case OP_muls:
    case OP_mvns:
    case OP_orns:
    case OP_orrs:
    case OP_rors:
    case OP_rrxs:
    case OP_rsbs:
    case OP_rscs:
    case OP_msr: // writes to cspr directly
    case OP_sbcs:
    case OP_subs:

    case OP_smlals:
    case OP_smmls:
    case OP_smulls:
    case OP_umlals:
    case OP_umulls:

        return true;
    }

    return false;
}

static bool
instr_predicate_is_true(instr_t *where, uint cspr)
{
    switch (instr_get_predicate(where))
    {
    case DR_PRED_EQ:
        return DRT_TEST_FLAG_UP(cspr, EFLAGS_Z);

    case DR_PRED_NE:
        return DRT_TEST_FLAG_DOWN(cspr, EFLAGS_Z);

    case DR_PRED_CS:
        return DRT_TEST_FLAG_UP(cspr, EFLAGS_C);

    case DR_PRED_CC:
        return DRT_TEST_FLAG_DOWN(cspr, EFLAGS_C);

    case DR_PRED_MI:
        return DRT_TEST_FLAG_UP(cspr, EFLAGS_N);

    case DR_PRED_PL:
        return DRT_TEST_FLAG_DOWN(cspr, EFLAGS_N);

    case DR_PRED_VS:
        return DRT_TEST_FLAG_UP(cspr, EFLAGS_V);

    case DR_PRED_VC:
        return DRT_TEST_FLAG_DOWN(cspr, EFLAGS_V);

    case DR_PRED_HI: //+
        return DRT_TEST_FLAG_DOWN(cspr, EFLAGS_Z) && DRT_TEST_FLAG_UP(cspr, EFLAGS_C);

    case DR_PRED_LS: //+
        return DRT_TEST_FLAG_UP(cspr, EFLAGS_Z) || DRT_TEST_FLAG_DOWN(cspr, EFLAGS_C);

    case DR_PRED_GE: //+
        return DRT_TEST_FLAGS_UP(cspr, EFLAGS_N | EFLAGS_V) ||
               DRT_TEST_FLAGS_DOWN(cspr, EFLAGS_N | EFLAGS_V);

    case DR_PRED_LT: //+
        return DRT_TEST_FLAG_UP(cspr, EFLAGS_N) && DRT_TEST_FLAG_DOWN(cspr, EFLAGS_V) ||
               DRT_TEST_FLAG_DOWN(cspr, EFLAGS_N) && DRT_TEST_FLAG_UP(cspr, EFLAGS_V);

    case DR_PRED_GT: //+
        return (DRT_TEST_FLAGS_UP(cspr, EFLAGS_N | EFLAGS_V) && DRT_TEST_FLAG_DOWN(cspr, EFLAGS_Z)) ||
               DRT_TEST_FLAGS_DOWN(cspr, EFLAGS_N | EFLAGS_V | EFLAGS_Z);

    case DR_PRED_LE: //+
        return DRT_TEST_FLAG_UP(cspr, EFLAGS_Z) ||
               DRT_TEST_FLAG_UP(cspr, EFLAGS_N) && DRT_TEST_FLAG_DOWN(cspr, EFLAGS_V) ||
               DRT_TEST_FLAG_DOWN(cspr, EFLAGS_N) && DRT_TEST_FLAG_UP(cspr, EFLAGS_V);

    case DR_PRED_AL:
        return true;
    }

    return false;
}

static void
what_are_srcs(instr_t *where)
{
    int n = instr_num_srcs(where);

    if (n == 0)
        dr_printf("No args\n");
    else
    {
        dr_printf("%d args:", n);
        for (int i = 0; i < n; i++)
        {
            opnd_t opnd = instr_get_src(where, i);
            const char *s = opnd_is_reg(opnd)
                                ? "reg"
                                : opnd_is_null(opnd)
                                      ? "null"
                                      : opnd_is_immed(opnd)
                                            ? "imm"
                                            : opnd_is_memory_reference(opnd)
                                                  ? "mem"
                                                  : "unknown";

            dr_printf("%s ", s);
        }

        dr_printf("\n");
    }
}

static void
what_are_dsts(instr_t *where)
{
    int n = instr_num_dsts(where);

    if (n == 0)
        dr_printf("No args\n");
    else
    {
        dr_printf("%d args:", n);
        for (int i = 0; i < n; i++)
        {
            opnd_t opnd = instr_get_dst(where, i);
            const char *s = opnd_is_reg(opnd)
                                ? "reg"
                                : opnd_is_null(opnd)
                                      ? "null"
                                      : opnd_is_immed(opnd)
                                            ? "imm"
                                            : opnd_is_memory_reference(opnd)
                                                  ? "mem"
                                                  : "unknown";

            dr_printf("%s ", s);
        }

        dr_printf("\n");
    }
}