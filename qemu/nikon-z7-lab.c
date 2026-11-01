/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal research machine. This is NOT a Nikon EXPEED peripheral model. */
#include "qemu/osdep.h"
#include "nikon-z7-shoulder.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/arm/machines-qom.h"
#include "hw/loader.h"
#include "hw/cpu/a15mpcore.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "chardev/char-fe.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "target/arm/cpu.h"
#include "system/runstate.h"
#include "nikon-z7-gpu.h"
#include "nikon-z7-isp-pixels.h"

typedef struct Z7SerialSink {
    struct Z7LabState *owner;
    unsigned channel;
    QEMUTimer *timer;
    bool active;
    uint16_t fifo[64];
    uint32_t rx_fifo[64];
    unsigned count, rx_count, rx_head;
    uint32_t pending;
} Z7SerialSink;

typedef struct Z7LabState {
    MemoryRegion main_rom;
    MemoryRegion inferred_ram;
    MemoryRegion lab_ram;
    MemoryRegion unknown;
    MemoryRegion boot_sram;
    MemoryRegion shadow_ram;
    MemoryRegion processing_ram;
    MemoryRegion auxiliary_ram;
    MemoryRegion image_workspace_ram;
    MemoryRegion image_large_pool;
    MemoryRegion image_curve_ram;
    MemoryRegion image_preview_pool;
    MemoryRegion application_heap;
    MemoryRegion controller_sram;
    MemoryRegion gui_pool;
    MemoryRegion output_pools;
    MemoryRegion capture_pools[6];
    MemoryRegion capture_pool_aliases[6], gui_pool_alias, image_preview_alias;
    bool boot_profile;
    bool platform_profile;
    uint32_t platform_regs[0x7000 / 4];
    bool platform_written[0x7000 / 4];
    uint32_t platform_saved[512];
    QEMUTimer *platform_timer;
    uint32_t event_regs[6];
    unsigned driver_fifo_depth;
    uint32_t serial_disabled[11];
    bool serial_initialized[11];
    uint32_t serial_config[11][0x68 / 4];
    uint32_t driver_control[3];
    bool recovery_profile;
    bool graphics_profile;
    Z7GPU *gpu;
    uint32_t display_regs[0xc000 / 4];
    uint32_t display_lut[2][0x6200 / 4];
    QEMUTimer *lcd_timer;
    bool lcd_scanout_test;
    unsigned lcd_frames;
    Z7SerialSink serial_sink[11];
    bool exmcu_test_device;
    bool exmcu_no_lens;
    bool exmcu_no_lens_reported;
    bool exmcu_capture_sync_test, exmcu_capture_sync_level;
    QEMUTimer *exmcu_capture_sync_timer;
    uint8_t exmcu_tx[256], exmcu_rx[8][256];
    unsigned exmcu_tx_count, exmcu_rx_head, exmcu_rx_count, exmcu_rx_offset;
    uint32_t exmcu_gpio_pending, exmcu_gpio_mask, exmcu_gpio_polarity, exmcu_gpio_mode;
    bool exmcu_tx_ready, exmcu_tx_ack;
    Z7ShoulderObserver shoulder;
    uint32_t graphics_regs[0x1600 / 4];
    uint32_t graphics_cmd_regs[2][512];
    uint8_t graphics_tables[2][0x10000];
    unsigned graphics_table_lengths[2];
    unsigned graphics_upload_sequence;
    bool graphics_upload_pending[2];
    QEMUTimer *graphics_timer;
    uint32_t channel_control[3];
    uint32_t channel_preset_latch, channel_preset[12];
    uint32_t channel_config[12][2][0x44 / 4];
    QEMUTimer *channel_timer;
    uint32_t channel_enable, channel_irq_mask, channel_pending[12];
    uint32_t channel_counter[12], channel_repetitions[12];
    int64_t channel_epoch[12], channel_deadline[12];
    bool channel_clocked[12];
    uint32_t copy_regs[4][9];
    QEMUTimer *copy_timer;
    bool copy_pending[4];
    int64_t copy_deadline[4];
    uint32_t i2c_regs[4][11];
    uint8_t i2c_tx[4][16];
    unsigned i2c_tx_count[4];
    bool i2c_pending[4];
    int64_t i2c_deadline[4];
    QEMUTimer *i2c_timer;
    uint32_t peripheral_dma_regs[2][8][4];
    uint32_t sequence_regs[11][15];
    uint32_t sequence_active[11][15];
    uint32_t sequence_start_mask, sequence_stop_mask, sequence_running;
    uint32_t sequence_stop_pending;
    QEMUTimer *sequence_stop_timer;
    uint32_t capture_timing[6][10];
    uint32_t capture_control[5];
    uint32_t capture_irq_enable[3], capture_irq_status[3];
    QEMUTimer *capture_test_timer;
    bool capture_test_pulse, capture_test_running;
    QEMUTimer *capture_periodic_test_timer;
    bool capture_periodic_test;
    bool capture_periodic_paired_test;
    unsigned capture_periodic_test_count;
    char *capture_ingress_data;
    size_t capture_ingress_length;
    char *capture_native0_data;
    size_t capture_native0_length;
    unsigned capture_native0_frames, capture_statistics_frames;
    bool capture_ingress_done;
    bool capture_drain_pending;
    uint32_t capture_isp_gate[3];
    uint32_t capture_isp_irq_enable[8], capture_isp_irq_status[8];
    uint32_t capture_isp_stop_pending;
    QEMUTimer *capture_isp_stop_timer;
    bool capture_isp_boundary_test;
    unsigned capture_isp_boundary_stage;
    uint32_t capture_sync[0x500 / 4];
    uint32_t capture_processing[0x2000 / 4];
    uint16_t capture_curve[129];
    uint32_t capture_image_channel[10][0x84 / 4];
    uint32_t capture_analysis_config[0x380 / 4];
    uint16_t capture_measure_config[2][10];
    uint32_t capture_filter_config[0x584 / 4];
    uint32_t capture_filter2_config[0x100 / 4];
    uint32_t image_worker_config[0x1a4 / 4];
    uint32_t image_route_config[0xa8 / 4];
    uint32_t image_sequence_config[0x94 / 4];
    uint32_t image_command_config[2][0xa8 / 4];
    uint32_t image_transfer_config[16][13];
    uint32_t image_transfer_selected;
    uint32_t image_unit_config[0x6a0 / 4];
    uint16_t image_unit_lut[0x6000 / 4];
    uint32_t image_output_config[8][2];
    uint32_t image_command_lists[2][64];
    uint32_t image_processor_config[2][0x10000 / 4];
    bool image_process_test;
    unsigned image_process_batches, image_process_last_input;
    Z7ISPImages *image_pixels;
    int64_t image_process_host_us;
    unsigned capture_period_ns;
    QEMUTimer *image_process_timer;
    QEMUTimer *image_drain_timer;
    unsigned image_drain_requests;
    uint32_t recovery_config[256];
    QEMUTimer *clock_timer;
    uint32_t clock_transaction;
    uint32_t engine_regs[9];
    QEMUTimer *engine_timer;
    uint32_t early_control[9];
    int64_t counter_epoch;
    uint32_t setup_regs[0x8000 / 4];
    bool setup_written[0x8000 / 4];
    uint32_t ddr_regs[0x40000 / 4];
    bool ddr_written[0x40000 / 4];
    uint32_t fill_regs[0x34 / 4];
    uint8_t uart_regs[16];
    CharFrontend uart;
    A15MPPrivState *priv;
    bool faulted;
    bool debug_rejected;
} Z7LabState;

static void z7_image_process_schedule(Z7LabState *s);
static bool z7_image_process_ready(Z7LabState *s);
static void z7_image_raw_irq(Z7LabState *s);

static void z7_unknown_access(Z7LabState *s, hwaddr addr, unsigned size, bool write,
                              uint64_t value)
{
    if (!current_cpu) {
        /* GDB may scan before the ROM while unwinding. Reject the diagnostic
         * transaction; never invent bytes or terminate a stopped guest. */
        s->debug_rejected = true;
        fprintf(stderr, "Z7_DEBUG_UNMAPPED %s addr=0x%08" HWADDR_PRIx " size=%u\n",
                write ? "write" : "read", addr, size);
        return;
    }
    if (!s->faulted) {
        uint32_t pc = current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
        s->faulted = true;
        fprintf(stderr, "Z7_STOP_VIRTUAL_NS=%" PRId64 "\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        error_report("Z7_LAB_UNMODELED %s addr=0x%08" HWADDR_PRIx
                     " size=%u value=0x%08" PRIx64 " pc=0x%08x; stopped without inventing a device response",
                     write ? "write" : "read", addr, size, value, pc);
        if (s->boot_profile) {
            CPUState *cs;
            GICState *gic = &s->priv->gic;
            fprintf(stderr, "Z7_GIC_STATE dist_ctl=0x%x cpu_ctl=0x%x pmr=0x%x running=0x%x pending=%u timer_enabled=0x%x\n",
                    gic->ctlr, gic->cpu_ctlr[0], gic->priority_mask[0], gic->running_priority[0],
                    gic->current_pending[0], gic->irq_state[29].enabled);
            fprintf(stderr, "Z7_GIC_TIMER pending=0x%x level=0x%x active=0x%x group=0x%x priority=0x%x\n",
                    gic->irq_state[29].pending, gic->irq_state[29].level, gic->irq_state[29].active,
                    gic->irq_state[29].group, gic->priority1[29][0]);
            CPU_FOREACH(cs) {
                ARMCPU *cpu = ARM_CPU(cs);
                fprintf(stderr, "Z7_CPU_STATE cpu=%d pc=0x%08x cpsr=0x%08x sp=0x%08x lr=0x%08x\n",
                        cs->cpu_index, cpu->env.regs[15], cpsr_read(&cpu->env),
                        cpu->env.regs[13], cpu->env.regs[14]);
                for (unsigned r = 0; r < 13; r++) {
                    fprintf(stderr, "Z7_CPU_REG cpu=%d r%u=0x%08x\n", cs->cpu_index, r, cpu->env.regs[r]);
                }
                for (unsigned t = 0; t < NUM_GTIMERS; t++) {
                    fprintf(stderr, "Z7_TIMER_STATE cpu=%d timer=%u ctl=0x%" PRIx64 " cval=0x%" PRIx64 "\n",
                            cs->cpu_index, t, cpu->env.cp15.c14_timer[t].ctl, cpu->env.cp15.c14_timer[t].cval);
                }
            }
        }
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC, 2);
    }
}

/* Narrow hypotheses inferred from startup, deliberately not a whole MMIO RAM.
 * 04264000: startup write, then native 11f9da9e/dab8 read/modify/write bit3
 * (relocated a2a40646/660); retain the value. +20 remains startup-write-only.
 * 04281020/+28/+2c: configuration read/modify/write; reset zero is provisional.
 * Anything outside this exact whitelist still stops. */
static int z7_early_index(hwaddr addr)
{
    switch (addr) {
    case 0x04264000: return 0;
    case 0x04264020: return 1;
    case 0x04281020: return 2;
    case 0x04281028: return 3;
    case 0x0428102c: return 4;
    /* 1002012c clears/loads this counter-control block before CNTFRQ setup. */
    case 0x04266000: return 5;
    case 0x04266008: return 6;
    case 0x0426600c: return 7;
    case 0x04266010: return 8;
    default: return -1;
    }
}

static void z7_mmio_trace(hwaddr addr, unsigned size, bool write, uint64_t value)
{
    static int quiet = -1;
    static int capture_trace = -1;
    if (quiet < 0) { quiet = getenv("Z7_MMIO_QUIET") != NULL; }
    if (capture_trace < 0) { capture_trace = getenv("Z7_CAPTURE_TRACE") != NULL; }
    if (quiet && !(capture_trace && ((addr >= 0x20060000 && addr < 0x20070000) ||
         (addr >= 0x20100000 && addr < 0x20400000)))) { return; }
    uint32_t pc = current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
    fprintf(stderr, "Z7_MMIO %c 0x%08" HWADDR_PRIx " size=%u value=0x%08" PRIx64
            " pc=0x%08x\n", write ? 'W' : 'R', addr, size, value, pc);
}

/* Two symmetric initialization blocks, seen at 10024c6c and 10025354.
 * Writes are retained; reads of an unwritten register still stop.
 * Completion/W1C transitions are hypotheses from trigger -> poll -> ack code.
 * This is a transaction model, not an electrical DDR/PHY simulation. */
static bool z7_ddr_write(Z7LabState *s, hwaddr addr, uint32_t value)
{
    /* Offsets observed on the original boot path; value semantics remain
     * provisional for configuration words. Other writes are not accepted. */
    static const unsigned allowed[] = {
        0x0, 0x100, 0x10c, 0x1004, 0x1008, 0x100c, 0x1010, 0x1014,
        0x1020, 0x1024, 0x1028, 0x102c, 0x1030, 0x1038, 0x2000, 0x2100,
        0x2300, 0x2408, 0x2414, 0x3000, 0x3100, 0x3118, 0x3300, 0x3318,
        0x3404, 0x3408, 0x340c, 0x3500, 0x3504, 0x3508, 0x350c, 0x3510,
        0x3514, 0x3600, 0x3604, 0x360c, 0x3610, 0x3614, 0x3618, 0x361c,
        0x3620, 0x3624, 0x3628, 0x362c, 0x3630, 0x3634, 0x3700, 0x3708,
        0x370c, 0x3800, 0x3804, 0x3808, 0x3900, 0x3904, 0x3a00, 0x3a04,
        0x3b00, 0x3b04, 0x4000, 0x4100, 0x4300, 0x4400, 0x4404, 0x4408,
        0x8000, 0x8010, 0x8030, 0x8040, 0x8800, 0x8810, 0x8830, 0x8c00,
        0x8c10, 0x8c30, 0x9000, 0x9010, 0x9030, 0x9040, 0x9800, 0x9810,
        0x9830, 0x9c00, 0x9c10, 0x9c30, 0xa000, 0xa010, 0xa030, 0xa040,
        0xa800, 0xa810, 0xa830, 0xac00, 0xac10, 0xac30, 0xb000, 0xb010,
        0xb030, 0xb040, 0xb800, 0xb810, 0xb830, 0xbc00, 0xbc10, 0xbc30,
        0xc000, 0xc010, 0xc030, 0xc040, 0xc800, 0xc810, 0xc830, 0xcc00,
        0xcc10, 0xcc30, 0xe000, 0xe100, 0xe400, 0xe600, 0xe604, 0xe608,
        0xe700, 0xe704, 0xe708, 0xe70c, 0xe710, 0x10010, 0x10014, 0x10018,
        0x1001c, 0x10020, 0x10024, 0x10030, 0x10034, 0x10040, 0x10044, 0x10048,
        0x10050, 0x10054, 0x10058, 0x1005c, 0x10060, 0x10074, 0x10078, 0x1007c,
        0x10080, 0x10094, 0x10098, 0x1009c, 0x100a0, 0x100b8, 0x100e4, 0x100e8,
        0x100ec, 0x100f0, 0x10128, 0x1012c, 0x10130, 0x10134, 0x10148, 0x1014c,
        0x10150, 0x10154, 0x10820, 0x10824, 0x10828, 0x1082c, 0x10840, 0x10850,
        0x10910,
    };
    unsigned index, off;
    bool phy;
    bool found = false;
    hwaddr bank;
    if (addr < 0x20020000 || addr >= 0x20060000 || (addr & 3)) {
        return false;
    }
    index = (addr - 0x20020000) / 4;
    bank = addr & ~0x1ffffULL;
    off = addr - bank;
    for (unsigned n = 0; n < ARRAY_SIZE(allowed); n++) {
        if (allowed[n] == off) { found = true; break; }
    }
    if (!found) { return false; }
    phy = (off & ~0x30U) == 0x8000 || (off & ~0x30U) == 0x8800 ||
          (off & ~0x30U) == 0x8c00 || (off & ~0x30U) == 0x9000 ||
          (off & ~0x30U) == 0x9800 || (off & ~0x30U) == 0x9c00 ||
          (off & ~0x30U) == 0xa000 || (off & ~0x30U) == 0xa800 ||
          (off & ~0x30U) == 0xac00 || (off & ~0x30U) == 0xb000 ||
          (off & ~0x30U) == 0xb800 || (off & ~0x30U) == 0xbc00 ||
          (off & ~0x30U) == 0xc000 || (off & ~0x30U) == 0xc800 ||
          (off & ~0x30U) == 0xcc00;
    /* Status registers are acknowledged by writing the observed completion bit. */
    if (off == 0x2300 || off == 0x3300 || off == 0x3318 || off == 0x4300 || off == 0xe300 ||
        (phy && (off & 0x30) == 0x30)) {
        s->ddr_regs[index] &= ~value;
    } else {
        s->ddr_regs[index] = value;
    }
    s->ddr_written[index] = true;
    if ((off == 0x2100 || off == 0x3100 || off == 0x3118 || off == 0x4100 || off == 0xe100) && (value & 1)) {
        s->ddr_regs[index + 0x200 / 4] |= 1;
        s->ddr_written[index + 0x200 / 4] = true;
        if (off == 0x3118) {
            /* 10025c10 and symmetric bank: nonzero PHY result is an error.
             * Assumption: virtual RAM has no training failure. */
            unsigned result_index = (bank + 0x1016c - 0x20020000) / 4;
            s->ddr_regs[result_index] = 0;
            s->ddr_written[result_index] = true;
        }
    }
    if (phy && (off & 0x30) == 0x10 && (value & 1)) {
        s->ddr_regs[index + 0x20 / 4] |= 1;
        s->ddr_written[index + 0x20 / 4] = true;
    }
    z7_mmio_trace(addr, 4, true, value);
    return true;
}

/* Original 1002c1a4 programs a 16 KiB block fill for the BSS. */
static bool z7_fill_access(Z7LabState *s, hwaddr addr, unsigned size,
                           bool write, uint64_t *value)
{
    unsigned off = addr - 0x20070000;
    if (addr < 0x20070000 || off >= sizeof(s->fill_regs) || (off & 3) || size != 4) {
        return false;
    }
    if (!write) {
        *value = s->fill_regs[off / 4];
    } else if (off == 8) {
        s->fill_regs[2] &= ~*value;
    } else {
        s->fill_regs[off / 4] = *value;
        if (off == 0x10 && (*value & 1)) {
            uint32_t base = s->fill_regs[0x30 / 4];
            uint32_t width = s->fill_regs[0x20 / 4] & 0xffff;
            uint32_t stride = s->fill_regs[0x24 / 4] & 0xffff;
            uint32_t rows = s->fill_regs[0x28 / 4];
            static const uint8_t zeros[0x4000];
            uint64_t end = (uint64_t)base + (uint64_t)rows * stride;
            bool bss = width == sizeof(zeros) && stride == width &&
                       base >= 0xa1800000 && end <= 0xa3000000;
            /* 1008d9c6 now clears the original 720x480x4 GUI buffers. */
            bool gui = s->graphics_profile && width && width <= sizeof(zeros) &&
                       stride >= width && base >= 0xb045f200 && end <= 0xb3ccb400;
            if ((!bss && !gui) || rows == 0 ||
                s->fill_regs[0x18 / 4] != 0 || !(s->fill_regs[0x14 / 4] & 1) ||
                (gui && (s->fill_regs[0x1c / 4] || s->fill_regs[0x2c / 4]))) {
                z7_unknown_access(s, addr, size, true, *value);
                return true;
            }
            for (uint32_t row = 0; row < rows; row++) {
                cpu_physical_memory_write(base + row * stride, zeros, width);
            }
            s->fill_regs[2] |= 1;
            if (s->graphics_profile) { s->fill_regs[0x10 / 4] = 2; }
            fprintf(stderr, "Z7_DMA_ZERO base=0x%08x bytes=0x%x rows=%u\n", base, rows * stride, rows);
        } else if (off == 0x10 && !*value && s->graphics_profile) {
            s->fill_regs[0x10 / 4] = 2; /* 1008db10 polls stopped state */
        }
    }
    if (s->graphics_profile) {
        /* 1008d8ae registers channel zero ISR 1008d866 on IRQ 103. */
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 103 - 32),
                     (s->fill_regs[2] & s->fill_regs[1]) != 0);
    }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

/* 10020180 initializes this 16-byte channel; 100201ec polls status bit 1
 * at +3 and transmits through +4. No receive or UART IRQ is modeled yet. */
static bool z7_uart_access(Z7LabState *s, hwaddr addr, unsigned size,
                          bool write, uint64_t *value)
{
    unsigned off = addr - 0x2c090000;
    if (addr < 0x2c090000 || off >= 16 || off + size > 16 || size > 2) {
        return false;
    }
    if (write) {
        for (unsigned i = 0; i < size; i++) {
            s->uart_regs[off + i] = *value >> (i * 8);
        }
        if (off == 4 && (s->uart_regs[1] & 1)) {
            uint8_t ch = *value;
            if (qemu_chr_fe_backend_connected(&s->uart)) {
                qemu_chr_fe_write_all(&s->uart, &ch, 1);
            } else {
                putchar(ch);
                fflush(stdout);
            }
        }
    } else {
        *value = 0;
        for (unsigned i = 0; i < size; i++) {
            uint8_t v = s->uart_regs[off + i];
            if (off + i == 3 && (s->uart_regs[1] & 1)) {
                v |= 2; /* Host sink consumes each transmitted character immediately. */
            }
            *value |= (uint64_t)v << (i * 8);
        }
    }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

static void z7_clock_irq(Z7LabState *s)
{
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 250 - 32),
        (s->setup_regs[0x7004 / 4] & s->setup_regs[0x7008 / 4] & 1) != 0);
}

static void z7_clock_complete(void *opaque)
{
    Z7LabState *s = opaque;
    uint32_t bit = 1U << ((s->clock_transaction >> 12) & 7);
    if (s->clock_transaction & 0x100) { s->setup_regs[0x700c / 4] |= bit; }
    else { s->setup_regs[0x700c / 4] &= ~bit; }
    s->setup_regs[0x7004 / 4] |= 1;
    z7_clock_irq(s);
    fprintf(stderr, "Z7_CLOCK_COMPLETE status=0x%x irq250=%u\n",
        s->setup_regs[0x700c / 4], s->setup_regs[0x7008 / 4] & 1);
}

