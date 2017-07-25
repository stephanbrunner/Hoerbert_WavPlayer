/*-----------------------------------------------------------------------
/  PFF - Low level disk interface module include file    (C)ChaN, 2014
/-----------------------------------------------------------------------*/

#ifndef _DISKIO_DEFINED
#define _DISKIO_DEFINED

#ifdef __cplusplus
extern "C" {
	#endif

	#include "integer.h"


	/* Status of Disk Functions */
	typedef BYTE	DSTATUS;


	/* Results of Disk Functions */
	typedef enum {
		RES_OK = 0,		/* 0: Function succeeded */
		RES_ERROR,		/* 1: Disk error */
		RES_NOTRDY,		/* 2: Not ready */
		RES_PARERR		/* 3: Invalid parameter */
	} DRESULT;
	
	/* Card type flags (CardType) */
	#define CT_MMC                0x01    /* MMC version 3 */
	#define CT_SD1                0x02    /* SD version 1 */
	#define CT_SD2                0x04    /* SD version 2 */
	#define CT_SDC                (CT_SD1|CT_SD2)    /* SD */
	#define CT_BLOCK            0x08    /* Block addressing */

	/*---------------------------------------*/
	/* Prototypes for disk control functions */

	DSTATUS disk_initialize (void);
	DRESULT disk_readp (BYTE* buff, DWORD sector, UINT offser, UINT count);
	DRESULT disk_writep (const BYTE* buff, DWORD sc);
	
	#define STA_NOINIT		0x01	/* Drive not initialized */
	#define STA_NODISK		0x02	/* No medium in the drive */

	#ifdef __cplusplus
}
#endif

#endif	/* _DISKIO_DEFINED */
