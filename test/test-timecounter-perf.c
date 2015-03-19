#include <sys/types.h>
#include <stdio.h>
#include <time.h>

static inline uint64_t
rdtsc(void)
{
  uint64_t res;

  __asm __volatile ("rdtscp; shl $32,%%rdx; or %%rdx,%%rax"
                    : "=a"(res)
                    :
                    : "%rcx", "%rdx"
                   );
  return res;
}

static inline uint64_t
_clock(void)
{
    uint64_t res = 0;
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME_PRECISE, &ts) != 0) {
        return res;
    }
    res = ts.tv_nsec + ts.tv_sec * 1000000000;
    return res;
}

int
main(void)
{
    uint64_t vals[64];
    size_t i;
#define F rdtsc
//#define F _clock

    vals[0] = F();
    vals[1] = F();
    vals[2] = F();
    vals[3] = F();
    vals[4] = F();
    vals[5] = F();
    vals[6] = F();
    vals[7] = F();
    vals[8] = F();
    vals[9] = F();
    vals[10] = F();
    vals[11] = F();
    vals[12] = F();
    vals[13] = F();
    vals[14] = F();
    vals[15] = F();
    vals[16] = F();
    vals[17] = F();
    vals[18] = F();
    vals[19] = F();
    vals[20] = F();
    vals[21] = F();
    vals[22] = F();
    vals[23] = F();
    vals[24] = F();
    vals[25] = F();
    vals[26] = F();
    vals[27] = F();
    vals[28] = F();
    vals[29] = F();
    vals[30] = F();
    vals[31] = F();
    vals[32] = F();
    vals[33] = F();
    vals[34] = F();
    vals[35] = F();
    vals[36] = F();
    vals[37] = F();
    vals[38] = F();
    vals[39] = F();
    vals[40] = F();
    vals[41] = F();
    vals[42] = F();
    vals[43] = F();
    vals[44] = F();
    vals[45] = F();
    vals[46] = F();
    vals[47] = F();
    vals[48] = F();
    vals[49] = F();
    vals[50] = F();
    vals[51] = F();
    vals[52] = F();
    vals[53] = F();
    vals[54] = F();
    vals[55] = F();
    vals[56] = F();
    vals[57] = F();
    vals[58] = F();
    vals[59] = F();
    vals[60] = F();
    vals[61] = F();
    vals[62] = F();
    vals[63] = F();

    for (i = 1; i < 64; ++i) {
        printf("%ld\n", vals[i] - vals[i - 1]);
    }

    return 0;
}