static bool z7_setup_access(Z7LabState *s, hwaddr addr, unsigned size,
                           bool write, uint64_t *value)
{
    unsigned off = addr - 0x04280000;
    unsigned i = off / 4;
    bool config = off == 0x30 || off == 0x34 || off == 0x38 ||
        (s->recovery_profile && off >= 0x64 && off <= 0x70) || /* 101f2538 indexed configuration */
        (off >= 0x78 && off <= 0x88) || (off >= 0x1000 && off <= 0x1014) ||
        off == 0x1024 || (off >= 0x1138 && off <= 0x114c) ||
        (s->recovery_profile && (off == 0x1018 || off == 0x101c)) || /* 10616a70 table */
        (off >= 0x2000 && off <= 0x242c && (off & 0xff) < 0x30);
    bool clock_command = off == 0x7000 || off == 0x7004 || off == 0x7010 ||
        (s->recovery_profile && (off == 0x7008 || (off == 0x700c && !write)));
    /* 101d7dbe sets/clears individual bits under a software lock. Initial
     * contents are synthetic zero in the platform profile; no clock effect. */
    bool runtime_gate = (s->platform_profile && (off == 0x1058 || off == 0x105c)) ||
        (s->driver_fifo_depth && off == 0x1034); /* 101d7d92 locked bit set/clear */
    if (addr < 0x04280000 || off >= 0x8000 || (off & 3) || size != 4 ||
        (!config && !clock_command && !runtime_gate)) {
        return false;
    }
    if (write) {
        if (off == 0x7004) { s->setup_regs[i] &= ~*value; }
        else { s->setup_regs[i] = *value; }
        s->setup_written[i] = true;
        if (off == 0x7010 && *value == 0xa1) {
            if (s->recovery_profile) {
                s->clock_transaction = s->setup_regs[0x7000 / 4];
                timer_mod(s->clock_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
            } else { s->setup_regs[0x7004 / 4] |= 1; }
            s->setup_written[0x7004 / 4] = true;
        }
        if (s->recovery_profile && (off == 0x7004 || off == 0x7008)) { z7_clock_irq(s); }
    } else {
        if (!s->setup_written[i] && off != 0x34 && off != 0x38 && !runtime_gate &&
            !(s->recovery_profile && (off == 0x7008 || off == 0x700c))) { return false; }
        *value = s->setup_regs[i];
    }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

/* 101044e6 saves these exact addresses through the table at a18fbcbc.
 * Hardware reset contents are unavailable: this profile explicitly supplies
 * synthetic state (zero or a diagnostic pattern), with +38 bit 0 initially ready.
 * Do not enable this hypothesis in the phase4 baseline machines. */
static const uint32_t z7_platform_saved_addresses[] = {
    0x20010038, 0x2001003c, 0x20010100, 0x20010104, 0x20010108,
    0x2001010c, 0x20010110, 0x20010114, 0x20010118, 0x2001011c,
    0x20015004, 0x20015020, 0x20015024, 0x20015028, 0x2001502c,
    0x20015030, 0x20015034, 0x20015038, 0x2001503c,
};

static void z7_platform_complete(void *opaque)
{
    Z7LabState *s = opaque;
    s->platform_regs[0x38 / 4] |= 1;
    fprintf(stderr, "Z7_PLATFORM_COMPLETE ready=1\n");
}

static bool z7_platform_access(Z7LabState *s, hwaddr addr, unsigned size,
                               bool write, uint64_t *value)
{
    unsigned off, index;
    bool config = false, saved = false;
    if (!s->platform_profile || size != 4 || (addr & 3)) { return false; }
    /* Save/read and restore/write windows are paired by 101044e6/101042dc.
     * Shared backing is a model hypothesis, not a confirmed physical alias. */
    if ((!write && addr >= 0x20014800 && addr < 0x20015000) ||
        (write && addr >= 0x24074000 && addr < 0x24074800)) {
        if (write) {
            if (s->platform_regs[0x60 / 4] != 1) { return false; }
            s->platform_saved[(addr - 0x24074000) / 4] = *value;
        } else {
            /* Debugger inspection is side-effect free and need not open the gate. */
            if (current_cpu && s->platform_regs[0x44 / 4] != 1) { return false; }
            *value = s->platform_saved[(addr - 0x20014800) / 4];
        }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    if (addr < 0x20010000 || addr >= 0x20017000) { return false; }
    off = addr - 0x20010000;
    index = off / 4;
    for (unsigned n = 0; n < ARRAY_SIZE(z7_platform_saved_addresses); n++) {
        if (addr == z7_platform_saved_addresses[n]) { saved = true; break; }
    }
    /* Exact table-driven stores in 101042dc: four 23+8-word groups,
     * four 8-word groups, four scalar words, four 9-word groups and five tails. */
    config = (off >= 0x3000 && off < 0x3400 &&
              ((off & 0xff) < 0x5c || ((off & 0xff) >= 0xbc && (off & 0xff) < 0xdc))) ||
             (off >= 0x6a00 && off < 0x6a80) ||
             (off >= 0x5010 && off <= 0x501c) ||
             (off >= 0x2000 && off < 0x2064 && (off & 0x3f) < 0x24) ||
             (off >= 0x2100 && off < 0x2164 && (off & 0x3f) < 0x24) ||
             off == 0x2648 || off == 0x2490 || off == 0x24a0 ||
             off == 0x2790 || off == 0x2798;
    if (!saved && !config && off != 0x34 && off != 0x44 && off != 0x60) {
        return false;
    }
    if (write) {
        if (saved && current_cpu && s->platform_regs[0x60 / 4] != 1) { return false; }
        if ((off == 0x34 || off == 0x44 || off == 0x60) && *value > 1) { return false; }
        s->platform_regs[index] = *value;
        s->platform_written[index] = true;
        if (off == 0x34 && *value == 1) {
            /* Observed command -> poll bit 0. One microsecond is an explicit
             * virtual latency hypothesis, not a measured hardware duration. */
            s->platform_regs[0x38 / 4] &= ~1U;
            timer_mod(s->platform_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
            fprintf(stderr, "Z7_PLATFORM_COMMAND ready=0 latency_ns=1000\n");
        }
    } else {
        if (!saved && !s->platform_written[index]) { return false; }
        *value = s->platform_regs[index];
    }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

/* 1031fd78: IRQ dispatcher reads +8 and acknowledges the same bits.
 * 1031fe54/feb4/fefa/ffdc configure five channels via +0/+4/+c/+14.
 * This models only configuration and an empty pending register. External
 * signal levels (+10), event production and delivery to IRQ 237 remain unknown. */
static bool z7_event_access(Z7LabState *s, hwaddr addr, unsigned size,
                            bool write, uint64_t *value)
{
    unsigned off = addr - 0x2b000000;
    if (!s->platform_profile || size != 4) { return false; }
    if (s->recovery_profile && addr == 0x2b000010 && !write) {
        /* 1031ffae samples five input pairs. Explicit disconnected-low
         * recovery input hypothesis, consistent with no external events. */
        *value = 0;
        z7_mmio_trace(addr, size, false, *value);
        return true;
    }
    if (write && addr == 0x28000000 && *value == 0xf) {
        z7_mmio_trace(addr, size, write, *value);
        return true; /* Exact initialization store at 1031fe3e, effect unknown. */
    }
    if (addr < 0x2b000000 || (off != 0 && off != 4 && off != 8 && off != 12 && off != 20)) {
        return false;
    }
    if (write) {
        if (off == 8) { s->event_regs[off / 4] &= ~*value; }
        else { s->event_regs[off / 4] = *value; }
    } else { *value = s->event_regs[off / 4]; }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

/* Narrow init transactions from 1017f9e6, 10068cb0, 10109a56, 100cbae0.
 * Two control words and one disabled-channel word have synthetic zero reset.
 * Write-only commands are NOT exposed as readable RAM or completed transfers. */
static bool z7_driver_init_access(Z7LabState *s, hwaddr addr, unsigned size,
                                 bool write, uint64_t *value)
{
    int index = -1;
    if (!s->driver_fifo_depth || size != 4) { return false; }
    if (addr == 0x0428002c && !write) {
        /* 101f2514 extracts low five bits, stored by 10091d94. Hardware
         * meaning/reset value is unavailable; explicit synthetic input zero. */
        *value = 0;
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    switch (addr) {
    case 0x2c020000: index = 0; break;
    case 0x2c030000: index = 1; break;
    case 0x2c080000: index = 2; break;
    default: break;
    }
    if (index >= 0) {
        uint32_t mask = index == 2 ? 3 : 0x90000000;
        if (write) {
            if (*value & ~mask) { return false; }
            s->driver_control[index] = *value;
        } else { *value = s->driver_control[index]; }
    } else if (!write) {
        return false;
    } else if ((addr == 0x0426f004 && *value == 0) ||
               ((addr == 0x0426f000 || addr == 0x0426f010) && *value == 0xff)) {
        /* Only the three initialization stores; GPIO signals remain unknown. */
    } else if ((addr == 0x2c018028 || addr == 0x2c019028 ||
                addr == 0x2c01a028 || addr == 0x2c01b028) && *value == 0x2ff) {
        /* Initialization acknowledges all pending causes in these channels.
         * No pending sources or channel IRQs are produced by this profile. */
    } else if (addr >= 0x04281238 && addr <= 0x0428124c && !(addr & 3)) {
        /* Clock enable commands from the pointer table at a1806e94. */
    } else { return false; }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

/* Optional EX MCU protocol test peer. Packet layout/checksum follows
 * 1005c1ca/1aa and 1005bab6. It does not run the EX MCU firmware, drive a
 * shutter, supply calibration, or produce sensor samples. Only the two
 * CAPCOM control replies accepted by 10235d58/dc8 are implemented by default.
 * A separate opt-in models an absent lens and an idle lens controller. */
static void z7_exmcu_irq(Z7LabState *s)
{
    if (s->exmcu_capture_sync_test) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 65 - 32),
                    (s->exmcu_gpio_pending & ~s->exmcu_gpio_mask & 1) != 0);
    }
    for (unsigned bit = 2; bit <= 3; bit++) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 65 + bit - 32),
                    (s->exmcu_gpio_pending & ~s->exmcu_gpio_mask & (1U << bit)) != 0);
    }
}

/* CAPCOM has a separate request/completion wire pair, not a packet ACK.
 * Native 1017cd70/98 assert GPIO0x65 (04282018 bit8); GPIO0x54 is input bit7
 * at04282014, interrupt index0/IRQ65. Its 1017cdc0 callback delivers
 * 03030024/25 -> 1010416e -> 10104094 -> CAPSEQ03030004/05.
 * The explicit test peer supplies a completion edge 100us after a request.
 * This is a functional timing hypothesis, not shutter/mechanical emulation. */
static void z7_exmcu_capture_sync_complete(void *opaque)
{
    Z7LabState *s = opaque;
    s->exmcu_capture_sync_level = true;
    s->exmcu_gpio_pending |= 1;
    z7_exmcu_irq(s);
    fprintf(stderr, "Z7_EXMCU_CAPTURE_SYNC_TEST irq=65 cause=1 latency_ns=100000 diagnostic_only=1\n");
}

static void z7_exmcu_packet(Z7LabState *s)
{
    const uint8_t *tx = s->exmcu_tx;
    uint8_t sum = 0;
    for (unsigned i = 0; i < 256; i++) { sum += tx[i]; }
    unsigned length = lduw_le_p(tx + 2);
    bool valid = sum == 0xff && length <= 240 && tx[1] >= 1 && tx[1] <= 3 &&
                 tx[252] == 1 && tx[253] == 1;
    s->exmcu_tx_ack = valid;
    s->exmcu_tx_ready = true;
    s->exmcu_gpio_pending |= 4;
    fprintf(stderr, "Z7_EXMCU_TEST_TX channel=%u length=%u command=%u checksum=%02x valid=%u data=",
            tx[0], length, tx[8], sum, valid);
    for (unsigned i = 0; i < MIN(length, 32); i++) { fprintf(stderr, "%02x", tx[8+i]); }
    fprintf(stderr, "\n");
    uint8_t response = valid && tx[0] == 9 && tx[8] == 2 && length == 6 ? 3 :
                       valid && tx[0] == 9 && tx[8] == 4 && length == 5 ? 5 : 0;
    /* Channel table 11521c14 -> 102351de. The original serializers
     * 10235244/286 produce 16-byte requests; 1023539a/3ce decode 0x81/82.
     * This hypothesis is deliberately limited to the observed idle operation
     * (10136842/4a), including their fixed argument bytes. Byte +4
     * is the operation sequence;
     * +5 is a result (zero = no error). Never ACK arbitrary lens movement.
     * The 0x91 report (102354a6) supplies byte +12 = 0 (lens absent), handled
     * by 10065fc6. This is an emulated controller, not measured EX MCU behavior.
     */
    bool lens_reply = valid && s->exmcu_no_lens && tx[0] == 13 &&
                      length == 16 && (ldl_le_p(tx + 8) == 1 || ldl_le_p(tx + 8) == 2) &&
                      tx[13] == 0 && tx[14] == 0 && tx[15] == 0 &&
                      ldl_le_p(tx + 16) == 0x100 && ldl_le_p(tx + 20) == 0;
    if (lens_reply) { response = 0x80 | tx[8]; }
    unsigned replies = response ? 1 : 0;
    if (lens_reply && !s->exmcu_no_lens_reported) { replies++; }
    if (s->exmcu_rx_count + replies > ARRAY_SIZE(s->exmcu_rx)) {
        s->exmcu_tx_ack = false;
        fprintf(stderr, "Z7_EXMCU_TEST_REJECT reason=reply_queue_full\n");
        replies = 0;
    }
    for (unsigned reply = 0; reply < replies; reply++) {
        unsigned index = (s->exmcu_rx_head + s->exmcu_rx_count) % ARRAY_SIZE(s->exmcu_rx);
        uint8_t *rx = s->exmcu_rx[index];
        memset(rx, 0, 256);
        /* 10058f70 maps logical routes 0..2 to wire values 1..3.
         * 10058eac rejects zero before calling the protocol decoder. */
        rx[0] = tx[0]; rx[1] = tx[1]; rx[2] = lens_reply ? 16 : 4;
        rx[8] = response;
        if (lens_reply) {
            rx[12] = tx[12];
            if (!s->exmcu_no_lens_reported) {
                rx[8] = 0x91;
                s->exmcu_no_lens_reported = true;
            }
        }
        rx[252] = rx[253] = 1;
        sum = 0;
        for (unsigned i = 0; i < 255; i++) { sum += rx[i]; }
        rx[255] = 0xff - sum;
        s->exmcu_rx_count++;
        s->exmcu_gpio_pending |= 8;
        fprintf(stderr, "Z7_EXMCU_TEST_REPLY command=%u queued=%u diagnostic_only=1 channel=%u sequence=%u\n",
                rx[8], s->exmcu_rx_count, rx[0], rx[12]);
    }
    z7_exmcu_irq(s);
}

static uint16_t z7_exmcu_shift(Z7LabState *s, unsigned channel, uint16_t tx)
{
    if (channel == 0) {
        if (s->exmcu_tx_count == 0) {
            s->exmcu_tx_ready = s->exmcu_tx_ack = false;
        }
        stw_le_p(s->exmcu_tx + s->exmcu_tx_count, tx);
        s->exmcu_tx_count += 2;
        if (s->exmcu_tx_count == 256) {
            z7_exmcu_packet(s);
            s->exmcu_tx_count = 0;
        }
        return 0xffff;
    }
    if (!s->exmcu_rx_count) { return 0xffff; }
    uint16_t value = lduw_le_p(s->exmcu_rx[s->exmcu_rx_head] + s->exmcu_rx_offset);
    s->exmcu_rx_offset += 2;
    if (s->exmcu_rx_offset == 256) {
        s->exmcu_rx_offset = 0;
        s->exmcu_rx_head = (s->exmcu_rx_head + 1) % ARRAY_SIZE(s->exmcu_rx);
        s->exmcu_rx_count--;
    }
    return value;
}

static bool z7_exmcu_access(Z7LabState *s, hwaddr addr, unsigned size,
                            bool write, uint64_t *value)
{
    if (!s->exmcu_test_device || size != 4) { return false; }
    if (write && addr == 0x04282018 && s->exmcu_capture_sync_test) {
        uint32_t *port = &s->setup_regs[0x2018 / 4];
        uint32_t old = *port, mask = *value >> 16;
        *port = (old & ~mask) | (*value & mask);
        if (mask & 0x100) {
            if ((*port & 0x100) && !(old & 0x100)) {
                s->exmcu_capture_sync_level = false;
                timer_mod(s->exmcu_capture_sync_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
            }
        }
    } else if (!write && (addr == 0x04282014 || addr == 0x04282018)) {
        *value = s->setup_regs[(addr - 0x04280000) / 4];
        if (addr == 0x04282014) {
            *value = (*value & ~0x600ULL) | (s->exmcu_tx_ready ? 0x200 : 0) |
                     (s->exmcu_rx_count ? 0x400 : 0);
            if (s->exmcu_capture_sync_test) {
                *value = (*value & ~0x80ULL) | (s->exmcu_capture_sync_level ? 0x80 : 0);
            }
        } else { *value = (*value & ~0x8000ULL) | (s->exmcu_tx_ack ? 0x8000 : 0); }
    } else if (addr == 0x0426f000) {
        if (write) { s->exmcu_gpio_mask = *value; }
        else { *value = s->exmcu_gpio_mask; }
    } else if (addr == 0x0426f008 && !write) { *value = s->exmcu_gpio_pending; }
    else if (addr == 0x0426f010) {
        if (write) { s->exmcu_gpio_pending &= ~*value; }
        else { *value = s->exmcu_gpio_pending; }
    } else if (addr == 0x0426f014) {
        if (write) { s->exmcu_gpio_polarity = *value; }
        else { *value = s->exmcu_gpio_polarity; }
    } else if (addr == 0x0426f018) {
        if (write) { s->exmcu_gpio_mode = *value; }
        else { *value = s->exmcu_gpio_mode; }
    } else { return false; }
    z7_exmcu_irq(s);
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

/* Disconnected SPI sinks: channel 9 LCD and channels 6/8 configuration traffic.
 * 1006851a: +14 bit4 TX full; 100685f0: bit2 TX idle; 100684ca: bit5 RX empty.
 * a1802268/a1802328/a1802388: IRQ pairs 205/204, 209/208 and 211/210. SPI clocks also fill RX when the caller
 * discards it. The disconnected sink samples MISO high, not a panel ID/ACK. */
static void z7_panel_irq(Z7SerialSink *p)
{
    Z7LabState *s = p->owner;
    unsigned tx_irq = 193 + p->channel * 2;
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), tx_irq - 32),
                 (p->pending & s->serial_config[p->channel][0x20 / 4] & 3) != 0);
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), tx_irq - 1 - 32),
                 (p->pending & s->serial_config[p->channel][0x20 / 4] & 4) != 0);
}

