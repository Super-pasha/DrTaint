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

static int drtaint_init_count;

static client_id_t client_id;


bool
drtaint_init(client_id_t id)
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

void
drtaint_exit(void)
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


bool
drtaint_insert_app_to_taint(void *drcontext, instrlist_t *ilist, instr_t *where,
                            reg_id_t reg_addr, reg_id_t scratch)
{
    return drtaint_shadow_insert_app_to_shadow(drcontext, ilist, where,
                                               reg_addr, scratch);
}


bool
drtaint_insert_reg_to_taint(void *drcontext, instrlist_t *ilist, instr_t *where,
                            reg_id_t shadow, reg_id_t regaddr)
{
    return drtaint_shadow_insert_reg_to_shadow(drcontext, ilist, where,
                                               shadow, regaddr);
}


bool
drtaint_insert_reg_to_taint_load(void *drcontext, instrlist_t *ilist, instr_t *where,
                                 reg_id_t shadow, reg_id_t regaddr)
{
    return drtaint_shadow_insert_reg_to_shadow_load(drcontext, ilist, where,
                                                    shadow, regaddr);
}


bool
drtaint_get_reg_taint(void *drcontext, reg_id_t reg, byte *result)
{
    return drtaint_shadow_get_reg_taint(drcontext, reg, result);
}


bool
drtaint_set_reg_taint(void *drcontext, reg_id_t reg, byte value)
{
    return drtaint_shadow_set_reg_taint(drcontext, reg, value);
}


bool
drtaint_get_app_taint(void *drcontext, app_pc app, byte *result)
{
    return drtaint_shadow_get_app_taint(drcontext, app, result);
}


bool
drtaint_set_app_taint(void *drcontext, app_pc app, byte result)
{
    return drtaint_shadow_set_app_taint(drcontext, app, result);
}

/* ======================================================================================
 * main implementation, taint propagation step
 * ==================================================================================== */
static void
propagate_ldr(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    ldr reg1, [mem2]

    We need to save the value that will be placed to reg1 to its shadow register
*/
{
    auto sreg1 = drreg_reservation { ilist, where };
    auto sapp2 = drreg_reservation { ilist, where };
    
    reg_id_t reg1 = opnd_get_reg(instr_get_dst(where, 0));
    opnd_t   mem2 = instr_get_src(where, 0);

    // dereference the memory address at mem2 and store the result to %sapp2% register
    drutil_insert_get_mem_addr(drcontext, ilist, where, mem2, sapp2, sreg1);

    // get shadow memory addresses of reg1 and [mem2] and place them to %sreg1% and %sapp2%   
    drtaint_insert_app_to_taint(drcontext, ilist, where, sapp2, sreg1);
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg1, sreg1);
    
    // place to %sapp2% the value placed at address [mem2]  
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_load_1byte      // ldr sapp2, [sapp2]
                             (drcontext,
                              opnd_create_reg(sapp2),                   // dst: sapp2 
                              OPND_CREATE_MEM8(sapp2, 0)));             // src: sapp2
    
    // save the value of %sapp2% to shadow register of reg1
    instrlist_meta_preinsert_xl8(ilist, where, XINST_CREATE_store_1byte // str sapp2, [sreg1]
                                 (drcontext,
                                  OPND_CREATE_MEM8(sreg1, 0),           // dst_mem: sreg1
                                  opnd_create_reg(sapp2)));             // src_reg: sapp2
}

static void
propagate_str(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    str reg1, [mem2]  

    We need to save the value that will be placed to [mem2] to its shadow memory
*/
{
    auto sreg1 = drreg_reservation { ilist, where };
    auto sapp2 = drreg_reservation { ilist, where };

    reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 0));
    opnd_t   mem2 = instr_get_dst(where, 0);
    
    // dereference the memory address at mem2 and store the result to %sapp2% register
    drutil_insert_get_mem_addr(drcontext, ilist, where, mem2, sapp2, sreg1);
    
    // get shadow memory address of [mem2] and place it to %sapp2%   
    drtaint_insert_app_to_taint(drcontext, ilist, where, sapp2, sreg1);
    
    // get value of shadow register of reg1 and place it to %sreg1%
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);
    
    // write the value of reg1 to [mem2] shadow address
    instrlist_meta_preinsert_xl8(ilist, where, XINST_CREATE_store_1byte     // str sreg1, [sapp2]
                                 (drcontext,
                                  OPND_CREATE_MEM8(sapp2, 0),               // dst_mem: sapp2
                                  opnd_create_reg(sreg1)));                 // src_reg: sreg1
}

static void
propagate_mov_regs(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where,
                   reg_id_t reg1, reg_id_t reg2)
/*
    mov reg2, reg1

    Need to save the value that will be placed to reg2
*/
{
    auto sreg2 = drreg_reservation { ilist, where };
    auto sreg1 = drreg_reservation { ilist, where };

    // get value of shadow register of reg1 and place it to %sreg1%
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);

    // get shadow register address of reg2 and place it to %sreg2%   
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg2, sreg2);
    
    // write shadow value of reg1 to shadow value of reg2
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte     // str sreg1, [sreg2]
                             (drcontext,
                              OPND_CREATE_MEM8(sreg2, 0),               // dst_mem: sreg2
                              opnd_create_reg(sreg1)));                 // src_reg: sreg1
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

    Need to save the value of immediate constant that will be placed to reg2
