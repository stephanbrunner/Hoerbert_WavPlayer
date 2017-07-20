/*----------------------------------------------------------------------------/
/  20-pin SD Sound Generator R0.01a                                           /
/-----------------------------------------------------------------------------/
/ This project, program codes and circuit diagrams, is opened under license
/ policy of following trems.
/
/  Copyright (C) 2013, ChaN, all right reserved.
/  Copyright (C) 2017, Stephan Brunner, all right reserved.
/ 
/ * This project is a free software and there is NO WARRANTY.
/ * No restriction on use. You can use, modify and redistribute it for
/   personal, non-profit or commercial products UNDER YOUR RESPONSIBILITY.
/ * Redistributions of source code must retain the above copyright notice.
/
/----------------------------------------------------------------------------*/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include "pff.h"

// fuses
FUSES = {0xC1, 0xDD, 0xFF};	/* ATtiny861 fuse bytes: Low, High, Extended.
This is the fuse settings of this project. The fuse data will be included
in the output hex file with program code. However some old flash programmers
cannot load the fuse bits from hex file. If it is the case, remove this line
and use these values to program the fuse bits. */

// constants
#define FCC(c1,c2,c3,c4)	(((DWORD)c4<<24)+((DWORD)c3<<16)+((WORD)c2<<8)+(BYTE)c1)	/* FourCC */
#define MODE 1 // stereo

// external methods
void delay_ms (WORD);	/* Defined in asmfunc.S */
void delay_us (WORD);	/* Defined in asmfunc.S */
EMPTY_INTERRUPT(PCINT_vect);

// variables
volatile BYTE FifoRi, FifoWi, FifoCt;	/* FIFO controls */
BYTE Buff[256];		/* Audio output FIFO */
FATFS Fs;			/* File system object */
DIR Dir;			/* Directory object */
FILINFO Fno;		/* File information */
WORD rb;			/* Return value. Put this here to avoid avr-gcc's bug */

 
// Initializes the analog in needed for reading the button:
// 	   ADC Prescaler needs to be set so that the ADC input frequency is between 50 - 200kHz.
//
//            For more information, see table 17.5 "ADC Prescaler Selections" in
//            chapter 17.13.2 "ADCSRA – ADC Control and Status Register A"
//           (pages 140 and 141 on the complete ATtiny25/45/85 datasheet, Rev. 2586M–AVR–07/10)
//
//            Valid prescaler values for various clock speeds
//
// 	     Clock   Available prescaler values
//            ---------------------------------------
//              1 MHz   8 (125kHz), 16 (62.5kHz)
//              4 MHz   32 (125kHz), 64 (62.5kHz)
//              8 MHz   64 (125kHz), 128 (62.5kHz)
//             16 MHz   128 (125kHz)
//
//   8-bit resolution:
//   set ADLAR to 1 to enable the Left-shift result (only bits ADC9..ADC2 are available)
//   then, only reading ADCH is sufficient for 8-bit results (256 values)
void initADC() {
  ADMUX =
            (1 << ADLAR) |     // left shift result -> 8-bit mode
			// ref. voltage to VCC
            (0 << REFS1) |     // Set ref. voltage bit 1
            (0 << REFS0) |     // Set ref. voltage bit 0
			// use ADC6 as analog input
            (0 << MUX3)  |     // MUX bit 3
            (1 << MUX2)  |     // MUX bit 2
            (1 << MUX1)  |     // MUX bit 1
            (0 << MUX0);       // MUX bit 0

  ADCSRA = 
            (1 << ADEN)  |     // Enable ADC 
            (1 << ADPS2) |     // set prescaler bit 2 
            (1 << ADPS1) |     // set prescaler bit 1 
            (1 << ADPS0);      // set prescaler bit 0  
}

// Ramp-up/down audio output (anti-pop feature) 
// 
// @param up: 1 to ramp up, 0 to ramp down
static void ramp (int up) {
	BYTE value;
	BYTE direction;

	if (up) {
		value = 0; 
		direction = 1;
	} else {
		value = 128; 
		direction = 0xFF;
	}

	for (BYTE i = 0; i < 128; i++) {
		value += direction;
		OCR1A = value;
		OCR1B = value;
		delay_us(100);
	}
}

/* Enable audio output functions */
static void audio_on (void)	{
	if (!TCCR0B) {
		FifoCt = 0; FifoRi = 0; FifoWi = 0;		/* Reset audio FIFO */
		PLLCSR = 0b00000110;	/* Select PLL clock for TC1.ck */
		TCCR1A = 0b10100011;	/* Start TC1 with OC1A/OC1B PWM enabled */
		TCCR1B = 0b00000001;
		ramp(1);				/* Ramp-up to center level */
		TCCR0A = 0b00000001;	/* Enable TC0.ck = 2MHz as interval timer */
		TCCR0B = 0b00000010;
		TIMSK = _BV(OCIE0A);
	}
}

