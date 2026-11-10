// F-ASTRA FM duty early start (11/10 boot stage; user 11/09: Fault Management runs the whole duty, the PL is never off
// then). A U-Boot standalone program (`go`): the boot script runs it about 1.2 s after power-on when BOOT.BIN's FSBL
// configured the duty PL (SD f-astra-nn/duty.conf names that BOOT.BIN: boot_bin, boot_bin_size). It
//   1. touches nothing unless the PL is configured (devcfg INT_STS PCFG_DONE) and the level shifters are on,
//   2. holds the PL reset, writes FCLK1 (the duty's 1x clock) with that clock halted (FPGA1_THR_CNT bit 0), runs it,
//      releases the reset,
//   3. checks the SEM window's ID, hands the ICAP to the SEM controller (devcfg CTRL PCAP_PR = 0), clears its counters,
//      grants it, closes every clock gate (engine MMCM RUN 0) and waits for the observation state.
// Linux then adopts this PL (f-astra-fm-duty --load with boot_bin) instead of configuring it again.
// usage (U-Boot): go 0x02000000 SEM_BASE GATES_ADDR FCLK1_CTRL      hex words; GATES_ADDR 0 = no gate register
// rc: 0 observing; 1 usage; 2 PL not configured; 3 level shifters off (1..3: nothing written); 4 no SEM window at
//     SEM_BASE (FCLK1 and the reset done, ICAP not handed over); 5 granted, no observation within the wait.
// Freestanding: no library, no data or bss, r9 (U-Boot's global data) untouched (-ffixed-r9), linked at 0x02000000
// with this function first (tools/te0715-boot-pl.py builds it).
typedef unsigned int u32;

#define REG(a) (*(volatile u32 *)(a))
enum { SLCR_UNLOCK = 0xF8000008u, FPGA1_CLK = 0xF8000180u, FPGA1_THR = 0xF8000188u, FPGA_RST = 0xF8000240u,
       LVL_SHFTR_EN = 0xF8000900u, DEVCFG_CTRL = 0xF8007000u, DEVCFG_INT_STS = 0xF800700Cu,
       DEVCFG_UNLOCK = 0xF8007034u };
enum { SEM_ID = 0x00u, SEM_CTRL = 0x04u, SEM_STATUS = 0x08u };
#define SEM_ID_VALUE 0x53454D43u
#define PCFG_DONE    (1u << 2)
#define PCAP_PR      (1u << 27)
#define OBSERVATION  (1u << 2)
#define WAIT_READS   5000000u                 /* SEM status reads over GP0 while waiting: about 1-2 s */

static u32 word(const char *s)
{
    u32 v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (; *s; s++) {
        u32 d = *s >= '0' && *s <= '9' ? (u32)(*s - '0')
              : *s >= 'a' && *s <= 'f' ? (u32)(*s - 'a' + 10)
              : *s >= 'A' && *s <= 'F' ? (u32)(*s - 'A' + 10) : 16u;
        if (d > 15u)
            break;
        v = v << 4 | d;
    }
    return v;
}

static void spin(u32 n)
{
    while (n--)
        __asm__ volatile("nop");
}

__attribute__((section(".text.entry"))) int duty_early(int argc, char *const argv[])
{
    if (argc != 4)
        return 1;
    u32 sem   = word(argv[1]);
    u32 gates = word(argv[2]);
    u32 fclk1 = word(argv[3]);
    if (!sem || !fclk1)
        return 1;
    if (!(REG(DEVCFG_INT_STS) & PCFG_DONE))
        return 2;
    if ((REG(LVL_SHFTR_EN) & 0xFu) != 0xFu)
        return 3;
    u32 thr = REG(FPGA1_THR);
    REG(SLCR_UNLOCK) = 0xDF0Du;
    REG(FPGA_RST)    = 0xFu;                  /* the PL held in reset while its 1x clock changes */
    REG(FPGA1_THR)   = thr | 1u;
    REG(FPGA1_CLK)   = fclk1;
    REG(FPGA1_THR)   = thr & ~1u;
    spin(100000u);
    REG(FPGA_RST) = 0u;
    spin(100000u);                            /* the reset blocks release the AXI side (16 cycles and more) */
    if (REG(sem + SEM_ID) != SEM_ID_VALUE)
        return 4;
    REG(DEVCFG_UNLOCK)   = 0x757BDF0Du;
    REG(DEVCFG_CTRL)     = REG(DEVCFG_CTRL) & ~PCAP_PR;
    REG(sem + SEM_CTRL)  = 2u;                /* counters cleared */
    REG(sem + SEM_CTRL)  = 1u;                /* grant */
    if (gates)
        REG(gates) = 0u;
    for (u32 i = 0; i < WAIT_READS; i++)
        if (REG(sem + SEM_STATUS) & OBSERVATION)
            return 0;
    return 5;
}
