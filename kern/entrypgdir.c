#include <inc/memlayout.h>
#include <inc/mmu.h>

pde_t entry_pgdir[NPDENTRIES] __attribute__((aligned(16 * 1024))) = {
    [0x0] = 0x00000002,
    [0x1] = 0x00100002,
    [0x2] = 0x00200002,
    [0x3] = 0x00300002,
    [0x4] = 0x00400002,
    [0x5] = 0x00500002,
    [0x6] = 0x00600002,
    [0x7] = 0x00700002,
    [0x8] = 0x00800002,
    [0x9] = 0x00900002,
    [0xa] = 0x00a00002,
    [0xb] = 0x00b00002,
    [0xc] = 0x00c00002,
    [0xd] = 0x00d00002,
    [0xe] = 0x00e00002,
    [0xf] = 0x00f00002,

    [GPIOBASE >> 20] = 0x3f200002,

    [0xf00] = 0x00000002,
    [0xf01] = 0x00100002,
    [0xf02] = 0x00200002,
    [0xf03] = 0x00300002,
    [0xf04] = 0x00400002,
    [0xf05] = 0x00500002,
    [0xf06] = 0x00600002,
    [0xf07] = 0x00700002,
    [0xf08] = 0x00800002,
    [0xf09] = 0x00900002,
    [0xf0a] = 0x00a00002,
    [0xf0b] = 0x00b00002,
    [0xf0c] = 0x00c00002,
    [0xf0d] = 0x00d00002,
    [0xf0e] = 0x00e00002,
    [0xf0f] = 0x00f00002,
};