
#include "Arduino.h"

#ifndef __LINEINFO_H__
#define __LINEINFO_H__

typedef struct
{
	uint32_t	dwDTERate;
	uint8_t		bCharFormat;
	uint8_t 	bParityType;
	uint8_t 	bDataBits;
	uint8_t		lineState;
} LineInfo;


#endif