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
#define FCC(c1,c2,c3,c4)	(((unsigned long)c4<<24)+((unsigned long)c3<<16)+((WORD)c2<<8)+(unsigned char)c1)	/* FourCC */
#define MODE 1 // stereo
#define FF_SPEED 100 // size of jump in kB
#define RW_SPEED 200 // size of jump in kB
#define FF_RW_AUDIO_CLUSTER_SIZE 50 // The size (in kB) of the Audio clusters hearable while rw/ff
#define FF_RW_FAST_AUDIO_CLUSTER_SIZE 10 // The size (in kB) of the Audio clusters hearable while fast rw/ff
#define FF_RW_PUSH_DURATION 200 //ms
#define SKIP_DOUBLECLICK_DELAY 200 // ms
#define SKIP_BACKWARDS_THRESHOLD 100 // size of threshold in kB
#define NUMBER_OF_JUMPS_TO_SWITCH_TO_FAST_FF_RW 5 // after a couple of jumps while FF RW, the jump size increases, like it used to do with CD players
#define FAST_FF_RW_FACTOR 5 // times faster after NUMBER_OF_JUMPS_TO_SWITCH_TO_FAST_FF_RW jumps
#define SWITCH_TO_IDLE_DURATION 60000 // ms
#define IDLE_EFFECT_FREQUENCE 4000 // ms
#define BLINK_SPEED 70 // ms

// error codes
#define INVALIDE_FILE 11
#define NOT_A_WAVE_FILE 12
#define WRONG_CHUNK_SIZE 13
#define NOT_LPCM_CODING_TYPE 14
#define WRONG_NUMBER_OF_CHANNELS 15
#define WRONG_RESOLUTION 16
#define WRONG_SAMPLING_FREQ 17
#define WRONG_OFFSET 18
#define UNKNOWN_CHUNK 19
#define END_OF_FILE 20
#define HIGHEST_ERROR_CODE 20

// LED numbering
#define TRACK_1_LED 0
#define TRACK_2_LED 1
#define TRACK_3_LED 2
#define TRACK_4_LED 3
#define TRACK_5_LED 4
#define TRACK_6_LED 5
#define TRACK_7_LED 6
#define TRACK_8_LED 7
#define RW_LED 8
#define FF_LED 9

// pinning
#define LED_DATA PA3
#define LED_CLK PA2
#define LED_LE PA1

// structs and enums
typedef struct {
	unsigned long numberOfSamples;
	unsigned long dataOffset; 
} AUDIOFILE_INFO;
typedef enum {
	PLAY_MODE,
	RW_MODE,
	FF_MODE
} PLAYER_MODE;

// external methods
void delay_ms (WORD);	/* Defined in asmfunc.S */
void delay_us (WORD);	/* Defined in asmfunc.S */
EMPTY_INTERRUPT(PCINT_vect);

