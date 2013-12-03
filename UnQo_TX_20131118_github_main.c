//******************************************************************************
//  MSP430G2xx3 Demo - Timer_A, Ultra-Low Pwr UART 9600 Echo, 32kHz ACLK
//
//  Description: Use Timer_A CCR0 hardware output modes and SCCI data latch
//  to implement UART function @ 9600 baud. Software does not directly read and
//  write to RX and TX pins, instead proper use of output modes and SCCI data
//  latch are demonstrated. Use of these hardware features eliminates ISR
//  latency effects as hardware insures that output and input bit latching and
//  timing are perfectly synchronised with Timer_A regardless of other
//  software activity. In the Mainloop the UART function readies the UART to
//  receive one character and waits in LPM3 with all activity interrupt driven.
//  After a character has been received, the UART receive function forces exit
//  from LPM3 in the Mainloop which configures the port pins (P1 & P2) based
//  on the value of the received byte (i.e., if BIT0 is set, turn on P1.0).

//  ACLK = TACLK = LFXT1 = 32768Hz, MCLK = SMCLK = default DCO
//  //* An external watch crystal is required on XIN XOUT for ACLK *//
//
//               MSP430G2xx3
//            -----------------
//        /|\|              XIN|-
//         | |                 | 32kHz
//         --|RST          XOUT|-
//           |                 |
//           |   CCI0B/TXD/P1.1|-------->
//           |                 |             4800 8N1 or 9600 8N1
//           |   CCI0A/RXD/P1.2|<--------
//           |                 |
//           |             P1.3|<--------    mode input (SW2)
//           |             P2.0|<--------    Cadence pulse input (not used)
//           |             P2.2|<--------    Torque pulse (interrupt) input
//           |                 |
//
//
//  D. Dang
//  Texas Instruments Inc.
//  December 2010
//   Built with CCS Version 4.2.0 and IAR Embedded Workbench Version: 5.10
//******************************************************************************







//******************************************************************************
//P1.1TXD
//P1.2RXD
//P1.3=mode change switch
//P2.0=cadence capture
//P2.2=torque capture
//
//you need ANT NETWORK_ID at ANTAP1_AssignNetwork()
//visit this is transient com and make free acount!
//goloveski
//******************************************************************************


//#include "msp430g2452.h"
#include <stdint.h>
#include "msp430g2553.h"

//------------------------------------------------------------------------------
// Hardware-related definitions
//------------------------------------------------------------------------------
#define UART_TXD   0x02                     // TXD on P1.1 (Timer0_A.OUT0)
#define UART_RXD   0x04                     // RXD on P1.2 (Timer0_A.CCI1A)

//------------------------------------------------------------------------------
// Conditions for 9600 Baud SW UART, SMCLK = 1MHz
//------------------------------------------------------------------------------
//#define UART_TBIT_DIV_2     (1000000 / (9600 * 2))
//#define UART_TBIT           (1000000 / 9600)

#define UART_TBIT_DIV_2     (1000000 / (4800 * 2))
#define UART_TBIT           (1000000 / 4800)

//------------------------------------------------------------------------------
// Global variables used for full-duplex UART communication
//------------------------------------------------------------------------------
unsigned int txData;                        // UART internal variable for TX
unsigned char rxBuffer;                     // Received UART character
const char string1[] = { "Hello World\r\n" };
unsigned int i;

//------------------------------------------------------------------------------
// Function prototypes
//------------------------------------------------------------------------------
void TimerA_UART_init(void);
void TimerA_UART_tx(unsigned char byte);

void Timer1_A_period_CAL_init(void);
void Timer1_A_period_init(void);

#define msecConv(x) ((x)/32)