*/
{
    auto sreg2 = drreg_reservation { ilist , where };
    auto simm2 = drreg_reservation { ilist , where };
    reg_id_t reg2 = opnd_get_reg(instr_get_dst(where, 0));

    // get shadow register address of reg2 and place it to %sreg2%   
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg2, sreg2);

    // place constant imm to register %simm2%
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_move        // mov simm2, imm
                             (drcontext,
                              opnd_create_reg(simm2),               // dst: simm2
                              opnd_create_immed_int(0, OPSZ_1)));   // src: imm
    
    // move to shadow register of reg2 the value of imm1
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte // str simm2, [sreg2]
                             (drcontext,
                              OPND_CREATE_MEM8(sreg2, 0),           // dst_mem: sreg2
                              opnd_create_reg(simm2)));             // src_reg: simm2
}
 
static void
propagate_arith_imm_reg(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    add reg2, imm, reg1

    Need to mark reg2 tainted
*/
{
    auto sreg2 = drreg_reservation { ilist, where };
    auto sreg1 = drreg_reservation { ilist, where };
    reg_id_t reg2 = opnd_get_reg(instr_get_dst(where, 0));
    reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 1));

    // get value of shadow register of reg1 and place it to %sreg1%
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);

    // get shadow register address of reg2 and place it to %sreg2%   
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg2, sreg2);

    // write the result to shadow register of reg2
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte // str sreg1, [sreg2]
                             (drcontext,
                              OPND_CREATE_MEM8(sreg2, 0),           // dst_mem: sreg2
                              opnd_create_reg(sreg1)));             // src_reg: sreg1
}

// ?
static void
propagate_arith_reg_imm(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    add reg2, reg1, imm

    Need to mark reg2 tainted
*/
{
    auto sreg2 = drreg_reservation { ilist, where };
    auto sreg1 = drreg_reservation { ilist, where };
    reg_id_t reg2 = opnd_get_reg(instr_get_dst(where, 0));
    reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 0));

    // get value of shadow register of reg1 and place it to %sreg1%
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);
    
    // get shadow register address of reg2 and place it to %sreg2% 
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg2, sreg2);

    // write the result to shadow register of reg2
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte // str sreg1, [sreg2]
                             (drcontext,                
                              OPND_CREATE_MEM8(sreg2, 0),           // dst_mem: sreg2
                              opnd_create_reg(sreg1)));             // src_reg: sreg1
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
    
    auto sreg1 = drreg_reservation { ilist, where };
    auto sreg2 = drreg_reservation { ilist, where };
    reg_id_t sreg3 = sreg2;
    reg_id_t sreg4 = sreg3; /* we reuse a register for this */

    reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 2));
    reg_id_t reg2 = opnd_get_reg(instr_get_src(where, 1));
    reg_id_t reg3 = opnd_get_reg(instr_get_src(where, 0));
    reg_id_t reg4 = opnd_get_reg(instr_get_dst(where, 0));

    // get value of shadow registers of reg1, reg2 and place it to %sreg1%, %sreg2%  
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg2, sreg2);
    
    instrlist_meta_preinsert(ilist, where, INSTR_CREATE_orr     // sreg1 = sreg1 || sreg2
                             (drcontext,
                              opnd_create_reg(sreg1),           // dst:  sreg1
                              opnd_create_reg(sreg2),           // src1: sreg2
                              opnd_create_reg(sreg1)));         // src2: sreg1
    
    // get value of shadow register of reg3 and place it to %sreg3%  
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg3, sreg3);
    
    instrlist_meta_preinsert(ilist, where, INSTR_CREATE_orr     // sreg1 = sreg3 || sreg1
                             (drcontext,
                              opnd_create_reg(sreg1),           // dst:  sreg1
                              opnd_create_reg(sreg3),           // src1: sreg3
                              opnd_create_reg(sreg1)));         // src2: sreg1
    
    // get address of shadow register of reg4 and place it to %sreg4%  
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg4, sreg4);
    
    // save the result to shadow register of reg4
    instrlist_meta_preinsert_xl8(ilist, where, XINST_CREATE_store_1byte // str sreg1, [sreg4]
                                 (drcontext,
                                  OPND_CREATE_MEM8(sreg4, 0),           // dst_mem: sreg4
                                  opnd_create_reg(sreg1)));             // src_reg: sreg1
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
    auto sreg1 = drreg_reservation { ilist, where };
    auto sreg2 = drreg_reservation { ilist, where };
    auto sreg3 = drreg_reservation { ilist, where };
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
                              opnd_create_reg(sreg1),       // dst:  sreg1
                              opnd_create_reg(sreg2),       // src1: sreg2
                              opnd_create_reg(sreg1)));     // src2: sreg1
    
    // get address of shadow register of reg3 and place it to %sreg3%
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg3, sreg3);
    
    // save the higher part of result to shadow register of reg3
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte // str sreg1, [sreg3]
                             (drcontext,
                              OPND_CREATE_MEM8(sreg3, 0),
                              opnd_create_reg(sreg1)));
    
    // get address of shadow register of reg4 and place it to %sreg4%
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg4, sreg4);
    
    // save the the lower part of result to shadow register of reg4
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte // str sreg1, [sreg4]
                             (drcontext,
                              OPND_CREATE_MEM8(sreg4, 0),           
                              opnd_create_reg(sreg1)));             
}

