/*  XY2_100 library
    Copyright (c) 2018 Lutz Lisseck

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
*/

/*
 * Chip Datasheet: https://www.pjrc.com/teensy/K20P64M72SF1RM.pdf
 * Flex Timer: https://www.nxp.com/docs/en/application-note/AN5142.pdf
 * XY2-100: http://www.newson.be/doc.php?id=XY2-100
 * T_41 DMA: https://forum.pjrc.com/threads/63353-Teensy-4-1-How-to-start-using-DMA?p=266991&viewfull=1#post266991
 * DS100: Datasheet page 100.
 */

#include <string.h>
#include "XY2_100.h"



#if defined(__IMXRT1062__)
#define GPIO_PIN_SHIFT 16
#define DMA_MEM_SIZE 20

typedef uint16_t uint_dma;
typedef uint64_t uint_per_cycle;
#endif

#if defined(__MK20DX256__) || defined(__MKL26Z64__)
#define GPIO_PIN_SHIFT 16
#define DMA_MEM_SIZE 10

typedef uint8_t uint_dma;
typedef uint32_t uint_per_cycle;
#endif

uint16_t XY2_100::lastX;
uint16_t XY2_100::lastY;
void *XY2_100::pingBuffer;
void *XY2_100::pongBuffer;
DMAChannel XY2_100::dma;



// Bit0: 0=Ping buffer content is beeing transmitted
static volatile uint8_t txPing = 0;

// static: file scope variable; DMAMEM: specifies this variable save to dmabuffers
// DMAMEM: #define DMAMEM __attribute__ ((section(".dmabuffers"), used))
static DMAMEM uint32_t pingMemory[DMA_MEM_SIZE];
static DMAMEM uint32_t pongMemory[DMA_MEM_SIZE];

XY2_100::XY2_100() {
    pingBuffer = pingMemory;
    pongBuffer = pongMemory;
    txPing = 0;
}


