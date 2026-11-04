/*
 * pwm.c
 *
 *  Created on: 2023Äê5ÔÂ22ÈÕ
 *      Author: hit
 */


#include "clock.h"
#include "common.h"


#define TIME_REG_IN0    ((unsigned int)4)

#define TIME_REG_OUT0    ((unsigned int)309)
#define TIME_REG_OUT1    ((unsigned int)310)


void time_div(TimeReg_p pTimeReg, unsigned int data)
{
	TimeReg_n nTimeReg;
	//unsigned int *pLedReg = NULL;
	unsigned int *pDate;

	pTimeReg->timeDiv = data - 1;
	
	pDate = (unsigned int *) pTimeReg;
	write_reg(TIME_REG_IN0, pDate[0]);
}

void time_reset(TimeReg_p pTimeReg, unsigned int data)
{
	TimeReg_n nTimeReg;
	//unsigned int *pLedReg = NULL;
	unsigned int *pDate;

	pTimeReg->reset = data;
	
	pDate = (unsigned int *) pTimeReg;
	write_reg(TIME_REG_IN0, pDate[0]);
	
}

void init_time(TimeReg_p pTimeReg)
{
	pTimeReg->timeDiv = 1000 - 1;
	pTimeReg->reset = 0;
}



void get_time(unsigned int *lData, unsigned int *hData)
{
	read_reg(TIME_REG_OUT0, lData);
	read_reg(TIME_REG_OUT1, hData);
}