static void
propagate_arith_reg_reg(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
/*
    add reg3, reg2, reg1

    Need to mark reg3 tainted. 
    Because its value depends on values of reg2, reg1, 
    we use OR to combine their impacts to reg3
*/
{
    
    auto sreg2 = drreg_reservation { ilist, where };
    auto sreg1 = drreg_reservation { ilist, where };
    reg_id_t sreg3 = sreg2; /* we reuse a register for this */
    reg_id_t reg3 = opnd_get_reg(instr_get_dst(where, 0));
    reg_id_t reg2 = opnd_get_reg(instr_get_src(where, 0));
    reg_id_t reg1 = opnd_get_reg(instr_get_src(where, 1));

    // get value of shadow registers of reg1, reg2 and place it to %sreg1%, %sreg2%  
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg1, sreg1);
    drtaint_insert_reg_to_taint_load(drcontext, ilist, where, reg2, sreg2);

    
    instrlist_meta_preinsert(ilist, where, INSTR_CREATE_orr // sreg1 |= sreg2
                             (drcontext,
                              opnd_create_reg(sreg1),
                              opnd_create_reg(sreg2),
                              opnd_create_reg(sreg1)));
    
    // save tainted bits in shadow register of reg3
    drtaint_insert_reg_to_taint(drcontext, ilist, where, reg3, sreg3);
    instrlist_meta_preinsert(ilist, where, XINST_CREATE_store_1byte // str sreg1, [sreg3]
                             (drcontext,
                              OPND_CREATE_MEM8(sreg3, 0),
                              opnd_create_reg(sreg1)));
}