void XY2_100::begin(void) {
    uint32_t bufsize, frequency;
    bufsize = 40; // simply the size of pingMemory and pongMemory;

    // set up the buffers
    memset(pingBuffer, 0, bufsize);
    memset(pongBuffer, 0, bufsize);

    frequency = 4000000;  // 4MHz

    // DMA channel writes the data
    dma.sourceBuffer((uint_dma *) pingBuffer, bufsize);


    dma.transferSize(1);
    dma.transferCount(bufsize);
    dma.disableOnCompletion();
    dma.interruptAtCompletion();              // will call isr when complete (buffer has read out, bufsize triggers is meet).

#if defined(__IMXRT1062__)                    // Teensy 4.1
    // GPIO1_DR 32bit, here we utilize 16~23bit
    GPIO1_DR_CLEAR = 0xFFFFFFFF;
    GPIO1_DR = 0x00000000;

    IOMUXC_GPR_GPR26 &= ~(0x00FF0000);             // select standard GPIO instead of fast GPIO. 0 for GPIO1, 1 for GPIO6
    GPIO1_GDIR |= 0xFF0000;                        // set 16~23 bit as output. 0 for input, 1 for output.

    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_00 = 5;       // route pin19 pad AD_B1_00 (GPIO1_IO16) to GPIO module.
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_01 = 5;       // route pin18 pad AD_B1_00 (GPIO1_IO17) to GPIO module.
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_02 = 5;       // route pin14 pad AD_B1_00 (GPIO1_IO18) to GPIO module.
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_03 = 5;       // route pin15 pad AD_B1_00 (GPIO1_IO19) to GPIO module.
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_04 = 5;       // route pin40 pad AD_B1_00 (GPIO1_IO20) to GPIO module.
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_05 = 5;       // route pin41 pad AD_B1_00 (GPIO1_IO21) to GPIO module.
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_06 = 5;       // route pin17 pad AD_B1_00 (GPIO1_IO22) to GPIO module.
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B1_07 = 5;       // route pin16 pad AD_B1_00 (GPIO1_IO23) to GPIO module.

    dma.destination(GPIO1_DR);

    CCM_CSCMR1 &= 0xFFFFFFBF;                      // set PERCLK_CLK_SEL to 0: select the 150MHz click.
    PIT_MCR = 0x00;                                // turn on PIT
//    IMXRT_PIT_CHANNELS[0].LDVAL = F_BUS_ACTUAL / frequency;      // Default click frequency is 132MHz
//    IMXRT_PIT_CHANNELS[0].TCTRL |= PIT_TCTRL_TEN;  // Start PIT0

    IMXRT_PIT_CHANNEL_t * pit = IMXRT_PIT_CHANNELS + dma.channel;
    pit->LDVAL = F_BUS_ACTUAL / frequency;
    pit->TCTRL |= PIT_TCTRL_TEN;

    dma.triggerContinuously();
    volatile uint32_t *mux = &DMAMUX_CHCFG0 + dma.channel;
    *mux |= DMAMUX_CHCFG_TRIG;

    dma.enable();
#endif

#if defined(__MK20DX256__) || defined(__MKL26Z64__)
    // configure the 8 output pins
    GPIOD_PCOR = 0xFF;                       // clear Data Output Register (PDOR). 0: not change; 1: to logic 0;
    GPIOD_PDOR = 0x0F;                       // Port data out put. data need to be transferred goes to this register.

    // flowing are GPIOD controller lower 8 bit pin.
    // Writing to GPIOD_PDOR register will result logic level change, 0 for logic low, 1for logic high.
    pinMode(2, OUTPUT);           // bit 0 PTD0
    pinMode(14, OUTPUT);          // bit 1 PTD1
    pinMode(7, OUTPUT);           // bit 2 PTD2
    pinMode(8, OUTPUT);           // bit 3 PTD3
    pinMode(6, OUTPUT);           // bit 4 PTD4
    pinMode(20, OUTPUT);          // bit 5 PTD5
    pinMode(21, OUTPUT);          // bit 6 PTD6
    pinMode(5, OUTPUT);           // bit 7 PTD7

    dma.destination(GPIOD_PDOR);
#endif


    pinMode(9, OUTPUT); // testing: oscilloscope trigger

/* ====================================================================================
 * Timer setup
 * ====================================================================================*/
#if defined(__MK20DX256__)
    // TEENSY 3.1/3.2
    FTM2_SC = 0;
    FTM2_CNT = 0;                                         // Initialize FTM Counter before FTM2_MOD is set

    // frequency manipulation refer to DS823
    uint32_t mod = (F_BUS + frequency / 2) / frequency;   // mod = 9
    // minus 1 because counting starts from FTM2_CNTIN (counter initial value), defaults is 1.
    FTM2_MOD = mod - 1;                                   // 11 @96Mhz, 8 @72MHz; FTM2_MOD = 8

    // FTM_SC_CLKS(1): select System clock. FTM_SC_PS(0): select Prescale, (000) divide by 1
    FTM2_SC = FTM_SC_CLKS(1) | FTM_SC_PS(0);              // increment on every TPM clock, prescaler 1

    // need ISR also, FTM2_C0SC: Channel 0 Status and Control register
    FTM2_C0SC = 0x69;                                     // 0x69: 0110 1001,  CHIE 1, MSB:MSA 10, ELSB:ELSA 10, DMA on. Edge-Aligned PWM, DS783
                                                          // https://skills.microchip.com/digital-power-converter-basics-using-dspic33-digital-signal-controllers/693874

    FTM2_C0V = (mod * 128) >> 8;                          // 256 = 100% of the time

    // route the timer interrupt to trigger the dma channel
    dma.triggerAtHardwareEvent(DMAMUX_SOURCE_FTM2_CH0);
    // enable a done interrupts when channel completes
    dma.attachInterrupt(isr);

    FTM2_C0SC = 0x28;                                     // 0x28: 0010 1000,  CHIE 0, MSB:MSA 10, ELSB:ELSA 10, DMA off
    noInterrupts();
    FTM2_SC = 0;                                          // stop FTM2 timer (hopefully before it rolls over)
    FTM2_CNT = 0;

    //PORTB_ISFR = (1<<18);    // clear any prior rising edge
    uint32_t tmp __attribute__((unused));
    FTM2_C0SC = 0x28;
    tmp = FTM2_C0SC;         // clear any prior timer DMA triggers, CHF bit will be cleared by reading form FTM2_C0SC. DS784
    FTM2_C0SC = 0x69;
    dma.enable();
    FTM2_SC = FTM_SC_CLKS(1) | FTM_SC_PS(0); // restart FTM2 timer

#elif defined(__MKL26Z64__)
    // TEENSY LC
    FTM2_SC = 0;
    FTM2_CNT = 0;
    uint32_t mod = F_CPU / frequency;
    FTM2_MOD = mod - 1;
    FTM2_SC = FTM_SC_CLKS(1) | FTM_SC_PS(0); // increment on every TPM clock, prescaler 1

    // route the timer interrupt to trigger the dma channel
    dma.triggerAtHardwareEvent(DMAMUX_SOURCE_FTM2_OV);
    // enable a done interrupts when channel completes
    dma.attachInterrupt(isr);

    uint32_t sc __attribute__((unused)) = FTM2_SC;
    noInterrupts();
    FTM2_SC = 0;		// stop FTM2 timer (hopefully before it rolls over)
    dma.clearComplete();
    dma.transferCount(bufsize);
    dma.sourceBuffer((uint8_t *)pingBuffer, bufsize);
    // clear any pending event flags
    FTM2_SC = FTM_SC_TOF;
    dma.enable();		// enable DMA channel
    FTM2_CNT = 0; // writing any value resets counter
    FTM2_SC = FTM_SC_DMA | FTM_SC_CLKS(1) | FTM_SC_PS(0);
#endif

    //digitalWriteFast(9, LOW);
    interrupts();
}

