#include <unistd.h>
#include <assert.h>

#define DRTAINT_SUCCESS 0xAA
#define FD_APP_START_TRACE 0xFFFFEEEE
#define FD_APP_STOP_TRACE 0xFFFFEEED
#define FD_APP_IS_TRACED 0xFFFFEEEF

#define MAKE_TAINTED(mem, mem_sz)                        \
    do                                                   \
    {                                                    \
        unsigned status = 0;                             \
        status = write(FD_APP_START_TRACE, mem, mem_sz); \
        assert(status == DRTAINT_SUCCESS);               \
    } while (0)

#define CLEAR(mem, mem_sz)                              \
    do                                                  \
    {                                                   \
        unsigned status = 0;                            \
        status = write(FD_APP_STOP_TRACE, mem, mem_sz); \
        assert(status == DRTAINT_SUCCESS);              \
    } while (0)

#define IS_TAINTED(mem, mem_sz) \
    (write(FD_APP_IS_TRACED, mem, mem_sz) == DRTAINT_SUCCESS)

#define IS_NOT_TAINTED(mem, mem_sz) \
    ((mem_sz) == 0 ? true : !IS_TAINTED(mem, mem_sz))


#define TEST_START bool _status_ = true

#define TEST_END return _status_

#define TEST_ASSERT(q)              \
    if (!(q))                       \
    {                               \
        printf("%s: failed\n", #q); \
        _status_ = false;           \
    }                               \
    else                            \
        printf("%s: ok\n", #q)


typedef bool (*testfunc)(void);

typedef struct _Test
{
    const char *name;
    testfunc run;

} Test;

// test function prototypes
bool test_simple();
bool test_assign();
bool test_arith();
bool test_bitwise();
bool test_condex_op();
bool test_assign_ex();
bool test_struct();
bool test_func_call();
bool test_array();
bool test_untaint();
bool test_untaint_stack();

bool test_asm_ldr_imm();
bool test_asm_ldr_imm_ex();
bool test_asm_ldr_reg();
bool test_asm_ldr_reg_ex();
bool test_asm_ldrd_imm();
bool test_asm_ldrd_reg();
bool test_asm_ldrd_ex();

bool test_asm_ldm();
bool test_asm_ldm_w();
bool test_asm_ldm_ex();
bool test_asm_ldm_ex_w();

bool test_asm_str_imm();
bool test_asm_str_reg();
bool test_asm_strex();
bool test_asm_strd_imm();
bool test_asm_strd_reg();
bool test_asm_strexd();

bool test_asm_stm();
bool test_asm_stm_w();
bool test_asm_stm_ex();
bool test_asm_stm_ex_w();

bool test_asm_mov_reg();
bool test_asm_mov_imm();
bool test_asm_mov_ex();

bool test_asm_arith3_reg();
bool test_asm_arith3_imm();
bool test_asm_arith3_reg_ex();
bool test_asm_arith_1rd_3rs();
bool test_asm_arith_2rd_2rs();

bool test_asm_pkhXX();

bool test_asm_cond();