//------------------------------------------------------------------------------
// ANT Data
//------------------------------------------------------------------------------
#define MESG_NETWORK_KEY_ID      0x46
#define MESG_NETWORK_KEY_SIZE       9
#define ANT_CH_ID    0x00
#define ANT_CH_TYPE  0x10     //Master (0x10)
#define ANT_NET_ID   0x00
#define ANT_DEV_ID1  0x31     //49
#define ANT_DEV_ID2  0x00
#define ANT_DEV_TYPE 0x0B     // Device Type, HRM=0x78, Power=0x0B(11)
#define ANT_TX_TYPE  0x05     //ANT+ devices follow the transmission type definition as outlined in the ANT protocol.
#define ANT_CH_FREQ  0x0039   //   2457MHz
#define ANT_CH_PER   0x1FF6   //0x1FF6 8182/32768=4.004888780Hz // 0x1FA6 8102/32768=4.044Hz  8192/32768=4Hz

//Pedal Power define
#define Power_data   0x012C   // 012C=300W

//Standard Crank Torque data define
#define crank_period   0x0580   // 0x555 : 2048 / 1365 * 60  90rpm  max 0x10000 <
#define crank_torque   0x0420   // 1/32Nm 0x10000 <

//Crank Torque Frequency data define
#define ctf_time_stamp   0x0580   //
#define ctf_torque_ticks 0x0420   //

//#define kPeriod      0x2465 // 32768/37268*4=4Hz -> capture torque tickets
//#define kPeriod      0x1FF6   //0x1FF6 8182/32768=4.004888780Hz
//#define kPeriod      0xFFFF // 32768/37268*4=4Hz -> capture torque tickets
#define kPeriod      0x04FD   //0x1FF6 8182/32768=4.004888780Hz

typedef uint8_t uchar;
uchar txBuffer[256];
uint8_t txBufferSize;
uint8_t txBufferPos;

unsigned int new_timer=0;
unsigned int old_timer=0;
unsigned int timer_diff=0;
unsigned int old_transmit_timer=0;            // Last transmit time

unsigned int index=0;
unsigned int count = 0;

unsigned int PulseCount=0;
unsigned int TorqueTicket=0;
unsigned int TorqueTicket_carry=0;

unsigned int chatter_count13 = 0;

uint16_t ctf_time_stamp1;
uint16_t ctf_torque_ticks1;
uint16_t ctf_torque_ticks2;
uint8_t Rotation_event_counter;

enum{
    CTMMODE = 1,
    OFFSETMODE
};

//int unqomode = CTMMODE;         // for mode changer 1=CTM mode 2=OFFSET mode
int unqomode = OFFSETMODE;         // for mode changer 1=CTM mode 2=OFFSET mode

#define TORQUE_TICKET_MASK_TIME 2
#define CADENCE_THRESHOLD_PULSE 13
#define CHATTER_THRESHOLD 1


//------------------------------------------------------------------------------
//  TX: sync+data+sum+CR+LF
//------------------------------------------------------------------------------

void txMessage(uchar* message,uint8_t messageSize)
{
      uint8_t i;

    _BIC_SR(GIE);  // disable interrupt

    txBufferPos  = 0;                                      // set position to 0
    txBufferSize = messageSize + 3;                        // message plus syc, size and checksum
    txBuffer[0]  = 0xa4;                                   // sync byte
    txBuffer[1]  = (uchar) messageSize - 1;                // message size - command size (1)

    for(i=0; i<messageSize; i++)
        txBuffer[2+i] = message[i];

     // calculate the checksum
    txBuffer[txBufferSize - 1] = 0;                        //add

    for(i=0; i<txBufferSize - 1; ++i)
        txBuffer[txBufferSize - 1] = txBuffer[txBufferSize - 1] ^ txBuffer[i];

    _BIS_SR(GIE);                                          // enable interrupt

    // now send via UART
    for(i=0; i<txBufferSize; i++)
        TimerA_UART_tx(txBuffer[i]);
}

//------------------------------------------------------------------------------
//  ANT Data
//------------------------------------------------------------------------------

// Resets module
void reset()
{
    uchar setup[2];

    setup[0] = 0x4a;                           // ID Byte
    setup[1] = 0x00;                           // Data Byte N (N=LENGTH)
    txMessage(setup, sizeof(setup));
}