// Disable audio output functions
static void audio_off (void) {
	if (TCCR0B) {
		TCCR0B = 0;				/* Stop audio timer */
		ramp(0);				/* Ramp-down to GND level */
		TCCR1A = 0;	TCCR1B = 0;	/* Stop PWM */
	}
}

// Loads the header
// 
// @return error code FRESULT or if bigger than 1024, the number of samples
static DWORD load_header (void) {
	DWORD sz, f;
	BYTE b, al = 0;


	/* Check RIFF-WAVE file header */
	if (pf_read(Buff, 12, &rb)) return 2;
	if (rb != 12 || LD_DWORD(Buff+8) != FCC('W','A','V','E')) return 4;

	for (;;) {
		if (pf_read(Buff, 8, &rb)) return 2;		/* Get Chunk ID and size */
		if (rb != 8) return 4;
		sz = LD_DWORD(&Buff[4]);		/* Chunk size */

		switch (LD_DWORD(&Buff[0])) {	/* Switch by chunk type */
		case FCC('f','m','t',' ') :		/* 'fmt ' chunk */
			if (sz & 1) sz++;
			if (sz > 128 || sz < 16) return 4;		/* Check chunk size */
			if (pf_read(Buff, sz, &rb)) return 2;	/* Get the chunk content */
			if (rb != sz) return 4;
			if (Buff[0] != 1) return 4;				/* Check coding type (1: LPCM) */
			b = Buff[2];
			if (b < 1 && b > 2) return 4; 			/* Check channels (1/2: Mono/Stereo) */
			GPIOR0 = al = b;						/* Save channel flag */
			b = Buff[14];
			if (b != 8 && b != 16) return 4;		/* Check resolution (8/16 bit) */
			GPIOR0 |= b;							/* Save resolution flag */
			if (b & 16) al <<= 1;
			f = LD_DWORD(&Buff[4]);					/* Check sampling freqency (8k-48k) */
			if (f < 8000 || f > 48000) return 4;
			OCR0A = (BYTE)(16000000UL/8/f) - 1;		/* Set interval timer (sampling period) */
			break;

		case FCC('d','a','t','a') :		/* 'data' chunk (start to play) */
			if (!al) return 4;							/* Check if format valid */
			if (sz < 1024 || (sz & (al - 1))) return 4;	/* Check size */
			if (Fs.fptr & (al - 1)) return 4;			/* Check offset */
			return sz;

		case FCC('D','I','S','P') :		/* 'DISP' chunk (skip) */
		case FCC('f','a','c','t') :		/* 'fact' chunk (skip) */
		case FCC('L','I','S','T') :		/* 'LIST' chunk (skip) */
			if (sz & 1) sz++;
			if (pf_lseek(Fs.fptr + sz)) return 2;
			break;

		default :						/* Unknown chunk */
			return 4;
		}
	}
}

// Polls and returns the button state
//
// @return The button that is currently pressed (1..11) or 0 of no button is pressed
static BYTE buttonPressed() {
	ADCSRA |= (1 << ADSC);         // start ADC measurement
	while (ADCSRA & (1 << ADSC) ); // wait till conversion complete

	if (ADCH < 6) {
		return 0;
	} else if (ADCH < 17) {
		return 1;
	} else if (ADCH < 26) {
		return 2;
	} else if (ADCH < 40) {
		return 3;
	} else if (ADCH < 63) {
		return 4;
	} else if (ADCH < 89) {
		return 5;
	} else if (ADCH < 129) {
		return 6;
	} else if (ADCH < 155) {
		return 7;
	} else if (ADCH < 184) {
		return 8;
	} else if (ADCH < 205) {
		return 9;
	} else if (ADCH < 224) {
		return 10;
	} else {
		return 11;
	} 
}

