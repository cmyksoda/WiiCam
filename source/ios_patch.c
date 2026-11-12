#include "ios_patch.h"

#include <ogc/machine/processor.h>
#include <ogc/cache.h>
#include <ogc/system.h>
#include <stdio.h>
#include <string.h>

#define AHBPROT_REG 0x0D800064
#define MEM2_PROT   0x0D8B420A

// Module 13-0000000d (/dev/usb/usb), as it loads in IOS58 v6176, through the
// PowerPC's cached MEM2 window (physical 0x13650000 -> 0x93650000).
#define CORE(off) (0x93650000u + (off))

#define BUF_OLD 0x13654500u  // the fixed 2,048-byte configuration buffer
#define CMP_OLD 0xE35E0B02u  // cmp lr, #0x800
#define CMP_NEW 0xE35E0A01u  // cmp lr, #0x1000  (accept up to 4,096 bytes)
#define MS_OLD  0xE3A02B02u  // mov r2, #0x800
#define MS_NEW  0xE3A02A01u  // mov r2, #0x1000

// Everything below this address is PowerPC/our RAM, which IOS maps for IPC;
// everything above is the IOS module region the pristine buffer sits in.
#define PPC_RAM_TOP 0x13600000u

#define BUF_SIZE 4096u

static const u32 ptr_addr[4] = { CORE(0x0B30), CORE(0x0C28), CORE(0x0D80), CORE(0x0F90) };

static bool g_applied = false;  // we changed IOS and still owe it a restore

static bool ahbprot_open(void)
{
    return read32(AHBPROT_REG) == 0xffffffff;
}

static void flush_gate_region(void)
{
    for (u32 a = 0x93650B00u; a < 0x93651000u; a += 32) {
        DCFlushRange((void *)a, 32);
        ICInvalidateRange((void *)a, 32);
    }
}

// A 4 KB MEM2 buffer reserved from the top of the arena. IOS reaches it through
// the PowerPC-memory domain it already uses for every ioctl, and MEM2 is where
// USB DMA goes; this works where IOS's own between-module gaps do not.
static u8 *reserve_buffer(void)
{
    static u8 *buf = NULL;
    if (!buf) {
        uintptr_t hi = (uintptr_t)SYS_GetArena2Hi();
        uintptr_t lo = (uintptr_t)SYS_GetArena2Lo();
        uintptr_t base = (hi - BUF_SIZE) & ~(uintptr_t)31;
        if (base >= lo && base < hi) {
            buf = (u8 *)base;
            SYS_SetArena2Hi(buf);
        }
    }
    return buf;
}

IosPatchResult ios_patch_config_limit(char *detail, u32 detail_len)
{
    if (!ahbprot_open()) {
        snprintf(detail, detail_len, "no hardware access (AHBPROT)");
        return IOS_PATCH_NO_AHBPROT;
    }

    u8 *patch_buf = reserve_buffer();
    if (!patch_buf) {
        snprintf(detail, detail_len, "no MEM2 for buffer");
        return IOS_PATCH_BUF_NOT_EMPTY;
    }

    u32 buf_phys = (u32)patch_buf & 0x3FFFFFFFu;  // physical address IOS reads
    memset(patch_buf, 0, BUF_SIZE);
    DCFlushRange(patch_buf, BUF_SIZE);

    // IOS's MEM2 reads back as all-ones until this protection is lifted, so the
    // check has to happen inside the lifted window, not before it.
    u32 ahb = read32(AHBPROT_REG);
    u16 prot0 = read16(MEM2_PROT);
    u16 saved = prot0;
    write16(MEM2_PROT, 2);
    u16 prot1 = read16(MEM2_PROT);

    // Accept the pristine bytes or an earlier run of ours (gate is our cap; each
    // pointer is either pristine or already aimed at PowerPC RAM); refuse anything
    // else, which would be a different IOS build.
    u32 gate = read32(CORE(0x0BD0));
    u32 base = read32(CORE(0x0000));
    u32 ms = read32(CORE(0x0DB8));
    bool ok = (gate == CMP_OLD || gate == CMP_NEW) && (ms == MS_OLD || ms == MS_NEW);
    for (u32 i = 0; i < 4 && ok; i++) {
        u32 p = read32(ptr_addr[i]);
        if (p != BUF_OLD && p >= PPC_RAM_TOP)
            ok = false;
    }
    if (!ok) {
        write16(MEM2_PROT, saved);
        snprintf(detail, detail_len, "g %08x base %08x ahb %08x prot %04x>%04x",
                 (unsigned)gate, (unsigned)base, (unsigned)ahb,
                 (unsigned)prot0, (unsigned)prot1);
        return IOS_PATCH_WRONG_BUILD;
    }

    write32(CORE(0x0BD0), CMP_NEW);
    write32(CORE(0x0DB8), MS_NEW);
    for (u32 i = 0; i < 4; i++)
        write32(ptr_addr[i], buf_phys);

    u32 applied = (read32(CORE(0x0BD0)) == CMP_NEW) + (read32(CORE(0x0DB8)) == MS_NEW);
    for (u32 i = 0; i < 4; i++)
        applied += (read32(ptr_addr[i]) == buf_phys);
    flush_gate_region();
    write16(MEM2_PROT, saved);
    g_applied = true;

    snprintf(detail, detail_len, "%lu of 6 words set, buffer -> %08x cap 4096",
             (unsigned long)applied, (unsigned)buf_phys);
    return IOS_PATCH_APPLIED;
}

void ios_patch_restore(void)
{
    if (!g_applied || !ahbprot_open())
        return;

    u16 saved = read16(MEM2_PROT);
    write16(MEM2_PROT, 2);
    write32(CORE(0x0BD0), CMP_OLD);
    write32(CORE(0x0DB8), MS_OLD);
    for (u32 i = 0; i < 4; i++)
        write32(ptr_addr[i], BUF_OLD);
    flush_gate_region();
    write16(MEM2_PROT, saved);
    g_applied = false;
}
