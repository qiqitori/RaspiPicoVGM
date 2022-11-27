#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "config.h"
#include "pico/multicore.h"
#include "ff.h"

//Macros + Typedefs
#define BIT(x) (1 << x)
#define HEADER_U32(x) *(uint32_t*)(&header_data[x])
typedef uint8_t byte;

#include "databus.hpp"
#include "opl3.hpp"
#include "saa1099.hpp"

/*----------------------*/

#pragma region Communication Stuff

DataBus bus;
Opl3Chip opl3(&bus);
Saa1099Chip saa1099(&bus);

#pragma endregion

FRESULT fr;
FATFS fs;
FIL vgmFile;
char filename[] = "song.vgm";

static inline byte ReadByte() {
    uint bytesRead = 0;
    byte buf = 0;
    FRESULT r = f_read(&vgmFile, &buf, 1, &bytesRead);

    if (r != FR_OK || bytesRead != 1) {
        printf("Failed to read a single byte from the file. Hanging.\n");
        for (;;) {
            tight_loop_contents();
        }
    }

    return buf;
}

static inline uint16_t ReadU16() {
    uint bytesRead = 0;
    uint16_t buf = 0;
    FRESULT r = f_read(&vgmFile, &buf, 2, &bytesRead);

    if (r != FR_OK || bytesRead != 2) {
        printf("Failed to read two bytes from the file. Hanging.\n");
        for (;;) {
            tight_loop_contents();
        }
    }

    return buf;
}

static inline uint32_t ReadU32() {
    uint bytesRead = 0;
    uint32_t buf = 0;
    FRESULT r = f_read(&vgmFile, &buf, 4, &bytesRead);

    if (r != FR_OK || bytesRead != 4) {
        printf("Failed to read four bytes from the file. Hanging.\n");
        for (;;) {
            tight_loop_contents();
        }
    }

    return buf;
}

static inline uint32_t ReadIntoBuffer(void* buf, uint32_t bytesToRead) {
    uint bytesRead = 0;
    FRESULT r = f_read(&vgmFile, buf, bytesToRead, &bytesRead);

    if (r != FR_OK) {
        printf("Failed to read %u bytes from the file. Hanging.\n", bytesToRead);
        for (;;) {
            tight_loop_contents();
        }
    }

    return bytesRead;
}

static inline void SkipBytes(int bytesToSkip) {
    FRESULT r = f_lseek(&vgmFile, f_tell(&vgmFile) + bytesToSkip);

    if (r != FR_OK) {
        printf("Failed to skip %u bytes in the file. Hanging.\n", bytesToSkip);
        for (;;) {
            tight_loop_contents();
        }
    }
}

static inline void SeekFile(uint32_t fOffset) {
    FRESULT r = f_lseek(&vgmFile, fOffset);

    if (r != FR_OK) {
        printf("Failed to seek to offset 0x%X (%u) in the file. Hanging.\n", fOffset, fOffset);
        for (;;) {
            tight_loop_contents();
        }
    }
}