// Opens and plays a file.
// 
// @param play File number (1..255)
// @return 0:Normal end, 1:Continue to play, 2:Disk error, 3:No file, 4:Invalid file
static BYTE play (BYTE fn) {
	DWORD sz, spa, sza;
	FRESULT res;
	WORD btr;
	BYTE n, i, rc;

	/* Open an audio file "nnn.WAV" (nnn=001..255) */
	i = 2; 
	n = fn;
	do {
		Buff[i] = (BYTE)(n % 10) + '0'; n /= 10;
	} while (i--);
	strcpy_P((char*)&Buff[3], PSTR(".WAV"));
	res = pf_open((char*)Buff);
	if (res == FR_NO_FILE) return 3;
	if (res != FR_OK) return 2;

	/* Get file parameters */
	sz = load_header();
	if (sz <= 4) return (BYTE)sz;	/* Invalid format */
	spa = Fs.fptr; 
	sza = sz;		/* Save offset and size of audio data */

	audio_on();		/* Enable audio output */

	for (;;) {
		if (pf_read(0, 512 - (Fs.fptr % 512), &rb) != FR_OK) {		/* Snip sector unaligned part */
			rc = 2; 
			break;
		}
		sz -= rb;
		
		BYTE buttonsDisabled = 0; // to make jumps hearable when FF or RW
		do {
			/* Forward a bunch of the audio data to the FIFO */
			btr = (sz > 1024) ? 1024 : (WORD)sz;
			pf_read(0, btr, &rb);
			if (btr != rb) {
				rc = 2; 
				break;
			}
			sz -= rb;
						
			// check if some button is pressed
			if (buttonsDisabled) buttonsDisabled--;
			BYTE buttonValue = buttonPressed();
			if (!buttonsDisabled && buttonValue != 0) {
				// debounce
				delay_ms(1);
				BYTE nextButtonValue = buttonPressed();
				while(buttonValue != nextButtonValue) {
					buttonValue = nextButtonValue;
					delay_ms(1);
					nextButtonValue = buttonPressed();
				}
				
				if (buttonValue == 10) {
					// jump backwards
					if (Fs.fptr > (DWORD)500 * btr) {
						pf_lseek(Fs.fptr - (DWORD)500 * btr);
						sz += (DWORD)1000 * btr;
					} else {
						// if current position is to close too the start of the file
						pf_lseek(0);
						sz = sza;
						// TODO maybe skip to last file in channel?
						// wait until no button is pressed, as funny noises may occure otherwise
						while(buttonPressed() != 0);
					}
				} else if (buttonValue == 11) {
					// jump forward
					if(sz > (DWORD)100 * btr) {
						pf_lseek(Fs.fptr + (DWORD)100 * btr);
						sz -= (DWORD)100 * btr;
					} else {
						// if current position is too close to the end of the file
						// TODO return and jump to next file if possible
					}
					
				} else if (buttonValue != 0) {
					// if any other button was pressed, return
					// TODO return and play chosen file
					return 0;
				}
				buttonsDisabled = 50;
			}

			/* Check input code change */
			rc = 0;

		} while (!rc && rb == 1024);	/* Repeat until all data read or code change */
		
		// TODO if file ended, ++ the current file count

		if (rc) break;
		if (pf_lseek(spa) != FR_OK) {	/* Return top of audio data */
			rc = 3; 
			break;
		}
		sz = sza;
	}

	while (FifoCt) ;			/* Wait for audio FIFO empty */
	OCR1A = 0x80; OCR1B = 0x80;	/* Return DAC out to center */
	
	return rc;
}

int main (void) {
	BYTE rc;

	initADC(); // initialize Analog input (control buttons)
	
	MCUSR = 0;								/* Clear reset status */
	//WDTCR = _BV(WDE) | 0b110;				/* Enable WDT (1s) */
	set_sleep_mode(SLEEP_MODE_PWR_DOWN);	/* Select power down mode for sleep */
	PCMSK0 = 0b11111000;					/* Select pin change interrupt pins (SW1..SW8) */
	PCMSK1 = 0b01110000;

	/* Initialize ports */
	PORTA = 0b01111110;		/* PORTA [-ppppppL]*/
	DDRA  = 0b00000001;
	PORTB = 0b01110001;		/* PORTB [-pHHLLLp] */
	DDRB  = 0b00111110;

	sei();

	for (;;) {
		if (pf_mount(&Fs) == FR_OK) {	/* Initialize FS */
			/* Main loop */
			do {
				// TODO button evaluation should be outside of that loop and only executed after startup or when channel is done
				// wait for a button to be pressed
				while (buttonPressed() == 0);
				delay_ms(1); // the electronics around the button needs time to stabilize.
				int buttonValue = buttonPressed();
				
				// wait for no button pressed
				// TODO make the button event change a current file variable, and make the loop independend of buttons. Only file ends or buttons pushed in play() will keep the loop alive.
				while (buttonPressed() != 0);
				if (buttonValue != 10 && buttonValue != 11) {
					rc = play(buttonValue); 
				}
				
				// for debugging. Make the error returned hearable
				if (rc > 0) play(rc);
				
			} while (rc != 2);				/* Continue while no disk error */

			audio_off();	/* Disable audio output */
		}
	}
}

