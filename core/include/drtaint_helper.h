#ifndef DRTAINT_HELPER_H_
#define DRTAINT_HELPER_H_

#include "dr_api.h"
#include "drreg.h"

#define TEST(mask, var) (((mask) & (var)) != 0)

#define IS_BIT_UP(word, bit) (((word) & (1 << bit)))
#define IS_BIT_DOWN(word, bit) (!((word) & (1 << bit)))

class drreg_reservation
{
private:
    void *drcontext = nullptr;
    instrlist_t *ilist = nullptr;
    instr_t *where = nullptr;
    reg_id_t reg = DR_REG_NULL;

public:
    drreg_reservation(void *drcontext, instrlist_t *ilist, instr_t *where);

    ~drreg_reservation();

    operator reg_id_t() const { return reg; }

    void unreserve();
};

class instr_decoded
{
private:
    void *drcontext = nullptr;
    instr_t* instr = nullptr;

public:
    instr_decoded(void* drcontext, app_pc pc);

    ~instr_decoded();
    
    operator instr_t*() const { return instr; }

    void destroy();
};

bool instr_is_simd(instr_t *where);

bool ldr_is_pre_addr(uint raw_instr_bits);

bool ldr_is_post_addr(uint raw_instr_bits);

bool ldr_is_pre_or_offs_addr(uint raw_instr_bits);

bool ldr_is_offs_addr(uint raw_instr_bits);

void unimplemented_opcode(instr_t *where);

// Always use this when adding instrumentation which stores to shadow app!
void instrlist_meta_preinsert_xl8(instrlist_t *ilist, instr_t *where, instr_t *insert);

void what_are_srcs(instr_t *where);

void what_are_dsts(instr_t *where);

#endif