static void z7_panel_complete(void *opaque)
{
    Z7SerialSink *p = opaque;
    if (!p->active) { return; }
    if (p->count) {
        if (p->channel == 9) { fprintf(stderr, "Z7_PANEL_TX words=%u data=", p->count); }
        else { fprintf(stderr, "Z7_SPI%u_TX words=%u data=", p->channel, p->count); }
        for (unsigned i = 0; i < p->count; i++) {
            fprintf(stderr, "%04x%s", p->fifo[i], i + 1 == p->count ? "\n" : ",");
        }
    }
    /* Observe transmitted words before the existing sink consumes them. */
    for (unsigned i = 0; i < p->count; i++) {
        if (z7_shoulder_word(&p->owner->shoulder, p->channel,
                ((p->owner->serial_config[p->channel][0x10 / 4] >> 8) & 31) + 1,
                p->fifo[i], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
            fprintf(stderr, "Z7_SHOULDER_FRAME source=guest-main-firmware spi=0 data=");
            for (unsigned b = 0; b < 256; b++) {
                fprintf(stderr, "%02x", p->owner->shoulder.frame[b]);
            }
            fprintf(stderr, "\n");
        }
    }
    for (unsigned i = 0; i < p->count; i++) {
        unsigned index = (p->rx_head + p->rx_count + i) % ARRAY_SIZE(p->rx_fifo);
        p->rx_fifo[index] = p->owner->exmcu_test_device && p->channel <= 1 ?
            z7_exmcu_shift(p->owner, p->channel, p->fifo[i]) :
            (UINT64_C(1) << (((p->owner->serial_config[p->channel][0x10 / 4] >> 8) & 31) + 1)) - 1;
    }
    p->rx_count += p->count;
    p->count = 0;
    p->pending |= 3; /* TX space and idle causes, 100688ac */
    if (p->rx_count) { p->pending |= 4; }
    z7_panel_irq(p);
}

static bool z7_panel_access(Z7LabState *s, hwaddr addr, unsigned size,
                           bool write, uint64_t *value)
{
    if (!s->graphics_profile || addr < 0x2c0a0000 || addr >= 0x2c0a0b00 ||
        size != 4 || (addr & 3)) { return false; }
    unsigned ch = (addr - 0x2c0a0000) / 0x100, off = addr & 0xff;
    if ((ch > 1 && ch != 6 && ch != 8 && ch != 9) || !s->serial_initialized[ch]) { return false; }
    Z7SerialSink *p = &s->serial_sink[ch];
    switch (off) {
    case 0:
        if (!write || *value > 0xffff || !p->active ||
            p->count + p->rx_count >= s->driver_fifo_depth) { goto rejected; }
        p->fifo[p->count++] = *value;
        p->pending &= ~1U;
        timer_mod(p->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        break;
    case 4:
        if (write || !p->rx_count) { goto rejected; }
        *value = p->rx_fifo[p->rx_head];
        p->rx_head = (p->rx_head + 1) % ARRAY_SIZE(p->rx_fifo);
        p->rx_count--;
        if (!p->rx_count) { p->pending &= ~4U; }
        break;
    case 0x14:
        if (write) { goto rejected; }
        *value = (!p->rx_count ? 0x20 : 0) |
                 (p->count + p->rx_count >= s->driver_fifo_depth ? 0x10 : 0) |
                 (!p->count ? 4 : 0);
        break;
    case 0x3c:
        if (write) {
            if (*value & ~0xfULL) { goto rejected; }
            s->serial_config[p->channel][off / 4] = *value;
            p->active = *value & 1;
            if (!p->active) {
                if (ch == 0) { z7_shoulder_reset(&s->shoulder); }
                timer_del(p->timer);
                p->count = p->rx_count = p->rx_head = p->pending = 0;
                if (s->exmcu_test_device && ch == 0) {
                    s->exmcu_tx_count = 0;
                    s->exmcu_tx_ready = true;
                }
                if (s->exmcu_test_device && ch == 1) { s->exmcu_rx_offset = 0; }
            }
        } else { *value = s->serial_config[p->channel][off / 4]; }
        break;
    case 0x24:
        if (write) { goto rejected; }
        *value = p->pending;
        break;
    case 0x28:
        if (!write || (*value & ~0x1ffULL)) { goto rejected; }
        p->pending &= ~*value;
        break;
    case 0x20:
        if (write) {
            if (*value & ~0xffULL) { goto rejected; }
            s->serial_config[p->channel][off / 4] = *value;
        } else { *value = s->serial_config[p->channel][off / 4]; }
        break;
    case 0x64:
        if (!write || *value > 1) { goto rejected; }
        p->active = *value;
        s->serial_config[p->channel][off / 4] = *value;
        if (*value) {
            timer_mod(p->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        } else {
            if (ch == 0) { z7_shoulder_reset(&s->shoulder); }
            timer_del(p->timer);
            p->pending = 0;
        }
        break;
    default: return false;
    }
    z7_panel_irq(p);
    z7_mmio_trace(addr, size, write, *value);
    return true;
rejected:
    z7_unknown_access(s, addr, size, write, *value);
    return true;
}

static bool z7_serial_config_access(Z7LabState *s, hwaddr addr, unsigned size,
                                    bool write, uint64_t *value)
{
    if (!s->recovery_profile || size != 4 || addr < 0x2c0a0000 ||
        addr >= 0x2c0a0b00 || (addr & 3)) { return false; }
    unsigned ch = (addr - 0x2c0a0000) / 0x100, off = addr & 0xff;
    uint32_t mask;
    if (!s->serial_initialized[ch]) { return false; }
    switch (off) {
    case 0x08: case 0x18: case 0x1c: case 0x38: mask = 0xff; break;
    case 0x0c: mask = 0xc3f; break; /* 100687de additionally selects DMA */
    case 0x10: mask = 0x1fbf; break;
    case 0x34: mask = UINT32_MAX; break;
    case 0x3c: mask = 0xf; break;
    case 0x4c: mask = 0xffff; break;
    case 0x20: mask = 0xff; break; /* 10068788 and original IRQ handler */
    case 0x64: mask = 0; break; /* 10068824 stops; active master start remains unknown */
    default: return false;
    }
    if (write) {
        if (*value & ~(uint64_t)mask) { return false; }
        if (ch == 0 && s->serial_config[ch][off / 4] != *value) {
            z7_shoulder_reset(&s->shoulder);
        }
        s->serial_config[ch][off / 4] = *value;
    } else { *value = s->serial_config[ch][off / 4]; }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

/* Original 1010e7ce chooses 0x31 for its elapsed-time mode. CAPTG presets
 * fffff45b for the ~3000-unit delay at 10178c64. One microsecond per tick is
 * an explicit timing hypothesis; other clock selectors remain unclocked. */
static void z7_channel_irq(Z7LabState *s, unsigned ch)
{
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 172 + ch - 32),
                 !(s->channel_irq_mask & (1U << ch)) &&
                 (s->channel_pending[ch] & s->channel_config[ch][0][0]));
}

static void z7_channel_schedule(Z7LabState *s)
{
    int64_t next = INT64_MAX;
    for (unsigned ch = 0; ch < 12; ch++) {
        if (s->channel_clocked[ch]) { next = MIN(next, s->channel_deadline[ch]); }
    }
    if (next == INT64_MAX) { timer_del(s->channel_timer); }
    else { timer_mod(s->channel_timer, next); }
}

static uint32_t z7_channel_count(Z7LabState *s, unsigned ch, int64_t now)
{
    return s->channel_counter[ch] + (s->channel_clocked[ch] ?
            (now - s->channel_epoch[ch]) / 1000 : 0);
}

static void z7_channel_expire(void *opaque)
{
    Z7LabState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned ch = 0; ch < 12; ch++) {
        if (!s->channel_clocked[ch] || s->channel_deadline[ch] > now) { continue; }
        s->channel_counter[ch] = 0;
        s->channel_epoch[ch] = s->channel_deadline[ch];
        s->channel_deadline[ch] += ((uint64_t)s->channel_config[ch][1][2] + 1) * 1000;
        s->channel_pending[ch] |= 1;
        /* 1010e8f6 selects a finite run, 1010eb62/1005b578 program
         * +38 enable and +3c repetition limit. In particular the native
         * 101de518(0, callback) delay is one terminal, NOT a 1MHz ticker.
         * The driver's +58 pointer reads repetitions and its IRQ mask
         * includes bit3 for the finite-run end. Clock rate is still inferred. */
        if (ch <= 4 && s->channel_config[ch][1][0x38 / 4]) {
            s->channel_repetitions[ch]++;
            uint32_t limit = s->channel_config[ch][1][0x3c / 4];
            if (limit && s->channel_repetitions[ch] >= limit) {
                s->channel_clocked[ch] = false;
                s->channel_enable &= ~(1U << ch);
                s->channel_pending[ch] |= 8;
                fprintf(stderr, "Z7_COUNTER_FINITE_END channel=%u repetitions=%u\n",
                        ch, s->channel_repetitions[ch]);
            }
        }
        z7_channel_irq(s, ch);
        fprintf(stderr, "Z7_COUNTER_TERMINAL channel=%u pending=0x%x irq=%u\n",
                ch, s->channel_pending[ch], 172 + ch);
    }
    z7_channel_schedule(s);
}

/* Original pointer table a1805f40: 12 channels, 23 pointers each. */
static bool z7_channel_access(Z7LabState *s, hwaddr addr, unsigned size,
                              bool write, uint64_t *value)
{
    if (!s->recovery_profile || size != 4 || (addr & 3)) { return false; }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (addr == 0x2c011000 || addr == 0x2c01100c) {
        uint32_t *reg = addr == 0x2c011000 ? &s->channel_enable : &s->channel_irq_mask;
        if (write) {
            if (*value & ~0xfffULL) { return false; }
            if (addr == 0x2c011000) {
                unsigned changed = *reg ^ *value;
                /* Reject the entire transaction before changing any channel. */
                for (unsigned ch = 0; ch < 12; ch++) {
                    if ((changed & *value & (1U << ch)) &&
                        s->channel_config[ch][1][1] == 0x31 &&
                        z7_channel_count(s, ch, now) > s->channel_config[ch][1][2]) {
                        return false;
                    }
                }
                for (unsigned ch = 0; ch < 12; ch++) {
                    if (!(changed & (1U << ch))) { continue; }
                    s->channel_counter[ch] = z7_channel_count(s, ch, now);
                    s->channel_clocked[ch] = false;
                    if ((*value & (1U << ch)) && s->channel_config[ch][1][1] == 0x31) {
                        uint32_t terminal = s->channel_config[ch][1][2];
                        s->channel_epoch[ch] = now;
                        s->channel_deadline[ch] = now + ((uint64_t)terminal - s->channel_counter[ch] + 1) * 1000;
                        s->channel_clocked[ch] = true;
                        fprintf(stderr, "Z7_COUNTER_START channel=%u initial=0x%x terminal=0x%x tick_ns=1000 hypothesis=1\n",
                                ch, s->channel_counter[ch], terminal);
                    }
                }
                z7_channel_schedule(s);
            }
            *reg = *value;
            for (unsigned ch = 0; ch < 12; ch++) { z7_channel_irq(s, ch); }
        } else { *value = *reg; }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    /* 1005b3f4..3fe submits a shared 32-bit initial value followed by the
     * selected channel bit. Mode 0x31 subsequently counts from this preset. */
    if (addr == 0x2c011004 || addr == 0x2c011008) {
        if (!write) { return false; }
        if (addr == 0x2c011004) {
            s->channel_preset_latch = *value;
        } else {
            if (!*value || (*value & ~0xfffULL) || (*value & (*value - 1))) { return false; }
            unsigned selected = ctz32(*value);
            if (s->channel_enable & (1U << selected)) { return false; }
            s->channel_preset[selected] = s->channel_preset_latch;
            s->channel_counter[selected] = s->channel_preset_latch;
            fprintf(stderr, "Z7_CHANNEL_PRESET channel=%u value=0x%x\n",
                    selected, s->channel_preset_latch);
        }
        z7_mmio_trace(addr, size, true, *value);
        return true;
    }
    unsigned bank, ch, off;
    if (addr >= 0x2c010100 && addr < 0x2c010d00) { bank = 0; }
    else if (addr >= 0x2c011100 && addr < 0x2c011d00) { bank = 1; }
    else { return false; }
    ch = ((addr & 0xfff) >> 8) - 1;
    off = addr & 0xff;
    if (!bank && off == 0x10) {
        if (write) { s->channel_pending[ch] &= ~*value; z7_channel_irq(s, ch); }
        else { *value = s->channel_pending[ch]; }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    if (bank && off == 0x0c && !write) {
        *value = z7_channel_count(s, ch, now);
        z7_mmio_trace(addr, size, false, *value);
        return true;
    }
    if (bank && off == 0x40 && ch <= 4 && !write) {
        *value = s->channel_repetitions[ch];
        z7_mmio_trace(addr, size, false, *value);
        return true;
    }
    uint32_t mask;
    if (!bank) {
        switch (off) {
        case 0: mask = 0x3f; break;
        case 4: case 8: mask = 0x3f; break;
        default: return false;
        }
    } else {
        switch (off) {
        case 0: mask = 1; break; /* reset pulse */
        case 4: mask = 0xff; break; /* byte config */
        case 8: mask = UINT32_MAX; break; /* reload word */
        case 0x10: if (ch != 5 && ch != 6) { return false; } mask = 1; break;
        case 0x1c: if (ch > 6) { return false; } mask = 1; break;
        case 0x24: case 0x38:
            if (ch > 4) { return false; } mask = 1; break;
        case 0x28: if (ch > 4) { return false; } mask = 1; break; /* 1005b480 */
        case 0x2c: case 0x3c:
            if (ch > 4) { return false; } mask = UINT32_MAX; break; /* 1005b476/592 */
        case 0x20: if (ch > 6) { return false; } mask = 0xf; break; /* 1005b548 */
        default: return false;
        }
    }
    if (write) {
        if (*value & ~(uint64_t)mask) { return false; }
        s->channel_config[ch][bank][off / 4] = *value;
        if (bank && off == 0 && *value) {
            s->channel_clocked[ch] = false;
            s->channel_counter[ch] = s->channel_pending[ch] = 0;
            s->channel_repetitions[ch] = 0;
            z7_channel_schedule(s);
        }
        z7_channel_irq(s, ch);
    } else { *value = s->channel_config[ch][bank][off / 4]; }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

static void z7_capture_irq(Z7LabState *s, unsigned group)
{
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 86 + group - 32),
                (s->capture_irq_status[group] & s->capture_irq_enable[group]) != 0);
}

/* Native 1010bb4e..1010bb76 registers IRQs 94..98. The eight status
 * words are 20068210+20*n; each ISR writes back the observed causes. */
static void z7_isp_irq(Z7LabState *s)
{
    static const unsigned groups[8] = {0, 0, 1, 2, 2, 3, 3, 4};
    uint32_t pending[5] = {0};
    for (unsigned bank = 0; bank < 8; bank++) {
        pending[groups[bank]] |= s->capture_isp_irq_enable[bank] & s->capture_isp_irq_status[bank];
    }
    for (unsigned group = 0; group < 5; group++) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 94 + group - 32), pending[group] != 0);
    }
}

/* End-of-sequence must survive either ordering of TG stop and DMA drain.
 * Only an actual engine1 stop arms it; clear the latch on delivery. */
static void z7_capture_maybe_drained(Z7LabState *s)
{
    if (s->capture_drain_pending && s->capture_native0_frames &&
        !(s->capture_isp_gate[1] & 0xeff) &&
        (!s->capture_control[2] || (s->capture_control[0] & 3))) {
        s->capture_drain_pending = false;
        s->capture_isp_irq_status[0] |= 0x80;
        z7_isp_irq(s);
        fprintf(stderr, "Z7_CAPTURE_DRAINED raw_batches=%u common_cause=0x80 new_frame=0\n",
                s->capture_native0_frames);
    }
}

static void z7_isp_stopped(void *opaque)
{
    Z7LabState *s = opaque;
    uint32_t engines = s->capture_isp_stop_pending;
    s->capture_isp_stop_pending = 0;
    /* Asynchronous pipeline drain notifications, distinct from the low-level
     * stop causes below. Native PREPRO registers these in 100bd070 and clears
     * its outstanding bitmap a261d6f0 in 100bd04c/bccf2/bcdd4/bceec.
     * A +0c request has already stopped our synchronous DMA worker. Report
     * the idle/drained channels on the deferred timer, without a frame IRQ.
     * The 4/8 pair has one shared cause and must both be stopped first.
     * Timing is a functional hypothesis; these bit mappings are from ROM. */
    if (engines & 1) { s->capture_isp_irq_status[0] |= 0x200; }
    if ((engines & 0xc) && !(s->capture_isp_gate[1] & 0xc)) {
        s->capture_isp_irq_status[0] |= 0x8000;
    }
    if (engines & 0x10) { s->capture_isp_irq_status[0] |= 0x80000000; }
    if (engines & 0x20) { s->capture_isp_irq_status[0] |= 0x08000000; }
    if (engines & 0x40) { s->capture_isp_irq_status[0] |= 0x20000; }
    if (engines & 0x80) { s->capture_isp_irq_status[0] |= 0x80000; }
    if (engines & 0x400) { s->capture_isp_irq_status[1] |= 8; }
    if (engines & 0x200) { s->capture_isp_irq_status[1] |= 0x80; }
    if (engines & 0x800) { s->capture_isp_irq_status[1] |= 0x800; }
    /* Engine2's drain callback is in bank7: 100bcfe8 -> PREPRO flag0x20
     * -> 100bc900 -> 04190006 -> master flag0x20 -> 100730ae.
     * Low-level bits4/5 alone never deliver this native stop handshake. */
    if (engines & 2) { s->capture_isp_irq_status[7] |= 8; }
    /* Full capture shutdown also has a common end-of-sequence event.
     * 100bc532 enables bank0 bit7; 100bcc98 -> PREPRO0x10 -> 100bfc0e
     * -> 04190004 clears the master's non-0x80 channels (100742ec).
     * Bank7 bit3 above clears its remaining 0x80 channel (100777d4).
     * No new pixels/frame are generated: synchronous input writes have
     * finished and both the TG and all submitted channels are stopped. */
    if (engines & 1) { s->capture_drain_pending = true; }
    z7_capture_maybe_drained(s);
    /* Exact stop-cause decoder 1010b836; the common ISR disables each
     * stopped engine and calls the callback registered by 1010be54.
     * These acknowledge an idle stop request, never a captured frame. */
    if (engines & 0x20) { s->capture_isp_irq_status[0] |= 0x10000000; }
    if (engines & 0x10) { s->capture_isp_irq_status[1] |= 1; }
    if (engines & 0x400) { s->capture_isp_irq_status[1] |= 0x10; }
    if (engines & 0x200) { s->capture_isp_irq_status[1] |= 0x100; }
    if (engines & 0x800) { s->capture_isp_irq_status[1] |= 0x1000; }
    if (engines & 2) { s->capture_isp_irq_status[7] |= 0x30; }
    z7_isp_irq(s);
    fprintf(stderr, "Z7_ISP_STOP engines=0x%x frame_completed=0\n", engines);
}

/* Explicit diagnostic signal, not a sensor frame or an automatic success
 * response. Tests exercise the hardware IRQ -> original ISR -> event path.
 * The signal's physical meaning and timing have not been established. */
static void z7_capture_test_pulse(void *opaque)
{
    Z7LabState *s = opaque;
    s->capture_irq_status[1] |= 0x10;
    z7_capture_irq(s, 1);
    fprintf(stderr, "Z7_TG_TEST_PULSE irq=87 status=0x10 raw_frames=0 diagnostic_only=1\n");
}

/* Isolated timing experiment. IRQ 87 bit 7 reaches 1017889e -> 100f52fe
 * (02010001) -> 101de3de (041f0141). This establishes the event route,
 * not the physical sensor clock rate or a completed pixel transfer. */
/* Functional stationary-scene statistics input. The native configuration
 * uses three u16 planes (60x40, row128) in six slots separated by0x36000.
 * Regional CFA averages model scene measurements only; Nikon's statistic
 * calibration, AF and accumulation units are not bit-accurate here. */
static void z7_capture_rgb_statistics(Z7LabState *s)
{
    if (!s->image_process_test || !s->capture_native0_data) { return; }
    for (unsigned ch = 0; ch < 3; ch++) {
        uint32_t *r = s->capture_image_channel[ch + 2];
        if (r[0x14/4] != 3 || r[0x18/4] != 1 || r[0x40/4] != 1 ||
            r[0x48/4] != 5 || r[0x80/4] != 0x36000) { return; }
        for (unsigned a = 0x50/4; a <= 0x6c/4; a++) {
            if (r[a] != 0x4d4dc000 + ch * 0x12000) { return; }
        }
    }
    uint8_t planes[3][128 * 40] = {{0}};
    const uint8_t *raw = (uint8_t *)s->capture_native0_data;
    for (unsigned y = 0; y < 40; y++) {
        for (unsigned x = 0; x < 60; x++) {
            uint64_t sum[3] = {0}; unsigned n = 0;
            for (unsigned sy = y * 466 / 40; sy < (y + 1) * 466 / 40; sy++) {
                for (unsigned sx = x * 1390 / 60; sx < (x + 1) * 1390 / 60; sx++) {
                    const uint8_t *p = raw + sy * 2 * 5568 + sx * 4;
                    sum[0] += lduw_le_p(p) & 4095;
                    sum[1] += ((lduw_le_p(p + 2) & 4095) + (lduw_le_p(p + 5568) & 4095) + 1) / 2;
                    sum[2] += lduw_le_p(p + 5570) & 4095;
                    n++;
                }
            }
            for (unsigned ch = 0; ch < 3; ch++) {
                stw_le_p(planes[ch] + y * 128 + x * 2, (sum[ch] + n / 2) / n);
            }
        }
    }
    /* All history slots see the same stationary photo. Each write is bounded
     * by the original pool20 descriptor11524990, not an application-state edit. */
    for (unsigned slot = 0; slot < 6; slot++) {
        for (unsigned ch = 0; ch < 3; ch++) {
            uint32_t base = s->capture_image_channel[ch + 2][0x50 / 4];
            cpu_physical_memory_write(base + slot * 0x36000, planes[ch], sizeof(planes[ch]));
        }
    }
    if (!s->capture_statistics_frames++) {
        fprintf(stderr, "Z7_CMOS_RGB_STATISTICS width=60 height=40 stride=128 planes=3 ring_slots=6 source=photo_RGGB12 bit_accurate=0\n");
    }
}

static void z7_capture_ingress_test(Z7LabState *s)
{
    if (s->capture_native0_data &&
        (!s->capture_ingress_done || s->capture_isp_boundary_stage % 3 == 1) &&
        !(s->capture_isp_gate[0] & 0x400) && (s->capture_isp_gate[1] & 0x400) &&
        (s->capture_isp_gate[2] & 0x400) && s->capture_processing[0x1500 / 4] == 0 &&
        s->capture_processing[0x1504 / 4] == 0x0adc0000 &&
        s->capture_processing[0x1508 / 4] == 0x03a40000 &&
        s->capture_processing[0x1808 / 4] == 5568) {
        /* Native 1010a19e geometry and 1010a0b6 engine 400. The separate
         * 2780x932 LE-u16 CFA layout is an explicit input-format hypothesis.
         * Fill every submitted ring slot with the same user-selected photo. */
        unsigned bank = s->capture_processing[0x183c / 4] & 1;
        for (unsigned slot = 0; slot < 4; slot++) {
            uint32_t dst = s->capture_processing[(0x1850 + bank * 0x10 + slot * 4) / 4];
            if (dst < 0x53900000 || (uint64_t)dst + s->capture_native0_length > 0x9e5b3000) { return; }
        }
        g_autofree char *hash = !s->capture_native0_frames ?
            g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                (uint8_t *)s->capture_native0_data, s->capture_native0_length) : NULL;
        for (unsigned slot = 0; slot < 4; slot++) {
            uint32_t dst = s->capture_processing[(0x1850 + bank * 0x10 + slot * 4) / 4];
            cpu_physical_memory_write(dst, s->capture_native0_data, s->capture_native0_length);
            if (!s->capture_native0_frames) {
                fprintf(stderr, "Z7_RAW_NATIVE0_TEST slot=%u destination=0x%08x bytes=%zu sha256=%s consumed=0 packing_verified=0\n",
                        slot, dst, s->capture_native0_length, hash);
            }
        }
        fprintf(stderr, "Z7_RAW_NATIVE0_STREAM seq=%u bank=%u slots=4 bytes_per_slot=%zu packing_verified=0\n",
                ++s->capture_native0_frames, bank, s->capture_native0_length);
        z7_image_process_schedule(s);
        z7_capture_rgb_statistics(s);
        s->capture_ingress_done = true;
    }
    if (!s->capture_ingress_data || s->capture_ingress_done ||
        (s->capture_isp_gate[0] & 0x200) ||
        !(s->capture_isp_gate[1] & 0x200) ||
        !(s->capture_isp_gate[2] & 0x200) ||
        s->capture_processing[0x1514 / 4] != 6 ||
        s->capture_processing[0x1518 / 4] != 0x05640000 ||
        s->capture_processing[0x151c / 4] != 0x03960000 ||
        s->capture_processing[0x1a08 / 4] != 2080) { return; }
    uint32_t destination = s->capture_processing[0x1a50 / 4];
    /* Native pool 44c00000..4fbd2400. Follow the original submitted slot.
     * 1010a0b6/1010a0e2 select ISP engine 0x200 for this processing path;
     * 1010bc16 submits, 1010b634 enables. Channel +3c is a descriptor
     * option (1010a3f2), not sufficient evidence of an engine start bit.
     * Gate interpretation and pixel packing remain diagnostic hypotheses. */
    if (destination < 0x44c00000 || (uint64_t)destination + s->capture_ingress_length > 0x4fbd2400) { return; }
    g_autofree uint8_t *check = g_malloc(s->capture_ingress_length);
    cpu_physical_memory_write(destination, s->capture_ingress_data, s->capture_ingress_length);
    cpu_physical_memory_read(destination, check, s->capture_ingress_length);
    if (memcmp(check, s->capture_ingress_data, s->capture_ingress_length)) {
        error_report("Z7_RAW_INGRESS_TEST readback mismatch");
        exit(2);
    }
    s->capture_ingress_done = true;
    g_autofree char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, check, s->capture_ingress_length);
    fprintf(stderr, "Z7_RAW_INGRESS_TEST destination=0x%08x bytes=%zu sha256=%s transferred=1 consumed=0 completion_irq=0 packing_verified=0\n",
            destination, s->capture_ingress_length, hash);
}