void ANTAP1_AssignNetwork()
{
    uchar setup[10];

    setup[0] = MESG_NETWORK_KEY_ID;
    setup[1] = ANT_CH_ID;                      // chan
    setup[2] = 0x00;                           //NETWORK_KEY_ID
    setup[3] = 0x00;
    setup[4] = 0x00;
    setup[5] = 0x00;
    setup[6] = 0x00;
    setup[7] = 0x00;
    setup[8] = 0x00;
    setup[9] = 0x00;
    txMessage(setup, sizeof(setup));
}


// Assigns CH=0, CH Type=10(TX), Net#=0
void assignch()
{
    uchar setup[4];

    setup[0] = 0x42;
    setup[1] = ANT_CH_ID;                      // Channel ID
    setup[2] = ANT_CH_TYPE;                    // CH Type
    setup[3] = ANT_NET_ID;                     // Network ID
    txMessage(setup, sizeof(setup));
}

// set RF frequency
void setrf()
{
    uchar setup[3];

    setup[0] = 0x45;
    //setup[1] = (ANT_CH_FREQ & 0xFF00) >> 8;
    //setup[2] = (ANT_CH_FREQ & 0xFF);         // RF Frequency
    setup[1] = ANT_CH_ID;                      // Channel ID
    setup[2] = 0x39;                           // RF Frequency
    txMessage(setup, sizeof(setup));
}

// set channel period
void setchperiod()
{
    uchar setup[4];

    setup[0] = 0x43;
    setup[1] = ANT_CH_ID;
    setup[2] = (ANT_CH_PER & 0xFF);            // Channel Period LSB
    setup[3] = ((ANT_CH_PER & 0xFF00) >> 8);   // Channel Period MSB
    txMessage(setup, sizeof(setup));
}

// Assigns CH#, Device#=0000, Device Type ID=00, Trans Type=00
void setchid()
{
    uchar setup[6];

    setup[0] = 0x51;
    setup[1] = ANT_CH_ID;                      // Channel Number, 0x00 for HRM
    setup[2] = ANT_DEV_ID1;                    // Device Number LSB
    setup[3] = ANT_DEV_ID2;                    // Device Number MSB
    setup[4] = ANT_DEV_TYPE;                   // Device Type, 0x78 for HRM
    setup[5] = ANT_TX_TYPE;
    txMessage(setup, sizeof(setup));
}


void setInfoData()
{
    uchar setup[7];

    setup[0] = 0x51;
    setup[1] = 0xFF;                    // Channel Number, 0x00 for HRM
    setup[2] = 0xFF;                    // Device Number LSB
    setup[3] = 1234;                    // Device Number MSB
    setup[4] = ANT_DEV_TYPE;            // Device Type, 0x78 for HRM
    setup[5] = ANT_TX_TYPE;
    setup[6] = 12;
    setup[7] = 34;
    txMessage(setup, sizeof(setup));
}

/////////////////////////////////////////////////////////////////////////
// Priority:
//
// ucPower_:   0 = TX Power -20dBM
//             1 = TX Power -10dBM
//             2 = TX Power -5dBM
//             3 = TX Power 0dBM
//
/////////////////////////////////////////////////////////////////////////
/*
void ChannelPower()
{
        uchar setup[3];
        setup[0] = 0x47;
        setup[1] = 0x00;
        setup[2] = 0x03;


        txMessage(setup, 3);
}
*/

// Opens CH 0
void opench()
{
    uchar setup[2];

    setup[0] = 0x4b;
    setup[1] = ANT_CH_ID;
    txMessage(setup, sizeof(setup));
}


// Sends sendPower_n
void sendPower_n(uchar num)
{
    uchar setup[10];

    setup[0] = 0x4e;                                    //broadcast data
    setup[1] = ANT_CH_ID;                               //
    setup[2] = 0x10;                                    // 0x10 Data Page Number
    setup[3] = num;                                     //Event Count max256
    setup[4] = 0xB8;                                    //Pedal Power 0xFF > pedal power not used
    setup[5] = 0x5F;                                    //Instantaneous Cadence 0x5A=90
    setup[6] =  (0xFF & (Power_data * num ));           //Accumulated Power LSB
    setup[7] = ((0xFF00 & (Power_data * num)) >>8);     //Accumulated Power MSB
    setup[8] = (0xFF & Power_data);                     //Instantaneous Power LSB
    setup[9] = ((0xFF00 & Power_data) >>8);             //Instantaneous Power MSB
    txMessage(setup, sizeof(setup));
}