void XY2_100::isr(void) {
    //digitalWriteFast(9, LOW);

    dma.clearInterrupt();
    if (txPing & 2) { // x & 10
        txPing &= ~2;

        if (txPing & 1) {
            dma.sourceBuffer((uint_dma *) pongBuffer, 40);
        } else {
            dma.sourceBuffer((uint_dma *) pingBuffer, 40);
        }
    }
    //txPing |= 128;

#if defined(__MK20DX256__)
    FTM2_SC = 0;
    FTM2_SC = FTM_SC_TOF;
    uint32_t tmp __attribute__((unused));
    FTM2_C0SC = 0x28;
    tmp = FTM2_C0SC;         // clear any prior timer DMA triggers
    FTM2_C0SC = 0x69;
    FTM2_CNT = 0;
    dma.enable();		// enable DMA channel
    FTM2_SC = FTM_SC_CLKS(1) | FTM_SC_PS(0); // restart FTM2 timer
#elif defined(__MKL26Z64__)
    FTM2_SC = 0;
    FTM2_SC = FTM_SC_TOF;
    dma.enable();		// enable DMA channel
    FTM2_CNT = 0; // writing any value resets counter
    FTM2_SC = FTM_SC_DMA | FTM_SC_CLKS(1) | FTM_SC_PS(0);
#endif

    //digitalWriteFast(9, HIGH); // oscilloscope trigger
}

uint8_t XY2_100::stat(void) {
    uint8_t ret = txPing;
    txPing &= ~128;
    return ret;
}

void XY2_100::setSignedXY(int16_t X, int16_t Y) {
    // -32768 => 0; 32767 => 65535;
    int32_t xu = (int32_t) X + 32768L, yu = (int32_t) Y + 32768L;
    setXY((uint16_t) xu, (uint16_t) yu);
}