static void z7_capture_periodic_test(void *opaque)
{
    Z7LabState *s = opaque;
    unsigned limit = s->capture_periodic_paired_test ? 180 : 90;
    if (!s->capture_test_running || (!s->capture_native0_data && s->capture_periodic_test_count >= limit)) { return; }
    unsigned frame_phase = s->capture_isp_boundary_stage % 3;
    unsigned native0_before = s->capture_native0_frames;
    z7_capture_ingress_test(s);
    bool native0_written = s->capture_native0_frames != native0_before;
    if (s->capture_isp_boundary_test && s->capture_ingress_done) {
        /* Isolated event-route experiment, NOT a modeled ISP completion.
         * 100bcc40 maps these to input worker flags 1 and 4. Statistics,
         * actual boundary meaning and processing latency remain unknown. */
        unsigned stage = s->capture_isp_boundary_stage++ % 3;
        uint32_t boundary = stage == 0 ? 1 : stage == 1 ? 0x40 : 0x18;
        s->capture_isp_irq_status[0] |= boundary;
        uint32_t bank3 = 0, bank4 = 0;
        if (native0_written) {
            /* Mode 0 / engine 400 enables bank3 bits30/31. Native
             * 100bcdd4 maps bit31 to PREPRO 2000 -> 100bc07e(100),
             * which advances the original FRNTELB1 ring. Signal only
             * after DMA above actually wrote the submitted addresses. */
            bank3 |= 0x80000000;
        } else if (!s->capture_native0_frames && s->capture_isp_boundary_stage == 2) {
            /* Mode 1 / engine 200 uses 100bcd76, bank4 bit8. */
            bank4 |= 0x100;
        }
        if (stage == 2) {
            /* Native 100bc4ce waits on 0x05b00000 in this configuration.
             * The driver callbacks map these additional channel causes to
             * that mask. Route experiment only; no processed data claimed. */
            bank3 |= 0x21;
            if (s->capture_native0_frames && (s->capture_isp_gate[2] & 0x400)) {
                bank3 |= 0x40000000;
            } else {
                bank4 |= 0x80;
            }
        }
        s->capture_isp_irq_status[3] |= bank3;
        s->capture_isp_irq_status[4] |= bank4;
        z7_isp_irq(s);
        fprintf(stderr, "Z7_ISP_BOUNDARY_TEST stage=%u bank0=0x%x bank3=0x%x bank4=0x%x processed_frames=0 diagnostic_only=1\n",
                stage, boundary, bank3, bank4);
    }
    /* Complementary edge experiment: 02010002 wakes the native parameter
     * apply worker (100f49e0 -> 1017b6c6) and exits sensor state 8.
     * The paired-edge physical timing is still a diagnostic assumption. */
    unsigned cause = s->capture_periodic_paired_test && (s->capture_periodic_test_count & 1) ? 0x10 : 0x80;
    if (s->capture_native0_data && s->capture_isp_boundary_test &&
        s->capture_periodic_paired_test) {
        /* One parameter/frame edge pair for each RAW transaction. Previously
         * TG had a two-tick cycle while PREPRO/RAW had a three-tick cycle:
         * it produced 30 parameter events/s for 20 frames/s and overflowed
         * the native 60-entry CAPSVC queue (lv264). Keep edge ordering and
         * all native event consumers; no guest queue/state is patched. */
        cause = frame_phase == 0 ? 0x80 : frame_phase == 1 ? 0x10 : 0;
    }
    if (cause) {
        s->capture_irq_status[1] |= cause;
        z7_capture_irq(s, 1);
    }
    fprintf(stderr, "Z7_TG_PERIODIC_TEST seq=%u irq=87 status=0x%x diagnostic_only=1\n",
            ++s->capture_periodic_test_count, cause);
    timer_mod(s->capture_periodic_test_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->capture_period_ns);
}

