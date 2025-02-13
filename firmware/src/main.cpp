#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "config.h"
#include "pico/multicore.h"

//Macros + Typedefs
#define BIT(x) (1 << x)
typedef uint8_t byte;

#include "databus.hpp"
#include "opm.hpp"
#include "vgmparser.hpp"

/*----------------------*/

#pragma region Communication Stuff

DataBus bus;
OpmChip opm(&bus);
VgmParser vgm;

#pragma endregion

//can't directly pass a class method into the IRQ function, just gives me "invalid use of non-static member function"
void tick_irq_callback(uint gpio, uint32_t event_mask) {
    vgm.tick();
}

void core1_thing() {
    //init song tick
    gpio_set_irq_enabled_with_callback(0, GPIO_IRQ_EDGE_RISE, true, tick_irq_callback);

    for (;;) {
        tight_loop_contents();
    }
}

int main() {
    stdio_init_all();

    //init gpio pins
    gpio_init_mask(MASK_DATABUS | MASK_LED_BUILTIN | MASK_FM_CTRL | MASK_SAA_CTRL);
    gpio_set_function(PIN_FM_CLK, GPIO_FUNC_PWM);
    gpio_set_function(1, GPIO_FUNC_PWM);
    
    //init gpio directions
    gpio_set_dir_out_masked(MASK_DATABUS | MASK_LED_BUILTIN | MASK_FM_CTRL | MASK_SAA_CTRL);

    //init default gpio states
    gpio_put(PIN_FM_CS, GPIO_ON); //basically all of the control lines are active low
    gpio_put(PIN_FM_WR, GPIO_ON);
    gpio_put(PIN_FM_IC, GPIO_OFF);
    bus.set(0);
    opm.setAddr(0);

    //init clocks
    //changing system clock for better accuracy
    if (!set_sys_clock_khz(142800, false)) { // should be achievable according to vcocalc.py
        float curSysClk = (float)(clock_get_hz(clk_sys));

        printf("WARNING: Failed to set clock to 142.8 MHz! Songs will be noticeably out of tune!\n");
        printf("Current sys clock is %f MHz.", curSysClk / 1000000.0F);
    }

    opm.clockInit();

    //tick
    uint slice_num_tick = pwm_gpio_to_slice_num(1);
    pwm_set_wrap(slice_num_tick, 512); //divide by 512 prescaler, at 142.8 MHz this is 278,906.25 Hz
    pwm_set_chan_level(slice_num_tick, PWM_CHAN_B, 256);
    pwm_set_clkdiv(slice_num_tick, 6.3244F); //freq = (prescaled_sys_clk / 2) / divider // not achievable i think as there are only four bits after the decimal point
    pwm_set_enabled(slice_num_tick, true);

    //chip init
    opm.chipInit();

    //reading header into RAM buffer
    vgm.readHeader();
    //checking for dual chips - OPL3
    if (vgm.isDual(VgmHeaderChip::YMF262)) {
        printf("FATAL: Song expects dual YMF262! This is not supported!\n");
        printf("Hanging.\n");
        for (;;) {
            tight_loop_contents();
        }
    }

    //checking for dual chips - OPL2
    if (vgm.isDual(VgmHeaderChip::YM3812)) {
        //TODO: there were some stereo soundblasters with dual OPL2s, and I think one OPL3 can mimic this
        printf("FATAL: Song expects dual YM3812! This is not supported!\n");
        printf("Hanging.\n");
        for (;;) {
            tight_loop_contents();
        }
    }

    //TODO: check OPL1 clock and dual chip bit

    //checking for dual chips - SAA1099
    if (vgm.isDual(VgmHeaderChip::SAA1099)) {
        printf("Song expects dual SAA1099, using both SAA1099 chips.\n");
    }
    //checking for dual chips - YM2151
    if (vgm.isDual(VgmHeaderChip::YM2151)) {
        printf("FATAL: Song expects dual YM2151! This is not supported!\n");
        printf("Hanging.\n");
        for (;;) {
            tight_loop_contents();
        }
    }
    //printing some info
    printf("File version: 0x%04X\n", vgm.getVersion());
    printf("Length: %f seconds (%u samples)\n", vgm.getLengthSeconds(), vgm.getLengthSamples());
    printf("Song data begins at offset 0x%x.\n", vgm.getStartOffset());
    printf("Loop offset is at 0x%x.\n", vgm.getLoopOffset());

    //seeking to beginning offset
    vgm.curr_ofs = vgm.getStartOffset();

    //do song tick things on core 1
    multicore_launch_core1(core1_thing);

    //now core 0 is free to run a user interface
    //TODO
    for (;;) {
        //main loop
        tight_loop_contents();
    }
}