// Sends sendPower_SCT
void sendPower_SCT(uchar num)
{
    uchar setup[10];

    setup[0] = 0x4e;                                    //broadcast data
    setup[1] = ANT_CH_ID;                               //0x41
    setup[2] = 0x12;                                    //0x12 Data Page Number Standard Crank Torque
    setup[3] = num;                                     //Event Count max 256
    setup[4] = num;                                     //Crank Revolutions
    setup[5] = 0xFF;                                    //Crank cadence  if available 0x5A=90 Otherwise: 0xFF
    setup[6] =  (0xFF & (crank_period * num ));         //Accumulated crank period LSB
    setup[7] = ((0xFF00 & (crank_period * num)) >>8);   //Accumulated crank period MSB
    setup[8] = (0xFF & crank_torque* num);              //Accumulated torque LSB
    setup[9] = ((0xFF00 & crank_torque* num) >>8);      //Accumulated torque MSB
    txMessage(setup, sizeof(setup));
}


// Sends sendPower_CTF1
void sendPower_CTF1()
{
    uchar setup[10];

    setup[0] = 0x4e;                                    //broadcast data
    setup[1] = ANT_CH_ID;                               //0x41
    setup[2] = 0x20;                                    //0x20 Data Page Number Crank Torque Frequency
    setup[3] = Rotation_event_counter;                  //Rotation event counter increments with each completed pedal revolution.
    setup[4] = 0x01;                                    //Slope MSB 1/10 Nm/Hz
    setup[5] = 0xF4;                                    //Slope LSB 1/10 Nm/Hz
    setup[6] = ((0xFF00 & (ctf_time_stamp1)) >>8);      //Accumulated Time Stamp MSB 1/2000s
    setup[7] =  (0x00FF & (ctf_time_stamp1));           //Accumulated Time Stamp LSB 1/2000s
    setup[8] = ((0xFF00 & ctf_torque_ticks1) >>8);      //Accumulated Torque Ticks Stamp MSB
    setup[9] =  (0x00FF & ctf_torque_ticks1);           //Accumulated Torque Ticks Stamp LSB
    txMessage(setup, sizeof(setup));

    if(Rotation_event_counter >= 0xFF)
        Rotation_event_counter = 0x00;

}

// Sends sendPower_CTF1_Calibration
void sendPower_CTF1_CAL()
{
    uchar setup[10];

    setup[0] = 0x4e;                                    //broadcast data
    setup[1] = ANT_CH_ID;                               //0x41;     //
    setup[2] = 0x01;                                    //Data page Number : Calibration massage
    setup[3] = 0x10;                                    //Calibration ID : CTF defined massage
    setup[4] = 0x01;                                    //CTF Defined ID : Zero offset
    setup[5] = 0xFF;                                    //Reserved :
    setup[6] = 0xFF;                                    //Reserved :
    setup[7] = 0xFF;                                    //Reserved :
    setup[8] = ((0xFF00 & ctf_torque_ticks2) >>8);      //Offset MSB
    setup[9] = ( 0x00FF & ctf_torque_ticks2);           //Offset LSB
    txMessage(setup, sizeof(setup));
}


int calc_time_diff(int end_t,int start_t)
{
	/*TAIFG�t���O���g���Ă������񂾂��ǁc*/

    if(end_t >= start_t)
        return((unsigned int)(end_t - start_t));
    else	/* if timer return to 0 */
        return((unsigned int)((0xffff - start_t) + 1 + end_t));
}


