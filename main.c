/*----------------------------------------------------------------------------/
/  20-pin SD Sound Generator R0.01a                                           /
/-----------------------------------------------------------------------------/
/ This project, program codes and circuit diagrams, is opened under license
/ policy of following trems.
/
/  Copyright (C) 2013, ChaN, all right reserved.
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

#ifndef MODE
#error Wrong make file.
#endif

FUSES = {0xC1, 0xDD, 0xFF};	/* ATtiny861 fuse bytes: Low, High, Extended.
This is the fuse settings of this project. The fuse data will be included
in the output hex file with program code. However some old flash programmers
cannot load the fuse bits from hex file. If it is the case, remove this line
and use these values to program the fuse bits. */

#define FCC(c1,c2,c3,c4)	(((DWORD)c4<<24)+((DWORD)c3<<16)+((WORD)c2<<8)+(BYTE)c1)	/* FourCC */

#define LED_ON()	PORTB |= _BV(2)
#define LED_OFF()	PORTB &= ~_BV(2)

void delay_ms (WORD);	/* Defined in asmfunc.S */
void delay_us (WORD);	/* Defined in asmfunc.S */

EMPTY_INTERRUPT(PCINT_vect);


/*---------------------------------------------------------*/
/* Work Area                                               */
/*---------------------------------------------------------*/

volatile BYTE FifoRi, FifoWi, FifoCt;	/* FIFO controls */
BYTE Buff[256];		/* Audio output FIFO */

BYTE InMode, Cmd;	/* Input mode and received command value */

FATFS Fs;			/* File system object */
DIR Dir;			/* Directory object */
FILINFO Fno;		/* File information */

WORD rb;			/* Return value. Put this here to avoid avr-gcc's bug */



/*---------------------------------------------------------*/
/* Sub-routines                                            */
/*---------------------------------------------------------*/


static
void led_sign (
	BYTE ct		/* Number of flashes */
)
{
	do {
		delay_ms(200);
		LED_ON();
		delay_ms(100);
		LED_OFF();
	} while (--ct);
	delay_ms(1000);
}



static
BYTE chk_input (void)	/* 0:Not changed, 1:Changed */
{
	BYTE k, n;
	static BYTE pk, nk;


	wdt_reset();

	k = ~((PINA & 0xF8) | ((PINB >> 4) & 0x07));
	GIFR = _BV(PCIF);
	n = nk; nk = k;
	if (n != k || pk == k) return 0;

	pk = k; Cmd = k;

	return 1;
}



static
void ramp (		/* Ramp-up/down audio output (anti-pop feature) */
	int dir		/* 0:Ramp-down, 1:Ramp-up */
)
{
#if MODE != 0	/* This function is enebled on non-OCL output cfg. */
	BYTE v, d, n;


	if (dir) {
		v = 0; d = 1;
	} else {
		v = 128; d = 0xFF;
	}

	n = 128;
	do {
		v += d;
		OCR1A = v; OCR1B = v;
		delay_us(100);
	} while (--n);
#endif
}



