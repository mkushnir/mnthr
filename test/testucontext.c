#include <sys/mman.h>
#include <mncommon/btrie.h>
#include "mnthr_private.h"

#ifdef __clang__
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

static char *uc0_stack;
static char *uc1_stack;
ucontext_t uc0, uc1;

int
f(void)
{
    printf("qwe\n");
    swapcontext(&uc1, &uc0);
    printf("qwe1\n");
    return 0;
}

int
g(void)
{
    printf("asd0\n");
    swapcontext(&uc0, &uc1);
    printf("asd1\n");
    swapcontext(&uc0, &uc1);
    printf("asd2\n");
    return 0;
}

int
main(void)
{
    int res;

    uc0_stack = mmap(NULL, STACKSIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    uc1_stack = mmap(NULL, STACKSIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    printf("uc0_stack=%p uc1_stack=%p\n", uc0_stack, uc1_stack);

    memset(&uc0, 0x00, sizeof(ucontext_t));
    memset(&uc1, 0x00, sizeof(ucontext_t));

    res = getcontext(&uc0);
    uc0.uc_link = NULL;
    uc0.uc_stack.ss_sp = uc0_stack;
    uc0.uc_stack.ss_size = STACKSIZE;
    printf("res=%d flags=%d\n", res, uc0.uc_stack.ss_flags);
    makecontext(&uc0, (void(*)(void))g, 0);

    res = getcontext(&uc1);
    printf("res=%d\n", res);
    uc1.uc_link = &uc0;
    uc1.uc_stack.ss_sp = uc1_stack;
    uc1.uc_stack.ss_size = STACKSIZE;
    makecontext(&uc1, (void(*)(void))f, 0);

    setcontext(&uc0);

    return 0;
}