// variables
volatile unsigned char FifoRi, FifoWi, FifoCt;	/* FIFO controls */
unsigned char Buff[256];		/* Audio output FIFO, needed by asmfunc.S too */
FATFS fileSystem;			/* File system object */
DIR directory;			/* Directory object */
FILINFO fileInfo;		/* File information */
AUDIOFILE_INFO audioFileInfo;
UINT rb;			/* Return value. Put this here to avoid avr-gcc's bug */ // TODO Maybe this is not a problem anymore? Remove?
unsigned char currentChannel = 0;
unsigned char currentFile = 0;
uint16_t ledStates = 0;

 
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
	unsigned char value;
	unsigned char direction;

	if (up) {
		value = 0; 
		direction = 1;
	} else {
		value = 128; 
		direction = 0xFF;
	}

	for (unsigned char i = 0; i < 128; i++) {
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
static unsigned long load_header (void) {
	unsigned long ret = 0;

	/* Check RIFF-WAVE file header */
	ret = pf_read(Buff, 12, &rb);
	if (ret) {
		return ret;
	}
	if (rb != 12 || LD_DWORD(Buff+8) != FCC('W','A','V','E')) {
		return NOT_A_WAVE_FILE;
	}

	unsigned char al = 0;
	for (;;) {
		// Get Chunk ID and size
		ret = pf_read(Buff, 8, &rb); 
		if (ret) {
			return ret;
		}
		unsigned long chunkSize = LD_DWORD(&Buff[4]);
		unsigned long id = LD_DWORD(&Buff[0]);
		
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
			if (ret) {
				return ret;
			}
				
			// Check coding type (1: LPCM)
			if (Buff[0] != 1) {
				return NOT_LPCM_CODING_TYPE;				
			}
				
			// Check channels (1/2: Mono/Stereo)
			unsigned char numberOfChannels = Buff[2];
			if (numberOfChannels < 1 && numberOfChannels > 2) {
				return WRONG_NUMBER_OF_CHANNELS; 			
			}
				
			// Save channel flag
			GPIOR0 = numberOfChannels;
			al = numberOfChannels;	
									
			/* Check resolution (8/16 bit) */
			unsigned char resolution = Buff[14];
			if (resolution != 8 && resolution != 16) {
				return WRONG_RESOLUTION;
			}
				
			// Save resolution flag
			GPIOR0 |= resolution;							
			if (resolution & 16) {
				al <<= 1;
			}
				
			// Check sampling frequency (8k-48k)
			unsigned long frequency = LD_DWORD(&Buff[4]);					
			if (frequency < 8000 || frequency > 48000) {
				return WRONG_SAMPLING_FREQ;
			}
				
			// Set interval timer (sampling period)
			OCR0A = (unsigned char)(16000000UL/8/frequency) - 1;	
		} else if (id == FCC('d','a','t','a')) {
			// Check if format valid
			if (!al) {
				return INVALIDE_FILE;
			}
				
			// TODO What does the following code do exactly?
			// Check size
			if (chunkSize < 1024 || (chunkSize & (al - 1))) {
				return WRONG_CHUNK_SIZE;	
			}
			
			// TODO The following code seems to make problems with intact files. What should it do and why isn't it working?
			// Check offset
// 			if (fileSystem.fptr & (al - 1)) {
// 				return WRONG_OFFSET;		
// 			}
			
			// return number of samples, file is ready to play now
			return chunkSize;
		} else if (id == FCC('D','I','S','P') || id == FCC('f','a','c','t') || id == FCC('L','I','S','T')) {
			// skip unused chunks
			if (chunkSize & 1) {
				chunkSize++; // TODO What is this? If odd?...
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
static unsigned char buttonPressed() {
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
		Buff[i] = (unsigned char)(filenNumber % 10) + '0'; 
		filenNumber /= 10;
	}
	strcpy_P((char*)&Buff[3], PSTR(".WAV"));
	FRESULT ret = pf_open((char*)Buff);
	if (ret) {
		// An error has occurred while opening file
		return ret;
	}

	// Get file parameters
	unsigned long numberOfSamples = load_header();
	if (numberOfSamples <= HIGHEST_ERROR_CODE) {
		// An error has occurred while loading header
		return (unsigned char)numberOfSamples;
	}

	// save audio file specs	
	audioFileInfo.numberOfSamples = numberOfSamples;
	audioFileInfo.dataOffset = fileSystem.fptr;

	// enable audio output
	audio_on();
	
	return 0;
}

// Calculates the samples left to read from the current file
//
// @return The number of samples left to read
static unsigned long samplesLeftToRead() {
	return audioFileInfo.numberOfSamples + audioFileInfo.dataOffset - fileSystem.fptr;
}

// Fills the audio buffer with new data
// 
// @return 0 if everything OK, or an error code else
static unsigned char updateAudioBuffer() {
	unsigned char ret = 0;
	
	// Snip sector unaligned part
	ret = pf_read(0, 512 - (fileSystem.fptr % 512), &rb);	
	if (ret) {
		return ret;
	}
		
	/* Forward a bunch of audio data to the FIFO */
	unsigned long size = samplesLeftToRead();
	WORD btr = (size > 1024) ? 1024 : (WORD)size;
	ret = pf_read(0, btr, &rb);
	if (ret) {
		return ret;
	}
	
	if (rb != 1024) {
		// Wait for audio FIFO empty
		while (FifoCt);
			
		// Return DAC out to center
		OCR1A = 0x80;
		OCR1B = 0x80;
		
		return END_OF_FILE;
	} else {
		return 0;
	}
}

static unsigned char storePosition() {
	unsigned char writeBuffer[2];
	writeBuffer[0] = currentChannel;
	writeBuffer[1] = currentFile;
	FRESULT ret = pf_open("POSITION.DAT");
	ret = pf_write(writeBuffer, 2, &rb);
	if (ret != 0 && rb != 2) {
		return ret;
	}
	ret = pf_write(0, 0, &rb);
	if (ret != 0) {
		return ret;
	}
	return 0;
}

static unsigned char readAndUpdatePosition() {
	FRESULT ret = pf_open("POSITION.DAT");
	if (ret != 0) {
		return ret;
	}
	unsigned char readBuffer[2];
	ret = pf_read(readBuffer, 2, &rb);
	if (ret != 0 || rb != 2) {
		return ret;
	}
	currentChannel = readBuffer[0];
	currentFile = readBuffer[1];
	return 0;
}

static unsigned int loadCurrentFile() {
	// exclude error message files
	if (currentChannel == 0 || currentFile == 0) {
		currentChannel = 0;
		currentFile = 0;
		storePosition();
		return FR_NO_FILE;
	}
	
	// first check, if able to load file
	unsigned char ret = load(currentChannel * 100 + currentFile);
	if (ret == 0) {
		// if OK, load position file and store current position
		ret = storePosition();
		if (ret != 0) {
			return ret;
		}
		// load current file again, as otherwise we would be trying to listen to the position file
		unsigned char ret = load(currentChannel * 100 + currentFile);
		if (ret != 0) {
			return ret;
		}
	} else {
		currentChannel = 0;
		currentFile = 0;
		ret = storePosition();
		if (ret != 0) {
			return ret;
		}
		return ret;
	}
	
	return 0;
}

static FRESULT skipToNext() {
	currentFile++;
	return loadCurrentFile();
}

static FRESULT skipToLast() {
	if (currentFile > 1) {
		currentFile--;
		return loadCurrentFile();
	} else {
		return 0;
	}
}

void showLED() {
	for (int i = 0; i < 16; i++) {
		if ((0x8000 >> i) & ledStates) {
			PORTA |= (1 << LED_DATA);
			} else {
			PORTA &= ~(1 << LED_DATA);
		}
		PORTA |= (1 << LED_CLK);
		PORTA &= ~(1 << LED_CLK);
	}
	PORTA |= (1 << LED_LE);
	PORTA &= ~(1 << LED_LE);
}

void lightLEDs(uint16_t states) {
	ledStates = states;
	showLED();
}

void lightLED(uint8_t n, uint8_t on) {
	if (on) {
		ledStates |= (1 << n);
	} else {
		ledStates &= ~(1 << n);
	}
}

void error(BYTE b) {
	lightLEDs(0);
	for (int i = 0; i < 5; i++) {
		lightLED(FF_LED, 0);
		lightLED(RW_LED, 1);
		showLED();
		delay_ms(500);
		lightLED(FF_LED, 1);
		lightLED(RW_LED, 0);
		showLED();
		delay_ms(500);
	}
	lightLEDs(b);
	showLED();
	delay_ms(1000);
}

void blink(uint16_t led) {
	for (int i = 0; i < 2; i++) {
		lightLED(led, 1);
		showLED();
		delay_ms(BLINK_SPEED);
		lightLED(led, 0);
		showLED();
		delay_ms(BLINK_SPEED);
	}
}

void blinkFfRw() {
	for (int i = 0; i < 2; i++) {
		lightLED(FF_LED, 1);
		lightLED(RW_LED, 1);
		showLED();
		delay_ms(BLINK_SPEED);
		lightLED(FF_LED, 0);
		lightLED(RW_LED, 0);
		showLED();
		delay_ms(BLINK_SPEED);
	}
	lightLED(currentChannel - 1, 1);
	showLED();
}

void blinkSkipFf() {
	lightLEDs(0);
	lightLED(FF_LED, 0);
	lightLED(RW_LED, 1);
	lightLED(TRACK_1_LED, 1);
	lightLED(TRACK_3_LED, 1);
	lightLED(TRACK_5_LED, 1);
	lightLED(TRACK_7_LED, 1);
	showLED();
	delay_ms(BLINK_SPEED * 2);
	lightLED(FF_LED, 1);
	lightLED(RW_LED, 0);
	lightLED(TRACK_1_LED, 0);
	lightLED(TRACK_3_LED, 0);
	lightLED(TRACK_5_LED, 0);
	lightLED(TRACK_7_LED, 0);
	lightLED(TRACK_2_LED, 1);
	lightLED(TRACK_4_LED, 1);
	lightLED(TRACK_6_LED, 1);
	lightLED(TRACK_8_LED, 1);
	showLED();
	delay_ms(BLINK_SPEED * 2);
	
	lightLEDs(0);
	lightLED(currentChannel - 1, 1);
	showLED();
}

void blinkSkipRw() {
	lightLEDs(0);
	lightLED(FF_LED, 1);
	lightLED(RW_LED, 0);
	lightLED(TRACK_2_LED, 1);
	lightLED(TRACK_4_LED, 1);
	lightLED(TRACK_6_LED, 1);
	lightLED(TRACK_8_LED, 1);
	showLED();
	delay_ms(BLINK_SPEED * 2);
	lightLED(FF_LED, 0);
	lightLED(RW_LED, 1);
	lightLED(TRACK_2_LED, 0);
	lightLED(TRACK_4_LED, 0);
	lightLED(TRACK_6_LED, 0);
	lightLED(TRACK_8_LED, 0);
	lightLED(TRACK_1_LED, 1);
	lightLED(TRACK_3_LED, 1);
	lightLED(TRACK_5_LED, 1);
	lightLED(TRACK_7_LED, 1);
	showLED();
	delay_ms(BLINK_SPEED * 2);
	
	lightLEDs(0);
	lightLED(currentChannel - 1, 1);
	showLED();
}

void toggleRwFf() {
	ledStates ^= 1 << FF_LED;
	ledStates ^= 1 << RW_LED;
	showLED();
}

void kittSequence() {
		// rising bar
		for (int i = 0; i < 5; i++){
			lightLED(FF_LED - i * 2 + 2, 0);
			lightLED(FF_LED - i * 2 + 1, 0);
			lightLED(FF_LED - i * 2, 1);
			lightLED(FF_LED - i * 2 - 1, 1);
			showLED();
			delay_ms(BLINK_SPEED);
		}
		
		// falling bar
		for (int i = 0; i < 5; i++){
			lightLED(i * 2 + TRACK_1_LED - 2, 0);
			lightLED(i * 2 + TRACK_1_LED - 1, 0);
			lightLED(i * 2 + TRACK_1_LED, 1);
			lightLED(i * 2 + TRACK_1_LED + 1, 1);
			showLED();
			delay_ms(BLINK_SPEED);
		}
}

void doublBlinkPlayButtons() {
		// doubleblink all but last line
		for (int i = 0; i < 2; i++) {
			lightLEDs(0x0000);
			showLED();
			delay_ms(BLINK_SPEED);
			lightLEDs(0b0000000011111111);
			showLED();
			delay_ms(BLINK_SPEED);
		}
}

void ledSequence() {
	kittSequence();
	
	// light all lines sequncely line wise, bottom to top
	for(int i = 0; i < 5; i++) {
		lightLED(FF_LED - 2 * i, 1);
		lightLED(FF_LED - 2 * i - 1, 1);
		showLED();
		delay_ms(BLINK_SPEED);
	}
	
	doublBlinkPlayButtons();
}

int main (void) {
	initADC(); // initialize Analog input (control buttons)
	
	MCUSR = 0;								/* Clear reset status */
	//WDTCR = _BV(WDE) | 0b110;				/* Enable WDT (1s) */
	set_sleep_mode(SLEEP_MODE_PWR_DOWN);	/* Select power down mode for sleep */
	PCMSK0 = 0b11111000;					/* Select pin change interrupt pins (SW1..SW8) */
	PCMSK1 = 0b01110000;

	/* Initialize ports */
	PORTA = 0b00000000;		/* PORTA [-LLLLLLL]*/
	DDRA  = 0b01111111;
	PORTB = 0b01110001;		/* PORTB [-pHHLLLp] */
	DDRB  = 0b00111110;

	sei();
			
	while (1) {
		lightLEDs(0);
		ledSequence();
		if (pf_mount(&fileSystem) == FR_OK) {	/* Initialize FS */
			
			// check if a position is stored in position file
			unsigned char ret = readAndUpdatePosition();
			if (ret != 0) {
				error(ret);
			}
			
			// if no position is defined yet, wait for a button to be pressed
			if (currentFile == 0) {
				// wait for a button to be pressed
				uint16_t idleCounter = SWITCH_TO_IDLE_DURATION;
				uint16_t effectCounter = 0;
				while (buttonPressed() == 0) {
					if (idleCounter) {
						idleCounter--;
					} else {
						if (effectCounter > IDLE_EFFECT_FREQUENCE) {
							effectCounter = 0;
							doublBlinkPlayButtons();
						} else {
							effectCounter++;
							lightLEDs(0);
							showLED();	
						}
					}
					delay_ms(1);
				}
				delay_ms(1); // the electronics around the button needs time to stabilize.
				unsigned char buttonValue = buttonPressed();
				
				// clear leds
				lightLEDs(0);
				showLED();
				
				// wait for no button pressed
				while (buttonPressed() != 0);
				
				// evaluate pressed button
				if (buttonValue != 10 && buttonValue != 11) {
					currentChannel = buttonValue;
					currentFile = 1;
				}
			}
											
			// light corresponding LED
			lightLEDs(0);
			lightLED(currentChannel - 1, 1);
			showLED();
								
			// load file
			PLAYER_MODE playerMode = PLAY_MODE;
			ret = loadCurrentFile();
			unsigned char playFfRwAudioCluster = 0; // to make jumps hearable when FF or RW
			unsigned char numberOfFfRwJumps = 0;
			while (ret == 0) {				
				// update Buffer and handle end of file error and other errors
				ret = updateAudioBuffer();
				if (ret == END_OF_FILE) {
					ret = skipToNext(currentChannel, currentFile);
					// quit routine if playlist is finished
					if (ret) {
						break;
					} else {
						blinkSkipFf();
						lightLED(currentChannel - 1, 1);
						showLED();
					}
				} else if (ret) {
					error(ret);
					break;
				}
								
				// poll buttons
				unsigned char buttonValue = buttonPressed();
				if (buttonValue != 0) {
					// debounce, as unsettled values were measured sometimes
					delay_ms(1);
					unsigned char nextButtonValue = buttonPressed();
					if(buttonValue != nextButtonValue) {
						buttonValue = 0;
					}
									
					// evaluate pressed button
					if (buttonValue == 10 && playerMode == PLAY_MODE) {
						for (int i = 0; i < FF_RW_PUSH_DURATION; i++) {
							if (buttonPressed() == 0) {
								// if button was released, react immediately, as for skipping, people might want to push short and fast
								break;
							}
							delay_ms(1);
						}
						
						// check if button is still pressed
						if (buttonPressed() == 10) {
							playerMode = RW_MODE;
							lightLED(currentChannel - 1, 1);
							showLED();
						} else {
							// wait for a potential second button press
							uint8_t skip = 0;
							for (int i = 0; i < SKIP_DOUBLECLICK_DELAY; i++) {
								if (buttonPressed() == 10) {
									skip = 1;
									break;
								}
								delay_ms(1);
							}
							
							// evaluate and execute "skip to last" or "replay current file"
							// skip backwards or to the start of the file
							if (currentFile > 1 && (skip || fileSystem.fptr < (unsigned long)SKIP_BACKWARDS_THRESHOLD * 1024)) {
								blinkSkipRw();
								ret = skipToLast();
								if (ret) {
									error(ret);
									break;
								}
							} else {
								blinkFfRw();
								ret= loadCurrentFile();
								if (ret) {
									error(ret);
									break;
								}
							}
						}
					} else if (buttonValue == 11 && playerMode == PLAY_MODE) {
						for (int i = 0; i < FF_RW_PUSH_DURATION; i++) {
							if (buttonPressed() == 0) {
								// if button was released, react immediately, as for skipping, people might want to push short and fast
								break;
							}
							delay_ms(1);
						}
						
						// check if button is still pressed
						if (buttonPressed() == 11) {
							playerMode = FF_MODE;
							lightLED(currentChannel - 1, 1);
							showLED();
						} else {
							// skip forward
							blinkSkipFf();
								ret = skipToNext();
							if (ret) {
								error(ret);
								break;
							}
						}
					} else if (buttonValue != 0 && playerMode == PLAY_MODE) {
						// if any other button
						if (buttonValue == currentChannel) {
							blinkSkipFf();
							ret = skipToNext();
							if (ret) {
								error(ret);
								break;
							}
						} else {
							currentChannel = buttonValue;
							currentFile = 1;
							lightLEDs(0);
							lightLED(currentChannel - 1, 1);
							showLED();							
							ret = loadCurrentFile();
							if (ret) {
								error(ret);
								break;
							}
						}
						
						// wait until button is released
						while (buttonPressed());
					}
				} 
				
				// This part can't be written as an else, as buttonValue might have changed during last if
				if (buttonValue == 0) {
					playerMode = PLAY_MODE;
					playFfRwAudioCluster = 0;
					numberOfFfRwJumps = 0;
					// after using toggleRwFf(), one of the LEDs might still be on
					if ((1 << FF_LED & ledStates) || (1 << RW_LED & ledStates)) {
						lightLED(FF_LED, 0);
						lightLED(RW_LED, 0);
						showLED();
					}
				}
				
				// if RW or FF, jump position every FF_RW_AUDIO_CLUSTER_SIZE iteration
				if (playFfRwAudioCluster) {
					playFfRwAudioCluster--;
				}
				if (playerMode == RW_MODE && !playFfRwAudioCluster) {
					toggleRwFf();
					unsigned long jumpSize = 0;
					if (numberOfFfRwJumps > NUMBER_OF_JUMPS_TO_SWITCH_TO_FAST_FF_RW) {
						jumpSize = (unsigned long)FAST_FF_RW_FACTOR * RW_SPEED * 1024;
						playFfRwAudioCluster = FF_RW_FAST_AUDIO_CLUSTER_SIZE;
					} else {
						jumpSize = (unsigned long)RW_SPEED * 1024;
						numberOfFfRwJumps++;
						playFfRwAudioCluster = FF_RW_AUDIO_CLUSTER_SIZE;
					}
					// jump backwards
					if (fileSystem.fptr > jumpSize) {
						ret = pf_lseek(fileSystem.fptr - jumpSize);
						if (ret) {
							error(ret);
							break;
						}
					} else {
						// if current position is to close too the start of the file
 						if (currentFile == 1) {
							ret = pf_lseek(audioFileInfo.dataOffset);
							if (ret) {
								error(ret);
								break;
							}
							// wait until no button is pressed, as funny noises may occur otherwise
							while(buttonPressed() != 0);
						} else {
							ret = skipToLast();
							blinkSkipRw();
							if (ret) {
								error(ret);
								break;
							}
							
							// jump to almost end of file
 							ret = pf_lseek(fileSystem.fptr + audioFileInfo.numberOfSamples - jumpSize);
							if (ret) {
								error(ret);
								break;
							}
						}
					}
				} else if (playerMode == FF_MODE && !playFfRwAudioCluster) {
					toggleRwFf();
					unsigned long jumpSize = 0;
					if (numberOfFfRwJumps > NUMBER_OF_JUMPS_TO_SWITCH_TO_FAST_FF_RW) {
						jumpSize = (unsigned long)FAST_FF_RW_FACTOR * FF_SPEED * 1024; 
						playFfRwAudioCluster = FF_RW_FAST_AUDIO_CLUSTER_SIZE;
					} else {
						jumpSize = (unsigned long)FF_SPEED * 1024;
						numberOfFfRwJumps++;
						playFfRwAudioCluster = FF_RW_AUDIO_CLUSTER_SIZE;
					}
					// jump forward
					if(samplesLeftToRead() > jumpSize) {
						ret = pf_lseek(fileSystem.fptr + jumpSize);
						if (ret) {
							error(ret);
							break;
						}
					} else {
						// if current position is too close to the end of the file
						ret = skipToNext();
						blinkSkipFf();
						// quit if playlist is finished
						if (ret) {
							break;
						}
					}
				}
			}

			audio_off();	/* Disable audio output */
		} else {
			error(pf_mount(&fileSystem));
		}
	}
}

