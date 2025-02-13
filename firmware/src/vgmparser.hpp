#include "vgm_data.h"
#include <string.h>

#define HEADER_U32(x) *(uint32_t*)(header_data+x)

//struct to unpack dual chip flag and clock from header
typedef struct {
    bool t6w28 : 1;      //bit 31
    bool dual : 1;       //bit 30
    uint32_t clock : 29; //bits 0-29
} DualChipClk;

//Enum with all the chips we care about and their offsets in the header
//only defining chips that are used or might be used
enum class VgmHeaderChip : byte {
    SN76489 = 0xc,
    YM2612 = 0x2c, //OPN2
    YM3812 = 0x50, //OPL2
    YM3526 = 0x54, //OPL1
    YMF262 = 0x5c, //OPL3
    SAA1099 = 0xc8,
    YM2151 = 0x30  //OPM
};

class VgmParser {
    private:
        OpmChip* opm;

        uint16_t delayCycles = 0;
        uint32_t startOffset = 0;
        uint32_t music_length = 0;
        uint32_t loopOffset = 0;
        uint32_t headerSize = 0;
        byte header_data[256];

        DualChipClk* getHeaderClock(byte offset) {
            printf("header_data %08x\n", header_data);
            printf("offset %08x\n", offset);
            if (offset > headerSize) return 0;
            else return (DualChipClk*)(header_data+offset);
        }

    public:
        VgmParser() {
        }

        uint32_t curr_ofs = 0;

        void tick();

        void readHeader() {
            printf("Reading 0xFF bytes from file and parsing header.\n");
            curr_ofs = 0;
            memcpy(header_data, vgm_data, 256);

            music_length = HEADER_U32(0x4) + 4; //this is a relative offset instead of an absolute one because fuck you
            loopOffset = HEADER_U32(0x1c) + 0x1c; //ditto
            //determining file start & header size
            if (getVersion() < 0x0150) {
                startOffset = 0x40;
                headerSize = 0x40;
            } else {
                startOffset = HEADER_U32(0x34) + 0x34;
                headerSize = startOffset; //TODO: double check if this is the case
            }
        }

        //VGM file's version, as a BCD coded number
        uint32_t getVersion() {
            return HEADER_U32(0x08);
        }

        //Chip's expected clock speed from file header, in Hz
        uint32_t getChipClock(VgmHeaderChip chip) {
            DualChipClk* headerClk = getHeaderClock((byte)chip);
            return headerClk->clock;
        }

        //Dual chip bit from file header
        bool isDual(VgmHeaderChip chip) {
            DualChipClk* headerClk = getHeaderClock((byte)chip);
            
            if (chip == VgmHeaderChip::SAA1099) {
                //SAA1099 has some special conditions
                if (!(getVersion() >= 0x0171)) { //if file is older than 1.71
                    return false;
                }
            }
            
            return headerClk->dual;
        }

        //Song's length, in samples
        uint32_t getLengthSamples() {
            return HEADER_U32(0x18);
        }

        //Song's length, in seconds
        float getLengthSeconds() {
            return (1.0F / 44100.0F) * getLengthSamples();
        }

        //File offset where song actually starts
        uint32_t getStartOffset() {
            return startOffset;
        }

        //File offset where song loops back to
        uint32_t getLoopOffset() {
            return loopOffset;
        }

        //Check if chip is present
        bool isPresent(VgmHeaderChip chip) {
            if (chip == VgmHeaderChip::SAA1099) {
                //SAA1099 has some special conditions
                if (!(getVersion() >= 0x0171)) { //if file is older than 1.71
                    return false;
                }
            }
            
            return getChipClock(chip) > 0;
        }
};

void VgmParser::tick() {
    gpio_xor_mask(MASK_LED_BUILTIN);

    if (delayCycles > 0) {
        delayCycles--;
        return;
    }
    //Parse VGM commands
    //Full command listing and VGM file spec: https://vgmrips.net/wiki/VGM_Specification
    byte curByte = vgm_data[curr_ofs++];
    switch (curByte) {
        case 0x4F: { //Game Gear PSG stereo register write
            curr_ofs++;
            break;
        }

        //TODO: SN76489 stuff because of the tandy 3 voice sound
        case 0x50: { //SN76489/SN76496 write
            curr_ofs++;
            break;
        }

        case 0x61: { //Wait x cycles
            delayCycles = vgm_data[curr_ofs++];
            delayCycles |= (vgm_data[curr_ofs++] << 8);
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
            if (loopOffset == 0x1c) {
                printf("%X: Got command 0x66. Song isn't looped, restarting song from beginning.\n");
                curr_ofs = startOffset;
            } else {
                printf("%X: Got command 0x66. Looping back to offset 0x%X.\n", curr_ofs, loopOffset);
                curr_ofs = loopOffset;
            }
            break;
        }

        //OPM
        case 0x54: { //YM2151
            byte regi = vgm_data[curr_ofs++];
            byte data = vgm_data[curr_ofs++];

            //register
            opm->write(0, regi);
            busy_wait_us_32(OPM_WRITE_PULSE_US);

            //data
            opm->write(1, data);
            busy_wait_us_32(OPM_WRITE_PULSE_US);

            break;
        }

        //Chips that won't be handled
        //Commands with 2 byte long parameters
        case 0x51: //YM2413
        case 0x55: //YM2203
        case 0x56: //YM2608 port 0
        case 0x57: //YM2608 port 1
        case 0x58: //YM2610 port 0
        case 0x59: //YM2610 port 1
        case 0x5C: //Y8950
        case 0x5D: //YMZ280B
        case 0x52: //YM2612 port 0
        case 0x53: { //YM2612 port 1
            curr_ofs++;
            break;
        }

        //PCM stuff
        case 0x67: { //data block
            //TODO: skip these
            break;
        }
        case 0xE0: { //seek to offset in PCM data bank
            curr_ofs += 3;
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
            break;
        }

        default: {
            printf("%X: Encountered unknown command %X. Ignoring.\n", curr_ofs, curByte);
            break;
        }
    }

    if (curr_ofs == music_length) {
        curr_ofs = startOffset;
        printf("Reached end of song. Looping.\n");
    }

}