static void z7_capture_test_update(Z7LabState *s)
{
    if (!s->capture_test_pulse && !s->capture_periodic_test) { return; }
    bool running = s->capture_control[2] && !(s->capture_control[0] & 3);
    if (running && !s->capture_test_running) {
        /* A fresh TG stream must not inherit the previous stream's valid
         * DMA latch or a half-finished boundary cycle. Otherwise restart
         * can emit PREPRO completion before its first RAW descriptors exist.
         * Keep lifetime counters, but require fresh DMA before boundaries. */
        s->capture_ingress_done = false;
        s->capture_isp_boundary_stage = 0;
        if (s->capture_test_pulse) {
            timer_mod(s->capture_test_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
        }
        if (s->capture_periodic_test) {
            timer_mod(s->capture_periodic_test_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 33333333);
        }
    } else if (!running) {
        timer_del(s->capture_test_timer);
        timer_del(s->capture_periodic_test_timer);
    }
    s->capture_test_running = running;
}

/* The original three ISRs 1010c39a/370/35c acknowledge by writing exactly
 * the observed cause bits. 1010c9b4 enables registered callbacks at +34;
 * 1010c67e/69e update the two callback domains sharing +40. */
static bool z7_capture_irq_access(Z7LabState *s, hwaddr addr, unsigned size,
                                  bool write, uint64_t *value)
{
    if (!s->recovery_profile || addr < 0x20060034 || addr > 0x20060050) { return false; }
    unsigned off = addr - 0x20060034, group = off / 12, reg = off % 12;
    if (reg > 4) { return false; }
    uint32_t mask = group == 1 ? 0x3fffff : UINT32_MAX;
    if (size != 4 || (off & 3) || (write && (*value & ~(uint64_t)mask))) {
        z7_unknown_access(s, addr, size, write, *value);
        return true;
    }
    if (write) {
        if (reg == 4) { s->capture_irq_status[group] &= ~(uint32_t)*value; }
        else { s->capture_irq_enable[group] = *value; }
        z7_capture_irq(s, group);
    } else {
        *value = reg == 4 ? s->capture_irq_status[group] : s->capture_irq_enable[group];
    }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

/* 1010c5ae/5de select the set or clear alias for each mask; 1010c60e
 * reads the resulting state through the set address. A zero mask must not
 * undo another channel's request. Reset state zero remains a hypothesis. */
static bool z7_capture_control_access(Z7LabState *s, hwaddr addr, unsigned size,
                                      bool write, uint64_t *value)
{
    static const uint32_t masks[] = { 0x3f, 0x7fff, 1, 1, 0x7ff };
    if (!s->recovery_profile || addr < 0x20060000 || addr > 0x20060024) {
        return false;
    }
    unsigned off = addr - 0x20060000, bank = off / 8;
    if (size != 4 || (off & 3) || (write && (*value & ~(uint64_t)masks[bank]))) {
        z7_unknown_access(s, addr, size, write, *value);
        return true;
    }
    if (write) {
        if (off & 4) { s->capture_control[bank] &= ~(uint32_t)*value; }
        else { s->capture_control[bank] |= *value; }
        z7_capture_test_update(s);
        z7_capture_maybe_drained(s);
    } else {
        /* Clear-alias readback is not exercised by the original driver. */
        if (off & 4) {
            z7_unknown_access(s, addr, size, false, 0);
            return true;
        }
        *value = s->capture_control[bank];
    }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

/* SDES configuration; frame status is not inferred from these writes. */
static bool z7_capture_sync_access(Z7LabState *s, hwaddr addr, unsigned size,
                                   bool write, uint64_t *value)
{
    if (!s->graphics_profile || size != 4 || (addr & 3) ||
        addr < 0x20061000 || addr >= 0x20061500) { return false; }
    unsigned off = addr - 0x20061000;
    bool rw = off == 0 || off == 0xe8 || off == 0xf4;
    /* Original SDES configuration setters 100cd422/558/55e/5ae/62c/
     * 634/656/65e/666/790/7da/7e2/80c/818/8a0. Status starts at +300
     * and is deliberately absent: config writes do not synthesize input. */
    bool config = rw || off == 4 || off == 0x10 ||
        (off >= 0x14 && off <= 0x48) || (off >= 0x5c && off <= 0x94) ||
        (off >= 0xa0 && off <= 0xac) || off == 0xb4 ||
        (off >= 0xb8 && off <= 0xd4) || off == 0xe0 || off == 0xe4 || off == 0xec ||
        off == 0xf8 || off == 0xfc || (off >= 0x100 && off <= 0x120) ||
        off == 0x12c || off == 0x130 || off == 0x134 || off == 0x138 || off == 0x13c ||
        (off >= 0x200 && off <= 0x28c) ||
        /* 106168ac + 100cd96e: five sets of five byte configuration fields. */
        (off >= 0x400 && off <= 0x4b0 && (off & 0x1f) <= 0x10 &&
         (off < 0x480 || off >= 0x4a0));
    if (!config || (!write && !rw)) { return false; }
    uint32_t mask = off == 0 ? 0xf0ffff : off == 0xe8 || off == 0xf4 ? 3 : off >= 0x400 ? 0xff : UINT32_MAX;
    if (write) {
        if (*value & ~(uint64_t)mask) { return false; }
        s->capture_sync[off / 4] = *value;
    } else { *value = s->capture_sync[off / 4]; }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

/* Pointer table a18069fc: ten image channels, stride 0x100. Original
 * 1010c256/27e/2ac set controls; 1010c2ee submits address sets. No DMA
 * completion or received-data status is synthesized by configuration. */
static bool z7_capture_image_channel_access(Z7LabState *s, hwaddr addr, unsigned size,
                                           bool write, uint64_t *value)
{
    /* 1010bfbc/bfec, 1010c028/c0c8/c16a/c1a8 and table 1061687c:
     * geometry, crop and analysis configuration only. +3e0 status is absent. */
    if (s->graphics_profile && write && size == 4 && !(addr & 3) &&
        addr >= 0x2006a200 && addr <= 0x2006a370) {
        unsigned off = addr - 0x2006a000;
        s->capture_analysis_config[off / 4] = *value;
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    if (!s->graphics_profile || size != 4 || (addr & 3) ||
        addr < 0x2006a400 || addr >= 0x2006ae00) { return false; }
    unsigned ch = (addr - 0x2006a400) / 0x100, off = addr & 0xff;
    bool planes = ch == 2 || ch == 3 || ch == 4 || ch == 9;
    bool control = off == 0x14 || off == 0x18 || off == 0x24 ||
        off == 0x3c || off == 0x40 || off == 0x44 || off == 0x48 || off == 0x4c || off == 0x80;
    bool address = off == 0x50 || off == 0x60 ||
        (planes && ((off >= 0x54 && off <= 0x5c) || (off >= 0x64 && off <= 0x6c)));
    if (!(control || address || off == 0x74) || (!write && off != 0x3c)) { return false; }
    if (write) {
        s->capture_image_channel[ch][off / 4] = *value;
        if (address) {
            fprintf(stderr, "Z7_CAPTURE_ADDRESS channel=%u offset=0x%x value=0x%08" PRIx64 " dma_completed=0\n",
                    ch, off, *value);
        }
    } else { *value = s->capture_image_channel[ch][off / 4]; }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

static bool z7_capture_processing_access(Z7LabState *s, hwaddr addr, unsigned size,
                                        bool write, uint64_t *value)
{
    /* Explicit discovery-only config recorder. This does not execute the
     * 2038/203c processor or generate completion. Readback is restricted
     * to the RMW config field proven by native10106340. */
    if (s->graphics_profile && s->capture_isp_boundary_test && getenv("Z7_IMAGE_CONFIG_RECORD") &&
        size == 4 && !(addr & 3) &&
        ((addr >= 0x20380008 && addr < 0x20390000) ||
         (addr >= 0x203c0008 && addr < 0x203d0000))) {
        unsigned bank = addr >= 0x203c0000, off = addr & 0xffff;
        if (s->image_process_test && !bank && off == 0x24) {
            if (write) { s->image_processor_config[0][off / 4] &= ~*value; z7_image_raw_irq(s); }
            else { *value = s->image_processor_config[0][off / 4]; }
            z7_mmio_trace(addr, size, write, *value);
            return true;
        }
        if (write) {
            s->image_processor_config[bank][off / 4] = *value;
            if (s->image_process_test && !bank) {
                if (off == 0xf0 && *value == 0xfc400) {
                    s->image_drain_requests = 0;
                }
                if (s->image_processor_config[0][0xf0 / 4] == 0xfc400 &&
                    *value == 1 && (off == 0x54 || off == 0x60 || off == 0x6c || off == 0x78)) {
                    s->image_drain_requests |= 1u << ((off - 0x54) / 12);
                    if (s->image_drain_requests == 15) {
                        timer_del(s->image_process_timer);
                        timer_mod(s->image_drain_timer,
                                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
                    }
                }
            }
            if (!bank && off == 8 && (*value & 1)) { z7_image_process_schedule(s); }
            z7_mmio_trace(addr, size, true, *value);
            fprintf(stderr, "Z7_IMAGE_CONFIG_UNDECODED address=0x%08" HWADDR_PRIx " value=0x%08" PRIx64 " executed=0\n", addr, *value);
            return true;
        }
        if (off == 0x3c4) {
            *value = s->image_processor_config[bank][off / 4];
            z7_mmio_trace(addr, size, false, *value);
            return true;
        }
    }
    /* 1017da00: two lists, four rows of sixteen words. */
    if (s->graphics_profile && s->capture_isp_boundary_test && write && size == 4 &&
        !(addr & 3) && addr >= 0x20102200 && addr < 0x20102400) {
        s->image_command_lists[(addr - 0x20102200) / 0x100][(addr & 0xff) / 4] = *value;
        z7_mmio_trace(addr, size, true, *value);
        return true;
    }
    /* 1017e48a and table10615d68: eight two-word output configs. */
    if (s->graphics_profile && s->capture_isp_boundary_test && write && size == 4 &&
        !(addr & 3) && addr >= 0x20104000 && addr < 0x20104080 && (addr & 0xf) <= 4) {
        s->image_output_config[(addr - 0x20104000) / 0x10][(addr & 0xf) / 4] = *value;
        z7_mmio_trace(addr, size, true, *value);
        return true;
    }
    /* 1017e566, ROM106166d8: six coefficient banks, uint16 entries
     * uploaded in word accesses. Preserve native LUTs verbatim. */
    if (s->graphics_profile && s->capture_isp_boundary_test && write && size == 4 &&
        !(addr & 3) && addr >= 0x24000000 && addr < 0x24006000 && *value <= 0xffff) {
        s->image_unit_lut[(addr - 0x24000000) / 4] = *value;
        return true;
    }
    /* Table10616308, native1017e748: reset descriptor cursor (+14),
     * then submit parameter address (+10). Retain before decoding units.
     * No completion is synthesized from a parameter address write. */
    if (s->graphics_profile && s->capture_isp_boundary_test && size == 4 && !(addr & 3)) {
        static const uint32_t bases[] = { 0x20100800, 0x20100820, 0x20100840,
            0x20100880, 0x201008a0, 0x201008c0, 0x201008e0, 0x20100c80,
            0x20100900, 0x20100ac0, 0x20100cc0, 0x20100ce0, 0x20100940,
            0x20100960, 0x20100980, 0x201009a0, 0x201009c0, 0x201009e0,
            0x20100ba0, 0x20100bc0, 0x20100d40, 0x20100d60, 0x20100d80,
            0x20100da0, 0x20100dc0, 0x20100de0, 0x20100e00, 0x20100e20,
            0x20100a40, 0x20100a80, 0x20100aa0, 0x20100a60, 0x20100d00,
            0x20100d20, 0x20100e40, 0x20100a00, 0x20100a20, 0x20100b60,
            0x20100b80, 0x20100920, 0x20100e60, 0x20100e80 };
        for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
            /* 1017e436 tests low two bits for busy(3), via table10616308.
             * The functional pipeline executes atomically in its timer callback;
             * a queued transaction is busy, otherwise no unit remains in flight.
             * Do not expose idle success in the config-only experiment. */
            if (!write && addr == bases[i] && s->image_process_test) {
                *value = timer_pending(s->image_process_timer) && z7_image_process_ready(s) ? 3 : 0;
                z7_mmio_trace(addr, size, false, *value);
                return true;
            }
            bool extra = (bases[i] >= 0x20100940 && bases[i] <= 0x201009e0) ||
                         bases[i] == 0x20100ba0 || bases[i] == 0x20100bc0;
            if (write && (addr == bases[i] + 0x10 || addr == bases[i] + 0x14 ||
                (extra && (addr == bases[i] + 8 || addr == bases[i] + 0xc)))) {
                s->image_unit_config[(addr - 0x20100800) / 4] = *value;
                z7_mmio_trace(addr, size, true, *value);
                return true;
            }
        }
    }
    /* 1017e296 packs 13 words per channel; ROM10615d28 lists the
     * sixteen 0x40-spaced banks. The +2c write selects the uploaded
     * channel. Pixel transfer execution is a separate contract. */
    if (s->graphics_profile && s->capture_isp_boundary_test && write && size == 4 && !(addr & 3)) {
        if (addr >= 0x20100400 && addr < 0x20100800 && (addr & 0x3f) <= 0x30) {
            s->image_transfer_config[(addr - 0x20100400) / 0x40][(addr & 0x3f) / 4] = *value;
            z7_mmio_trace(addr, size, true, *value);
            return true;
        }
        if (addr == 0x2010002c && *value < 16) {
            s->image_transfer_selected = *value;
            z7_mmio_trace(addr, size, true, *value);
            return true;
        }
    }
    /* 1017da58/db00/db2a: paired sequence controls at 20102600/40.
     * Retain only the explicit native config/command offsets. Execution
     * and completion of these sequences are not yet modeled. */
    if (s->graphics_profile && s->capture_isp_boundary_test && write && size == 4 &&
        !(addr & 3) && addr >= 0x20102600 && addr <= 0x20102690) {
        unsigned off = addr - 0x20102600, pair_off = off & 0x3f;
        if ((off < 0x80 && (pair_off == 0 || pair_off == 4 || pair_off == 8 ||
             pair_off == 0xc || pair_off == 0x10 || pair_off == 0x18 || pair_off == 0x20)) ||
            off == 0x80 || off == 0x84 || off == 0x90) {
            s->image_sequence_config[off / 4] = *value;
            if (off == 8 || off == 0x48) { z7_image_process_schedule(s); }
            z7_mmio_trace(addr, size, true, *value);
            return true;
        }
    }
    /* 1017d92e..d9fe: two command-list configuration banks. */
    if (s->graphics_profile && s->capture_isp_boundary_test && size == 4 &&
        !(addr & 3) && addr >= 0x20102000 && addr <= 0x201021a4) {
        unsigned bank = (addr - 0x20102000) / 0x100, off = addr & 0xff;
        if (bank < 2 && (off == 0 || off == 4 || off == 8 || off == 0x10 ||
            off == 0x14 || off == 0x18 || off == 0xa4)) {
            if (write) {
                s->image_command_config[bank][off / 4] = *value;
                if (!off) { z7_image_process_schedule(s); }
            }
            else { *value = s->image_command_config[bank][off / 4]; }
            z7_mmio_trace(addr, size, write, *value);
            return true;
        }
    }
    /* Native table10616568: ten 3-word routing descriptors and ten
     * 2x10-bit geometry words. Setters1017dc76/dcb2, gettersdc92/dce8.
     * These retain configuration only, not processed output. */
    if (s->graphics_profile && s->capture_isp_boundary_test && size == 4 && !(addr & 3) &&
        ((addr >= 0x20102880 && addr <= 0x201028f4) ||
         (addr >= 0x20102900 && addr <= 0x20102924))) {
        unsigned off = addr - 0x20102880;
        if (write) { s->image_route_config[off / 4] = *value; }
        else { *value = s->image_route_config[off / 4]; }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    /* 1017da4e / resource allocator 1010f56a: per-channel busy words.
     * No 20102000 command engine has started in this model. Report the
     * reset-idle state only; unsupported submissions still fail closed. */
    if (s->graphics_profile && s->capture_isp_boundary_test && !write && size == 4 &&
        ((addr >= 0x20102040 && addr <= 0x2010207c && !(addr & 3)) ||
         addr == 0x20100148)) {
        *value = 0;
        return true;
    }
    /* Original 1010861a..10108854: explicit 20201000 worker registers.
     * Retain submitted descriptors before implementing the operation. Start
     * deliberately does not imply completion or generate a success IRQ. */
    if (s->graphics_profile && s->capture_isp_boundary_test && size == 4 && !(addr & 3) &&
        addr >= 0x20201000 && addr <= 0x202011a0) {
        unsigned off = addr - 0x20201000;
        bool config = off == 0 || off == 4 || off == 0x14 || off == 0x20 ||
            off == 0x44 || off == 0x48 || off == 0x4c || off == 0x50 || off == 0x54 ||
            off == 0x5c || off == 0x60 || (off >= 0xa0 && off <= 0xcc) ||
            (off >= 0x100 && off <= 0x150) || (off >= 0x160 && off <= 0x194);
        if (write && config) {
            s->image_worker_config[off / 4] = *value;
            fprintf(stderr, "Z7_IMAGE_WORKER_CONFIG offset=0x%x value=0x%08" PRIx64 " completed=0\n", off, *value);
            z7_mmio_trace(addr, size, true, *value);
            return true;
        }
    }
    /* Boundary-route diagnostic only: no statistics engine has completed.
     * 1010b066 exposes four signed-24 / unsigned-29 / unsigned-17 triplets.
     * Empty history has zero samples; these are NOT measurements of RAW.
     * Keep this reset hypothesis out of the ordinary machine profile. */
    if (s->graphics_profile && s->capture_isp_boundary_test && !write && size == 4 && !(addr & 3) &&
        ((addr >= 0x2006a100 && addr <= 0x2006a12c) ||
         (addr >= 0x2006bf00 && addr <= 0x2006bf2c) ||
         /* 1010c01c, table 1061688c: four measurement result words. */
         (addr >= 0x2006a3e0 && addr <= 0x2006a3ec))) {
        *value = 0;
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    /* 1010b2c6..1010b556 explicit configuration packers. 1010b4d2
     * reads back the configured +3c0 mode before issuing a command. */
    if (s->graphics_profile && size == 4 && !(addr & 3) &&
        addr >= 0x2006b310 && addr <= 0x2006b3fc &&
        (write || addr == 0x2006b3c0)) {
        unsigned off = addr - 0x2006b310;
        if (write) { s->capture_filter2_config[off / 4] = *value; }
        else { *value = s->capture_filter2_config[off / 4]; }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    /* 10109f4e reads buffer selectors/current addresses. Reset/idle-zero
     * hypothesis only: this engine has not accepted or completed a frame. */
    if (s->graphics_profile && !write && size == 4 && !(addr & 3) &&
        ((addr >= 0x2006bb70 && addr <= 0x2006bb7c) ||
         addr == 0x2006bc78 || addr == 0x2006bc7c ||
         addr == 0x2006bd78 || addr == 0x2006bd7c)) {
        *value = 0;
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    /* 10109fdA input geometry; table 10616804 has 24 byte fields;
     * 10109efa uploads 65 uint16 curve entries; 10109e38 and 10109f*
     * configure processing and address sets. Status at +188/+370 is absent. */
    if (s->graphics_profile && write && size == 4 && !(addr & 3) &&
        addr >= 0x2006b800 && addr <= 0x2006bd80) {
        unsigned off = addr - 0x2006b800;
        bool config = off <= 0x184 || (off >= 0x200 && off <= 0x28c) ||
            (off >= 0x28c && off <= 0x2b8) || off == 0x318 || off == 0x324 ||
            off == 0x33c || off == 0x344 || off == 0x348 || off == 0x34c ||
            off == 0x350 || off == 0x360 || off == 0x380 ||
            off == 0x450 || off == 0x460 || off == 0x480 ||
            off == 0x550 || off == 0x560 || off == 0x580;
        if (config) {
            s->capture_filter_config[off / 4] = *value;
            z7_mmio_trace(addr, size, write, *value);
            return true;
        }
    }
    /* 1010b012 packs eight uint16 and two uint8 configuration fields;
     * result reads at +100 remain unmodeled until measurements exist. */
    if (s->graphics_profile && write && size == 4 && !(addr & 3) &&
        ((addr >= 0x2006a000 && addr <= 0x2006a024) ||
         (addr >= 0x2006be00 && addr <= 0x2006be24))) {
        unsigned bank = addr >= 0x2006be00, off = addr - (bank ? 0x2006be00 : 0x2006a000);
        if (bank < 2 && off <= 0x24 && *value <= (off < 0x20 ? 0xffff : 0xff)) {
            s->capture_measure_config[bank][off / 4] = *value;
            z7_mmio_trace(addr, size, write, *value);
            return true;
        }
    }
    /* 1010c244 uploads exactly 129 uint16 coefficients as word writes. */
    if (s->graphics_profile && write && size == 4 && !(addr & 3) &&
        addr >= 0x2006b000 && addr <= 0x2006b200 && *value <= 0xffff) {
        s->capture_curve[(addr - 0x2006b000) / 4] = *value;
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    if (!s->graphics_profile || size != 4 || (addr & 3) ||
        addr < 0x20068000 || addr >= 0x2006a000) { return false; }
    unsigned off = addr - 0x20068000;
    if (off >= 0x200 && off <= 0x2f0 && ((off & 0x1f) == 0 || (off & 0x1f) == 0x10)) {
        unsigned bank = (off - 0x200) / 0x20;
        bool status = off & 0x10;
        uint32_t *reg = status ? &s->capture_isp_irq_status[bank] : &s->capture_isp_irq_enable[bank];
        if (write) {
            if (status) { *reg &= ~(uint32_t)*value; }
            else { *reg = *value; }
            z7_isp_irq(s);
        } else { *value = *reg; }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    /* Original 1010b628 pulses +0/+4 around engine initialization;
     * 1010bc16 submits +8 and 1010b634 enables/disables +10/+14.
     * Retain set/clear state instead of just the last command mask. */
    if (off <= 0x14) {
        unsigned bank = off / 8;
        if (write) {
            if (bank && (*value & ~0xfffULL)) { return false; }
            if (off & 4) { s->capture_isp_gate[bank] &= ~(uint32_t)*value; }
            else { s->capture_isp_gate[bank] |= *value; }
            if (off == 0 || off == 8) {
                s->capture_isp_stop_pending &= ~(uint32_t)*value;
                if (!s->capture_isp_stop_pending) { timer_del(s->capture_isp_stop_timer); }
            }
            if (off == 0xc && (*value & 0xeff)) {
                s->capture_isp_stop_pending |= *value & 0xeff;
                timer_mod(s->capture_isp_stop_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
            }
        } else { *value = s->capture_isp_gate[bank]; }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    /* Four processing address channels, original 1010a2e4..1010a412.
     * Odd channels provide only address sets and line configuration. */
    if (off >= 0x1800 && off < 0x1c00) {
        unsigned ch = (off - 0x1800) / 0x100, reg = off & 0xff;
        bool control = (!(ch & 1) && (reg == 0x14 || reg == 0x18 || reg == 0x3c ||
                        reg == 0x40 || reg == 0x44 || reg == 0x48 || reg == 0x4c)) ||
                       (ch == 2 && reg == 0x24) || reg == 8 || ((ch == 2 || ch == 3) && reg == 0x80);
        bool address = reg >= 0x50 && reg <= 0x6c;
        if ((control || address) && (write || (ch == 2 && reg == 0x3c))) {
            if (write) {
                s->capture_processing[off / 4] = *value;
                if (address) {
                    fprintf(stderr, "Z7_PROCESSING_ADDRESS channel=%u offset=0x%x value=0x%08" PRIx64 " dma_completed=0\n",
                            ch, reg, *value);
                }
            } else { *value = s->capture_processing[off / 4]; }
            z7_mmio_trace(addr, size, write, *value);
            return true;
        }
    }
    /* 1010a884/1010a898 read-modify-write control bits 0 and 4. */
    if (off == 0x1f00 || off == 0x152c) {
        uint64_t mask = off == 0x1f00 ? 0x11 : 3;
        if (write) {
            if (*value & ~mask) { return false; }
            s->capture_processing[off / 4] = *value;
        } else { *value = s->capture_processing[off / 4]; }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    if (!write) { return false; }
    /* 1010a916..ae2e pack these configuration structures. The six groups
     * 10616864 fill +71c..7c0; +900 readback is intentionally not modeled.
     * 1010ae50/ae9e/aec8/af2c write LUTs with exact loop bounds below. */
    bool config = off == 0x500 || (off >= 0x508 && off <= 0x81c) ||
        (off >= 0xa00 && off <= 0xafc) || (off >= 0xc00 && off <= 0xe3c) ||
        (off >= 0xf00 && off <= 0x100c) || (off >= 0x1100 && off <= 0x117c) ||
        /* 1010a5c6 geometry/control packing, 1010a508 paired coefficient
         * sets, and 1010a694 processing fields. These writes only retain
         * configuration; they do not execute an ISP or transfer pixels. */
        (off >= 0x1d00 && off <= 0x1efc) ||
        /* 1010a19e/a1e4 geometry, a274 thresholds and a286 LUT. */
        (off >= 0x1500 && off <= 0x1530) || (off >= 0x1600 && off <= 0x17fc);
    if (!config || (off == 0x500 && *value > 0x3fff)) { return false; }
    s->capture_processing[off / 4] = *value;
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

/* 10616994 supplies eleven register groups for 100ce030..154. These are
 * sequence configuration words; the commit command is deliberately separate. */
/* Native 1017c81c requests stop mask101 through100cdf58, then waits
 * for IRQ87 bit18 (101788d4 -> event10). The alternate1017c876 stop
 * also waits for bit10 (101788e0 -> event20). These identify the
 * channel8 and channel0 stop routes. Other channel causes are unknown.
 * Only the opt-in TG experiment executes these sequence commands.
 * Delay is a functional timing assumption, not a measured sensor time. */
static void z7_sequence_stopped(void *opaque)
{
    Z7LabState *s = opaque;
    uint32_t stopped = s->sequence_stop_pending;
    s->sequence_stop_pending = 0;
    s->sequence_running &= ~stopped;
    uint32_t cause = ((stopped & 0x100) ? 0x40000 : 0) |
                     ((stopped & 1) ? 0x400 : 0);
    s->capture_irq_status[1] |= cause;
    z7_capture_irq(s, 1);
    fprintf(stderr, "Z7_SEQUENCE_STOP channels=0x%x irq87_cause=0x%x new_frame=0 timing_verified=0\n",
            stopped, cause);
}

static bool z7_sequence_access(Z7LabState *s, hwaddr addr, unsigned size,
                                bool write, uint64_t *value)
{
    if (s->recovery_profile && size == 4 &&
        (addr == 0x200632d0 || addr == 0x200632d4 ||
         addr == 0x200632e0 || addr == 0x200632e4)) {
        bool stop = addr >= 0x200632e0;
        bool strobe = addr & 4;
        uint32_t *mask = stop ? &s->sequence_stop_mask : &s->sequence_start_mask;
        if (!write) {
            if (strobe) { return false; }
            *value = *mask;
        } else if (!strobe) {
            if (*value & ~UINT64_C(0x7ff)) { return false; }
            *mask = *value;
        } else {
            if (*value != 1) { return false; }
            if (s->capture_periodic_test) {
                if (stop) {
                    s->sequence_stop_pending |= *mask & s->sequence_running;
                    if (s->sequence_stop_pending) {
                        timer_mod(s->sequence_stop_timer,
                                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
                    }
                } else {
                    s->sequence_running |= *mask;
                    s->sequence_stop_pending &= ~*mask;
                }
            }
            /* Commands consume their mask; a later RMW is a new request. */
            *mask = 0;
        }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    /* 106168c0/10616920 pointer groups used by 1010c6c4..7ce. Six
     * channels with timing fields; no timing edges inferred. */
    if (s->recovery_profile && write && size == 4 &&
        addr >= 0x200600c8 && addr <= 0x200602cc) {
        unsigned relative = addr - 0x200600c8, ch = relative / 0x60;
        unsigned off = relative % 0x60;
        if (ch < 6 && !(off & 3) && (off <= 20 || off == 32 || off == 36)) {
            if (off == 32 && *value > 0xff) { return false; }
            s->capture_timing[ch][off / 4] = *value;
            z7_mmio_trace(addr, size, write, *value);
            return true;
        }
    }
    static const unsigned base[] = {0, 0x40, 0x80, 0xd0, 0x110, 0x150,
                                    0x190, 0x1d0, 0x210, 0x250, 0x290};
    if (!s->recovery_profile || size != 4 || !write || (addr & 3) || addr < 0x20063000) { return false; }
    unsigned off = addr - 0x20063000;
    for (unsigned ch = 0; ch < ARRAY_SIZE(base); ch++) {
        unsigned reg = off - base[ch], end = ch == 2 ? 0x38 : 0x24;
        if (reg > end) { continue; }
        if (reg == end - 4) {
            if (*value != 1) { return false; }
            /* 100ce0a6 writes config then this one-shot update strobe.
             * Preserve a submitted config snapshot; no sequence completion
             * or external output/IRQ is inferred from submission alone. */
            memcpy(s->sequence_active[ch], s->sequence_regs[ch], sizeof(s->sequence_regs[ch]));
            fprintf(stderr, "Z7_SEQUENCE_SUBMIT channel=%u config=0x%x\n",
                    ch, s->sequence_regs[ch][(end - 8) / 4]);
        }
        s->sequence_regs[ch][reg / 4] = *value;
        z7_mmio_trace(addr, size, true, *value);
        return true;
    }
    return false;
}

static bool z7_peripheral_dma_access(Z7LabState *s, hwaddr addr, unsigned size,
                                     bool write, uint64_t *value)
{
    if (!s->recovery_profile || size != 4 || (addr & 3)) { return false; }
    unsigned bank;
    if (addr >= 0x2c020010 && addr <= 0x2c02008c) { bank = 0; }
    else if (addr >= 0x2c030010 && addr <= 0x2c03008c) { bank = 1; }
    else { return false; }
    unsigned ch = ((addr & 0xfff) - 0x10) / 0x10, i = (addr & 0xf) / 4;
    /* Original a180574c table and 10068d36 config bit fields. Bit31 arms a
     * channel; transfers wait for serial requests and are not completed here. */
    uint32_t mask = i == 0 ? 0xbfffffff : i == 1 ? 0x3fff0000 : UINT32_MAX;
    if (write) {
        if (*value & ~(uint64_t)mask) { return false; }
        s->peripheral_dma_regs[bank][ch][i] = *value;
    } else { *value = s->peripheral_dma_regs[bank][ch][i]; }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

static void z7_i2c_irq(Z7LabState *s, unsigned ch)
{
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 214 + ch - 32),
                (s->i2c_regs[ch][4] & ~s->i2c_regs[ch][8] & 0x2ff) != 0);
}

static void z7_i2c_complete(void *opaque)
{
    Z7LabState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), next = INT64_MAX;
    for (unsigned ch = 0; ch < 4; ch++) {
        if (!s->i2c_pending[ch]) { continue; }
        if (s->i2c_deadline[ch] > now) { next = MIN(next, s->i2c_deadline[ch]); continue; }
        s->i2c_pending[ch] = false;
        s->i2c_regs[ch][1] &= ~0x100U;
        /* Empty bus: no slave acknowledges an address, so report NACK.
         * No receive bytes or successful peripheral reply are fabricated. */
        s->i2c_regs[ch][4] |= 4;
        z7_i2c_irq(s, ch);
        fprintf(stderr, "Z7_I2C_NACK ch=%u address=0x%x tx_count=%u irq=%u\n",
                ch, s->i2c_regs[ch][2], s->i2c_tx_count[ch], 214 + ch);
        /* xiicps_master.c documents NACK on each slave-monitor retry.
         * Original error ISR clears the FIFO and disables the controller IRQ. */
        if ((s->i2c_regs[ch][0] & 0x22) == 0x22) {
            s->i2c_pending[ch] = true;
            s->i2c_deadline[ch] = now + 100000;
            next = MIN(next, s->i2c_deadline[ch]);
        }
    }
    if (next != INT64_MAX) { timer_mod(s->i2c_timer, next); }
}

/* Register-compatible I2C hypothesis from original 10109afc..d4c and the
 * primary xiicps_hw.h definitions (phase7/evidence). Nikon channel 3 additionally
 * uses control bit7. Timing 100 us is synthetic; all four buses are empty. */
static bool z7_i2c_access(Z7LabState *s, hwaddr addr, unsigned size,
                          bool write, uint64_t *value)
{
    if (!s->recovery_profile || size != 4 || (addr & 3) ||
        addr < 0x2c018000 || addr >= 0x2c01c000) { return false; }
    unsigned ch = (addr - 0x2c018000) / 0x1000, off = addr & 0xfff;
    if (off > 0x28) { return false; }
    uint32_t *r = s->i2c_regs[ch];
    unsigned i = off / 4;
    uint32_t mask = i == 0 ? 0xffff : i == 2 ? 0x3ff :
                    i == 3 || i == 7 ? 0xff : i == 5 ? 0x3f :
                    i == 6 ? 0xf : 0x2ff;
    if (write) {
        if (i == 1 || i == 8 || (*value & ~(uint64_t)mask)) { return false; }
        if (i == 0) {
            if (*value & 0x40) {
                s->i2c_tx_count[ch] = 0; r[5] = 0; r[1] &= ~0x140U;
                s->i2c_pending[ch] = false;
            }
            r[0] = *value & ~0x40U;
        } else if (i == 2) {
            /* Slave mode merely programs own address. */
            r[2] = *value;
            if (r[0] & 2) {
                r[1] |= 0x100;
                s->i2c_pending[ch] = true;
                s->i2c_deadline[ch] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000;
                int64_t next = s->i2c_deadline[ch];
                for (unsigned n = 0; n < 4; n++) {
                    if (s->i2c_pending[n]) { next = MIN(next, s->i2c_deadline[n]); }
                }
                timer_mod(s->i2c_timer, next);
            }
        } else if (i == 3) {
            if (s->i2c_tx_count[ch] == 16) { r[4] |= 0x40; }
            else { s->i2c_tx[ch][s->i2c_tx_count[ch]++] = *value; r[1] |= 0x40; }
        } else if (i == 4) { r[4] &= ~*value; }
        else if (i == 9) { r[8] &= ~*value; }
        else if (i == 10) { r[8] |= *value; }
        else { r[i] = *value; }
        z7_i2c_irq(s, ch);
    } else {
        if (i == 3) { return false; } /* No RX data exists on an empty bus. */
        *value = i == 5 && !(r[0] & 1) ? s->i2c_tx_count[ch] : i >= 9 ? 0 : r[i];
    }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

static void z7_copy_irq(Z7LabState *s, unsigned ch)
{
    /* Original ISR 10068ec4 reads status low nibble then clears those bits
     * by writing zero. 101c30ca expects completion code 8. */
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 45 + ch - 32),
                 (s->copy_regs[ch][8] & 0xf) && (s->copy_regs[ch][6] & 2));
}

static void z7_copy_complete(void *opaque)
{
    Z7LabState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), next = INT64_MAX;
    for (unsigned ch = 0; ch < 4; ch++) {
        if (!s->copy_pending[ch]) { continue; }
        if (s->copy_deadline[ch] > now) { next = MIN(next, s->copy_deadline[ch]); continue; }
        uint32_t *r = s->copy_regs[ch];
        uint32_t len = r[0] + 1, src = r[1], dst = r[2];
        uint8_t *buf = g_malloc(len);
        /* Missing first 128 KiB is explicitly erased input, just like the
         * absent persistent flash; present payload bytes are copied exactly. */
        unsigned prefix = src >= 0x10000000 && src < 0x10020000 ? MIN(len, 0x10020000 - src) : 0;
        memset(buf, 0xff, prefix);
        if (prefix < len) { cpu_physical_memory_read(src + prefix, buf + prefix, len - prefix); }
        cpu_physical_memory_write(dst, buf, len);
        g_free(buf);
        s->copy_pending[ch] = false;
        r[0] = 0;
        r[8] = (r[8] & ~0xfU) | 8;
        z7_copy_irq(s, ch);
        fprintf(stderr, "Z7_COPY_COMPLETE ch=%u src=0x%08x dst=0x%08x len=0x%x erased_prefix=0x%x irq=%u\n",
                ch, src, dst, len, prefix, 45 + ch);
    }
    if (next != INT64_MAX) { timer_mod(s->copy_timer, next); }
}

/* 10068f6c configures four transfer channels via a180668c table. Only the
 * contiguous incrementing ROM-to-shadow-RAM mode observed at boot is known. */
static bool z7_copy_ram_range(uint32_t addr, uint64_t len)
{
    uint64_t end = (uint64_t)addr + len;
    return len && len <= 0x2000000 &&
        ((addr >= 0xa0000000 && end <= 0xae9e0000) ||
         (addr >= 0x40000000 && end <= 0x40400000) ||
         (addr >= 0x44c00000 && end <= 0x4fbd2400) ||
         (addr >= 0x4fc00000 && end <= 0x5382da00) ||
         (addr >= 0x9fc00000 && end <= 0x9ff5a600) ||
         (addr >= 0x53900000 && end <= 0x9e5b3000) ||
         (addr >= 0xdc55a800 && end <= 0xdcc6a800) ||
         (addr >= 0xaea00000 && end <= 0xb5ecb400) ||
         (addr >= 0xb5f00000 && end <= 0xd2cd6000) ||
         (addr >= 0xd2d00000 && end <= 0xdb53b000) ||
         (addr >= 0x04008000 && end <= 0x04009000));
}

static bool z7_copy_access(Z7LabState *s, hwaddr addr, unsigned size,
                           bool write, uint64_t *value)
{
    if (!s->recovery_profile || size != 4 || (addr & 3) ||
        addr < 0x04251010 || addr > 0x042510c0) { return false; }
    unsigned ch = (addr - 0x04251010) / 0x30, off = (addr - 0x04251010) % 0x30;
    if (off > 0x20 || off == 0x1c) { return false; }
    unsigned i = off / 4;
    uint32_t mask = i <= 2 ? UINT32_MAX :
                    i <= 4 ? 0x00030f05 : i == 5 ? 0xfffffff1 :
                    i == 6 ? 0x11f09113 : 0xf;
    if (write) {
        if (*value & ~(uint64_t)mask) { return false; }
        if (s->copy_pending[ch] && i != 8) { return false; }
        if (i == 6 && (*value & 3) == 3 && !(s->copy_regs[ch][6] & 1)) {
            uint32_t *r = s->copy_regs[ch];
            uint64_t len = (uint64_t)r[0] + 1;
            bool source_ok = (r[1] >= 0x10000000 && (uint64_t)r[1] + len <= 0x12020000) ||
                             z7_copy_ram_range(r[1], len);
            if (r[3] != 0x30f00 || r[4] != 0x30f00 || r[5] != 0 ||
                !source_ok || !z7_copy_ram_range(r[2], len)) {
                fprintf(stderr, "Z7_COPY_REJECT ch=%u src=%08x dst=%08x len=%" PRIx64 " mode=%x/%x stride=%x command=%" PRIx64 "\n",
                        ch, r[1], r[2], len, r[3], r[4], r[5], *value);
                return false;
            }
            s->copy_pending[ch] = true;
            s->copy_deadline[ch] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000 + len * 10;
            int64_t next = s->copy_deadline[ch];
            for (unsigned n = 0; n < 4; n++) {
                if (s->copy_pending[n]) { next = MIN(next, s->copy_deadline[n]); }
            }
            timer_mod(s->copy_timer, next);
            fprintf(stderr, "Z7_COPY_START ch=%u src=0x%08x dst=0x%08x len=0x%" PRIx64 "\n",
                    ch, r[1], r[2], len);
        }
        if (i == 8) { s->copy_regs[ch][i] &= *value; }
        else {
        s->copy_regs[ch][i] = *value;
        }
        z7_copy_irq(s, ch);
    } else { *value = s->copy_regs[ch][i]; }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

static void z7_engine_irq(Z7LabState *s)
{
    uint32_t pending = (s->engine_regs[6] & s->engine_regs[3]) |
                       (s->engine_regs[7] & s->engine_regs[4]) |
                       (s->engine_regs[8] & s->engine_regs[5]);
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 102 - 32), pending != 0);
}

static void z7_engine_stopped(void *opaque)
{
    Z7LabState *s = opaque;
    /* 1017dd2a stop -> ISR 1017d6b0 status +18 bit0 -> callback
     * 1010efb8 -> event bit2 releases the original 1010ebf0 waiter. */
    s->engine_regs[6] |= 1;
    z7_engine_irq(s);
    fprintf(stderr, "Z7_ENGINE_STOP_COMPLETE irq102 pending0=0x%x\n", s->engine_regs[6]);
}

static bool z7_engine_access(Z7LabState *s, hwaddr addr, unsigned size,
                             bool write, uint64_t *value)
{
    if (!s->recovery_profile || size != 4 || addr < 0x20100000 ||
        addr > 0x20100020 || (addr & 3)) { return false; }
    unsigned i = (addr - 0x20100000) / 4;
    if (i == 2) { return false; }
    if (write) {
        if ((i == 0 && *value != 0) || (i == 1 && (*value & ~UINT64_C(3)))) { return false; }
        if (i >= 6) { s->engine_regs[i] &= ~*value; }
        else {
            if (i == 1 && (s->engine_regs[1] & 1) && !(*value & 1)) {
                timer_mod(s->engine_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
            }
            s->engine_regs[i] = *value;
        }
        z7_engine_irq(s);
    } else { *value = s->engine_regs[i]; }
    z7_mmio_trace(addr, size, write, *value);
    return true;
}

#include "nikon-z7-isp.h"

/* Display experiment: original 104feae2 uploads two command streams, then
 * 104fec90 waits for IRQ 129 -> 104fee62 -> event bit 1. The source buffers
 * use word counts (1047bd42); hardware length is in 64-bit units.
 * Captured streams also drive the experimental software GUI rasterizer. */
static void z7_graphics_capture_memory(const char *prefix, unsigned reg,
                                       uint32_t addr, uint64_t length)
{
    uint64_t limit;
    if (addr >= 0xa1000000 && addr < 0xae9e0000) { limit = 0xae9e0000; }
    else if (addr >= 0xb045f200 && addr < 0xb3ccb400) { limit = 0xb3ccb400; }
    else if (addr >= 0xd2d00000 && addr < 0xdb53b000) { limit = 0xdb53b000; }
    else if (addr >= 0x10020000 && addr < 0x12020000) { limit = 0x12020000; }
    else { return; }
    length = MIN(MIN(length, limit - addr), 0x800000);
    if (!length) { return; }
    g_autofree uint8_t *data = g_malloc(length);
    cpu_physical_memory_read(addr, data, length);
    g_autofree char *path = g_strdup_printf("%s-r%03x-%08x.bin", prefix, reg, addr);
    g_file_set_contents(path, (char *)data, length, NULL);
}

/* Capture referenced path/texture bytes at each draw, before guest allocators
 * can recycle them. This observer decodes headers only; it does not rasterize. */
static void z7_graphics_capture_draws(Z7LabState *s, unsigned ch, const char *dir)
{
    uint32_t *regs = s->graphics_cmd_regs[ch];
    unsigned at = 0, draw = 0, length = s->graphics_table_lengths[ch];
    uint8_t *data = s->graphics_tables[ch];
    while (at + 4 <= length) {
        uint32_t header = ldl_le_p(data + at);
        at += 4;
        if (!header) { continue; }
        unsigned op = header & 7, count = ((header >> 3) & 255) + 1;
        if (at + count * 4 > length || (op != 2 && op != 4 && op != 6)) { return; }
        uint32_t mask = 0;
        for (unsigned b = 0; b < 4; b++) {
            if (header & (1U << (12 + b))) { mask |= 255U << (8 * b); }
        }
        for (unsigned i = 0; i < count && op != 2; i++) {
            unsigned reg = (header >> 16) + (op == 6 ? i : 0);
            if (reg >= 512) { return; }
            regs[reg] = (regs[reg] & ~mask) | (ldl_le_p(data + at + i * 4) & mask);
            if ((ch == 0 && reg == 6 && regs[reg] == 1) ||
                (ch == 1 && reg == 0x1a6 && (regs[reg] & 255) == 1)) {
                g_autofree char *prefix = g_strdup_printf("%s/%06u-d%03u", dir,
                                                          s->graphics_upload_sequence, draw++);
                g_autofree char *path = g_strdup_printf("%s-regs.bin", prefix);
                uint8_t raw[512 * 4];
                for (unsigned r = 0; r < 512; r++) { stl_le_p(raw + r * 4, regs[r]); }
                g_file_set_contents(path, (char *)raw, sizeof(raw), NULL);
                if (!ch) {
                    if (regs[3] & 0x10000000) {
                        z7_graphics_capture_memory(prefix, 2, regs[2], MAX(4096U, regs[3] & 0xffff));
                        z7_graphics_capture_memory(prefix, 4, regs[4], MAX(4096U, (regs[5] & 0xffff) * 4));
                    }
                } else {
                    uint32_t dim = regs[0x152];
                    uint64_t bytes = (uint64_t)(dim & 0xffff) * (dim >> 16) * 4;
                    z7_graphics_capture_memory(prefix, 0x155, regs[0x155], MAX(bytes, 4096));
                }
            }
        }
        at += count * 4;
    }
}

static void z7_graphics_complete(void *opaque)
{
    Z7LabState *s = opaque;
    bool completed = false;
    for (unsigned ch = 0; ch < 2; ch++) {
        if (!s->graphics_upload_pending[ch]) { continue; }
        unsigned off = 0x10d0 + ch * 0x10;
        uint32_t src = s->graphics_regs[(off + 4) / 4];
        unsigned size = s->graphics_regs[(off + 12) / 4] * 8;
        cpu_physical_memory_read(src, s->graphics_tables[ch], size);
        s->graphics_table_lengths[ch] = size;
        const char *dump_dir = getenv("Z7_GRAPHICS_DUMP_DIR");
        if (dump_dir && s->graphics_upload_sequence < 4096) {
            g_autofree char *path = g_strdup_printf("%s/%06u-ch%u.bin", dump_dir,
                                                  s->graphics_upload_sequence, ch);
            GError *error = NULL;
            if (!g_file_set_contents(path, (const char *)s->graphics_tables[ch], size, &error)) {
                error_report("Graphics capture failed: %s", error->message);
                g_error_free(error);
            }
            z7_graphics_capture_draws(s, ch, dump_dir);
        }
        z7_gpu_execute(s->gpu, ch, s->graphics_tables[ch], size);
        s->graphics_upload_sequence++;
        s->graphics_upload_pending[ch] = false;
        s->graphics_regs[off / 4] = 0;
        fprintf(stderr, "Z7_GRAPHICS_UPLOAD ch=%u src=0x%08x bytes=%u\n", ch, src, size);
        completed = true;
    }
    if (completed) {
        s->graphics_regs[0x1020 / 4] = 1;
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 129 - 32), 1);
    }
}

/* Native10241012 routes the interface mode (not instance number) to IRQ111..113.
 * 1024350e enables causes; 10240fb0 reads and acknowledges +1214 as W1C. */
static void z7_display_irq_update(Z7LabState *s)
{
    bool level[3] = { false, false, false };
    for (unsigned inst = 0; inst < 3; inst++) {
        uint32_t *r = s->display_regs + inst*0x4000/4;
        unsigned mode = r[0x1100/4] & 3;
        if (mode && (r[0x1210/4] & r[0x1214/4])) { level[mode-1] = true; }
    }
    for (unsigned i = 0; i < 3; i++) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 111+i-32), level[i]);
    }
}

/* Read only the native LCD source/overlay descriptors. No photo, UI element,
 * or exposure value is inserted here. The conversion matrix and presentation
 * cadence are functional assumptions; programmable display LUTs are not yet
 * executed. The GUI-surface probe remains available when this is disabled. */
static void z7_lcd_present(void *opaque)
{
    Z7LabState *s = opaque;
    uint32_t *r = s->display_regs + 0x4000/4;
    if (!s->lcd_scanout_test) { return; }
    timer_mod(s->lcd_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000000);
    unsigned w = r[0x204/4] & 0xffff, h = r[0x204/4] >> 16;
    unsigned bank = (r[0x260/4] >> 8) & 3;
    uint32_t ya = r[0x210/4 + bank], ua = r[0x220/4 + bank];
    unsigned ys = r[0x240/4], us = r[0x250/4];
    /* Native10241706 selects format4 for processed live view. Its preview
     * slots cannot hold full-height chroma at the submitted UV offset;
     * model this two-plane420 layout as NV12 (packing still experimental). */
    bool nv12 = r[0x200/4] == 0x300004;
    unsigned uvh = nv12 ? h/2 : h;
    if (!(r[0x1200/4] & 1) || !(r[0x110/4] & 1) || (r[0x200/4] != 0x300000 && !nv12) ||
        w != 1024 || h != 682 || ys != w || us != w ||
        !z7_gpu_range(ya, (uint64_t)ys*h, false) ||
        !z7_gpu_range(ua, (uint64_t)us*uvh, false)) { return; }
    g_autofree uint8_t *yp = g_malloc((size_t)ys*h), *up = g_malloc((size_t)us*uvh);
    cpu_physical_memory_read(ya, yp, (size_t)ys*h);
    cpu_physical_memory_read(ua, up, (size_t)us*uvh);
    bool overlay = (r[0x2110/4] & 1) && r[0x2200/4] == 0x30006 &&
        r[0x2204/4] == r[0x204/4] && r[0x2400/4] == r[0x204/4] &&
        r[0x2500/4] == w*2 && r[0x2540/4] == w &&
        r[0x2580/4] == 0 && (r[0x25c0/4] & 1);
    uint32_t ca = r[0x2440/4], aa = r[0x24c0/4];
    g_autofree uint8_t *cp = NULL, *ap = NULL;
    if (overlay && z7_gpu_range(ca, (uint64_t)w*h*2, false) &&
        z7_gpu_range(aa, (uint64_t)w*h, false)) {
        cp = g_malloc((size_t)w*h*2); ap = g_malloc((size_t)w*h);
        cpu_physical_memory_read(ca, cp, (size_t)w*h*2);
        cpu_physical_memory_read(aa, ap, (size_t)w*h);
    } else { overlay = false; }
    if (!s->gpu->lcd_scanout) {
        s->gpu->lcd_scanout = true;
        qemu_console_resize(s->gpu->console, w, h);
    }
    DisplaySurface *surface = qemu_console_surface(s->gpu->console);
    uint8_t *out = surface_data(surface);
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            size_t i = (size_t)y*w+x;
            uint8_t pixel[4], top[4];
            size_t uvindex = (size_t)(nv12 ? y/2 : y)*us+(x & ~1U);
            z7_yuv_rgb(yp[y*ys+x], up[uvindex], up[uvindex+1], pixel);
            if (overlay) {
                z7_gpu_pixel_read(cp, ap, 0x107, false, i, top);
                for (unsigned c = 0; c < 3; c++) {
                    pixel[c] = (top[c]*top[3] + pixel[c]*(255-top[3]) + 127)/255;
                }
            }
            stl_p(out + y*surface_stride(surface) + x*4,
                  (pixel[0]<<16) | (pixel[1]<<8) | pixel[2]);
        }
    }
    dpy_gfx_update(s->gpu->console, 0, 0, w, h);
    /* Functional frame boundary, only after reading validated native buffers.
     * 102442e4 maps status bit16 to event2, consumed by1023cd6e/display worker.
     * Exact physical VSync phase/rate remains unverified. */
    r[0x1214/4] |= 1U << 16;
    z7_display_irq_update(s);
    if (!(s->lcd_frames++ % 30)) {
        fprintf(stderr, "Z7_LCD_SCANOUT frame=%u y=0x%08x uv=0x%08x overlay=%u color=0x%08x alpha=0x%08x size=%ux%u input=%s overlay_format=YYUV422A bit_accurate=0\n",
                s->lcd_frames, ya, ua, overlay, ca, aa, w, h, nv12 ? "NV12" : "NV16");
    }
}

static bool z7_display_access(Z7LabState *s, hwaddr addr, unsigned size,
                              bool write, uint64_t *value)
{
    if (s->graphics_profile && addr >= 0x24040000 && addr < 0x24060000 &&
        size == 4 && !(addr & 3)) {
        unsigned inst = (addr - 0x24040000) / 0x10000;
        unsigned lo = addr & 0xffff;
        uint32_t mask;
        /* 10242c40/10242df2: two banks of 3 x 32 twelve-bit entries. */
        if ((lo < 0xc0) || (lo >= 0x100 && lo < 0x1c0) ||
            (lo >= 0x2000 && lo < 0x20c0) || (lo >= 0x2100 && lo < 0x21c0)) {
            mask = 0x0fff0fff;
        /* 10242f88: two banks of 3 x 33 eight-bit entries. */
        } else if (lo >= 0x5000 && lo < 0x5400 &&
                   (lo & 0x1ff) < 0x144 && (lo & 0x7f) <= 0x40) {
            mask = 0x00ff00ff;
        /* 10243198/326c: two additional pairs of 33-entry curves. */
        } else if (lo >= 0x6000 && lo < 0x6200 && (lo & 0x7f) <= 0x40) {
            mask = (lo & 0x7f) == 0x40 ? 0xffff : 0x00ff00ff;
        } else { return false; }
        if (write && (*value & ~(uint64_t)mask)) {
            z7_unknown_access(s, addr, size, write, *value);
            return true;
        }
        if (write) { s->display_lut[inst][lo / 4] = *value; }
        else { *value = s->display_lut[inst][lo / 4]; }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    if (!s->graphics_profile || addr < 0x20140000 || addr >= 0x2014c000 ||
        size != 4 || (addr & 3)) { return false; }
    unsigned index = (addr - 0x20140000) / 4;
    unsigned off = (addr - 0x20140000) % 0x4000;
    if (off == 0x1210 || off == 0x1214) {
        if (write) {
            if (off == 0x1214) { s->display_regs[index] &= ~(uint32_t)*value; }
            else { s->display_regs[index] = *value; }
            z7_display_irq_update(s);
        } else { *value = s->display_regs[index]; }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    /* 102427ce/2924: ten regions per overlay, two overlay components. */
    unsigned plane_off = off >= 0x3000 ? off - 0x1000 : off;
    uint64_t region_mask = 0;
    if ((plane_off >= 0x2400 && plane_off < 0x2428) ||
        (plane_off >= 0x2580 && plane_off < 0x25a8)) {
        region_mask = 0x3fffffff;
    } else if ((plane_off >= 0x2480 && plane_off < 0x24a4) ||
               (plane_off >= 0x24c0 && plane_off < 0x24e8)) {
        region_mask = UINT32_MAX;
    } else if ((plane_off >= 0x2500 && plane_off < 0x2528) ||
               (plane_off >= 0x2540 && plane_off < 0x2568)) {
        region_mask = 0xffff;
    } else if (plane_off == 0x25c0 || plane_off == 0x25c4 || plane_off == 0x2600) {
        region_mask = 0x3ff;
    } else if (plane_off >= 0x25e0 && plane_off < 0x25f4) {
        region_mask = 0x3f3f3f3f;
    } else if (plane_off == 0x2604) { region_mask = 3; }
    if (region_mask) {
        if (write && (*value & ~region_mask)) { goto rejected; }
        goto retained;
    }
    /* 1024151e and ROM 106159f4: three rows of packed coefficients. */
    bool matrix = (off >= 0x4a0 && off < 0x4b8) ||
                  (off >= 0x4d0 && off < 0x4e8) ||
                  (off >= 0x1490 && off < 0x14a8) ||
                  (off >= 0x14f0 && off < 0x1508) ||
                  (off >= 0x2840 && off < 0x284c) ||
                  (off >= 0x3840 && off < 0x384c);
    if (matrix) {
        if (write && off >= 0x2840 && (*value & ~0xffffffULL)) { goto rejected; }
        goto retained;
    }
    switch (off) {
    case 0x1100: /* 102410d4 mode; formerly a recovery-only latch. */
        if (write && *value > 3) { goto rejected; }
        break;
    case 0x4f0: /* 10243348: lookup-table controls */
        if (write && (*value & ~0xffULL)) { goto rejected; }
        break;
    case 0x1480: /* 1024209c: output component order */
        if (write && (*value & ~0x3333ULL)) { goto rejected; }
        break;
    /* 1024246a: additional output conversion geometry. */
    case 0x14b0: case 0x14b8: case 0x14c0: case 0x14c8:
        break;
    case 0x14b4:
        if (write && (*value & ~0xffffULL)) { goto rejected; }
        break;
    case 0x14bc: case 0x14c4:
        if (write && (*value & ~0x3fffffffULL)) { goto rejected; }
        break;
    /* 10242186: output crop/scaling rectangle. */
    case 0x1600: case 0x1604: case 0x1608:
        if (write && (*value & ~0x3fffffffULL)) { goto rejected; }
        break;
    case 0x160c:
        if (write && (*value & ~0x3f003fULL)) { goto rejected; }
        break;
    case 0x1610:
        if (write && (*value & ~0xffff3fffULL)) { goto rejected; }
        break;
    case 0x1614:
        if (write && (*value & ~0x70007ULL)) { goto rejected; }
        break;
    case 0x1618:
        if (write && (*value & ~0x03ffffffULL)) { goto rejected; }
        break;
    case 0x4c0: case 0x1510:
        if (write && (*value & ~0x606ULL)) { goto rejected; }
        break;
    /* 10242a2c: two overlay components, source format and geometry. */
    case 0x2200: case 0x3200:
        if (write && (*value & ~0x137f0017ULL)) { goto rejected; }
        break;
    case 0x2204: case 0x3204: case 0x2208: case 0x3208:
        if (write && (*value & ~0x3fffffffULL)) { goto rejected; }
        break;
    case 0x2210: case 0x3210:
        if (write && (*value & ~0x03030303ULL)) { goto rejected; }
        break;
    case 0x2218: case 0x3218:
        if (write && *value > 1) { goto rejected; }
        break;
    case 0x2810: case 0x3810: /* 10242b9e: format conversion */
        if (write && (*value & ~0xffULL)) { goto rejected; }
        break;
    case 0x2814: case 0x3814:
        if (write && (*value & ~0x1f001f1fULL)) { goto rejected; }
        break;
    case 0x2818: case 0x3818:
        if (write && (*value & ~0x1f1fULL)) { goto rejected; }
        break;
    /* 10241698 / 10243594: four addresses per source plane. */
    case 0x210: case 0x214: case 0x218: case 0x21c:
    case 0x220: case 0x224: case 0x228: case 0x22c:
    case 0x2440: case 0x2444: case 0x2448: case 0x244c:
    case 0x3440: case 0x3444: case 0x3448: case 0x344c:
        break;
    case 0x260: case 0x2214: case 0x3214: /* 102416cc */
        if (write && (*value & ~0x30300ULL)) { goto rejected; }
        break;
    /* 10241706: source dimensions, pixel format and crop. */
    case 0x200:
        if (write && (*value & ~0x37f0311fULL)) { goto rejected; }
        break;
    case 0x204: case 0x600:
        if (write && (*value & ~0x3fffffffULL)) { goto rejected; }
        break;
    case 0x240: case 0x250:
        if (write && (*value & ~0xffffULL)) { goto rejected; }
        break;
    case 0x604:
        if (write && (*value & ~0x101ULL)) { goto rejected; }
        break;
    case 0x40c:
    case 0x400: /* 10241970 */
        if (write && (*value & ~0xffULL)) { goto rejected; }
        break;
    case 0x404:
        if (write && (*value & ~0x1f001f1fULL)) { goto rejected; }
        break;
    case 0x408:
        if (write && (*value & ~0x1f1fULL)) { goto rejected; }
        break;
    /* 10241890: conversion/clipping parameters. */
    case 0x410: case 0x430: /* 10241a0e: optional correction enable */
        if (write && (*value & ~0x301ULL)) { goto rejected; }
        break;
    case 0x414:
        if (write && (*value & ~0xff01ffULL)) { goto rejected; }
        break;
    case 0x418:
        if (write && (*value & ~0xff00ffULL)) { goto rejected; }
        break;
    case 0x41c: case 0x420:
        break;
    case 0x424:
        if (write && (*value & ~0xffffULL)) { goto rejected; }
        break;
    /* 1024255e: six component selectors, enables and byte remapping. */
    case 0x14d0:
        if (write && (*value & ~0x333333ULL)) { goto rejected; }
        break;
    case 0x14d4:
        if (write && (*value & ~0x7711ULL)) { goto rejected; }
        break;
    case 0x14d8:
        if (write && (*value & ~0x30131ULL)) { goto rejected; }
        break;
    case 0x14e0: case 0x14e4:
    case 0x1420: /* 1024270a: two halfwords */
        break;
    case 0x1424:
        if (write && (*value & ~0xffffULL)) { goto rejected; }
        break;
    case 0x1340: /* 10242752 */
        if (write && (*value & ~0xfffULL)) { goto rejected; }
        break;
    case 0x100: case 0x1200: case 0x2100: case 0x3100:
        if (write && (*value & ~0x13ULL)) { goto rejected; }
        break;
    /* 10241dc4: output conversion and timing configuration, packed from
     * byte/halfword fields. These are retained, not interpreted as pixels. */
    case 0x1400:
        if (write && (*value & ~0x030100ffULL)) { goto rejected; }
        break;
    case 0x1204: case 0x130c: case 0x1314: case 0x1318: case 0x131c:
    case 0x1328: case 0x132c:
        if (write && (*value & ~0xffffULL)) { goto rejected; }
        break;
    case 0x1208:
        if (write && (*value & ~0x301ULL)) { goto rejected; }
        break;
    case 0x1300:
        if (write && (*value & ~0x11111ULL)) { goto rejected; }
        break;
    case 0x1304: case 0x1320: case 0x1324: case 0x1330:
    case 0x120c: /* 10243532/40 */
    case 0x110: case 0x2110: case 0x3110: case 0x1220: /* 10241402/52 */
    case 0x500: case 0x2880: case 0x3880: /* 102414a4/c8 */
        if (write && (*value & ~0xffULL)) { goto rejected; }
        break;
    case 0x1308:
        if (write && (*value & ~0xbfff3fffULL)) { goto rejected; }
        break;
    case 0x1310:
        if (write && (*value & ~0x103ffULL)) { goto rejected; }
        break;
    case 0x1338: case 0x1224:
        if (write && *value > 1) { goto rejected; }
        break;
    default:
        return false;
    }
retained:
    if (write) { s->display_regs[index] = *value; }
    else { *value = s->display_regs[index]; }
    if (write && off == 0x1100) { z7_display_irq_update(s); }
    z7_mmio_trace(addr, size, write, *value);
    return true;
rejected:
    z7_unknown_access(s, addr, size, write, *value);
    return true;
}

static bool z7_graphics_access(Z7LabState *s, hwaddr addr, unsigned size,
                               bool write, uint64_t *value)
{
    if (!s->graphics_profile || size != 4 || (addr & 3) ||
        addr < 0x20180000 || addr >= 0x20181600) { return false; }
    unsigned off = addr - 0x20180000;
    switch (off) {
    case 0x58:
        /* 104fecd0 tests bit 26 after an upload interrupt. */
        if (write) { goto rejected; }
        *value = (s->graphics_upload_pending[0] || s->graphics_upload_pending[1]) ? (1U << 26) : 0;
        z7_mmio_trace(addr, size, false, *value);
        return true;
    case 0x10d4: case 0x10e4:
    case 0x10dc: case 0x10ec:
        if (!write || s->graphics_upload_pending[(off - 0x10d0) / 0x10]) { goto rejected; }
        break;
    case 0x10d0: case 0x10e0: {
        unsigned ch = (off - 0x10d0) / 0x10;
        if (!write || *value != 1 || s->graphics_upload_pending[ch]) { goto rejected; }
        uint32_t src = s->graphics_regs[(off + 4) / 4];
        uint64_t length = (uint64_t)s->graphics_regs[(off + 12) / 4] * 8;
        if (!length || length > sizeof(s->graphics_tables[ch]) ||
            src < 0xa1000000 || (uint64_t)src + length > 0xae9e0000) { goto rejected; }
        s->graphics_upload_pending[ch] = true;
        timer_mod(s->graphics_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        break;
    }
    case 0x1020:
        if (!write || *value != 0) { goto rejected; }
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->priv), 129 - 32), 0);
        break;
    default:
        return false;
    }
    s->graphics_regs[off / 4] = *value;
    z7_mmio_trace(addr, size, write, *value);
    return true;
rejected:
    /* This model owns these registers. Invalid transfers must not fall
     * through to the historical recovery profile's configuration latches. */
    z7_unknown_access(s, addr, size, write, *value);
    return true;
}

/* Recovery-only configuration latches. No completed transfer is inferred from
 * these writes. Each entry must have an original-code address/bit reference. */
static bool z7_recovery_config_access(Z7LabState *s, hwaddr addr, unsigned size,
                                      bool write, uint64_t *value)
{
    static const struct { hwaddr addr; uint32_t mask; bool write_only; unsigned width; } regs[] = {
        { 0x04281030, 0x3f07ff07 }, /* 101d7bbe..c24 + a1806e84: gate/divider fields; reset zero hypothesis */
        /* 100cbca2/100cbd1e and 100cbda0: two serial format/clock
         * configuration banks, selected by 100cc752. These retain fields;
         * FIFO readiness, transfers and interrupts are separate contracts. */
        { 0x2c080108, 0xff8f0003 }, { 0x2c080208, 0xff8f0003 },
        { 0x2c08010c, 0x0f8f0000 }, { 0x2c08020c, 0x0f8f0000 },
        { 0x2c080114, 0x5f1f0000 }, { 0x2c080214, 0x5f1f0000 },
        { 0x2c080118, 0x33030101 }, { 0x2c080218, 0x33030101 },
        { 0x04280024, 1, true }, /* 101f24fc output enable used by graphics init 1044fab0 */
        { 0x2b100000, 0x101, true }, { 0x2b100004, 1, true }, /* 103201e0/f6 disable controls */
        { 0x2b100008, UINT32_MAX, true }, { 0x2b10000c, 0x110000, true }, /* 10320204 config */
        { 0x2c04000c, 0xff, true }, /* 10180040 configuration byte stored as word */
        { 0x2c040000, 1, true }, { 0x2c040004, 1, true }, { 0x2c040008, 1, true }, /* 1017ff2c..72 reset/config gates */
        { 0x2c040014, UINT32_MAX, true }, /* 1017ff3a program address, processor execution unmodeled */
        { 0x2c040010, 0x01030001 }, /* 10180048 control/command bits; no auto-completion */
        { 0x20060028, 1, true }, { 0x2006002c, 1, true },
        { 0x20060030, 1, true }, /* 1010c62a/632 submission */
        { 0x20060314, UINT32_MAX, true }, { 0x20060328, 0xff, true },
        { 0x2006032c, 0xff, true }, /* 1010c7d0..7e4 */
        /* 1010b628..64e: paired command writes (callers also pass inverted
         * masks), trigger/cancel, and an ordinary interrupt gate at +18.
         * Submission alone produces no image, status or interrupt. */
        { 0x20068000, UINT32_MAX, true }, { 0x20068004, UINT32_MAX, true },
        { 0x20068008, 0xfff, true }, { 0x2006800c, 0xfff, true },
        { 0x20068010, 0xfff, true }, { 0x20068014, 0xfff, true },
        { 0x20068018, 0xfff },
        { 0x20068020, UINT32_MAX, true }, { 0x20068024, UINT32_MAX, true },
        { 0x20068028, 1, true }, /* 1010bc4a: optional pair plus gate */
        { 0x20068060, UINT32_MAX, true }, /* 1010bc56 submission */
        { 0x20068070, UINT32_MAX, true }, /* 1010bc5c: selectors at bits 1,3,5 */
        { 0x20068074, UINT32_MAX, true }, { 0x20068078, UINT32_MAX, true },
        { 0x20068080, UINT32_MAX, true }, /* 1010bc6c timing configuration */
        { 0x20068088, 0xff, true }, { 0x2006808c, 0xffffffff, true }, /* 1010bc1c */
        { 0x20068100, UINT32_MAX, true }, { 0x20068104, UINT32_MAX, true }, /* 1010bc3a/42 */
        /* 1010bc96/1010bd7a: eight independent interrupt mask words. */
        { 0x20068200, UINT32_MAX }, { 0x20068220, UINT32_MAX },
        { 0x20068240, UINT32_MAX }, { 0x20068260, UINT32_MAX },
        { 0x20068280, UINT32_MAX }, { 0x200682a0, UINT32_MAX },
        { 0x200682c0, UINT32_MAX }, { 0x200682e0, UINT32_MAX },
        { 0x24060008, UINT32_MAX, true }, { 0x2406000c, 0xff, true }, /* 100ce4ba config */
        { 0x24060000, 1 }, { 0x24060014, UINT32_MAX, true }, /* 100ce3ba reset/boot address */
        { 0x20016020, UINT32_MAX }, { 0x20016024, UINT32_MAX },
        { 0x20016028, UINT32_MAX }, { 0x2001602c, UINT32_MAX },
        { 0x20016030, UINT32_MAX }, { 0x20016034, UINT32_MAX },
        { 0x20016038, UINT32_MAX }, { 0x2001603c, UINT32_MAX }, /* 101bb76e snapshot */
        { 0x04280074, 0xff, true }, /* 101f2544 two 4-bit fields */
        { 0x200600b0, 1 }, /* 1010c992/9a4 enable state, reset zero hypothesis */
        { 0x20060070, 3, true }, { 0x20060074, 3, true },
        { 0x20060078, 3, true }, { 0x2006007c, 3, true },
        { 0x20060080, 3, true }, { 0x20060084, 3, true },
        { 0x20060088, 3, true }, { 0x2006008c, 3, true },
        { 0x20060090, 3, true }, { 0x20060094, 3, true },
        { 0x20060098, 3, true }, { 0x2006009c, 3, true }, /* 1010c7ea/846 per-channel state */
        { 0x200600a4, 0x1f }, /* 1010c8ae..954 bit controls */
        { 0x200600a0, UINT32_MAX, true }, /* 1010c8a6 submission */
        { 0x200600a8, UINT32_MAX, true }, { 0x200600ac, UINT32_MAX, true }, /* 1010c960/968 command writes */
        { 0x200600b4, 0xff, true }, { 0x200600b8, 0xff, true },
        { 0x200600bc, 0xff, true }, { 0x200600c0, 0xff, true },
        { 0x200600c4, 0xff, true }, /* 1010c970 byte configuration */
        { 0x200632d0, 0x7ff }, { 0x200632e0, 0x7ff }, /* 100cdf44/58 channel mask */
        { 0x200632d4, 1, true }, { 0x200632e4, 1, true }, /* mask submission */
        { 0x200632d8, 0x7ff7ff, true }, { 0x200632dc, 1, true }, /* 100ce158 banked mask submission */
        { 0x200632e8, UINT32_MAX, true }, /* 100ce16a configuration */
        { 0x200632ec, 0xfff, true }, { 0x200632f0, 0xfff, true }, /* 100ce172/182 byte pairs */
        { 0x200632f4, UINT32_MAX, true }, { 0x200632f8, UINT32_MAX, true }, /* 100ce192/19a */
        { 0x200632fc, 1, true }, { 0x20063300, 1, true }, /* 100ce1a2/1b2 */
        { 0x20063304, 0xf, true }, { 0x20063308, 1, true }, /* 100ce1c2 index 0..9 + submission */
        { 0x20202018, 1 }, /* 10104e7e/494: enable and disable */
        { 0x20070040, 1 }, /* 1008d998: second fill channel initially disabled */
        { 0x20102000, 0xf }, { 0x20102100, 0xf }, /* 1017d902/91a */
        { 0x20102820, 0x0f330f33 }, { 0x20102824, 0x0f330f33 },
        { 0x20102828, 0x0f330f33 }, { 0x2010282c, 0x0f330f33 },
        { 0x20102830, 0x0f330f33 }, { 0x20102834, 0x0f330f33 },
        { 0x20102838, 0xf33 }, /* 1017dbae setter / 1017dc1e getter: 13 routing entries */
        { 0x20102800, 1, true }, /* 1017dc18/dd14 submission; output not yet modeled */
        /* 104fea6e initial graphics configuration. Three IRQ handlers clear
         * +1020/+c18/+1c with zero (104fee62/6e/7a). No IRQ generated yet. */
        { 0x20181020, 0, true }, { 0x20180c18, 0, true }, { 0x2018001c, 0, true },
        { 0x20181248, 1, true }, { 0x20180034, 0, true },
        { 0x20180c1c, 0x8000, true }, { 0x2018151c, 1, true }, { 0x2018155c, 1, true },
        { 0x20181214, 0x0100ffff, true }, { 0x20181298, 0, true }, /* 104feae2 graphics config */
        { 0x201810d4, UINT32_MAX, true }, { 0x201810dc, 0x7fffffff, true },
        { 0x201810d0, 1, true }, { 0x201810e4, UINT32_MAX, true },
        { 0x201810ec, 0x7fffffff, true }, { 0x201810e0, 1, true }, /* 104feb0e..64 table bases/lengths */
        { 0x0426f000, UINT32_MAX }, { 0x0426f010, UINT32_MAX }, /* 1017fbd6/1017fc0a: per-pin IRQ masks */
        { 0x0426f014, UINT32_MAX }, { 0x0426f018, UINT32_MAX }, /* 1017fb50 edge configuration */
        { 0x20100030, 0x00ffff01 }, { 0x20100034, 0x00ffff01 }, /* 1017dff2 */
        { 0x201000f4, 0x000fff00, true }, /* 1017e04a */
        { 0x201000f8, 0xfffffff0, true }, { 0x201000fc, 0xfffffff0, true },
        { 0x20100100, 0xfffffff0, true }, { 0x20100104, 0xfffffff0, true },
        { 0x20100108, 0x000fff00, true }, { 0x2010010c, 0xff0fff00, true },
        { 0x20100120, 0xfffffff0, true }, { 0x20100124, 0xfffffff0, true },
        { 0x20100128, 0xfffffff0, true }, { 0x2010012c, 0xfffffff0, true }, /* 1017e05e..13e */
        { 0x2038000c, 1, true }, { 0x203c000c, 1, true },
        { 0x20380000, 3, true }, { 0x203c0000, 3, true },
        { 0x20380004, 3, true }, { 0x203c0004, 3, true }, /* 101054e6 */
        { 0x203800e8, UINT32_MAX, true }, { 0x203c00e8, UINT32_MAX, true },
        { 0x203800f0, UINT32_MAX, true }, { 0x203c00f0, UINT32_MAX, true }, /* 10105820/26 */
        { 0x203800d0, 1, true }, /* 101056e4/10105778 mode selector */
        { 0x201002c0, 1, true }, /* 1017e64a prepares 24010000 scratch */
        { 0x20201044, 3, true }, { 0x20201048, 7, true }, /* 10108834 */
        { 0x20204000, 1 }, { 0x20204004, 1 }, /* 10108d80/8e */
        { 0x20205308, 3 }, /* 1008dcae clears low two enable bits */
        { 0x20203010, 0, true }, /* 10108e70 initial disable */
        { 0x20141100, 3 }, { 0x20145100, 3 }, /* 102410d4 config, 10241004 getter */
        { 0x20140000, 0, true }, { 0x20144000, 0, true },
        { 0x20142000, 0, true }, { 0x20146000, 0, true },
        { 0x20143000, 0, true }, { 0x20147000, 0, true }, /* 1024104c */
        { 0x20141000, 0, true }, { 0x20145000, 0, true },
        { 0x2014d100, 1 }, /* 102434cc: configuration hold, write then clear after delay */
        { 0x2014d200, UINT32_MAX, true }, /* 102434de/f0 configuration under hold */
        { 0x2014c0a0, 1, true }, /* 102436f0/fc enable/disable, no external output modeled */
        { 0x2014e000, 0, true }, /* 10241074..88 */
        { 0x2016c000, 6, false, 1 }, { 0x2016c018, 0xf2, true, 1 }, /* 1025ad06 */
        { 0x2016c0a4, 8, true, 1 }, { 0x2016c0cc, 0xff, true, 1 },
        { 0x2016c0ac, 0xff, true, 1 }, { 0x2016c0b0, 0xff, true, 1 },
        { 0x2016c0b4, 0xff, true, 1 }, { 0x2016c0b8, 0xff, true, 1 },
        { 0x2016c0bc, 0xff, true, 1 }, { 0x2016c0c0, 0xff, true, 1 },
        { 0x2016c0c4, 0xff, true, 1 }, { 0x2016c0c8, 0xff, true, 1 },
        { 0x2016c0a8, 0, false, 1 }, /* 10259452: zero error during bounded poll */
        { 0x2017f81c, 8, true, 1 }, { 0x2017f84c, 0xff, true, 1 },
        { 0x2017f82c, 0xff, true, 1 }, { 0x2017f830, 0xff, true, 1 },
        { 0x2017f834, 0xff, true, 1 }, { 0x2017f838, 0xff, true, 1 },
        { 0x2017f83c, 0xff, true, 1 }, { 0x2017f840, 0xff, true, 1 },
        { 0x2017f844, 0xff, true, 1 }, { 0x2017f848, 0xff, true, 1 },
        { 0x2017f824, 0, false, 1 }, /* 102593dc: zero error during bounded poll */
        { 0x20160600, 0xff, true, 1 }, { 0x20160604, 0xff, true, 1 },
        { 0x20160608, 0x13, true, 1 }, { 0x2016060c, 0xb, true, 1 },
        { 0x20160610, 0x3d, true, 1 }, { 0x20160614, 0, true, 1 },
        { 0x20160618, 0x7f, true, 1 }, { 0x2016061c, 0xff, true, 1 },
        { 0x20160620, 0, true, 1 }, { 0x201607fc, 3, true, 1 }, /* 1025ad40..76 */
    };
    QEMU_BUILD_BUG_ON(ARRAY_SIZE(regs) > ARRAY_SIZE(s->recovery_config));
    if (!s->recovery_profile) { return false; }
    for (unsigned i = 0; i < ARRAY_SIZE(regs); i++) {
        if (addr != regs[i].addr) { continue; }
        if (size != (regs[i].width ? regs[i].width : 4)) { return false; }
        if (!write && regs[i].write_only) { return false; }
        if (write) {
            if (*value & ~(uint64_t)regs[i].mask) { return false; }
            s->recovery_config[i] = *value;
        } else { *value = s->recovery_config[i]; }
        z7_mmio_trace(addr, size, write, *value);
        return true;
    }
    return false;
}

static uint64_t z7_unknown_read(void *opaque, hwaddr addr, unsigned size)
{
    Z7LabState *s = opaque;
    int index = z7_early_index(addr);
    uint64_t fill_value = 0;
    if (z7_exmcu_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_capture_control_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_capture_irq_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_sequence_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_capture_sync_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_capture_processing_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_capture_image_channel_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_display_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_graphics_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_peripheral_dma_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_i2c_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (s->recovery_profile && addr == 0x2016c010 && size == 1) {
        /* 102595de/61e sample bit 1 and bits 4..7 into input-state outputs.
         * All-low external inputs are an unmeasured hypothesis, not completion. */
        z7_mmio_trace(addr, size, false, 0);
        return 0;
    }
    if (s->recovery_profile && addr == 0x24020080 && size == 4) {
        /* Absent processing coprocessor hypothesis: original 1017d402
         * requires signature 0x100 and returns -21 for this zero value.
         * Do not advertise a running processor without its mailbox model. */
        z7_mmio_trace(addr, size, false, 0);
        return 0;
    }
    if (s->recovery_profile && addr == 0x04280000 && size == 4) {
        /* 101f24c0 extracts low six and top four identification bits.
         * Hardware revision zero is an explicit, unmeasured hypothesis. */
        z7_mmio_trace(addr, size, false, 0);
        return 0;
    }
    if (z7_copy_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_channel_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_panel_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_serial_config_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_engine_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (z7_recovery_config_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (s->recovery_profile && addr == 0x04251000 && size == 4) {
        z7_mmio_trace(addr, size, false, s->channel_control[2]);
        return s->channel_control[2];
    }
    if (s->recovery_profile && addr >= 0x12020000 && addr + size <= 0x14000000) {
        fill_value = size == 4 ? UINT32_MAX : ((UINT64_C(1) << (size * 8)) - 1);
        z7_mmio_trace(addr, size, false, fill_value);
        return fill_value;
    }
    if (z7_driver_init_access(s, addr, size, false, &fill_value)) { return fill_value; }
    if (s->driver_fifo_depth && addr >= 0x2c0a0000 && addr < 0x2c0a0b00 && size == 4) {
        unsigned ch = (addr - 0x2c0a0000) / 0x100;
        unsigned off = addr & 0xff;
        if (off == 0x48 && s->serial_initialized[ch]) {
            z7_mmio_trace(addr, size, false, s->driver_fifo_depth);
            return s->driver_fifo_depth;
        }
        if (off == 0x60 && s->serial_initialized[ch]) {
            z7_mmio_trace(addr, size, false, s->serial_disabled[ch]);
            return s->serial_disabled[ch];
        }
    }
    if (z7_event_access(s, addr, size, false, &fill_value)) {
        return fill_value;
    }
    if (z7_platform_access(s, addr, size, false, &fill_value)) {
        return fill_value;
    }
    if (s->boot_profile && size == 4 && (addr == 0x04266008 || addr == 0x0426600c)) {
        uint64_t ticks = (uint64_t)s->early_control[7] << 32 | s->early_control[6];
        if (s->early_control[5] & 1) {
            ticks += muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->counter_epoch,
                              48000000, 1000000000);
        }
        fill_value = addr == 0x04266008 ? (uint32_t)ticks : ticks >> 32;
        z7_mmio_trace(addr, size, false, fill_value);
        return fill_value;
    }
    if (s->boot_profile && z7_setup_access(s, addr, size, false, &fill_value)) {
        return fill_value;
    }
    if (s->boot_profile && z7_uart_access(s, addr, size, false, &fill_value)) {
        return fill_value;
    }
    if (s->boot_profile && z7_fill_access(s, addr, size, false, &fill_value)) {
        return fill_value;
    }
    if (s->boot_profile && size == 4 && addr >= 0x20020000 && addr < 0x20060000 && !(addr & 3)) {
        unsigned ddr_index = (addr - 0x20020000) / 4;
        if (s->ddr_written[ddr_index]) {
            z7_mmio_trace(addr, size, false, s->ddr_regs[ddr_index]);
            return s->ddr_regs[ddr_index];
        }
    }
    if (s->boot_profile && size == 4 && (index >= 2 || index == 0)) {
        z7_mmio_trace(addr, size, false, s->early_control[index]);
        return s->early_control[index];
    }
    z7_unknown_access(s, addr, size, false, 0);
    return 0; /* Shutdown is requested; this is not a validated register value. */
}

static void z7_unknown_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    Z7LabState *s = opaque;
    if (z7_capture_control_access(s, addr, size, true, &value)) { return; }
    if (z7_exmcu_access(s, addr, size, true, &value)) { return; }
    if (z7_capture_irq_access(s, addr, size, true, &value)) { return; }
    if (z7_capture_sync_access(s, addr, size, true, &value)) { return; }
    if (z7_capture_processing_access(s, addr, size, true, &value)) { return; }
    if (z7_capture_image_channel_access(s, addr, size, true, &value)) { return; }
    if (z7_display_access(s, addr, size, true, &value)) { return; }
    if (z7_graphics_access(s, addr, size, true, &value)) { return; }
    if (z7_sequence_access(s, addr, size, true, &value)) { return; }
    int index = z7_early_index(addr);
    if (z7_peripheral_dma_access(s, addr, size, true, &value)) { return; }
    if (z7_i2c_access(s, addr, size, true, &value)) { return; }
    if (z7_copy_access(s, addr, size, true, &value)) { return; }
    if (z7_channel_access(s, addr, size, true, &value)) { return; }
    if (z7_panel_access(s, addr, size, true, &value)) { return; }
    if (z7_serial_config_access(s, addr, size, true, &value)) { return; }
    if (z7_engine_access(s, addr, size, true, &value)) { return; }
    if (z7_recovery_config_access(s, addr, size, true, &value)) { return; }
    if (s->recovery_profile && size == 4) {
        if (addr == 0x04251000 && !(value & ~UINT64_C(0x11100000))) {
            s->channel_control[2] = value;
            z7_mmio_trace(addr, size, true, value);
            return;
        }
        /* Original 1005b2b6 reset/enable sequence, no channel transfers yet. */
        if ((addr == 0x2c010000 && value <= 1) ||
            (addr == 0x2c010004 && value == 1)) {
            s->channel_control[(addr - 0x2c010000) / 4] = value;
            z7_mmio_trace(addr, size, true, value);
            return;
        }
    }
    if (z7_driver_init_access(s, addr, size, true, &value)) { return; }
    /* 100682a0 initializes eleven channels; 10068302/362 write +60=0/1
     * when opening/closing. Only initialization is modeled. +48 is interpreted
     * as FIFO depth by 100683b6, 100684c0 and 100689ac, but its hardware value
     * is unknown, hence explicitly named 16/64-word hypothesis machines. */
    if (s->driver_fifo_depth && addr >= 0x2c0a0000 && addr < 0x2c0a0b00 &&
        size == 4 && (addr & 0xff) == 0x60 && value <= 1) {
        unsigned ch = (addr - 0x2c0a0000) / 0x100;
        s->serial_disabled[ch] = value;
        s->serial_initialized[ch] = true;
        z7_mmio_trace(addr, size, true, value);
        return;
    }
    if (z7_event_access(s, addr, size, true, &value)) {
        return;
    }
    if (z7_platform_access(s, addr, size, true, &value)) {
        return;
    }
    if (s->boot_profile && z7_setup_access(s, addr, size, true, &value)) {
        return;
    }
    if (s->boot_profile && z7_uart_access(s, addr, size, true, &value)) {
        return;
    }
    if (s->boot_profile && z7_fill_access(s, addr, size, true, &value)) {
        return;
    }
    if (s->boot_profile && size == 4 && z7_ddr_write(s, addr, value)) {
        return;
    }
    if (s->boot_profile && size == 4 && index >= 0) {
        if (index == 8 && value != 48000000) {
            z7_unknown_access(s, addr, size, true, value);
            return;
        }
        if (index == 5 && !(value & 1) && (s->early_control[index] & 1)) {
            uint64_t ticks = ((uint64_t)s->early_control[7] << 32 | s->early_control[6]) +
                muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->counter_epoch, 48000000, 1000000000);
            s->early_control[6] = ticks;
            s->early_control[7] = ticks >> 32;
        }
        if (index == 5 && (value & 1) && !(s->early_control[index] & 1)) {
            s->counter_epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        s->early_control[index] = value;
        z7_mmio_trace(addr, size, true, value);
        return;
    }
    z7_unknown_access(s, addr, size, true, value);
}

/* MMIO callbacks hold the BQL, so the per-transaction diagnostic flag is serial. */
static MemTxResult z7_read_attrs(void *opaque, hwaddr addr, uint64_t *value,
                                unsigned size, MemTxAttrs attrs)
{
    Z7LabState *s = opaque;
    s->debug_rejected = false;
    *value = z7_unknown_read(s, addr, size);
    return s->debug_rejected ? MEMTX_DECODE_ERROR : MEMTX_OK;
}

static MemTxResult z7_write_attrs(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size, MemTxAttrs attrs)
{
    Z7LabState *s = opaque;
    s->debug_rejected = false;
    z7_unknown_write(s, addr, value, size);
    return s->debug_rejected ? MEMTX_DECODE_ERROR : MEMTX_OK;
}

static const MemoryRegionOps z7_unknown_ops = {
    .read_with_attrs = z7_read_attrs,
    .write_with_attrs = z7_write_attrs,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* Firmware pool boundaries are allocator boundaries, sometimes unaligned.
 * Give their RAM backing a matching page offset so TCG's IOTLB physical
 * pages stay aligned. The alias exposes exactly the original pool interval;
 * its padding is not mapped and DMA whitelists remain the original bounds. */
static void z7_map_pool(MemoryRegion *sysmem, MemoryRegion *ram, MemoryRegion *alias,
                        const char *name, uint32_t base, uint64_t size)
{
    unsigned prefix = base & 0xfff;
    uint64_t backing = (size + prefix + 0xfff) & ~UINT64_C(0xfff);
    g_autofree char *alias_name = g_strdup_printf("%s-window", name);
    memory_region_init_ram(ram, NULL, name, backing, &error_fatal);
    memory_region_init_alias(alias, NULL, alias_name, ram, prefix, size);
    memory_region_add_subregion(sysmem, base, alias);
}

static void z7_common_init(MachineState *machine, bool boot_profile, bool gic_v1,
                           bool platform_profile, bool pattern, bool heap_ram,
                           unsigned driver_fifo_depth, bool recovery_profile)
{
    Z7LabState *s = g_new0(Z7LabState, 1);
    s->boot_profile = boot_profile;
    s->platform_profile = platform_profile;
    s->driver_fifo_depth = driver_fifo_depth;
    s->recovery_profile = recovery_profile;
    s->graphics_profile = !strcmp(MACHINE_GET_CLASS(machine)->name, "nikon-z7-display");
    if (s->graphics_profile) {
        s->shoulder.enabled = g_strcmp0(getenv("Z7_SHOULDER_OBSERVE"), "1") == 0;
        s->exmcu_test_device = g_strcmp0(getenv("Z7_EXMCU_TEST_DEVICE"), "1") == 0;
        s->exmcu_no_lens = s->exmcu_test_device && g_strcmp0(getenv("Z7_EXMCU_NO_LENS"), "1") == 0;
        s->exmcu_capture_sync_test = s->exmcu_test_device &&
            g_strcmp0(getenv("Z7_EXMCU_CAPTURE_SYNC_TEST"), "1") == 0;
        s->exmcu_capture_sync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                                 z7_exmcu_capture_sync_complete, s);
        s->exmcu_tx_ready = true;
        s->exmcu_gpio_mask = UINT32_MAX;
        if (s->exmcu_test_device) {
            fprintf(stderr, "Z7_EXMCU_TEST_DEVICE transport=spi0_spi1 capcom=2_4 no_lens=%u diagnostic_only=1\n",
                    s->exmcu_no_lens);
        }
        s->gpu = z7_gpu_new();
        s->lcd_scanout_test = g_strcmp0(getenv("Z7_LCD_SCANOUT_TEST"), "1") == 0;
        s->lcd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_lcd_present, s);
        if (s->lcd_scanout_test) {
            timer_mod(s->lcd_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000000);
        }
        s->graphics_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_graphics_complete, s);
        for (unsigned ch = 0; ch <= 9; ch++) {
            if (ch > 1 && ch != 6 && ch != 8 && ch != 9) { continue; }
            Z7SerialSink *p = &s->serial_sink[ch];
            p->owner = s;
            p->channel = ch;
            p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_panel_complete, p);
        }
    }
    if (recovery_profile) {
        s->sequence_stop_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_sequence_stopped, s);
        s->capture_isp_stop_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_isp_stopped, s);
        s->capture_test_pulse = g_strcmp0(getenv("Z7_TG_TEST_PULSE"), "1") == 0;
        s->capture_test_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_capture_test_pulse, s);
        s->capture_periodic_test = g_strcmp0(getenv("Z7_TG_PERIODIC_TEST"), "1") == 0;
        s->capture_periodic_paired_test = g_strcmp0(getenv("Z7_TG_PAIRED_TEST"), "1") == 0;
        s->capture_isp_boundary_test = g_strcmp0(getenv("Z7_ISP_BOUNDARY_TEST"), "1") == 0;
        s->image_process_test = g_strcmp0(getenv("Z7_IMAGE_PROCESS_TEST"), "1") == 0;
        s->capture_period_ns = s->capture_periodic_paired_test ? 16666667 : 33333333;
        const char *period = getenv("Z7_TG_PERIOD_NS");
        if (period) {
            uint64_t ns;
            if (qemu_strtou64(period, NULL, 10, &ns) || ns < 16666667 || ns > 100000000) {
                error_report("Z7_TG_PERIOD_NS must be 16666667..100000000 (experimental timing)");
                exit(2);
            }
            s->capture_period_ns = ns;
        }
        const char *ingress = getenv("Z7_RAW_INGRESS_TEST");
        if (ingress) {
            struct stat info;
            GError *error = NULL;
            if (!s->capture_periodic_test || stat(ingress, &info) || info.st_size != 1909440 ||
                !g_file_get_contents(ingress, &s->capture_ingress_data, &s->capture_ingress_length, &error) ||
                s->capture_ingress_length != 1909440) {
                error_report("Z7_RAW_INGRESS_TEST needs periodic test and a 1909440-byte input: %s",
                             error ? error->message : "invalid test configuration");
                exit(2);
            }
            fprintf(stderr, "Z7_RAW_INGRESS_TEST_SOURCE width=1380 height=918 stride=2080 packing_verified=0 completion_irq=0\n");
        }
        const char *native0 = getenv("Z7_RAW_NATIVE0_TEST");
        if (native0) {
            GError *error = NULL;
            if (!s->capture_periodic_test ||
                !g_file_get_contents(native0, &s->capture_native0_data, &s->capture_native0_length, &error) ||
                s->capture_native0_length != 5568 * 932) {
                error_report("Z7_RAW_NATIVE0_TEST invalid input: %s", error ? error->message : "size/configuration");
                exit(2);
            }
        }
        s->capture_periodic_test_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_capture_periodic_test, s);
        if (s->capture_periodic_test) {
            fprintf(stderr, "Z7_TG_PERIODIC_TEST_SOURCE paired=%u edge_period_ns=%u limit=%u diagnostic_only=1\n",
                    s->capture_periodic_paired_test, s->capture_period_ns,
                    s->capture_periodic_paired_test ? 180 : 90);
        }
        if (s->capture_test_pulse) {
            fprintf(stderr, "Z7_TG_TEST_SOURCE delay_ns=1000000 status=0x10 diagnostic_only=1\n");
        }
        s->i2c_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_i2c_complete, s);
        for (unsigned ch = 0; ch < 4; ch++) {
            s->i2c_regs[ch][7] = 0xff;
            s->i2c_regs[ch][8] = 0x2ff;
        }
        s->copy_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_copy_complete, s);
        s->clock_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_clock_complete, s);
        s->engine_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_engine_stopped, s);
        s->image_process_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_image_process_tick, s);
        s->image_drain_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_image_drain_tick, s);
        s->channel_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_channel_expire, s);
        fprintf(stderr, "Z7_RECOVERY_HYPOTHESIS absent_flash_12020000_13ffffff=erased_ff; original_payload_unchanged\n");
    }
    if (driver_fifo_depth) {
        fprintf(stderr, "Z7_DRIVER_HYPOTHESIS serial_fifo_depth=%u system_input_0428002c=0 data_path=unmodeled\n", driver_fifo_depth);
    }
    if (platform_profile) {
        for (unsigned i = 0; i < ARRAY_SIZE(z7_platform_saved_addresses); i++) {
            s->platform_regs[(z7_platform_saved_addresses[i] - 0x20010000) / 4] =
                pattern ? (0x13570000U ^ (i * 0x010101U)) : 0;
        }
        s->platform_regs[0x38 / 4] |= 1;
        for (unsigned i = 0; i < ARRAY_SIZE(s->platform_saved); i++) {
            s->platform_saved[i] = pattern ? (0xa5c30000U ^ (i * 0x010203U)) : 0;
        }
        s->platform_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z7_platform_complete, s);
        fprintf(stderr, "Z7_PLATFORM_HYPOTHESIS initial_state=%s ready_bit=1 latency_ns=1000\n",
                pattern ? "synthetic-pattern" : "synthetic-zero");
    }
    MemoryRegion *sysmem = get_system_memory();
    for (unsigned n = 0; n < machine->smp.cpus; n++) {
        Object *cpu = object_new(machine->cpu_type);
        if (boot_profile) {
        /* Provisional placement: firmware discovers this through CBAR, not a
         * fixed literal. Its page table maps 20000000..2fffffff as device memory.
         * 10020324..40 uses CBAR+1000/+2000; this matches the A15 GICv2 layout. */
        object_property_set_int(cpu, "reset-cbar", 0x2c000000, &error_fatal);
        object_property_set_int(cpu, "cntfrq", 48000000, &error_fatal);
        if (gic_v1) { object_property_set_bool(cpu, "has_el2", false, &error_fatal); }
        }
        qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    }
    if (boot_profile) {
        DeviceState *priv = qdev_new(TYPE_A15MPCORE_PRIV);
        s->priv = A15MPCORE_PRIV(priv);
        if (gic_v1) { qdev_prop_set_uint32(DEVICE(&s->priv->gic), "revision", 1); }
        SysBusDevice *bus = SYS_BUS_DEVICE(priv);
        qdev_prop_set_uint32(priv, "num-cpu", machine->smp.cpus);
        qdev_prop_set_uint32(priv, "num-irq", 256);
        sysbus_realize_and_unref(bus, &error_fatal);
        sysbus_mmio_map(bus, 0, 0x2c000000);
        for (unsigned n = 0; n < machine->smp.cpus; n++) {
            DeviceState *cpu = DEVICE(qemu_get_cpu(n));
            sysbus_connect_irq(bus, n, qdev_get_gpio_in(cpu, ARM_CPU_IRQ));
            sysbus_connect_irq(bus, n + machine->smp.cpus, qdev_get_gpio_in(cpu, ARM_CPU_FIQ));
            sysbus_connect_irq(bus, n + 2 * machine->smp.cpus, qdev_get_gpio_in(cpu, ARM_CPU_VIRQ));
            sysbus_connect_irq(bus, n + 3 * machine->smp.cpus, qdev_get_gpio_in(cpu, ARM_CPU_VFIQ));
        }
    }

    memory_region_init_io(&s->unknown, OBJECT(machine), &z7_unknown_ops,
                          s, "z7-lab-unmodeled", UINT64_C(0x100000000));
    memory_region_add_subregion_overlap(sysmem, 0, &s->unknown, -100);
    memory_region_init_rom(&s->main_rom, NULL, "z7-main-rom",
                           0x02000000, &error_fatal);
    memory_region_add_subregion(sysmem, 0x10020000, &s->main_rom);
    /* Boot RAM from startup copies. Platform heap boundary ac400000 comes from
     * 101e3dac/3dd2; the guest runtime MMU maps VA 14000000 to PA a9200000.
     * No host virtual alias is needed: the unmodified guest builds its own PTEs. */
    memory_region_init_ram(&s->inferred_ram, NULL, "z7-inferred-ram",
                          heap_ram ? 0x0ac00000 : (boot_profile ? 0x07a00000 : 0x01800000), &error_fatal);
    memory_region_add_subregion(sysmem, 0xa1800000, &s->inferred_ram);
    if (recovery_profile) {
        /* Original 101ad9e6 shadows 7 MiB into ac400000..acafffff. */
        memory_region_init_ram(&s->shadow_ram, NULL, "z7-code-shadow-ram", 0x700000, &error_fatal);
        memory_region_add_subregion(sysmem, 0xac400000, &s->shadow_ram);
        /* Original graphics heap init 10412b16: acb00000 + d20000 and
         * ad900000 + 10e0000. The intervening e0000 is reserved by its caller.
         * Covers those original RAM allocations, ending at ae9e0000. */
        memory_region_init_ram(&s->application_heap, NULL, "z7-graphics-heap", 0x1ee0000, &error_fatal);
        memory_region_add_subregion(sysmem, 0xacb00000, &s->application_heap);
        /* 100d2224 places a firmware resource at physical 40300000.
         * This 4 MiB window is provisional; not the a0c00000 allocation. */
        memory_region_init_ram(&s->processing_ram, NULL, "z7-processing-ram", 0x400000, &error_fatal);
        memory_region_add_subregion(sysmem, 0x40000000, &s->processing_ram);
        /* 102e90ec copies the embedded ARM image here. Its reset code uses
         * initialized data and stacks as high as a14a9240. */
        memory_region_init_ram(&s->auxiliary_ram, NULL, "z7-auxiliary-ram", 0x800000, &error_fatal);
        memory_region_add_subregion(sysmem, 0xa1000000, &s->auxiliary_ram);
        /* Native map boundaries 101e3ebc/c0 and identity MMU mapping.
         * lv150 enters the original processing worker and clears its first
         * workspace at a0000000. This is RAM, not a device completion. */
        memory_region_init_ram(&s->image_workspace_ram, NULL, "z7-image-workspace-ram", 0x1000000, &error_fatal);
        memory_region_add_subregion(sysmem, 0xa0000000, &s->image_workspace_ram);
        /* Original startup RAM descriptor 10034094: base + size. Native
         * live view allocates its engine-400 ring in this pool (lv153). */
        memory_region_init_ram(&s->image_large_pool, NULL, "z7-image-large-pool", 0x4acb3000, &error_fatal);
        memory_region_add_subregion(sysmem, 0x53900000, &s->image_large_pool);
        /* Alternate native preview pool, descriptors11524ca8/11524d38:
         * four 1c4000-byte buffers; selected by current live-view mode. */
        z7_map_pool(sysmem, &s->image_preview_pool, &s->image_preview_alias,
                    "z7-image-preview-pool", 0xdc55a800, 0x710000);
        /* Native1017e64e..652 clears exactly 0x1000 bytes before unit22
         * parameter submission. Expose only this observed scratch extent. */
        memory_region_init_ram(&s->image_curve_ram, NULL, "z7-image-curve-ram", 0x1000, &error_fatal);
        memory_region_add_subregion(sysmem, 0x24010000, &s->image_curve_ram);
        /* 1017ff5e..6a copies 16 KiB from 40100000 into this program SRAM.
         * The controller ISA and execution remain unknown. */
        memory_region_init_ram(&s->controller_sram, NULL, "z7-controller-program-sram", 0x4000, &error_fatal);
        memory_region_add_subregion(sysmem, 0x04294000, &s->controller_sram);
    }
    if (s->graphics_profile) {
        /* ROM 115241a0 fixed image-pool descriptors: pool 39 is GUI memory;
         * pools 37,38,35,36 form this contiguous output-buffer interval.
         * Guest 10123432 initializes their allocators; mapping alone does not. */
        z7_map_pool(sysmem, &s->gui_pool, &s->gui_pool_alias,
                    "z7-gui-pool", 0xb045f200, 0x386c200);
        memory_region_init_ram(&s->output_pools, NULL, "z7-output-pools", 0x883b000, &error_fatal);
        memory_region_add_subregion(sysmem, 0xd2d00000, &s->output_pools);
        /* Other nonzero descriptors in ROM pointer table 115241a0.
         * Descriptor +4 is total bytes and +10 the physical base; adjacent
         * pools are merged but their reserved gaps remain unmapped. The
         * original MMU translates VA 37ddea00 to pool 40 at PA 531dea00.
         * These are RAM allocations, not sensor data or completed frames. */
        static const struct { uint32_t base, size; } pools[] = {
            { 0x44c00000, 0x0afd2400 }, { 0x4fc00000, 0x03c2da00 },
            { 0x9fc00000, 0x0035a600 }, { 0xaea00000, 0x01a5f200 },
            { 0xb3ccb400, 0x02200000 }, { 0xb5f00000, 0x1cdd6000 },
        };
        for (unsigned i = 0; i < ARRAY_SIZE(pools); i++) {
            g_autofree char *name = g_strdup_printf("z7-capture-pools-%u", i);
            z7_map_pool(sysmem, &s->capture_pools[i], &s->capture_pool_aliases[i],
                        name, pools[i].base, pools[i].size);
        }
    }
    /* Invented laboratory RAM, deliberately separate from the inferred Nikon map. */
    if (!boot_profile) {
        memory_region_init_ram(&s->lab_ram, NULL, "z7-lab-only-ram",
                              0x01000000, &error_fatal);
        memory_region_add_subregion(sysmem, 0x40000000, &s->lab_ram);
    }
    if (boot_profile) {
        qemu_chr_fe_init(&s->uart, qemu_chr_find("z7uart"), &error_fatal);
        /* 04008f00 initial SP and 04008ffc secondary-core mailbox are literals. */
        memory_region_init_ram(&s->boot_sram, NULL, "z7-boot-sram",
                              0x1000, &error_fatal);
        memory_region_add_subregion(sysmem, 0x04008000, &s->boot_sram);
    }

    if (machine->firmware) {
        g_autofree gchar *data = NULL;
        gsize length = 0;
        g_autoptr(GError) error = NULL;
        if (!g_file_get_contents(machine->firmware, &data, &length, &error)) {
            error_report("Cannot read main payload: %s", error->message);
            exit(1);
        }
        if (length != 33052088) {
            error_report("Expected the extracted Z7 3.80 main payload (33052088 bytes)");
            exit(1);
        }
        rom_add_blob_fixed("z7-main-payload", data, length, 0x10020000);
        /* Initial RAM data is supplied explicitly by the function-test launcher.
         * The boot-probe profile omits those loader devices. */
    }
    /* PC is selected explicitly by QEMU's generic loader, never guessed here. */
}