//------------------------------------------------------------------------------
// main()
//------------------------------------------------------------------------------
void main(void)
{
    WDTCTL = WDTPW + WDTHOLD;               // Stop watchdog timer

    DCOCTL = 0x00;                          // Set DCOCLK to 1MHz
    BCSCTL1 = CALBC1_1MHZ;
    DCOCTL = CALDCO_1MHZ;

    BCSCTL1 |= DIVA_3;                      // ACLK/8   32768/8=4096 0X1000

    // P1.X  setup
    P1OUT = 0x00;                           // Initialize all GPIO
    P1OUT |= BIT3;                          // P1.3 SW is VDD pulled up
    P1SEL = 0x00;
    P1DIR = 0xFF;

    P1SEL = UART_TXD + UART_RXD;            // Timer function for TXD/RXD pins
    P1DIR = 0xFF & ~UART_RXD;               // Set all pins but RXD to output

// P1.6 LED setup
    P1DIR |=  BIT6;

// 1.0 LED setup
    P1DIR |=  BIT0;

    __enable_interrupt();
    TimerA_UART_init();                     // Start Timer_A UART

// ANT chip configuration
    P1OUT |= BIT0;                         //LED ON P1.0

    reset();
    P1OUT |= BIT0;
    __delay_cycles(5000);                  // Delay between comm cycles
    P1OUT &= ~BIT0;
    //__delay_cycles(5000);

    ANTAP1_AssignNetwork();
    P1OUT |= BIT0;
    __delay_cycles(5000);                  // Delay between comm cycles
    P1OUT &= ~BIT0;
    //__delay_cycles(5000);

    assignch();
    P1OUT |= BIT0;
    __delay_cycles(5000);                  // Delay between comm cycles
    P1OUT &= ~BIT0;
    //__delay_cycles(5000);

    setrf();
    P1OUT |= BIT0;
    __delay_cycles(5000);                  // Delay between comm cycles
    P1OUT &= ~BIT0;
    //__delay_cycles(5000);

    setchperiod();
    P1OUT |= BIT0;
    __delay_cycles(5000);                  // Delay between comm cycles
    P1OUT &= ~BIT0;
    //__delay_cycles(5000);

//  setInfoData();
    setchid();
    P1OUT |= BIT0;
    __delay_cycles(5000);                  // Delay between comm cycles
    P1OUT &= ~BIT0;
    //__delay_cycles(5000);

//    ChannelPower();
//    P1OUT |= BIT0;
//     __delay_cycles(5000);               // Delay between comm cycles
//     P1OUT &= ~BIT0;
//     __delay_cycles(5000);

    opench();
    P1OUT |= BIT0;
    __delay_cycles(5000);         // Delay between comm cycles
    P1OUT &= ~BIT0;
    //__delay_cycles(5000);

    __delay_cycles(250000);       // Delay between comm cycles
    P1OUT &= ~BIT0;               //LED OFF P1.0
//    __delay_cycles(250000);     // Delay between comm cycles

    P1DIR &= ~BIT3;               // config P1-3 port set to input
    P1OUT |= BIT3;                // set to Output
    P1IES |= BIT3;                // interrupt edge to H -> L edge


// 2.2 pin for pulse ticket capture
    P1OUT |=  BIT0;               //LED ON P1.0
    P2DIR &= ~BIT2;               // INPUT mode
    P2REN |= BIT2;                // pull up enable
    P2OUT |= BIT2;                // pull VDD
    P2IES |= BIT2;                // H -> L edge

//    __delay_cycles(250000);     // Delay between comm cycles
    P1OUT &= ~BIT0;               //LED OFF P1.0

// P2.0 set to TimerA_A3.CCI0A : CADENCE capture
    P2REN |=  BIT0;               // pull up enable
    P2OUT |=  BIT0;               // pull VDD

    P2DIR &= ~BIT0;               // P2.0 set to TimerA_A3.CCI0A
    P2SEL |= BIT0;                // P2.0 Select ACLK function for pin

    __delay_cycles(500000);       // Delay between comm cycles

    Timer1_A_period_init();

    // interrupt enable
    //P1.3
    P1IE  |= BIT3;                // enable interrupt
    P1REN |= BIT3;                // interrupt Resistor Enable

    //P2.2 
    P2IE  |= BIT2;                // enable interrupt

    P1OUT &= ~BIT0;               //LED OFF P1.0

    for (;;)
    {
        __low_power_mode_3();
    }

}

//------------------------------------------------------------------------------
// PORT1 mode change capture P1.3
//------------------------------------------------------------------------------