uint16_t delayCycles = 0;
uint32_t startOffset = 0;
uint32_t music_length = 0;
uint32_t loopOffset = 0;
static void SongTick(uint gpio, uint32_t event_mask) {
    gpio_xor_mask(MASK_LED_BUILTIN);

    if (delayCycles > 0) {
        delayCycles--;
        return;
    }

    //parse a byte out of the VGM file
    //Parse VGM commands
    //Full command listing and VGM file spec: https://vgmrips.net/wiki/VGM_Specification
    byte curByte = ReadByte();
    switch (curByte) {
        case 0x4F: { //Game Gear PSG stereo register write
            SkipBytes(1);
            break;
        }

        //TODO: SN76489 stuff because of the tandy 3 voice sound
        case 0x50: { //SN76489/SN76496 write
            SkipBytes(1);
            break;
        }

        case 0x61: { //Wait x cycles
            //reading cycle count
            //VGM format is little endian and so is ARM
            delayCycles = ReadU16();
            
            break;
        }

        case 0x62: { //Wait 735 samples
            delayCycles = 735;
            break;
        }

        case 0x63: { //Wait 882 samples
            delayCycles = 882;
            break;
        }

        //Short Delays
        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
        case 0x74:
        case 0x75:
        case 0x76:
        case 0x77:
        case 0x78:
        case 0x79:
        case 0x7A:
        case 0x7B:
        case 0x7C:
        case 0x7D:
        case 0x7E:
        case 0x7F: {
            delayCycles = curByte & 0xf;
            break;
        }

        case 0x66: { //Loop
            printf("%X: Got command 0x66. Looping back to offset 0x%X.\n", f_tell(&vgmFile), loopOffset);
            SeekFile(loopOffset);
            break;
        }

        //YMF262 stuff
        case 0x5A: //YM3812 (OPL2)
        case 0x5B: //YM3526 (OPL1)
        case 0x5E: { //YMF262 port 0
            byte regi = ReadByte();
            byte data = ReadByte();

            //send register
            opl3.write(0, regi);
            busy_wait_us_32(FM_WRITE_PULSE_US);
            
            //send data
            opl3.write(1, data);
            busy_wait_us_32(FM_WRITE_PULSE_US);

            break;
        }

        case 0x5F: { //YMF262 port 1
            byte regi = ReadByte();
            byte data = ReadByte();

            //send register
            opl3.write(0b10, regi);
            busy_wait_us_32(FM_WRITE_PULSE_US);
            
            //send data
            opl3.write(0b11, data);
            busy_wait_us_32(FM_WRITE_PULSE_US);

            break;
        }

        //SAA1099 stuff
        case 0xBD: { //SAA1099 write
            byte regi = ReadByte();
            byte data = ReadByte();
            bool chip = false;

            //figuring out which chip to write to
            //bit 7: low = chip 1, high = chip 2
            if (regi & BIT(7)) {
                chip = true;
            } else {
                chip = false;
            }

            //send register
            saa1099.write(chip, true, regi);
            busy_wait_us_32(SAA_WRITE_PULSE_US);
            
            //send data
            saa1099.write(chip, false, data);
            busy_wait_us_32(SAA_WRITE_PULSE_US);

            break;
        }

        //Chips that won't be handled
        //Commands with 2 byte long parameters
        case 0x51: //YM2413
        case 0x54: //YM2151
        case 0x55: //YM2203
        case 0x56: //YM2608 port 0
        case 0x57: //YM2608 port 1
        case 0x58: //YM2610 port 0
        case 0x59: //YM2610 port 1
        case 0x5C: //Y8950
        case 0x5D: //YMZ280B
        case 0x52: //YM2612 port 0
        case 0x53: { //YM2612 port 1
            SkipBytes(1);
            //TODO: log to debug console?
            break;
        }

        //PCM stuff
        case 0x67: { //data block
            break;
        }
        case 0xE0: { //seek to offset in PCM data bank
            //uint32_t seekTo = ReadU32();
            //pcmOffset = seekTo;
            SkipBytes(3);

            break;
        }

        //YM2612 port 0 address 2A write from data bank, then wait n samples
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8A:
        case 0x8B:
        case 0x8C:
        case 0x8D:
        case 0x8E:
        case 0x8F: {
            //send register
            /*SendFMByte(0, 0x2A);
            busy_wait_us_32(FM_WRITE_PULSE_US);
            
            //send data
            //SendFMByte(1, pcm_buffer[pcmOffset]);
            SendFMByte(1, PCM_RAM_Read(pcmOffset));
            busy_wait_us_32(FM_WRITE_PULSE_US);

            pcmOffset++;
            delayCycles = curByte & 0xf;*/
            break;
        }

        //not necessary anymore, only was needed when the code was buggier and shittier
        /*case 0xFF: {
            printf("%X: Encountered 0xFF. Something has gone very wrong! Hanging.\n", f_tell(&vgmFile));
            for (;;) {
                tight_loop_contents();
            }
        }*/

        default: {
            printf("%X: Encountered unknown command %X. Ignoring.\n", f_tell(&vgmFile), curByte);
            break;
        }
    }

    if (f_tell(&vgmFile) == music_length) {
        f_lseek(&vgmFile, startOffset);
        printf("Reached end of song. Looping.\n");
    }
}

