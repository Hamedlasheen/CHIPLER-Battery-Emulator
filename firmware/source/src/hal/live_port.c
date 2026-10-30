/* Own IRQ glue: SDK pico_i2c_slave FINISH conflates STOP/restart/abort.
 * FIRST_DATA_BYTE, RX_DONE, STOP_DET and RD_REQ are kept distinct here.
 * Unknown/overread/boundary faults isolate the translator and disable the peripheral;
 * no fabricated FF response or valid PEC is ever supplied on rejection.
 * OWNER PHONE-TEST build differs: unknown reads return released-bus 0xFF and unusable writes are dropped
 * without isolating (smbus_target.c, r5), and the input spike filter is widened (r6, below).
 * Electrical NACK/isolation timing still needs owner hardware evidence.
 */
#include "live_port.h"
#include "../batt/live_wire.h"
#include "board_config.h"
#include "bus_en.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/regs/i2c.h"
/* Register primitives used by the IRQ. On target they are plain MMIO accesses (always_inline, so the
 * generated code is the same as direct member access). The host MMIO harness
 * (tests/host/test_live_port_mmio.c) defines LIVE_HW_ACCESS_OVERRIDE and supplies a model of the
 * DW_apb_i2c FIFO/flag semantics, so the PRODUCTION live_irq() below is what gets tested. */
#ifndef LIVE_HW_ACCESS_OVERRIDE
#define LIVE_HW static inline __attribute__((always_inline))
LIVE_HW uint32_t hw_stat(i2c_hw_t *h){return h->intr_stat;}
LIVE_HW uint32_t hw_rx_avail(i2c_hw_t *h){return h->rxflr;}
LIVE_HW uint32_t hw_rx_pop(i2c_hw_t *h){return h->data_cmd;}
LIVE_HW void hw_tx_push(i2c_hw_t *h,uint8_t b){h->data_cmd=b;}
LIVE_HW void hw_clr_rd_req(i2c_hw_t *h){(void)h->clr_rd_req;}
LIVE_HW void hw_clr_rx_done(i2c_hw_t *h){(void)h->clr_rx_done;}
LIVE_HW void hw_clr_stop(i2c_hw_t *h){(void)h->clr_stop_det;}
LIVE_HW void hw_clr_start(i2c_hw_t *h){(void)h->clr_start_det;}
LIVE_HW void hw_clr_restart(i2c_hw_t *h){(void)h->clr_restart_det;}
LIVE_HW uint32_t hw_abort_source(i2c_hw_t *h){return h->tx_abrt_source;}
#endif
static live_wire_t wire;
static bool installed,latched;
static uint32_t last_irq,last_abort;   /* diagnostics (ported from beta2): raw status of the last IRQ / abort source */
/* Last 8 IRQ batches, oldest overwritten: [15:0] raw IC_INTR_STAT, [20:16] RX bytes drained, [21] reading,
 * [22] active (engine state BEFORE the batch), [31:24] tx_off. Diagnostics only; frozen once the port isolates. */
static uint32_t hist[8];static uint8_t hpos;
/* IRQ-reachable helpers live in SRAM: no XIP fetch while the display core streams from flash. */
uint32_t __not_in_flash_func(live_port_guard_enter)(void *p){(void)p;return save_and_disable_interrupts();}
void __not_in_flash_func(live_port_guard_exit)(void *p,uint32_t s){(void)p;restore_interrupts(s);}
static bool pins_released;
/* IRQ-safe: only touches I2C registers. Pad/GPIO release happens in live_port_service (main loop),
 * because the SDK gpio helpers are not guaranteed to be in SRAM. With the peripheral disabled the
 * open-drain outputs are released and the external pull-ups hold the line. */