static void z7_lab_init(MachineState *machine)
{
    z7_common_init(machine, false, false, false, false, false, 0, false);
}

static void z7_boot_init(MachineState *machine)
{
    z7_common_init(machine, true, false, false, false, false, 0, false);
}

static void z7_boot_v1_init(MachineState *machine)
{
    z7_common_init(machine, true, true, false, false, false, 0, false);
}

static void z7_lab_machine_class_init(MachineClass *mc)
{
    mc->desc = "Nikon Z7 function research harness (no EXPEED peripherals)";
    mc->init = z7_lab_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a15");
    mc->max_cpus = 1;
    mc->default_cpus = 1;
    mc->no_serial = true;
    mc->no_parallel = true;
}

DEFINE_MACHINE_ARM("nikon-z7-lab", z7_lab_machine_class_init)

static void z7_boot_machine_class_init(MachineClass *mc)
{
    z7_lab_machine_class_init(mc);
    mc->desc = "Nikon Z7 main-payload boot experiment (incomplete peripherals)";
    mc->init = z7_boot_init;
    mc->max_cpus = 2;
    mc->default_cpus = 2;
}

DEFINE_MACHINE_ARM("nikon-z7-boot", z7_boot_machine_class_init)

static void z7_boot_v1_machine_class_init(MachineClass *mc)
{
    z7_boot_machine_class_init(mc);
    mc->desc = "Nikon Z7 boot experiment with explicit GICv1 hypothesis";
    mc->init = z7_boot_v1_init;
}