static bool
instr_is_simd(instr_t *where)
{
    switch (instr_get_opcode(where)) {
    case OP_vaba_s16:
    case OP_vaba_s32:
    case OP_vaba_s8:
    case OP_vaba_u16:
    case OP_vaba_u32:
    case OP_vaba_u8:
    case OP_vabal_s16:
    case OP_vabal_s32:
    case OP_vabal_s8:
    case OP_vabal_u16:
    case OP_vabal_u32:
    case OP_vabal_u8:
    case OP_vabd_s16:
    case OP_vabd_s32:
    case OP_vabd_s8:
    case OP_vabd_u16:
    case OP_vabd_u32:
    case OP_vabd_u8:
    case OP_vabdl_s16:
    case OP_vabdl_s32:
    case OP_vabdl_s8:
    case OP_vabdl_u16:
    case OP_vabdl_u32:
    case OP_vabdl_u8:
    case OP_vabs_f32:
    case OP_vabs_f64:
    case OP_vabs_s16:
    case OP_vabs_s32:
    case OP_vabs_s8:
    case OP_vacge_f32:
    case OP_vacgt_f32:
    case OP_vadd_f32:
    case OP_vadd_f64:
    case OP_vadd_i16:
    case OP_vadd_i32:
    case OP_vadd_i64:
    case OP_vadd_i8:
    case OP_vaddhn_i16:
    case OP_vaddhn_i32:
    case OP_vaddhn_i64:
    case OP_vaddl_s16:
    case OP_vaddl_s32:
    case OP_vaddl_s8:
    case OP_vaddl_u16:
    case OP_vaddl_u32:
    case OP_vaddl_u8:
    case OP_vaddw_s16:
    case OP_vaddw_s32:
    case OP_vaddw_s8:
    case OP_vaddw_u16:
    case OP_vaddw_u32:
    case OP_vaddw_u8:
    case OP_vand:
    case OP_vbic:
    case OP_vbic_i16:
    case OP_vbic_i32:
    case OP_vbif:
    case OP_vbit:
    case OP_vbsl:
    case OP_vceq_f32:
    case OP_vceq_i16:
    case OP_vceq_i32:
    case OP_vceq_i8:
    case OP_vcge_f32:
    case OP_vcge_s16:
    case OP_vcge_s32:
    case OP_vcge_s8:
    case OP_vcge_u16:
    case OP_vcge_u32:
    case OP_vcge_u8:
    case OP_vcgt_f32:
    case OP_vcgt_s16:
    case OP_vcgt_s32:
    case OP_vcgt_s8:
    case OP_vcgt_u16:
    case OP_vcgt_u32:
    case OP_vcgt_u8:
    case OP_vcle_f32:
    case OP_vcle_s16:
    case OP_vcle_s32:
    case OP_vcle_s8:
    case OP_vcls_s16:
    case OP_vcls_s32:
    case OP_vcls_s8:
    case OP_vclt_f32:
    case OP_vclt_s16:
    case OP_vclt_s32:
    case OP_vclt_s8:
    case OP_vclz_i16:
    case OP_vclz_i32:
    case OP_vclz_i8:
    case OP_vcmp_f32:
    case OP_vcmp_f64:
    case OP_vcmpe_f32:
    case OP_vcmpe_f64:
    case OP_vcnt_8:
    case OP_vcvt_f16_f32:
    case OP_vcvt_f32_f16:
    case OP_vcvt_f32_f64:
    case OP_vcvt_f32_s16:
    case OP_vcvt_f32_s32:
    case OP_vcvt_f32_u16:
    case OP_vcvt_f32_u32:
    case OP_vcvt_f64_f32:
    case OP_vcvt_f64_s16:
    case OP_vcvt_f64_s32:
    case OP_vcvt_f64_u16:
    case OP_vcvt_f64_u32:
    case OP_vcvt_s16_f32:
    case OP_vcvt_s16_f64:
    case OP_vcvt_s32_f32:
    case OP_vcvt_s32_f64:
    case OP_vcvt_u16_f32:
    case OP_vcvt_u16_f64:
    case OP_vcvt_u32_f32:
    case OP_vcvt_u32_f64:
    case OP_vcvta_s32_f32:
    case OP_vcvta_s32_f64:
    case OP_vcvta_u32_f32:
    case OP_vcvta_u32_f64:
    case OP_vcvtb_f16_f32:
    case OP_vcvtb_f16_f64:
    case OP_vcvtb_f32_f16:
    case OP_vcvtb_f64_f16:
    case OP_vcvtm_s32_f32:
    case OP_vcvtm_s32_f64:
    case OP_vcvtm_u32_f32:
    case OP_vcvtm_u32_f64:
    case OP_vcvtn_s32_f32:
    case OP_vcvtn_s32_f64:
    case OP_vcvtn_u32_f32:
    case OP_vcvtn_u32_f64:
    case OP_vcvtp_s32_f32:
    case OP_vcvtp_s32_f64:
    case OP_vcvtp_u32_f32:
    case OP_vcvtp_u32_f64:
    case OP_vcvtr_s32_f32:
    case OP_vcvtr_s32_f64:
    case OP_vcvtr_u32_f32:
    case OP_vcvtr_u32_f64:
    case OP_vcvtt_f16_f32:
    case OP_vcvtt_f16_f64:
    case OP_vcvtt_f32_f16:
    case OP_vcvtt_f64_f16:
    case OP_vdiv_f32:
    case OP_vdiv_f64:
    case OP_vdup_16:
    case OP_vdup_32:
    case OP_vdup_8:
    case OP_veor:
    case OP_vext:
    case OP_vfma_f32:
    case OP_vfma_f64:
    case OP_vfms_f32:
    case OP_vfms_f64:
    case OP_vfnma_f32:
    case OP_vfnma_f64:
    case OP_vfnms_f32:
    case OP_vfnms_f64:
    case OP_vhadd_s16:
    case OP_vhadd_s32:
    case OP_vhadd_s8:
    case OP_vhadd_u16:
    case OP_vhadd_u32:
    case OP_vhadd_u8:
    case OP_vhsub_s16:
    case OP_vhsub_s32:
    case OP_vhsub_s8:
    case OP_vhsub_u16:
    case OP_vhsub_u32:
    case OP_vhsub_u8:
    case OP_vld1_16:
    case OP_vld1_32:
    case OP_vld1_64:
    case OP_vld1_8:
    case OP_vld1_dup_16:
    case OP_vld1_dup_32:
    case OP_vld1_dup_8:
    case OP_vld1_lane_16:
    case OP_vld1_lane_32:
    case OP_vld1_lane_8:
    case OP_vld2_16:
    case OP_vld2_32:
    case OP_vld2_8:
    case OP_vld2_dup_16:
    case OP_vld2_dup_32:
    case OP_vld2_dup_8:
    case OP_vld2_lane_16:
    case OP_vld2_lane_32:
    case OP_vld2_lane_8:
    case OP_vld3_16:
    case OP_vld3_32:
    case OP_vld3_8:
    case OP_vld3_dup_16:
    case OP_vld3_dup_32:
    case OP_vld3_dup_8:
    case OP_vld3_lane_16:
    case OP_vld3_lane_32:
    case OP_vld3_lane_8:
    case OP_vld4_16:
    case OP_vld4_32:
    case OP_vld4_8:
    case OP_vld4_dup_16:
    case OP_vld4_dup_32:
    case OP_vld4_dup_8:
    case OP_vld4_lane_16:
    case OP_vld4_lane_32:
    case OP_vld4_lane_8:
    case OP_vldm:
    case OP_vldmdb:
    case OP_vldr:
    case OP_vmax_f32:
    case OP_vmax_s16:
    case OP_vmax_s32:
    case OP_vmax_s8:
    case OP_vmax_u16:
    case OP_vmax_u32:
    case OP_vmax_u8:
    case OP_vmaxnm_f32:
    case OP_vmaxnm_f64:
    case OP_vmin_f32:
    case OP_vmin_s16:
    case OP_vmin_s32:
    case OP_vmin_s8:
    case OP_vmin_u16:
    case OP_vmin_u32:
    case OP_vmin_u8:
    case OP_vminnm_f32:
    case OP_vminnm_f64:
    case OP_vmla_f32:
    case OP_vmla_f64:
    case OP_vmla_i16:
    case OP_vmla_i32:
    case OP_vmla_i8:
    case OP_vmlal_s16:
    case OP_vmlal_s32:
    case OP_vmlal_s8:
    case OP_vmlal_u16:
    case OP_vmlal_u32:
    case OP_vmlal_u8:
    case OP_vmls_f32:
    case OP_vmls_f64:
    case OP_vmls_i16:
    case OP_vmls_i32:
    case OP_vmls_i8:
    case OP_vmlsl_s16:
    case OP_vmlsl_s32:
    case OP_vmlsl_s8:
    case OP_vmlsl_u16:
    case OP_vmlsl_u32:
    case OP_vmlsl_u8:
    case OP_vmov:
    case OP_vmov_16:
    case OP_vmov_32:
    case OP_vmov_8:
    case OP_vmov_f32:
    case OP_vmov_f64:
    case OP_vmov_i16:
    case OP_vmov_i32:
    case OP_vmov_i64:
    case OP_vmov_i8:
    case OP_vmov_s16:
    case OP_vmov_s8:
    case OP_vmov_u16:
    case OP_vmov_u8:
    case OP_vmovl_s16:
    case OP_vmovl_s32:
    case OP_vmovl_s8:
    case OP_vmovl_u16:
    case OP_vmovl_u32:
    case OP_vmovl_u8:
    case OP_vmovn_i16:
    case OP_vmovn_i32:
    case OP_vmovn_i64:
    case OP_vmrs:
    case OP_vmsr:
    case OP_vmul_f32:
    case OP_vmul_f64:
    case OP_vmul_i16:
    case OP_vmul_i32:
    case OP_vmul_i8:
    case OP_vmul_p32:
    case OP_vmul_p8:
    case OP_vmull_p32:
    case OP_vmull_p8:
    case OP_vmull_s16:
    case OP_vmull_s32:
    case OP_vmull_s8:
    case OP_vmull_u16:
    case OP_vmull_u32:
    case OP_vmull_u8:
    case OP_vmvn:
    case OP_vmvn_i16:
    case OP_vmvn_i32:
    case OP_vneg_f32:
    case OP_vneg_f64:
    case OP_vneg_s16:
    case OP_vneg_s32:
    case OP_vneg_s8:
    case OP_vnmla_f32:
    case OP_vnmla_f64:
    case OP_vnmls_f32:
    case OP_vnmls_f64:
    case OP_vnmul_f32:
    case OP_vnmul_f64:
    case OP_vorn:
    case OP_vorr:
    case OP_vorr_i16:
    case OP_vorr_i32:
    case OP_vpadal_s16:
    case OP_vpadal_s32:
    case OP_vpadal_s8:
    case OP_vpadal_u16:
    case OP_vpadal_u32:
    case OP_vpadal_u8:
    case OP_vpadd_f32:
    case OP_vpadd_i16:
    case OP_vpadd_i32:
    case OP_vpadd_i8:
    case OP_vpaddl_s16:
    case OP_vpaddl_s32:
    case OP_vpaddl_s8:
    case OP_vpaddl_u16:
    case OP_vpaddl_u32:
    case OP_vpaddl_u8:
    case OP_vpmax_f32:
    case OP_vpmax_s16:
    case OP_vpmax_s32:
    case OP_vpmax_s8:
    case OP_vpmax_u16:
    case OP_vpmax_u32:
    case OP_vpmax_u8:
    case OP_vpmin_f32:
    case OP_vpmin_s16:
    case OP_vpmin_s32:
    case OP_vpmin_s8:
    case OP_vpmin_u16:
    case OP_vpmin_u32:
    case OP_vpmin_u8:
    case OP_vqabs_s16:
    case OP_vqabs_s32:
    case OP_vqabs_s8:
    case OP_vqadd_s16:
    case OP_vqadd_s32:
    case OP_vqadd_s64:
    case OP_vqadd_s8:
    case OP_vqadd_u16:
    case OP_vqadd_u32:
    case OP_vqadd_u64:
    case OP_vqadd_u8:
    case OP_vqdmlal_s16:
    case OP_vqdmlal_s32:
    case OP_vqdmlsl_s16:
    case OP_vqdmlsl_s32:
    case OP_vqdmulh_s16:
    case OP_vqdmulh_s32:
    case OP_vqdmull_s16:
    case OP_vqdmull_s32:
    case OP_vqmovn_s16:
    case OP_vqmovn_s32:
    case OP_vqmovn_s64:
    case OP_vqmovn_u16:
    case OP_vqmovn_u32:
    case OP_vqmovn_u64:
    case OP_vqmovun_s16:
    case OP_vqmovun_s32:
    case OP_vqmovun_s64:
    case OP_vqneg_s16:
    case OP_vqneg_s32:
    case OP_vqneg_s8:
    case OP_vqrdmulh_s16:
    case OP_vqrdmulh_s32:
    case OP_vqrshl_s16:
    case OP_vqrshl_s32:
    case OP_vqrshl_s64:
    case OP_vqrshl_s8:
    case OP_vqrshl_u16:
    case OP_vqrshl_u32:
    case OP_vqrshl_u64:
    case OP_vqrshl_u8:
    case OP_vqrshrn_s16:
    case OP_vqrshrn_s32:
    case OP_vqrshrn_s64:
    case OP_vqrshrn_u16:
    case OP_vqrshrn_u32:
    case OP_vqrshrn_u64:
    case OP_vqrshrun_s16:
    case OP_vqrshrun_s32:
    case OP_vqrshrun_s64:
    case OP_vqshl_s16:
    case OP_vqshl_s32:
    case OP_vqshl_s64:
    case OP_vqshl_s8:
    case OP_vqshl_u16:
    case OP_vqshl_u32:
    case OP_vqshl_u64:
    case OP_vqshl_u8:
    case OP_vqshlu_s16:
    case OP_vqshlu_s32:
    case OP_vqshlu_s64:
    case OP_vqshlu_s8:
    case OP_vqshrn_s16:
    case OP_vqshrn_s32:
    case OP_vqshrn_s64:
    case OP_vqshrn_u16:
    case OP_vqshrn_u32:
    case OP_vqshrn_u64:
    case OP_vqshrun_s16:
    case OP_vqshrun_s32:
    case OP_vqshrun_s64:
    case OP_vqsub_s16:
    case OP_vqsub_s32:
    case OP_vqsub_s64:
    case OP_vqsub_s8:
    case OP_vqsub_u16:
    case OP_vqsub_u32:
    case OP_vqsub_u64:
    case OP_vqsub_u8:
    case OP_vraddhn_i16:
    case OP_vraddhn_i32:
    case OP_vraddhn_i64:
    case OP_vrecpe_f32:
    case OP_vrecpe_u32:
    case OP_vrecps_f32:
    case OP_vrev16_16:
    case OP_vrev16_8:
    case OP_vrev32_16:
    case OP_vrev32_32:
    case OP_vrev32_8:
    case OP_vrev64_16:
    case OP_vrev64_32:
    case OP_vrev64_8:
    case OP_vrhadd_s16:
    case OP_vrhadd_s32:
    case OP_vrhadd_s8:
    case OP_vrhadd_u16:
    case OP_vrhadd_u32:
    case OP_vrhadd_u8:
    case OP_vrinta_f32_f32:
    case OP_vrinta_f64_f64:
    case OP_vrintm_f32_f32:
    case OP_vrintm_f64_f64:
    case OP_vrintn_f32_f32:
    case OP_vrintn_f64_f64:
    case OP_vrintp_f32_f32:
    case OP_vrintp_f64_f64:
    case OP_vrintr_f32:
    case OP_vrintr_f64:
    case OP_vrintx_f32:
    case OP_vrintx_f32_f32:
    case OP_vrintx_f64:
    case OP_vrintz_f32:
    case OP_vrintz_f32_f32:
    case OP_vrintz_f64:
    case OP_vrshl_s16:
    case OP_vrshl_s32:
    case OP_vrshl_s64:
    case OP_vrshl_s8:
    case OP_vrshl_u16:
    case OP_vrshl_u32:
    case OP_vrshl_u64:
    case OP_vrshl_u8:
    case OP_vrshr_s16:
    case OP_vrshr_s32:
    case OP_vrshr_s64:
    case OP_vrshr_s8:
    case OP_vrshr_u16:
    case OP_vrshr_u32:
    case OP_vrshr_u64:
    case OP_vrshr_u8:
    case OP_vrshrn_i16:
    case OP_vrshrn_i32:
    case OP_vrshrn_i64:
    case OP_vrsqrte_f32:
    case OP_vrsqrte_u32:
    case OP_vrsqrts_f32:
    case OP_vrsra_s16:
    case OP_vrsra_s32:
    case OP_vrsra_s64:
    case OP_vrsra_s8:
    case OP_vrsra_u16:
    case OP_vrsra_u32:
    case OP_vrsra_u64:
    case OP_vrsra_u8:
    case OP_vrsubhn_i16:
    case OP_vrsubhn_i32:
    case OP_vrsubhn_i64:
    case OP_vsel_eq_f32:
    case OP_vsel_eq_f64:
    case OP_vsel_ge_f32:
    case OP_vsel_ge_f64:
    case OP_vsel_gt_f32:
    case OP_vsel_gt_f64:
    case OP_vsel_vs_f32:
    case OP_vsel_vs_f64:
    case OP_vshl_i16:
    case OP_vshl_i32:
    case OP_vshl_i64:
    case OP_vshl_i8:
    case OP_vshl_s16:
    case OP_vshl_s32:
    case OP_vshl_s64:
    case OP_vshl_s8:
    case OP_vshl_u16:
    case OP_vshl_u32:
    case OP_vshl_u64:
    case OP_vshl_u8:
    case OP_vshll_i16:
    case OP_vshll_i32:
    case OP_vshll_i8:
    case OP_vshll_s16:
    case OP_vshll_s32:
    case OP_vshll_s8:
    case OP_vshll_u16:
    case OP_vshll_u32:
    case OP_vshll_u8:
    case OP_vshr_s16:
    case OP_vshr_s32:
    case OP_vshr_s64:
    case OP_vshr_s8:
    case OP_vshr_u16:
    case OP_vshr_u32:
    case OP_vshr_u64:
    case OP_vshr_u8:
    case OP_vshrn_i16:
    case OP_vshrn_i32:
    case OP_vshrn_i64:
    case OP_vsli_16:
    case OP_vsli_32:
    case OP_vsli_64:
    case OP_vsli_8:
    case OP_vsqrt_f32:
    case OP_vsqrt_f64:
    case OP_vsra_s16:
    case OP_vsra_s32:
    case OP_vsra_s64:
    case OP_vsra_s8:
    case OP_vsra_u16:
    case OP_vsra_u32:
    case OP_vsra_u64:
    case OP_vsra_u8:
    case OP_vsri_16:
    case OP_vsri_32:
    case OP_vsri_64:
    case OP_vsri_8:
    case OP_vst1_16:
    case OP_vst1_32:
    case OP_vst1_64:
    case OP_vst1_8:
    case OP_vst1_lane_16:
    case OP_vst1_lane_32:
    case OP_vst1_lane_8:
    case OP_vst2_16:
    case OP_vst2_32:
    case OP_vst2_8:
    case OP_vst2_lane_16:
    case OP_vst2_lane_32:
    case OP_vst2_lane_8:
    case OP_vst3_16:
    case OP_vst3_32:
    case OP_vst3_8:
    case OP_vst3_lane_16:
    case OP_vst3_lane_32:
    case OP_vst3_lane_8:
    case OP_vst4_16:
    case OP_vst4_32:
    case OP_vst4_8:
    case OP_vst4_lane_16:
    case OP_vst4_lane_32:
    case OP_vst4_lane_8:
    case OP_vstm:
    case OP_vstmdb:
    case OP_vstr:
    case OP_vsub_f32:
    case OP_vsub_f64:
    case OP_vsub_i16:
    case OP_vsub_i32:
    case OP_vsub_i64:
    case OP_vsub_i8:
    case OP_vsubhn_i16:
    case OP_vsubhn_i32:
    case OP_vsubhn_i64:
    case OP_vsubl_s16:
    case OP_vsubl_s32:
    case OP_vsubl_s8:
    case OP_vsubl_u16:
    case OP_vsubl_u32:
    case OP_vsubl_u8:
    case OP_vsubw_s16:
    case OP_vsubw_s32:
    case OP_vsubw_s8:
    case OP_vsubw_u16:
    case OP_vsubw_u32:
    case OP_vsubw_u8:
    case OP_vswp:
    case OP_vtbl_8:
    case OP_vtbx_8:
    case OP_vtrn_16:
    case OP_vtrn_32:
    case OP_vtrn_8:
    case OP_vtst_16:
    case OP_vtst_32:
    case OP_vtst_8:
    case OP_vuzp_16:
    case OP_vuzp_32:
    case OP_vuzp_8:
    case OP_vzip_16:
    case OP_vzip_32:
    case OP_vzip_8:
        return true;
    default:
        return false;
    }
}

