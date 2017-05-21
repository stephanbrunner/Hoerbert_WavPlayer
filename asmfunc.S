;---------------------------------------------------------------------------;
; USI control functions
;---------------------------------------------------------------------------;

.nolist
#include <avr/io.h>	// Include device specific definitions.
.list

#define	_FLAGS	_SFR_IO_ADDR(GPIOR0)


;---------------------------------------------------------------------------;
; Simple Delay
;---------------------------------------------------------------------------;
; void delay_ms(WORD ms);
; void delay_us(WORD us);

.global delay_ms
.func delay_ms
delay_ms:
	wdr			; Reset WDT
	sbiw	r24, 1		; if (ms-- == 0) return;
	brcs	9f		; /
	ldi	ZL, lo8(4000)	; 1ms delay (16MHz)
	ldi	ZH, hi8(4000)	; 
1:	sbiw	ZL, 1		; 
	brne	1b		; /
	rjmp	delay_ms
9:	ret
.endfunc


.global delay_us
.func delay_us
delay_us:
	ldi	r23, 2
1:	dec	r23
	brne	1b
	sbiw	r24, 1
	brne	delay_us
	wdr
	ret
.endfunc


;---------------------------------------------------------------------------;
; Receive a byte from the MMC
;---------------------------------------------------------------------------;
; BYTE rcv_spi (void);

.global rcv_spi
.func rcv_spi
rcv_spi:
	ldi	r24, _BV(2)			;SCK(PA2)
	.rept 16				;Toggle SCK 16 times
	out	_SFR_IO_ADDR(PINA), r24		;
	.endr					;/
	nop					;Read shift register
	in	r24, _SFR_IO_ADDR(USIDR)	;/
	ret
.endfunc



;---------------------------------------------------------------------------;
; Transmit a byte to the MMC
;---------------------------------------------------------------------------;
; void xmit_spi (BYTE);

.global xmit_spi
.func xmit_spi
xmit_spi:
	ldi	r25, _BV(2)			; SCK(PA2)
	in	r22, _SFR_IO_ADDR(PORTA)

	ldi	r23, 8
1:	bst	r24, 7				;DO(PA1) = data bit to be sent
	bld	r22, 1				;
	out	_SFR_IO_ADDR(PORTA), r22	;/
	lsl	r24				;Next bit
	out	_SFR_IO_ADDR(PINA), r25		;SCK(PA2) = H
	out	_SFR_IO_ADDR(PINA), r25		;SCK(PA2) = L
	dec	r23				;while(--r23)
	brne	1b				;/

	sbi	_SFR_IO_ADDR(PORTA), 1		;DO(PA1) = H
	ret
.endfunc



;---------------------------------------------------------------------------;
; Read and forward a part of the 512 byte data block
;---------------------------------------------------------------------------;
; void fwd_blk_part (void*, WORD, WORD);

.global fwd_blk_part
.func fwd_blk_part
fwd_blk_part:
	movw	XL, r24			;X = R25:R24 (memory address)
	movw	ZL, r22			;Z = R23:R22 (byte offset in the sector)

	ldi	r18, lo8(514)		;R19:R18 = 514, Number of bytes to receive
	ldi	r19, hi8(514)		;/
	sub	r18, ZL			;R19:R18 -= Z
	sbc	r19, ZH			;/
	sub	r18, r20		;R19:R18 -= R21:R20
	sbc	r19, r21		;/
	; Skip leading data bytes
	ldi	r24, _BV(2)		;SCK(PA2)
1:	sbiw	ZL, 1			;Skip leading data...
	brcs	2f			;
	.rept 16			;Discard a byte on USI
	out	_SFR_IO_ADDR(PINA), r24	;
	.endr				;/
	rjmp	1b			;
2:	sbiw	XL, 0			;Destination?
	breq	fb_wave

fb_mem:	; Store intermediate data bytes to the memory
	rcall	rcv_spi			;do
	st	X+, r24			; *X++ = rcv_spi()
	subi	r20, 1			;while (--r21:r20)
	sbci	r21, 0			;
	brne	fb_mem			;/
	rjmp	fb_exit

fb_wave: ; Forward intermediate data bytes to the wave FIFO
	sbic	_FLAGS, 4		;if (16bit data) R21:R20 /= 2;
	lsr	r21			;
	sbic	_FLAGS, 4		;
	ror	r20			;/
	sbic	_FLAGS, 1		;if (Stereo data) R21:R20 /= 2;
	lsr	r21			;
	sbic	_FLAGS, 1		;
	ror	r20			;/
	lds	r22, FifoWi		;r22 = FIFO write index

3:	ldi	XL, lo8(Buff)		;X = Buff + R22
	ldi	XH, hi8(Buff)		;
	add	XL, r22			;
	adc	XH, r1			;/