#pragma vector=PORT1_VECTOR
__interrupt void Port_1(void)
{
    if(0 == (P1IFG & BIT3))                              // interrupt flag check
        return;                                          // not BIT3 when return

    P1IFG &= ~BIT3;   // clear flag

    /*need chattering timer?*/
    if(unqomode == OFFSETMODE)
    {
        P1OUT &= ~BIT0;                                  // LED_OFF
        Timer1_A_period_init();
        unqomode = CTMMODE;
    }
    else
    {
        P1OUT ^= BIT0;                                   // LED_ON
        Timer1_A_period_CAL_init();
        unqomode = OFFSETMODE;
    }
}


#pragma vector=PORT2_VECTOR
__interrupt void Port_2(void)
{
    if(0 == (P2IFG & BIT2))                              // interrupt flag check
        return;                                          // not BIT2 when return

    P2IFG &= ~BIT2;                                      // clear flag

    new_timer = TA1CCR0;                                 // TIMER_A0->TIMER1_A0, TACCR0->TA1CCR0

    timer_diff = calc_time_diff(new_timer,old_timer);
    old_timer = new_timer;

    if(timer_diff > msecConv(TORQUE_TICKET_MASK_TIME))   //gap larger than "TORQUE_TICKET_MASK_TIME"msec
    {
        /*�ׂ����p���X���ō\������Ă���g���N�`�P�b�g�̐擪�����J�E���g*/
        TorqueTicket++;
        PulseCount = 1;                                  //Counter Clear
    }
    else
    {
        if(PulseCount >= 0xFFFE)
            PulseCount = 0xFFFF;                         //Overflow
        else
            PulseCount++;                                //Counter
    }

    if(PulseCount >= CADENCE_THRESHOLD_PULSE)
    {
        /*�ׂ����p���X����CADENCE_THRESHOLD_PULSE�{�ȏ゠��΁A�P�C�f���X*/
        P1OUT ^= BIT6;                                   // LED_ON

        Rotation_event_counter++;

        ctf_torque_ticks1 = TorqueTicket;
        TorqueTicket = 0;                                // added for reset

        ctf_time_stamp1 = (uint16_t)((calc_time_diff(new_timer,old_transmit_timer)) / 2);

        old_transmit_timer = new_timer;

        sendPower_CTF1();
    }
}


//------------------------------------------------------------------------------
// Timer1_A timer interrupt 4Hz each 250msec
//------------------------------------------------------------------------------
#pragma vector=TIMER1_A0_VECTOR
__interrupt void TIMER1_A0(void)
{
    if(0 == (TACTL & TAIFG))                              // interrupt flag check
        return;

    TACTL &= ~TAIFG;

    if(unqomode == OFFSETMODE)
    {
      if(chatter_count13 == 0)
      {
          chatter_count13 = 1;
          P1OUT ^= BIT6;                                   // LED_ON
      }
      else
      {
          chatter_count13 = 0;
          P1OUT &= ~BIT6;                                   // LED_OFF
      }

      ctf_torque_ticks2 = TorqueTicket;
      sendPower_CTF1_CAL();

//        /*for test*/
//        Rotation_event_counter++;
//
//        ctf_torque_ticks1 = 1234;
//        ctf_time_stamp1 = (uint16_t)(500 / 2);
//
//        sendPower_CTF1();

    }
}


//------------------------------------------------------------------------------
// Function configures Timer1_A for CTM period capture
//------------------------------------------------------------------------------
void Timer1_A_period_init(void)
{
//    TA1CCTL0 = CM_1 + SCS + CCIS_0 + CAP + CCIE;  // Rising edge + Timer1_A3.CCI0A (P2.0)
//                                                  // + Capture Mode + Interrupt
    TA1CCTL0 = 0;
    TA1CCTL1 = 0;
    TA1CCTL2 = 0;
    TA1CTL = TASSEL_1 + MC_2;                       // ACLK, Continus up mode
}


 //------------------------------------------------------------------------------
 // Function configures Timer1_A for cal period capture
 //------------------------------------------------------------------------------