// decrement before, increment after, decrement after, increment before  
typedef enum { DB, IA, DA, IB } stack_dir_t;

template <stack_dir_t T> app_pc
calculate_addr(instr_t *instr, void *base, int i, int top)
{ DR_ASSERT_MSG(false, "Unreachable"); }

template <> app_pc
calculate_addr<DB>(instr_t *instr, void *base, int i, int top)
{ return (app_pc)base - 4 * (top - i - 1); }

template <> app_pc
calculate_addr<IA>(instr_t *instr, void *base, int i, int top)
{ return (app_pc)base + 4 * i; }

/* XXX: these are probably not correct */
template <> app_pc
calculate_addr<DA>(instr_t *instr, void *base, int i, int top)
{ return (app_pc)base - 4 * i; }

template <> app_pc
calculate_addr<IB>(instr_t *instr, void *base, int i, int top)
{ return (app_pc)base + 4 * (top - i - 1); }


template <stack_dir_t c> void
propagate_ldm_cc_template(void *pc, void *base, bool writeback)
/*
    ldm r, { regs }

    When handling a ldm command we have to save all values of registers
    that will be popped from a stack to their shadow registers
*/
{
    void *drcontext = dr_get_current_drcontext();
    instr_t *instr = instr_create(drcontext);

    decode(drcontext, (byte *)pc, instr);

    for (int i = 0; i < instr_num_dsts(instr); ++i)
    {
        bool ok;
        
        // ? почему 1, а не 0
        // when executing assembler commands in 16bit thumb mode 
        // (writeback = true) the register r must not to be in {regs} list 
        if (writeback &&
            (opnd_get_reg(instr_get_dst(instr, i)) ==
             opnd_get_reg(instr_get_src(instr, 1))))
            break;

        // set taint from stack to the appropriate register 
        byte res;
        int top = writeback ?
            instr_num_dsts(instr) :
            instr_num_dsts(instr) - 1;
       
        ok = drtaint_get_app_taint(drcontext, calculate_addr<c>(instr, base, i, top), &res);
        DR_ASSERT(ok);
        ok = drtaint_set_reg_taint(drcontext, opnd_get_reg(instr_get_dst(instr, i)), res);
        DR_ASSERT(ok);
    }

    instr_destroy(drcontext, instr);
}

