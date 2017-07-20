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

// error codes
#define INVALIDE_FILE 11
#define NOT_A_WAVE_FILE 12
#define WRONG_CHUNK_SIZE 13
#define NOT_LPCM_CODING_TYPE 14
#define WRONG_NUMBER_OF_CHANNELS 15
#define WRONG_RESOLUTION 16
#define WRONG_SAMLING_FREQ 17
#define WRONG_OFFSET 18
#define UNKNOWN_CHUNK 19
#define END_OF_FILE 20
#define HIGHEST_ERROR_CODE 20

// structs
typedef struct {
	DWORD numberOfSamples;
	DWORD dataOffset; 
	} AUDIOFILE_INFO;

// external methods
void delay_ms (WORD);	/* Defined in asmfunc.S */
void delay_us (WORD);	/* Defined in asmfunc.S */
EMPTY_INTERRUPT(PCINT_vect);

// variables
volatile BYTE FifoRi, FifoWi, FifoCt;	/* FIFO controls */
BYTE Buff[256];		/* Audio output FIFO */ // TODO remove this, as it is not used as an audio buffer anymore
FATFS fileSystem;			/* File system object */
DIR directory;			/* Directory object */
FILINFO fileInfo;		/* File information */
AUDIOFILE_INFO audioFileInfo;
WORD rb;			/* Return value. Put this here to avoid avr-gcc's bug */ // TODO Maybe this is not a problem anymore? Remove?

 
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
// @return error code FRESULT or INVALIDE_FILE or if bigger than 1024, the number of samples
static DWORD load_header (void) {
	DWORD ret = 0;

	/* Check RIFF-WAVE file header */
	ret = pf_read(Buff, 12, &rb);
	if (rb != 12 || LD_DWORD(Buff+8) != FCC('W','A','V','E')) {
		return NOT_A_WAVE_FILE;
	}

	BYTE al = 0;
	for (;;) {
		// Get Chunk ID and size
		ret = pf_read(Buff, 8, &rb); 
		DWORD chunkSize = LD_DWORD(&Buff[4]);
		DWORD id = LD_DWORD(&Buff[0]);
		
		// analyze id
		if (id == FCC('f','m','t',' ')) {	
			// some size checks
			if (chunkSize & 1) {
				// TODO What is this? If odd?
				chunkSize++;
			} else if (chunkSize > 128 || chunkSize < 16) { 
				// Wrong chunk size
				return WRONG_CHUNK_SIZE;		
			}
				
			// Get the chunk content
			ret = pf_read(Buff, chunkSize, &rb); 
				
			// Check coding type (1: LPCM)
			if (Buff[0] != 1) {
				return NOT_LPCM_CODING_TYPE;				
			}
				
			// Check channels (1/2: Mono/Stereo)
			BYTE numberOfChannels = Buff[2];
			if (numberOfChannels < 1 && numberOfChannels > 2) {
				return WRONG_NUMBER_OF_CHANNELS; 			
			}
				
			// Save channel flag
			GPIOR0 = al = numberOfChannels;	
									
			/* Check resolution (8/16 bit) */
			BYTE resolution = Buff[14];
			if (resolution != 8 && resolution != 16) {
				return WRONG_RESOLUTION;
			}
				
			// Save resolution flag
			GPIOR0 |= resolution;							
			if (resolution & 16) {
				al <<= 1;
			}
				
			// Check sampling frequency (8k-48k)
			DWORD frequency = LD_DWORD(&Buff[4]);					
			if (frequency < 8000 || frequency > 48000) {
				return WRONG_SAMLING_FREQ;
			}
				
			// Set interval timer (sampling period)
			OCR0A = (BYTE)(16000000UL/8/frequency) - 1;	
			if (ret) {
				return ret;
			}	
		} else if (id == FCC('d','a','t','a')) {
			// Check if format valid
			if (!al) {
				return INVALIDE_FILE;
			}
				
			// Check size
			if (chunkSize < 1024 || (chunkSize & (al - 1))) {
				return WRONG_CHUNK_SIZE;	
			}
				
			// Check offset
			if (fileSystem.fptr & (al - 1)) {
				return WRONG_OFFSET;		
			}
			
			// return number of samples, file is ready to play now
			return chunkSize;
		} else if (id == FCC('D','I','S','P') || id == FCC('f','a','c','t') || id == FCC('L','I','S','T')) {
			// skip unused chunks
			if (chunkSize & 1) {
				chunkSize++; // TODO What is this?
			}
			ret  = pf_lseek(fileSystem.fptr + chunkSize);
			if (ret) {
				return ret;
			}
		} else {
			// unknown chunk
			return UNKNOWN_CHUNK;
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
// @param play File number (1..999)
// @return 0 if everything OK or FRESULT if not
static FRESULT load (SHORT filenNumber) {
	/* Open an audio file "nnn.WAV" (nnn=001..999) */
	for(int i = 2; i >= 0; i--) {
		Buff[i] = (BYTE)(filenNumber % 10) + '0'; 
		filenNumber /= 10;
	}
	strcpy_P((char*)&Buff[3], PSTR(".WAV"));
	FRESULT ret = pf_open((char*)Buff);
	if (ret) {
		// An error has occurred while opening file
		return ret;
	}

	// Get file parameters
	DWORD numberOfSamples = load_header();
	if (numberOfSamples <= HIGHEST_ERROR_CODE) {
		// An error has occurred while loading header
		return (BYTE)numberOfSamples;
	}

	// save audio file specs	
	audioFileInfo.numberOfSamples = numberOfSamples;
	audioFileInfo.dataOffset = fileSystem.fptr;

	// enable audio output
	audio_on();
	
	return ret;
}

// Calculates the samples left to read from the current file
//
// @return The number of samples left to read
DWORD samplesLeftToRead() {
	return audioFileInfo.numberOfSamples - audioFileInfo.dataOffset - fileSystem.fptr;
}

// Fills the audio buffer with new data
// 
// @return 0 if everything OK, or an error code else
BYTE updateAudioBuffer() {
	BYTE ret = 0;
	
	// Snip sector unaligned part
	ret = pf_read(0, 512 - (fileSystem.fptr % 512), &rb);	
		
	BYTE buttonsDisabled = 0; // to make jumps hearable when FF or RW
	do {
		/* Forward a bunch of the audio data to the FIFO */
		DWORD size = samplesLeftToRead();
		WORD btr = (size > 1024) ? 1024 : (WORD)size;
		ret = pf_read(0, btr, &rb);
						
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
				if (fileSystem.fptr > (DWORD)500 * btr) {
					pf_lseek(fileSystem.fptr - (DWORD)500 * btr);
				} else {
					// if current position is to close too the start of the file
					pf_lseek(audioFileInfo.dataOffset);
					// TODO maybe skip to last file in channel?
					// wait until no button is pressed, as funny noises may occure otherwise
					while(buttonPressed() != 0);
				}
			} else if (buttonValue == 11) {
				// jump forward
				if(samplesLeftToRead() > (DWORD)100 * btr) {
					pf_lseek(fileSystem.fptr + (DWORD)100 * btr);
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
	} while (!ret && rb == 1024);	/* Repeat until all data read or code change */
		
	// Wait for audio FIFO empty
	while (FifoCt);	
	
	// Return DAC out to center	
	OCR1A = 0x80; 
	OCR1B = 0x80;	
	
	if (rb != 1024) {
		return END_OF_FILE;
	} else {
		return ret;
	}
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

	while (1) {
		if (pf_mount(&fileSystem) == FR_OK) {	/* Initialize FS */
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
					rc = load(100 + buttonValue); 
					while (1) {
						updateAudioBuffer();
					}
				}
				
				// for debugging. Make the error returned hearable
				if (rc > 0) load(rc);
				
			} while (rc != 2);				/* Continue while no disk error */

			audio_off();	/* Disable audio output */
		}
	}
}