void core1_thing() {
    //init song tick
    gpio_set_irq_enabled_with_callback(0, GPIO_IRQ_EDGE_RISE, true, SongTick);

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
    gpio_set_function(PIN_SAA_CLK, GPIO_FUNC_PWM);
    
    //init gpio directions
    gpio_set_dir_out_masked(MASK_DATABUS | MASK_LED_BUILTIN | MASK_FM_CTRL | MASK_SAA_CTRL);

    //init default gpio states
    gpio_put(PIN_FM_CS, GPIO_ON); //basically all of the control lines are active low
    gpio_put(PIN_FM_WR, GPIO_ON);
    gpio_put(PIN_FM_IC, GPIO_ON);
    gpio_put(PIN_SAA_CS1, GPIO_ON);
    gpio_put(PIN_SAA_CS2, GPIO_ON);
    gpio_put(PIN_SAA_WR, GPIO_ON);
    gpio_put(PIN_SAA_A0, GPIO_OFF);
    bus.set(0);
    opl3.setAddr(0);

    //init clocks
    //changing system clock for better accuracy
    if (!set_sys_clock_khz(142800, false)) {
        float curSysClk = (float)(clock_get_hz(clk_sys));

        printf("WARNING: Failed to set clock to 142.8 MHz! Songs will be noticeably out of tune!\n");
        printf("Current sys clock is %f MHz.", curSysClk / 1000000.0F);
    }

    //TODO: accomodate for other system clock speeds if other boards have issues
    opl3.clockInit();
    saa1099.clockInit();

    //tick
    uint slice_num_tick = pwm_gpio_to_slice_num(1);
    pwm_set_wrap(slice_num_tick, 512); //divide by 512 prescaler, at 142.8 MHz this is 278,906.25 Hz
    pwm_set_chan_level(slice_num_tick, PWM_CHAN_B, 256);
    pwm_set_clkdiv(slice_num_tick, 6.3244F); //freq = (prescaled_sys_clk / 2) / divider
    pwm_set_enabled(slice_num_tick, true);

    //chip init
    opl3.chipInit();
    saa1099.chipInit();

    //waiting for host USB serial
    while (!stdio_usb_connected()) {
        tight_loop_contents();
    }
    printf("\n------------------------\n");
    printf("Host serial connected.\n");
    printf("Mounting SD card...\n");

    //Initialize SD card
    if (!sd_init_driver()) {
        printf("ERROR: Could not initialize SD card\n");
        for (;;) {
            tight_loop_contents();
        }
    } else {
        printf("Success!\n");
    }

    //Mount drive
    fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK) {
        printf("ERROR: Could not mount filesystem (%d)\n", fr);
        for (;;) {
            tight_loop_contents();
        }
    }

    //Open file for reading
    fr = f_open(&vgmFile, filename, FA_READ);
    if (fr != FR_OK) {
        printf("ERROR: Could not open file (%d)\n", fr);
        for (;;) {
            tight_loop_contents();
        }
    }

    //reading header into RAM buffer
    printf("Reading 0xFF bytes from file and parsing header.\n");
    byte header_data[0xff];
    uint bytesRead = 0;
    f_rewind(&vgmFile);
    fr = f_read(&vgmFile, header_data, 0xff, &bytesRead);
    if (fr != FR_OK) {
        printf("Error reading header from VGM file on SD card. Hanging.\n");
        for (;;) {
            tight_loop_contents();
        }
    }
    printf("Read %X (%u) bytes.\n", bytesRead, bytesRead);

    //parsing some info
    //uint32_t psgClk = *(uint32_t*)(&header_data[0xC]);
    uint32_t sampleCount = HEADER_U32(0x18);
    float songSeconds = (1.0F / 44100.0F) * sampleCount;
    uint32_t opl3Clk = HEADER_U32(0x5C);
    uint32_t opl2Clk = HEADER_U32(0x50);
    uint32_t fileVersion = HEADER_U32(0x08); //this is a BCD number
    music_length = HEADER_U32(0x4) + 4; //this is a relative offset instead of an absolute one because fuck you
    loopOffset = HEADER_U32(0x1c) + 0x1c; //ditto
    
    //saa1099 is only supported on 1.71 and newer
    uint32_t saaClk = 0;
    if (fileVersion >= 0x0171) {
        uint32_t saaClk = HEADER_U32(0xc8);
    }

    //determining file start
    if (fileVersion < 0x0150) {
        startOffset = 0x40;
    } else {
        startOffset = HEADER_U32(0x34) + 0x34;
    }

    //checking for dual chips - OPL3
    if (opl3Clk & BIT(30)) {
        printf("FATAL: Song expects dual YMF262! This is not supported!\n");
        printf("Hanging.\n");
        for (;;) {
            tight_loop_contents();
        }
    } else {
        //clearing the bit to not confuse any following code
        opl3Clk &= ~(BIT(30));
    }

    //checking for dual chips - OPL2
    if (opl2Clk & BIT(30)) {
        //TODO: there were some stereo soundblasters with dual OPL2s, and I think one OPL3 can mimic this
        printf("FATAL: Song expects dual YM3812! This is not supported!\n");
        printf("Hanging.\n");
        for (;;) {
            tight_loop_contents();
        }
    } else {
        //clearing the bit to not confuse any following code
        opl2Clk &= ~(BIT(30));
    }

    //TODO: check OPL1 clock and dual chip bit

    //checking for dual chips - SAA1099
    if (saaClk & BIT(30)) {
        printf("Song expects dual SAA1099, using both SAA1099 chips.\n");
    }
    saaClk &= ~(BIT(30));
    
    //printing some info
    printf("File version: 0x%04X\n", fileVersion);
    //printf("Expected SN76489 clock = %u Hz.\n", psgClk);
    printf("Expected YMF262 clock = %u Hz.\n", opl3Clk);
    printf("Expected YM3812 clock = %u Hz.\n", opl2Clk);
    printf("Expected SAA1099 clock = %u Hz.\n", saaClk);
    printf("Length: %f seconds (%u samples)\n", songSeconds, sampleCount);
    printf("Song data begins at offset 0x%x.\n", startOffset);
    printf("Loop offset is at 0x%x.\n", loopOffset);

    //seeking to beginning offset
    f_lseek(&vgmFile, startOffset);

    //do song tick things on core 1
    multicore_launch_core1(core1_thing);

    //now core 0 is free to run a user interface
    //TODO
    for (;;) {
        //main loop
        tight_loop_contents();
    }
}