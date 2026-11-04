/*
 * pwm.h
 *
 *  Created on: 2023Äê5ÔÂ22ÈÕ
 *      Author: hit
 */


typedef struct{
	unsigned int timeDiv : 16;
	unsigned int reserved0 : 15;
	unsigned int reset : 1;
} TimeReg_t, *TimeReg_p;

typedef union
{
	unsigned int u32;
	TimeReg_t myBit;
} TimeReg_n;




void time_div(TimeReg_p pTimeReg, unsigned int data);
void time_reset(TimeReg_p pTimeReg, unsigned int data);
void init_time(TimeReg_p pTimeReg);
void get_time(unsigned int *lData, unsigned int *hData);