DEFINE_MACHINE_ARM("nikon-z7-boot-v1", z7_boot_v1_machine_class_init)

static void z7_platform_init(MachineState *machine)
{
    z7_common_init(machine, true, true, true, false, true, 0, false);
}

static void z7_platform_pattern_init(MachineState *machine)
{
    z7_common_init(machine, true, true, true, true, true, 0, false);
}

static void z7_platform_machine_class_init(MachineClass *mc)
{
    z7_boot_v1_machine_class_init(mc);
    mc->desc = "Nikon Z7 platform experiment (GICv1, synthetic initial state)";
    mc->init = z7_platform_init;
}

DEFINE_MACHINE_ARM("nikon-z7-platform", z7_platform_machine_class_init)

static void z7_platform_pattern_machine_class_init(MachineClass *mc)
{
    z7_platform_machine_class_init(mc);
    mc->desc = "Nikon Z7 platform experiment (GICv1, diagnostic initial pattern)";
    mc->init = z7_platform_pattern_init;
}

DEFINE_MACHINE_ARM("nikon-z7-platform-pattern", z7_platform_pattern_machine_class_init)

static void z7_platform_no_heap_init(MachineState *machine)
{
    z7_common_init(machine, true, true, true, false, false, 0, false);
}

static void z7_platform_no_heap_machine_class_init(MachineClass *mc)
{
    z7_platform_machine_class_init(mc);
    mc->desc = "Nikon Z7 negative control: platform with missing heap RAM";
    mc->init = z7_platform_no_heap_init;
}