static
void audio_on (void)	/* Enable audio output functions */
{
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



static
void audio_off (void)	/* Disable audio output functions */
{
	if (TCCR0B) {
		TCCR0B = 0;				/* Stop audio timer */
		ramp(0);				/* Ramp-down to GND level */
		TCCR1A = 0;	TCCR1B = 0;	/* Stop PWM */
	}
}



static
void wait_status (void)	/* Wait for a code change */
{
	BYTE n;


	if (Cmd) return;

	audio_off();	/* Disable audio output */

	for (;;) {
		n = 10;				/* Wait for a code change at active mode (100ms max) */
		do {
			delay_ms(10);
			chk_input();
		} while (--n && !Cmd);
		if (Cmd) break;		/* Return if any code change is detected within 100ms */

		cli();							/* Enable pin change interrupt */
		GIMSK = _BV(PCIE1);
		WDTCR = _BV(WDE) | _BV(WDCE);	/* Disable WDT */
		WDTCR = 0;
		sleep_enable();					/* Wait for a code change at power-down mode */
		sei();
		sleep_cpu();
		sleep_disable();
		wdt_reset();					/* Enable WDT (1s) */
		WDTCR = _BV(WDE) | 0b110;
		GIMSK = 0;						/* Disable pin change interrupt */
	}
}



static
DWORD load_header (void)	/* 2:I/O error, 4:Invalid file, >=1024:Ok(number of samples) */
{
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



static
BYTE play (		/* 0:Normal end, 1:Continue to play, 2:Disk error, 3:No file, 4:Invalid file */
	BYTE fn		/* File number (1..255) */
)
{
	DWORD sz, spa, sza;
	FRESULT res;
	WORD btr;
	BYTE n, i, rc;


	if (InMode >= 2) Cmd = 0;	/* Clear command code (Edge triggered) */

	/* Open an audio file "nnn.WAV" (nnn=001..255) */
	i = 2; n = fn;
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
	spa = Fs.fptr; sza = sz;		/* Save offset and size of audio data */

	LED_ON();
	audio_on();		/* Enable audio output */

	for (;;) {
		if (pf_read(0, 512 - (Fs.fptr % 512), &rb) != FR_OK) {		/* Snip sector unaligned part */
			rc = 2; break;
		}
		sz -= rb;
		do {
			/* Forward a bunch of the audio data to the FIFO */
			btr = (sz > 1024) ? 1024 : (WORD)sz;
			pf_read(0, btr, &rb);
			if (btr != rb) {
				rc = 2; break;
			}
			sz -= rb;

			/* Check input code change */
			rc = 0;
			if (chk_input()) {
				switch (InMode) {
				case 4:		/* Mode 4: Edge triggered (one-shot) */
					if (!Cmd) rc = 1;
					break;
				case 3: 	/* Mode 3: Edge triggered (retriggerable) */
					if (Cmd) rc = 1;	/* Restart by a code change but zero */
					break;
				case 2:		/* Mode 2: Edge triggered */
					Cmd = 0;			/* Ignore code changes while playing */
					break;
				case 1:		/* Mode 1: Level triggered (sustained to end of the file) */
					if (Cmd && Cmd != fn) rc = 1;	/* Restart by a code change but zero */
					break;
				case 0:	/* Mode 0: Level triggered */
					if (Cmd != fn) rc = 1;	/* Restart by a code change */
				}
			}
		} while (!rc && rb == 1024);	/* Repeat until all data read or code change */

		if (rc || !Cmd || InMode >= 2) break;
		if (pf_lseek(spa) != FR_OK) {	/* Return top of audio data */
			rc = 3; break;
		}
		sz = sza;
	}

	while (FifoCt) ;			/* Wait for audio FIFO empty */
	OCR1A = 0x80; OCR1B = 0x80;	/* Return DAC out to center */

	LED_OFF();

	return rc;
}



/*-----------------------------------------------------------------------*/
/* Main                                                                  */

int main (void)
{
	BYTE rc;


	MCUSR = 0;								/* Clear reset status */
	WDTCR = _BV(WDE) | 0b110;				/* Enable WDT (1s) */
	set_sleep_mode(SLEEP_MODE_PWR_DOWN);	/* Select power down mode for sleep */
	PCMSK0 = 0b11111000;					/* Select pin change interrupt pins (SW1..SW8) */
	PCMSK1 = 0b01110000;

	/* Initialize ports */
	PORTA = 0b11111011;		/* PORTA [pppppLHp]*/
	DDRA  = 0b00000110;
	PORTB = 0b01110001;		/* PORTB [-pppLLLH] */
	DDRB  = 0b00001111;

	sei();

	for (;;) {
		if (pf_mount(&Fs) == FR_OK) {	/* Initialize FS */

			/* Load command input mode (if not exist, use mode 0 as default) */
			strcpy_P((char*)Buff, PSTR("000.TXT"));
			if (pf_open((char*)Buff) == FR_OK) {
				pf_read(&InMode, 1, &rb);
				InMode -= '0';
			}

			/* Main loop */
			do {
				wait_status();				/* Wait for any valid code */
				rc = play(Cmd);				/* Play corresponding audio file */
				if (rc >= 2) led_sign(rc);	/* Display if any error occured */
				if (rc != 1) Cmd = 0;		/* Clear code when normal end or error */
			} while (rc != 2);				/* Continue while no disk error */

			audio_off();	/* Disable audio output */
		}
		led_sign(2);	/* Disk error or Media mount failed */
	}
}

