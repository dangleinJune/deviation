/*
 This project is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Deviation is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Deviation.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef MODULAR
  //Allows the linker to properly relocate
  #define E015_Cmds PROTO_Cmds
  #pragma long_calls
#endif
#include "common.h"
#include "interface.h"
#include "mixer.h"
#include "config/model.h"
#include "config/tx.h" // for Transmitter
#include "music.h"
#include "telemetry.h"

#ifdef MODULAR
  //Some versions of gcc applythis to definitions, others to calls
  //So just use long_calls everywhere
  //#pragma long_calls_off
  extern unsigned _data_loadaddr;
  const unsigned long protocol_type = (unsigned long)&_data_loadaddr;
#endif

#ifdef PROTO_HAS_NRF24L01

#include "iface_nrf24l01.h"

#ifdef EMULATOR
    #define USE_FIXED_MFGID
    #define BIND_COUNT 4
    #define dbgprintf printf
#else
    #define BIND_COUNT 150
    //printf inside an interrupt handler is really dangerous
    //this shouldn't be enabled even in debug builds without explicitly
    //turning it on
    #define dbgprintf if(0) printf
#endif

#define PACKET_PERIOD       4500 // stock Tx=9000, but let's send more packets ...
#define INITIAL_WAIT        500
#define RF_CHANNEL          0x2d // 2445 MHz
#define ADDRESS_LENGTH      5
#define PACKET_SIZE         10 // bind packet = 9

static const u8 bind_address[ADDRESS_LENGTH] = {0x62, 0x54, 0x79, 0x38, 0x53};
static u8 tx_addr[ADDRESS_LENGTH];
static u8 packet[PACKET_SIZE];
static u8 phase;
static u16 bind_counter;
static u8 tx_power;
static u8 armed, arm_flags;
static u8 arm_channel_previous;

enum {
    BIND,
    DATA
};

// For code readability
enum {
    CHANNEL1 = 0,
    CHANNEL2,
    CHANNEL3,
    CHANNEL4,
    CHANNEL5,
    CHANNEL6,
    CHANNEL7,
    CHANNEL8,
    CHANNEL9,
    CHANNEL10,
};

#define CHANNEL_ARM      CHANNEL5
#define CHANNEL_LED      CHANNEL6
#define CHANNEL_FLIP     CHANNEL7
#define CHANNEL_HEADLESS CHANNEL9
#define CHANNEL_RTH      CHANNEL10

// flags packet[6]
#define FLAG_DISARM     0x80
#define FLAG_ARM        0x40

// flags packet[7]
#define FLAG_FLIP       0x80
#define FLAG_HEADLESS   0x10
#define FLAG_RTH        0x08
#define FLAG_LED        0x04
#define FLAG_EXPERT     0x02
#define FLAG_INTERMEDIATE 0x01

// Bit vector from bit position
#define BV(bit) (1 << bit)

//
// HS6200 emulation layer
//////////////////////////
static u8 hs6200_crc;
static u16 hs6200_crc_init;
static const u16 crc_poly = 0x1021;
static u8 hs6200_tx_addr[5];
static u8 hs6200_address_length;

static const u8 hs6200_scramble[] = {
    0x80,0xf5,0x3b,0x0d,0x6d,0x2a,0xf9,0xbc,
    0x51,0x8e,0x4c,0xfd,0xc1,0x65,0xd0}; // todo: find all 32 bytes ...

static u16 crc_update(u16 crc, u8 byte, u8 bits)
{
    crc = crc ^ (byte << 8);
    while(bits--)
    if((crc & 0x8000) == 0x8000) 
        crc = (crc << 1) ^ crc_poly;
    else 
        crc = crc << 1;
    return crc;
}

static void HS6200_SetTXAddr(const u8* addr, u8 len)
{
    if(len < 4)
        len = 4;
    else if(len > 5)
        len = 5;
    
    // use nrf24 address field as a longer preamble
    if(addr[len-1] & 0x80)
        NRF24L01_WriteRegisterMulti(NRF24L01_10_TX_ADDR, (u8*)"\x55\x55\x55\x55\x55", 5);
    else
        NRF24L01_WriteRegisterMulti(NRF24L01_10_TX_ADDR, (u8*)"\xaa\xaa\xaa\xaa\xaa", 5);
    
    // precompute address crc
    hs6200_crc_init = 0xffff;
    for(int i=0; i<len; i++)
        hs6200_crc_init = crc_update(hs6200_crc_init, addr[len-1-i], 8);
    memcpy(hs6200_tx_addr, addr, len);
    hs6200_address_length = len;
}

static u16 hs6200_calc_crc(u8* msg, u8 len)
{
    u8 pos;
    u16 crc = hs6200_crc_init;
    
    // pcf + payload
    for(pos=0; pos < len-1; pos++) { 
        crc = crc_update(crc, msg[pos], 8);
    }
    // last byte (1 bit only)
    if(len > 0) {
        crc = crc_update(crc, msg[pos+1], 1);
    }
    
    return crc;
}

static void HS6200_Configure(u8 flags)
{
    hs6200_crc = !!(flags & BV(NRF24L01_00_EN_CRC));
    flags &= ~(BV(NRF24L01_00_EN_CRC) | BV(NRF24L01_00_CRCO));
    NRF24L01_WriteReg(NRF24L01_00_CONFIG, flags & 0xff);      
}

static u8 HS6200_WritePayload(u8* msg, u8 len)
{
    u8 payload[32];
    const u8 no_ack = 1; // never ask for an ack
    static u8 pid;
    u8 pos = 0;
    
    if(len > sizeof(hs6200_scramble))
        len = sizeof(hs6200_scramble);
    
    // address
    for(int i=hs6200_address_length-1; i>=0; i--) {
        payload[pos++] = hs6200_tx_addr[i];
    }
    
    // guard bytes
    payload[pos++] = hs6200_tx_addr[0];
    payload[pos++] = hs6200_tx_addr[0];
    
    // packet control field
    payload[pos++] = ((len & 0x3f) << 2) | (pid & 0x03);
    payload[pos] = (no_ack & 0x01) << 7;
    pid++;
    
    // scrambled payload
    if(len > 0) {
        payload[pos++] |= (msg[0] ^ hs6200_scramble[0]) >> 1; 
        for(u8 i=1; i<len; i++)
            payload[pos++] = ((msg[i-1] ^ hs6200_scramble[i-1]) << 7) | ((msg[i] ^ hs6200_scramble[i]) >> 1);
        payload[pos] = (msg[len-1] ^ hs6200_scramble[len-1]) << 7; 
    }
    
    // crc
    if(hs6200_crc) {
        u16 crc = hs6200_calc_crc(&payload[hs6200_address_length+2], len+2);
        uint8_t hcrc = crc >> 8;
        uint8_t lcrc = crc & 0xff;
        payload[pos++] |= (hcrc >> 1);
        payload[pos++] = (hcrc << 7) | (lcrc >> 1);
        payload[pos++] = lcrc << 7;
    }
    
    return NRF24L01_WritePayload(payload, pos);
}
//
// end of HS6200 emulation layer
/////////////////////////////////

static void e015_init()
{
    NRF24L01_Initialize();
    NRF24L01_SetTxRxMode(TX_EN);
    HS6200_SetTXAddr(bind_address, ADDRESS_LENGTH);
    NRF24L01_FlushTx();
    NRF24L01_FlushRx();
    NRF24L01_WriteReg(NRF24L01_07_STATUS, 0x70);     // Clear data ready, data sent, and retransmit
    NRF24L01_WriteReg(NRF24L01_01_EN_AA, 0x00);      // No Auto Acknowldgement on all data pipes
    NRF24L01_WriteReg(NRF24L01_03_SETUP_AW, 0x03);
    NRF24L01_WriteReg(NRF24L01_04_SETUP_RETR, 0x00); // no retransmits
    NRF24L01_SetBitrate(NRF24L01_BR_1M);             // 1 Mbps
    NRF24L01_SetPower(tx_power);
    NRF24L01_WriteReg(NRF24L01_05_RF_CH, RF_CHANNEL);
    NRF24L01_Activate(0x73);                          // Activate feature register
    NRF24L01_WriteReg(NRF24L01_1C_DYNPD, 0x00);       // Disable dynamic payload length on all pipes
    NRF24L01_WriteReg(NRF24L01_1D_FEATURE, 0x01);     // Set feature bits on
    NRF24L01_Activate(0x73);
    
    // Check for Beken BK2421/BK2423 chip
    // It is done by using Beken specific activate code, 0x53
    // and checking that status register changed appropriately
    // There is no harm to run it on nRF24L01 because following
    // closing activate command changes state back even if it
    // does something on nRF24L01
    NRF24L01_Activate(0x53); // magic for BK2421 bank switch
    dbgprintf("Trying to switch banks\n");
    if (NRF24L01_ReadReg(NRF24L01_07_STATUS) & 0x80) {
        dbgprintf("BK2421 detected\n");
        // Beken registers don't have such nice names, so we just mention
        // them by their numbers
        // It's all magic, eavesdropped from real transfer and not even from the
        // data sheet - it has slightly different values
        NRF24L01_WriteRegisterMulti(0x00, (u8 *) "\x40\x4B\x01\xE2", 4);
        NRF24L01_WriteRegisterMulti(0x01, (u8 *) "\xC0\x4B\x00\x00", 4);
        NRF24L01_WriteRegisterMulti(0x02, (u8 *) "\xD0\xFC\x8C\x02", 4);
        NRF24L01_WriteRegisterMulti(0x03, (u8 *) "\x99\x00\x39\x21", 4);
        NRF24L01_WriteRegisterMulti(0x04, (u8 *) "\xD9\x96\x82\x1B", 4);
        NRF24L01_WriteRegisterMulti(0x05, (u8 *) "\x24\x06\x7F\xA6", 4);
        NRF24L01_WriteRegisterMulti(0x0C, (u8 *) "\x00\x12\x73\x00", 4);
        NRF24L01_WriteRegisterMulti(0x0D, (u8 *) "\x46\xB4\x80\x00", 4);
        NRF24L01_WriteRegisterMulti(0x04, (u8 *) "\xDF\x96\x82\x1B", 4);
        NRF24L01_WriteRegisterMulti(0x04, (u8 *) "\xD9\x96\x82\x1B", 4);
    } else {
        dbgprintf("nRF24L01 detected\n");
    }
    NRF24L01_Activate(0x53); // switch bank back
}

#define CHAN_RANGE (CHAN_MAX_VALUE - CHAN_MIN_VALUE)
static u8 scale_channel(u8 ch, u8 destMin, u8 destMax)
{
    s32 chanval = Channels[ch];
    s32 range = (s32) destMax - (s32) destMin;

    if (chanval < CHAN_MIN_VALUE)
        chanval = CHAN_MIN_VALUE;
    else if (chanval > CHAN_MAX_VALUE)
        chanval = CHAN_MAX_VALUE;
    return (range * (chanval - CHAN_MIN_VALUE)) / CHAN_RANGE + destMin;
}

static void check_arming(s32 channel_value) {
    u8 arm_channel = channel_value > 0;

    if (arm_channel != arm_channel_previous) {
        arm_channel_previous = arm_channel;
        if (arm_channel) {
            armed = 1;
            arm_flags ^= FLAG_ARM;
        } else {
            armed = 0;
            arm_flags ^= FLAG_DISARM;
        }
    }
}

#define GET_FLAG(ch, mask) (Channels[ch] > 0 ? mask : 0)
static void send_packet(u8 bind)
{
    if(bind) {
        packet[0] = 0x18;
        packet[1] = 0x04;
        packet[2] = 0x06;
        // data phase address
        packet[3] = tx_addr[0];
        packet[4] = tx_addr[1];
        packet[5] = tx_addr[2];
        packet[6] = tx_addr[3];
        packet[7] = tx_addr[4];
        // checksum
        packet[8] = packet[3];
        for(u8 i=4; i<8; i++)
            packet[8] += packet[i];
    }
    else {
        check_arming(Channels[CHANNEL_ARM]);
        packet[0] = scale_channel(CHANNEL3, 0, 225); // throttle
        packet[1] = scale_channel(CHANNEL4, 225, 0); // rudder
        packet[2] = scale_channel(CHANNEL1, 0, 225); // aileron
        packet[3] = scale_channel(CHANNEL2, 225, 0); // elevator
        packet[4] = 0x20; // elevator trim
        packet[5] = 0x20; // aileron trim
        packet[6] = arm_flags;
        packet[7] = FLAG_EXPERT
                  | GET_FLAG(CHANNEL_FLIP, FLAG_FLIP)
                  | GET_FLAG(CHANNEL_LED, FLAG_LED)
                  | GET_FLAG(CHANNEL_HEADLESS, FLAG_HEADLESS)
                  | GET_FLAG(CHANNEL_RTH, FLAG_RTH);
        packet[8] = 0;
        // checksum
        packet[9] = packet[0];
        for(u8 i=1; i<9; i++)
            packet[9] += packet[i];
    }
    
    // Power on, TX mode, CRC enabled
    HS6200_Configure(BV(NRF24L01_00_EN_CRC) | BV(NRF24L01_00_CRCO) | BV(NRF24L01_00_PWR_UP));
    
    NRF24L01_WriteReg(NRF24L01_07_STATUS, 0x70);
    NRF24L01_FlushTx();
   
    // transmit packet twice in a row without waiting for
    // the first one to complete, seems to help the hs6200
    // demodulator to start decoding.
    HS6200_WritePayload(packet, bind ? 9 : PACKET_SIZE);
    HS6200_WritePayload(packet, bind ? 9 : PACKET_SIZE);
    
    // Check and adjust transmission power. We do this after
    // transmission to not bother with timeout after power
    // settings change -  we have plenty of time until next
    // packet.
    if (tx_power != Model.tx_power) {
        tx_power = Model.tx_power;
        NRF24L01_SetPower(tx_power);
    }
}

MODULE_CALLTYPE
static u16 e015_callback()
{
    switch (phase) {
        case BIND:
            if (bind_counter == 0) {
                HS6200_SetTXAddr(tx_addr, 5);
                phase = DATA;
                PROTOCOL_SetBindState(0);
            } else {
                send_packet(1);
                bind_counter--;
            }
            break;
        case DATA:
            send_packet(0);
            break;
    }
    return PACKET_PERIOD;
}

static void initialize_txid()
{
    u32 lfsr = 0xb2c54a2ful;
    u8 i,j;
    
#ifndef USE_FIXED_MFGID
    u8 var[12];
    MCU_SerialNumber(var, 12);
    dbgprintf("Manufacturer id: ");
    for (i = 0; i < 12; ++i) {
        dbgprintf("%02X", var[i]);
        rand32_r(&lfsr, var[i]);
    }
    dbgprintf("\r\n");
#endif

    if (Model.fixed_id) {
       for (i = 0, j = 0; i < sizeof(Model.fixed_id); ++i, j += 8)
           rand32_r(&lfsr, (Model.fixed_id >> j) & 0xff);
    }
    // Pump zero bytes for LFSR to diverge more
    for (i = 0; i < sizeof(lfsr); ++i) rand32_r(&lfsr, 0);

    // tx address
    for(i=0; i<4; i++)
        tx_addr[i] = (lfsr >> (i*8)) & 0xff;
    rand32_r(&lfsr, 0);
    tx_addr[4] = lfsr & 0xff;
}

static void initialize()
{
    CLOCK_StopTimer();
    tx_power = Model.tx_power;
    initialize_txid();
    e015_init();
    bind_counter = BIND_COUNT;
    phase = BIND;
    PROTOCOL_SetBindState(BIND_COUNT * PACKET_PERIOD / 1000);
    armed = 0;
    arm_flags = 0;
    arm_channel_previous = Channels[CHANNEL_ARM] > 0;
    CLOCK_StartTimer(INITIAL_WAIT, e015_callback);
}

const void *E015_Cmds(enum ProtoCmds cmd)
{
    switch(cmd) {
        case PROTOCMD_INIT:  initialize(); return 0;
        case PROTOCMD_DEINIT:
        case PROTOCMD_RESET:
            CLOCK_StopTimer();
            return (void *)(NRF24L01_Reset() ? 1L : -1L);
        case PROTOCMD_CHECK_AUTOBIND: return (void *)1L; // always Autobind
        case PROTOCMD_BIND:  initialize(); return 0;
        case PROTOCMD_NUMCHAN: return (void *) 10L;
        case PROTOCMD_DEFAULT_NUMCHAN: return (void *)10L;
        case PROTOCMD_CURRENT_ID: return Model.fixed_id ? (void *)((unsigned long)Model.fixed_id) : 0;
        case PROTOCMD_GETOPTIONS: return (void *)0L;
        case PROTOCMD_TELEMETRYSTATE: return (void *)(long)PROTO_TELEM_UNSUPPORTED;
        default: break;
    }
    return 0;
}

#endif