DEFINE_MACHINE_ARM("nikon-z7-platform-no-heap", z7_platform_no_heap_machine_class_init)

static void z7_drivers16_init(MachineState *machine)
{
    z7_common_init(machine, true, true, true, false, true, 16, false);
}

static void z7_drivers64_init(MachineState *machine)
{
    z7_common_init(machine, true, true, true, false, true, 64, false);
}

static void z7_drivers16_class_init(MachineClass *mc)
{
    z7_platform_machine_class_init(mc);
    mc->desc = "Nikon Z7 driver experiment (synthetic 16-word serial FIFOs)";
    mc->init = z7_drivers16_init;
}

DEFINE_MACHINE_ARM("nikon-z7-drivers-fifo16", z7_drivers16_class_init)

static void z7_drivers64_class_init(MachineClass *mc)
{
    z7_drivers16_class_init(mc);
    mc->desc = "Nikon Z7 driver experiment (synthetic 64-word serial FIFOs)";
    mc->init = z7_drivers64_init;
}

DEFINE_MACHINE_ARM("nikon-z7-drivers-fifo64", z7_drivers64_class_init)

static void z7_recovery_init(MachineState *machine)
{
    z7_common_init(machine, true, true, true, false, true, 16, true);
}

static void z7_recovery_class_init(MachineClass *mc)
{
    z7_drivers16_class_init(mc);
    mc->desc = "Nikon Z7 recovery experiment with explicitly erased missing flash";
    mc->init = z7_recovery_init;
}

DEFINE_MACHINE_ARM("nikon-z7-recovery", z7_recovery_class_init)

static void z7_display_class_init(MachineClass *mc)
{
    z7_recovery_class_init(mc);
    mc->desc = "Nikon Z7 experimental graphics transactions";
}
DEFINE_MACHINE_ARM("nikon-z7-display", z7_display_class_init)