template <stack_dir_t c> void
propagate_stm_cc_template(void *pc, void *base, bool writeback)
/*
    stm r, { regs }
    
    When handling a stm command we have to set all memory 
    in the stack where the register values will be written tainted
*/
{
    void *drcontext = dr_get_current_drcontext();
    instr_t *instr = instr_create(drcontext);

    decode(drcontext, (byte *)pc, instr);

    for (int i = 0; i < instr_num_srcs(instr); ++i)
    {
        bool ok;

        // when executing assembler commands in 16bit thumb mode 
        // (writeback = true) the register r must not to be in {regs} list 
        if (writeback &&
            (opnd_get_reg(instr_get_src(instr, i)) ==
             opnd_get_reg(instr_get_dst(instr, 1))))
            break;

        // set taint from registers to the stack 
        byte res;
        ok = drtaint_get_reg_taint(drcontext, opnd_get_reg(instr_get_src(instr, i)), &res);
        DR_ASSERT(ok);
        
        int top = writeback ?
            instr_num_srcs(instr) :
            instr_num_srcs(instr) - 1;
        
        ok = drtaint_set_app_taint(drcontext, calculate_addr<c>(instr, base, i, top), res);
        DR_ASSERT(ok);
    }
    instr_destroy(drcontext, instr);
}

