/*
 * led.c

 *
 *  Created on: 2023Äê5ÔÂ22ÈÕ
 *      Author: hit
 */

#include "common.h"
#include "ad7915.h"

#define AD7915_CH0_REG_IN0    ((unsigned int)80)
#define AD7915_CH0_REG_IN1    ((unsigned int)81)
#define AD7915_CH0_REG_IN2    ((unsigned int)82)
#define AD7915_CH0_REG_IN3    ((unsigned int)83)

#define AD7915_CH1_REG_IN0    ((unsigned int)85)
#define AD7915_CH1_REG_IN1    ((unsigned int)86)
#define AD7915_CH1_REG_IN2    ((unsigned int)87)
#define AD7915_CH1_REG_IN3    ((unsigned int)88)

#define AD7915_CH0_REG_OUT0    ((unsigned int)340)
#define AD7915_CH0_REG_OUT1    ((unsigned int)341)
#define AD7915_CH0_REG_OUT2    ((unsigned int)342)

#define AD7915_CH1_REG_OUT0    ((unsigned int)345)
#define AD7915_CH1_REG_OUT1    ((unsigned int)346)
#define AD7915_CH1_REG_OUT2    ((unsigned int)347)



void ad7915_sys_clk_div(unsigned int ch, unsigned int data)
{
	unsigned int myCh;
	switch(ch)
	{
		case 0:
			myCh = AD7915_CH0_REG_IN0;
			break;
		case 1:
			myCh = AD7915_CH1_REG_IN0;
			break;
	}
	write_reg(myCh, data);
}


void ad7915_set_time(unsigned int ch, unsigned int data)
{
	unsigned int myCh;
	switch(ch)
	{
		case 0:
			myCh = AD7915_CH0_REG_IN3;
			break;
		case 1:
			myCh = AD7915_CH1_REG_IN3;
			break;
	}
	write_reg(myCh, data);
}


void init_ad7915(unsigned int ch, AD7915RegIn1_p pAD7915RegIn1)
{
	unsigned int myCh;
	AD7915RegIn1_n nAD7915RegIn1;
	switch(ch)
	{
		case 0: 
			myCh = AD7915_CH0_REG_IN1; 
			break;
		case 1: 
			myCh = AD7915_CH1_REG_IN1; 
			break;
	}

	nAD7915RegIn1.myBit.period = pAD7915RegIn1->period;
	nAD7915RegIn1.myBit.resetnSys = pAD7915RegIn1->resetnSys;
	write_reg(myCh, nAD7915RegIn1.u32);
}




void read_ad7915_fifo_stat(unsigned int ch, AD7915FiFoStat_p pAD7915FiFoStat)
{
	unsigned int myCh;
	unsigned int data;
	AD7915FiFoStat_t myBit;
	
	switch(ch)
	{
		case 0: 
			myCh = AD7915_CH0_REG_OUT0; 
			break;
		case 1: 
			myCh = AD7915_CH1_REG_OUT0; 
			break;
		default: break;
	}
	read_reg(myCh, &data);
	
	pAD7915FiFoStat->fifoCount = data & 0x000003FF;
	pAD7915FiFoStat->full = (data & 0x40000000)>>30;
	pAD7915FiFoStat->empty = (data & 0x80000000)>>31;	
}

void ad7915_fifo_read_en(unsigned int ch)
{
	unsigned int myCh;
	switch(ch)
	{
		case 0: 
			myCh = AD7915_CH0_REG_IN2; 
			break;
		case 1: 
			myCh = AD7915_CH1_REG_IN2; 
			break;
		default: break;
	}
	write_reg(myCh, 0x01);
	write_reg(myCh, 0x00);
}

void ad7915_rfifo_buffer(unsigned int ch, unsigned int *ad7915Buffer, unsigned int num)
{
	unsigned int i = 0;
	unsigned int data;
	
	for(i = 0; i <= num - 1; i++)
	{
		ad7915_fifo_read_en(ch);
		//read_ad7915_fifo_data(ch, &data);
		//ad7915Buffer[i] = data;
		read_ad7915_fifo_data(ch, ad7915Buffer + i);
	}
}

void read_ad7915_fifo_data(unsigned int ch, unsigned int *pdata)
{
	unsigned int myCh;
	switch(ch)
	{
		case 0: 
			myCh = AD7915_CH0_REG_OUT1; 
			break;
		case 1: 
			myCh = AD7915_CH1_REG_OUT1; 
			break;
		default: break;
	}
	read_reg(myCh, pdata);
}

void ad7915_get_sample_rate(unsigned int ch, unsigned int *pdata)
{
	unsigned int myCh;

	switch(ch)
	{
		case 0: 
			myCh = AD7915_CH0_REG_OUT2; 
			break;
		case 1: 
			myCh = AD7915_CH1_REG_OUT2; 
			break;
	}
	read_reg(myCh, pdata);
	
}

void ad7915_set_sample_rate(unsigned int ch, unsigned int frequency, AD7915RegIn1_p pAD7915RegIn1)
{
	unsigned int myCh;
	AD7915RegIn1_n nAD7915RegIn1;

	switch(ch)
	{
		case 0: 
			myCh = AD7915_CH0_REG_IN1; 
			break;
		case 1: 
			myCh = AD7915_CH1_REG_IN1; 
			break;
	}

	nAD7915RegIn1.myBit.period = pAD7915RegIn1->period;
	nAD7915RegIn1.myBit.resetnSys = pAD7915RegIn1->resetnSys;
	write_reg(myCh, nAD7915RegIn1.u32);
	
}