void Timer1_A_period_CAL_init(void)
{
//    TA1CCTL0 = CM_1 + SCS + CCIS_0 + CAP + CCIE;  // Rising edge + Timer1_A3.CCI0A (P2.0)
                                                    // + Capture Mode + Interrupt

    TA1CCR0 = kPeriod;                              // set interrupt cycle
    TA1CCTL0 = CCIE + OUTMOD_3;                     // enable interrupt + PWM toggle/reset
    TA1CCTL1 = 0;
    TA1CCTL2 = 0;

    TA1CTL = TASSEL_1 + MC_1;                       // ACLK, UP to CCR0

//  TA1CCTL0 = SCS + CCIS_0 + CAP + CCIE;  //Timer1_A3.CCI0A (P2.0)
                                                    // + Capture Mode + Interrupt

}

//------------------------------------------------------------------------------
// Function configures Timer_A for full-duplex UART operation
//------------------------------------------------------------------------------
void TimerA_UART_init(void)
{
    TACCTL0 = OUT;                          // Set TXD Idle as Mark = '1'
    TACCTL1 = SCS + CM1 + CAP + CCIE;       // Sync, Neg Edge, Capture, Int
    TACTL = TASSEL_2 + MC_2;                // SMCLK, start in continuous mode
}

//------------------------------------------------------------------------------
// Outputs one byte using the Timer_A UART
//------------------------------------------------------------------------------
void TimerA_UART_tx(unsigned char byte)
{
    while (TACCTL0 & CCIE);                 // Ensure last char got TX'd
    TACCR0 = TAR;                           // Current state of TA counter
    TACCR0 += UART_TBIT;                    // One bit time till first bit
    TACCTL0 = OUTMOD0 + CCIE;               // Set TXD on EQU0, Int
    txData = byte;                          // Load global variable
    txData |= 0x100;                        // Add mark stop bit to TXData
    txData <<= 1;                           // Add space start bit
}

//------------------------------------------------------------------------------
// Timer_A UART - Transmit Interrupt Handler
//------------------------------------------------------------------------------
#pragma vector = TIMER0_A0_VECTOR
__interrupt void Timer_A0_ISR(void)
{
    static unsigned char txBitCnt = 10;

    TACCR0 += UART_TBIT;                    // Add Offset to CCRx
    if (txBitCnt == 0) {                    // All bits TXed
        TACCTL0 &= ~CCIE;                   // All bits TXed, disable interrupt
        txBitCnt = 10;                      // Re-load bit counter
    }
    else {
        if (txData & 0x01) {
          TACCTL0 &= ~OUTMOD2;              // TX Mark '1'
        }
        else {
          TACCTL0 |= OUTMOD2;               // TX Space '0'
        }
        txData >>= 1;
        txBitCnt--;
    }
}

//------------------------------------------------------------------------------
// Timer_A UART - Receive Interrupt Handler
//------------------------------------------------------------------------------
#pragma vector = TIMER0_A1_VECTOR
__interrupt void Timer_A1_ISR(void)
{
    static unsigned char rxBitCnt = 8;
    static unsigned char rxData = 0;

    switch (__even_in_range(TA0IV, TA0IV_TAIFG)) { // Use calculated branching
        case TA0IV_TACCR1:                         // TACCR1 CCIFG - UART RX
            TACCR1 += UART_TBIT;                   // Add Offset to CCRx
            if (TACCTL1 & CAP) {                   // Capture mode = start bit edge
                TACCTL1 &= ~CAP;                   // Switch capture to compare mode
                TACCR1 += UART_TBIT_DIV_2;         // Point CCRx to middle of D0
            }
            else {
                rxData >>= 1;
                if (TACCTL1 & SCCI) {              // Get bit waiting in receive latch
                    rxData |= 0x80;
                }
                rxBitCnt--;
                if (rxBitCnt == 0) {               // All bits RXed
                    rxBuffer = rxData;             // Store in global variable
                    rxBitCnt = 8;                  // Re-load bit counter
                    TACCTL1 |= CAP;                // Switch compare to capture mode
                    __bic_SR_register_on_exit(LPM0_bits);  // Clear LPM0 bits from 0(SR)
                }
            }
            break;
    }
}