static bool
instr_handle_constant_func(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where)
{
    short opcode = instr_get_opcode(where);
    
    if (opcode == OP_eor  ||
        opcode == OP_eors ||
        opcode == OP_sub  ||
        opcode == OP_subs ||
        opcode == OP_sbc  ||
        opcode == OP_sbcs) 
    { 
        /* xor r1, r0, r0 */
        if (!opnd_is_reg(instr_get_src(where, 0)))
            return false;
        
        if (!opnd_is_reg(instr_get_src(where, 1)))
            return false;
        
        if (opnd_get_reg(instr_get_src(where, 0)) !=
            opnd_get_reg(instr_get_src(where, 1)))
            return false;
        
        /*mov r1, imm */
        propagate_mov_imm_src(drcontext, tag, ilist, where);
        return true;
    }
    return false;
}

static dr_emit_flags_t
event_app_instruction(void *drcontext, void *tag, instrlist_t *ilist, instr_t *where,
                      bool for_trace, bool translating, void *user_data)
{
    // no support for simd instructions
    if (instr_is_simd(where)) {
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
    do {                                                                \
        reg_id_t swap = DR_REG_NULL;                                    \
        drreg_restore_app_values(drcontext, ilist, where, opnd, &swap); \
        cb;                                                             \
        if (swap != DR_REG_NULL)                                        \
            drreg_unreserve_register(drcontext, ilist, where, swap);    \
    } while (false);

    switch (instr_get_opcode(where)) {
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
    case OP_ldrd:
    case OP_ldrh:
    case OP_ldrsh:
    case OP_ldrsb:
    case OP_ldrex:
        propagate_ldr(drcontext, tag, ilist, where);
        break;

    case OP_str:
    case OP_strb:
    case OP_strd:
    case OP_strh:
    case OP_strex:
        /* For OP_strex, failure is written to a second dst operand,
         * but this isn't controllable.
         */
        propagate_str(drcontext, tag, ilist, where);
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
        /* Some of these also write to eflags. If we taint eflags
         * we should do it here.
         */
        DR_ASSERT(instr_num_srcs(where) == 2 || instr_num_srcs(where) == 4);
        DR_ASSERT(instr_num_dsts(where) == 1);
        if (opnd_is_reg(instr_get_src(where, 0))) {
            if (opnd_is_reg(instr_get_src(where, 1)))
                propagate_arith_reg_reg(drcontext, tag, ilist, where);
            else
                propagate_arith_reg_imm(drcontext, tag, ilist, where);
        } else if (opnd_is_reg(instr_get_src(where, 1)))
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
        if (opnd_is_reg(instr_get_src(where, 0))) {
            propagate_mov_regs(drcontext, tag, ilist, where,
                               opnd_get_reg(instr_get_src(where, 0)),
                               DR_REG_PC);
        }
        /* we don't have to do anything for immediates */
        break;

    case OP_cbz:
    case OP_cbnz:
        /* Nothing to do here, unless we want to support tainting
         * eflags.
         */
        break;
    case OP_cmn:
    case OP_cmp:
    case OP_tst:
    case OP_it:
        /* Nothing to do here, unless we want to support tainting
         * eflags.
         */
        break;

    case OP_LABEL:
    case OP_svc:
    case OP_ldc:
    case OP_mcr:
    case OP_mrc:
    case OP_nop:
    case OP_pld:
    case OP_dmb:
        break;

    case OP_bfi:
    case OP_bfc:
    case OP_teq:
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
// ?
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
    
    if (TEST(arg->mode, DRSYS_PARAM_OUT)) {
        char *buffer = (char *)arg->start_addr;
        for (int i = 0; i < arg->size; ++i) {
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
    dr_syscall_result_info_t info = { sizeof(info), };
    dr_syscall_get_result_ex(drcontext, &info);

    /* all syscalls untaint rax */
    drtaint_set_reg_taint(drcontext, DR_REG_R0, 0);

    if (!info.succeeded) {
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