void XY2_100::setXY(uint16_t X, uint16_t Y) {
    uint32_t *p;

    // (000x xxxx xxxx xxxx xxx0 | 0010 0000 0000 0000 0000) & 0011 1111 1111 1111 1110
    // => 001x xxxx xxxx xxxx xxx0 & 0011 1111 1111 1111 1110
    // => 001x xxxx xxxx xxxx xxx0 (channel sequential data, last one is parity check);
    uint32_t Ch1 = (((uint32_t) X << 1) | 0x20000ul) & 0x3fffeul;
    uint32_t Ch2 = (((uint32_t) Y << 1) | 0x20000ul) & 0x3fffeul;

    uint8_t parity1 = 0;
    uint8_t parity2 = 0;

    // 0123456789abcdef
    // fedcba9876543210
    // every 16-bit word generates a clock pulse also
    // 1101 0010 1100 0011, 1001 0110 1000 0111, 0101 1010 0100 1011, 0001 1110 0000 1111
    const uint16_t Sync1[4] = {0xd2c3, 0x9687, 0x5a4b, 0x1e0f};
    // 1111 0000 1110 0001, 1011 0100 1010 0101, 0111 1000 0110 1001, 0011 1100 0010 1101
    const uint16_t Sync0[4] = {0xf0e1, 0xb4a5, 0x7869, 0x3c2d};

    lastX = X;
    lastY = Y;

    for (int i = 0; i < 20; i++) {
        if (Ch1 & (1 << i)) parity1++;
        if (Ch2 & (1 << i)) parity2++;
    }
    // Set parity bit
    if (parity1 & 1) Ch1 |= 1;
    if (parity2 & 1) Ch2 |= 1;

    if (txPing & 1) {
        p = ((uint32_t *) pingBuffer);
    } else {
        p = ((uint32_t *) pongBuffer);
    }

    // Clock cycle: 1-0, Sync 111...0
    /**
     * suppose data is 001a bcde ...
     *   bit 7 CLOCK+ <- |1|0 |1|0 |1|0 |1|0|...|1|0|
     *   bit 6 SYNC+  <- |1|1 |1|1 |1|1 |1|1|...|0|0|
     *   bit 5 CHAN1+ <- |0|0 |0|0 |1|1 |a|a|...|0|0|
     *   bit 4 CHAN2+ <- |0|0 |0|0 |1|1 |a|a|...|1|1|
     *   bit 3 CLOCK- <- |1|0 |1|0 |1|0 |1|0|...|1|0|
     *   bit 2 SYNC-  <- |1|1 |1|1 |1|1 |1|1|...|0|0|
     *   bit 1 CHAN1- <- |0|0 |0|0 |1|1 |a|a|...|0|0|
     *   bit 0 CHAN2- <- |0|0 |0|0 |1|1 |a|a|...|0|0|
     */
    for (int i = 19; i >= 0; i--) {
        int j = 0;
        uint_per_cycle d;
        // Ch1[i] = 1, Ch2[i] = 1, j = 11
        // Ch1[i] = 0, Ch2[i] = 1, j = 10
        // Ch1[i] = 1, Ch2[i] = 0, j = 01
        // Ch1[i] = 0, Ch2[i] = 0, j = 00
        if (Ch1 & (1 << i)) j = 1;
        if (Ch2 & (1 << i)) j |= 2;

        d = Sync1[j] << GPIO_PIN_SHIFT;
        i--;
        j = 0;
        if (Ch1 & (1 << i)) j = 1;
        if (Ch2 & (1 << i)) j |= 2;

        if (i != 0) {
            d |= (uint_per_cycle) (Sync1[j] << GPIO_PIN_SHIFT) << 16; // 1101 0010 | 1100 0011 | 1101 0010 | 1100 0011
        } else {
            d |= (uint_per_cycle) (Sync0[j] << GPIO_PIN_SHIFT) << 16;
        }

        *p++ = d;
    }

    noInterrupts();
    txPing ^= 1;
    txPing |= 2;
    interrupts();
}