static void __not_in_flash_func(isolate)(void) {
    bus_en_put(false);latched=true;
    i2c_hw_t *hw=i2c_get_hw(BAT_I2C_INSTANCE);hw->intr_mask=0;hw->enable=0;
}
static void __not_in_flash_func(live_irq)(void) {
    i2c_hw_t *h=i2c_get_hw(BAT_I2C_INSTANCE);uint32_t s=hw_stat(h);
    live_event_t e;st_zero(&e,sizeof e);live_reply_t reply;
    last_irq=s;last_abort=(s&I2C_IC_INTR_STAT_R_TX_ABRT_BITS)?hw_abort_source(h):0;
    e.error=(s&(I2C_IC_INTR_STAT_R_TX_ABRT_BITS|I2C_IC_INTR_STAT_R_RX_OVER_BITS|I2C_IC_INTR_STAT_R_RX_UNDER_BITS))!=0;
    e.request=(s&I2C_IC_INTR_STAT_R_RD_REQ_BITS)!=0;
    e.nack=(s&I2C_IC_INTR_STAT_R_RX_DONE_BITS)!=0;
    e.stop=(s&I2C_IC_INTR_STAT_R_STOP_DET_BITS)!=0;
    while(hw_rx_avail(h)&&e.count<16)e.rx[e.count++]=(uint16_t)hw_rx_pop(h);
    if(hw_rx_avail(h))e.error=true;
    {   const st_target_t *tg=wire.target;
        hist[hpos++&7u]=(s&0xFFFFu)|((uint32_t)e.count<<16)|((uint32_t)(tg&&tg->reading)<<21)|
                        ((uint32_t)(tg&&tg->active)<<22)|((uint32_t)(tg?tg->tx_off:0)<<24);
    }
    /* Drain RX before STOP so a final write byte/PEC is included in validation. */
    if(!live_wire_event(&wire,&e,time_us_32(),&reply)){isolate();return;}
    if(reply.have_tx)hw_tx_push(h,reply.tx);
    if(e.request)hw_clr_rd_req(h);
    if(e.nack)hw_clr_rx_done(h);
    if(e.stop)hw_clr_stop(h);
    if(s&I2C_IC_INTR_STAT_R_START_DET_BITS)hw_clr_start(h);
    if(s&I2C_IC_INTR_STAT_R_RESTART_DET_BITS)hw_clr_restart(h);
}
bool live_port_start(st_target_t *t) {
    if(installed||latched||!st_live_ready(t))return false;
    pins_released=false;
    bus_en_put(false);
    gpio_set_function(PIN_BAT_SDA,GPIO_FUNC_I2C);gpio_set_function(PIN_BAT_SCL,GPIO_FUNC_I2C);
    i2c_init(BAT_I2C_INSTANCE,400000);live_wire_init(&wire,t,time_us_32());last_irq=last_abort=0;
    unsigned irq=I2C0_IRQ+i2c_hw_index(BAT_I2C_INSTANCE);irq_set_enabled(irq,false);
    i2c_set_slave_mode(BAT_I2C_INSTANCE,true,t->addr7);
    i2c_hw_t *h=i2c_get_hw(BAT_I2C_INSTANCE);
    h->enable=0;
    h->con|=I2C_IC_CON_STOP_DET_IFADDRESSED_BITS;
#if CHIPLER_OWNER_PHONE_TEST
    /* OWNER r6: wider input spike filter. The SDK sets IC_FS_SPKLEN = lcnt/16 for 400 kHz (~14 clk_sys cycles,
     * ~93 ns at 150 MHz). Bench capture EMU_BOOT12_BOTH_SIDES_12M_500M_last showed bit slips on both sides while
     * the phone draws peak current; 2.5x filters glitches up to ~230 ns. The phone clock is ~375 kHz with
     * ~1.0 us high / ~1.6 us low, so the filter stays well inside a valid SCL phase. Must be written while disabled. */
    {   uint32_t spk=h->fs_spklen*5u/2u;
        if(spk<2u)spk=2u;
        if(spk>255u)spk=255u;
        h->fs_spklen=spk;
    }
#endif
    h->rx_tl=0;h->tx_tl=0;(void)h->clr_intr;
    h->intr_mask=I2C_IC_INTR_MASK_M_RX_FULL_BITS|I2C_IC_INTR_MASK_M_RD_REQ_BITS|
        I2C_IC_INTR_MASK_M_RX_DONE_BITS|I2C_IC_INTR_MASK_M_STOP_DET_BITS|
        I2C_IC_INTR_MASK_M_START_DET_BITS|I2C_IC_INTR_MASK_M_RESTART_DET_BITS|
        I2C_IC_INTR_MASK_M_TX_ABRT_BITS|I2C_IC_INTR_MASK_M_RX_OVER_BITS|I2C_IC_INTR_MASK_M_RX_UNDER_BITS;
    irq_set_exclusive_handler(irq,live_irq);irq_set_priority(irq,PICO_HIGHEST_IRQ_PRIORITY);
    installed=true;h->enable=1;irq_set_enabled(irq,true);
    /* An IRQ may run before enabling the translator. Never override its fault. */
    uint32_t g=save_and_disable_interrupts();
    bool ok=!latched&&!wire.blocked;if(ok)bus_en_put(true);
    restore_interrupts(g);return ok;
}
void live_port_stop(void) {
    bus_en_put(false);
    if(installed){
        unsigned irq=I2C0_IRQ+i2c_hw_index(BAT_I2C_INSTANCE);irq_set_enabled(irq,false);
        i2c_get_hw(BAT_I2C_INSTANCE)->intr_mask=0;i2c_get_hw(BAT_I2C_INSTANCE)->enable=0;
        irq_remove_handler(irq,live_irq);i2c_deinit(BAT_I2C_INSTANCE);installed=false;pins_released=false;
        if(wire.target)st_abort(wire.target);
    }
}
void live_port_service(uint32_t now) {
    /* main-loop half of isolation: hand GP6/GP7 back as hi-Z inputs without pulls (no EN line exists in P1) */
    if(installed&&latched&&!pins_released){bus_pins_release();pins_released=true;}
    if(!installed||latched)return;
    uint32_t g=save_and_disable_interrupts();
    if(live_wire_expire(&wire,now,100000))isolate();
    restore_interrupts(g);
}
bool live_port_fault(void){return latched;}
void live_port_diagnostic(st_result_t *r,uint32_t *irq,uint32_t *abort_source){*r=wire.fault;*irq=last_irq;*abort_source=last_abort;}
void live_port_clear_fault(void){if(!installed)latched=false;}
uint32_t live_port_last_event_us(void){return *(volatile uint32_t *)&wire.last_us;}   /* one aligned 32-bit load */
void live_port_history(uint32_t out[8],uint8_t *site){
    for(unsigned k=0;k<8;k++)out[k]=hist[(uint8_t)(hpos+k)&7u];   /* oldest first */
    if(site)*site=wire.site;
}