4:	lds	r24, FifoCt		;wait while FIFO full
	cpi	r24, 252		;
	brcc	4b			;/
#if MODE == 2	// Mono Hi-Res
	rcall	rcv_spi			;Get L-ch/Mono data into Z
	clr	ZL			;
	sbis	_FLAGS, 4		;
	rjmp	5f			;
	mov	ZL, r24			;
	rcall	rcv_spi			;
	subi	r24, 0x80		;
5:	mov	ZH, r24			;/
	sbis	_FLAGS, 1		;if Mono file, do not process R-ch data
	rjmp	8f			;/
	rcall	rcv_spi			;Get R-ch data and mix it to Z
	clr	r25			;
	sbis	_FLAGS, 4		;
	rjmp	6f			;
	mov	r25, r24		;
	rcall	rcv_spi			;
	subi	r24, 0x80		;
6:	add	ZL, r25			;
	adc	ZH, r24			;
	ror	ZH			;
	ror	ZL			;/
#elif MODE == 1	// Stereo
	sbic	_FLAGS, 4		;Get L-ch/Mono data into ZH, ZL
	rcall	rcv_spi			;
	rcall	rcv_spi			;
	sbic	_FLAGS, 4		;
	subi	r24, 0x80		;
	mov	ZL, r24			;
	mov	ZH, r24			;/
	sbis	_FLAGS, 1		;if Mono file, do not process R-ch data
	rjmp	8f			;/
	sbic	_FLAGS, 4		;Get R-ch data into ZL
	rcall	rcv_spi			;
	rcall	rcv_spi			;
	sbic	_FLAGS, 4		;
	subi	r24, 0x80		;
	mov	ZL, r24			;/
#else		// Mono OCL
	sbic	_FLAGS, 4		;Get L-ch/Mono data into ZH
	rcall	rcv_spi			;
	rcall	rcv_spi			;
	sbic	_FLAGS, 4		;
	subi	r24, 0x80		;
	mov	ZH, r24			;/
	sbis	_FLAGS, 1		;if Mono file, do not process R-ch data
	rjmp	5f			;/
	sbic	_FLAGS, 4		;Get R-ch data and mix it into ZH
	rcall	rcv_spi			;
	rcall	rcv_spi			;
	sbic	_FLAGS, 4		;
	subi	r24, 0x80		;
	add	ZH, r24			;
	ror	ZH			;/
5:	mov	ZL, ZH			;ZL = -ZH
	com	ZL			;/
#endif
8:	st	X+, ZL			;Store -/Rch/LSB data
	st	X+, ZH			;Store +/Lch/MSB data
	cli				;
	lds	r24, FifoCt		;
	subi	r24, -2			;
	sts	FifoCt, r24		;
	sei				;
	subi	r22, -2			;/

	subi	r20, lo8(1)		;while(--R21:R20)
	sbci	r21, hi8(1)		;
	brne	3b			;/
	sts	FifoWi, r22		;Save FIFO write index

fb_exit:
	ldi	r24, _BV(2)		;SCK(PA2)
9:	.rept 16			;Discard a byte on USI
	out	_SFR_IO_ADDR(PINA), r24	;
	.endr				;/
	subi	r18, lo8(1)		;Repeat r19:r18 times
	sbci	r19, hi8(1)		;
	brne	9b			;/

	ret
.endfunc



;---------------------------------------------------------------------------;
; ISR for Audio Output Interrupt
;---------------------------------------------------------------------------;
; ISR(TIMER0_COMPA_vect);
;
; Pop an audio sample from FIFO and put it to the DAC.

.global TIMER0_COMPA_vect
.func TIMER0_COMPA_vect
TIMER0_COMPA_vect:
	push	r24				;Save regs.
	in	r24, _SFR_IO_ADDR(SREG)		;
	push	r24				;
	push	ZL				;
	push	ZH				;/

	lds	r24, FifoCt			;Load FIFO data counter
	subi	r24, 2				;Check if a sample is available
	brcs	9f				; If not, exit function
	sts	FifoCt, r24			;Save FIFO data counter
	lds	ZL, FifoRi			;Get FIFO read index
	clr	ZH				;Z = pointer to the top of FIFO
	subi	ZL, lo8(-(Buff))		;
	sbci	ZH, hi8(-(Buff))		;/
	ld	r24, Z+				;Send -/Rch/LSB data to OC1A
	out	_SFR_IO_ADDR(OCR1A), r24	;/
	ld	r24, Z+				;Send +/Lch/MSB data to OC1B
	out	_SFR_IO_ADDR(OCR1B), r24	;/
	subi	ZL, lo8(Buff)			;Save FIFO read index
	sts	FifoRi, ZL			;/
9:
	pop	ZH				;Restore regs.
	pop	ZL				;
	pop	r24				;
	out	_SFR_IO_ADDR(SREG), r24		;
	pop	r24				;/
	reti
.endfunc

