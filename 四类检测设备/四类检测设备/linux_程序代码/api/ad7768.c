/*
 * led.c

 *
 *  Created on: 2023Äê5ÔÂ22ÈÕ
 *      Author: hit
 */

#include "common.h"
#include "ad7768.h"
#include "unistd.h"

#define AD7768_CH0_REG_IN0    ((unsigned int)90)
#define AD7768_CH0_REG_IN1    ((unsigned int)91)
#define AD7768_CH0_REG_IN2    ((unsigned int)92)
#define AD7768_CH0_REG_IN3    ((unsigned int)93)

#define AD7768_CH1_REG_IN0    ((unsigned int)95)
#define AD7768_CH1_REG_IN1    ((unsigned int)96)
#define AD7768_CH1_REG_IN2    ((unsigned int)97)
#define AD7768_CH1_REG_IN3    ((unsigned int)98)



#define AD7768_CH0_REG_OUT0    ((unsigned int)350)
#define AD7768_CH0_REG_OUT1    ((unsigned int)351)
#define AD7768_CH0_REG_OUT2    ((unsigned int)352)
#define AD7768_CH0_REG_OUT3    ((unsigned int)353)
#define AD7768_CH0_REG_OUT4    ((unsigned int)354)
#define AD7768_CH0_REG_OUT5    ((unsigned int)355)
#define AD7768_CH0_REG_OUT6    ((unsigned int)356)
#define AD7768_CH0_REG_OUT7    ((unsigned int)357)
#define AD7768_CH0_REG_OUT8    ((unsigned int)358)
#define AD7768_CH0_REG_OUT9    ((unsigned int)359)
#define AD7768_CH0_REG_OUT10    ((unsigned int)360)
#define AD7768_CH0_REG_OUT11    ((unsigned int)361)
#define AD7768_CH0_REG_OUT12    ((unsigned int)362)
#define AD7768_CH0_REG_OUT13    ((unsigned int)363)
#define AD7768_CH0_REG_OUT14    ((unsigned int)364)
#define AD7768_CH0_REG_OUT15    ((unsigned int)365)
#define AD7768_CH0_REG_OUT16    ((unsigned int)366)
#define AD7768_CH0_REG_OUT17    ((unsigned int)367)
#define AD7768_CH0_REG_OUT18    ((unsigned int)368)
#define AD7768_CH0_REG_OUT19    ((unsigned int)369)
#define AD7768_CH0_REG_OUT20    ((unsigned int)370)
#define AD7768_CH0_REG_OUT21    ((unsigned int)371)
#define AD7768_CH0_REG_OUT22    ((unsigned int)372)
#define AD7768_CH0_REG_OUT23    ((unsigned int)373)
#define AD7768_CH0_REG_OUT24    ((unsigned int)374)

#define AD7768_CH1_REG_OUT0    ((unsigned int)380)
#define AD7768_CH1_REG_OUT1    ((unsigned int)381)
#define AD7768_CH1_REG_OUT2    ((unsigned int)382)
#define AD7768_CH1_REG_OUT3    ((unsigned int)383)
#define AD7768_CH1_REG_OUT4    ((unsigned int)384)
#define AD7768_CH1_REG_OUT5    ((unsigned int)385)
#define AD7768_CH1_REG_OUT6    ((unsigned int)386)
#define AD7768_CH1_REG_OUT7    ((unsigned int)387)
#define AD7768_CH1_REG_OUT8    ((unsigned int)388)
#define AD7768_CH1_REG_OUT9    ((unsigned int)389)
#define AD7768_CH1_REG_OUT10    ((unsigned int)390)
#define AD7768_CH1_REG_OUT11    ((unsigned int)391)
#define AD7768_CH1_REG_OUT12    ((unsigned int)392)
#define AD7768_CH1_REG_OUT13    ((unsigned int)393)
#define AD7768_CH1_REG_OUT14    ((unsigned int)394)
#define AD7768_CH1_REG_OUT15    ((unsigned int)395)
#define AD7768_CH1_REG_OUT16    ((unsigned int)396)
#define AD7768_CH1_REG_OUT17    ((unsigned int)397)
#define AD7768_CH1_REG_OUT18    ((unsigned int)398)
#define AD7768_CH1_REG_OUT19    ((unsigned int)399)
#define AD7768_CH1_REG_OUT20    ((unsigned int)400)
#define AD7768_CH1_REG_OUT21    ((unsigned int)401)
#define AD7768_CH1_REG_OUT22    ((unsigned int)402)
#define AD7768_CH1_REG_OUT23    ((unsigned int)403)
#define AD7768_CH1_REG_OUT24    ((unsigned int)404)

#define bit0    ((unsigned int)0x01)
#define bit1    ((unsigned int)0x02)
#define bit2    ((unsigned int)0x04)
#define bit3    ((unsigned int)0x08)
#define bit4    ((unsigned int)0x10)
#define bit5    ((unsigned int)0x20)
#define bit6    ((unsigned int)0x40)
#define bit7    ((unsigned int)0x80)

void ad7768_sys_clk_div(unsigned int ch, unsigned int data)
{
	unsigned int myCh;
	switch(ch)
	{
		case 0:
			myCh = AD7768_CH0_REG_IN0;
			break;
		case 1:
			myCh = AD7768_CH1_REG_IN0;
			break;
	}
	write_reg(myCh, data);
}

void ad7768_pin_resetn(unsigned int ch, AD7768RegIn1_p pAD7768RegIn1)
{
	unsigned int myCh;
	AD7768RegIn1_n nAD7768RegIn1;

	switch(ch)
	{
		case 0: 
			myCh = AD7768_CH0_REG_IN1; 
			break;
		case 1: 
			myCh = AD7768_CH1_REG_IN1; 
			break;
	}
	
	nAD7768RegIn1.myBit.resetn = 0x00;
	nAD7768RegIn1.myBit.period = pAD7768RegIn1->period;
	nAD7768RegIn1.myBit.resetnSys = pAD7768RegIn1->resetnSys;
	write_reg(myCh, nAD7768RegIn1.u32);
	sleep(1);
	nAD7768RegIn1.myBit.resetn = 0x01;
	write_reg(myCh, nAD7768RegIn1.u32);
}


void ad7768_sys_resetn(unsigned int ch, AD7768RegIn1_p pAD7768RegIn1)
{
	unsigned int myCh;
	AD7768RegIn1_n nAD7768RegIn1;
	switch(ch)
	{
		case 0: 
			myCh = AD7768_CH0_REG_IN1; 
			break;
		case 1: 
			myCh = AD7768_CH1_REG_IN1; 
			break;
	}
	nAD7768RegIn1.myBit.resetn = pAD7768RegIn1->resetn;
	nAD7768RegIn1.myBit.period = pAD7768RegIn1->period;
	nAD7768RegIn1.myBit.resetnSys = 0x00;
	write_reg(myCh, nAD7768RegIn1.u32);
	sleep(1);
	nAD7768RegIn1.myBit.resetnSys = 0x01;
	write_reg(myCh, nAD7768RegIn1.u32);
}



void read_ad7768_fifo_stat(unsigned int ch, unsigned int subCh, AD7768FiFoStat_p pAD7768FiFoStat)
{
	unsigned int myCh;
	unsigned int data;
	AD7768FiFoStat_t myBit;
	
	if(ch == 0)
	{
		switch(subCh)
		{
			case 0: 
				myCh = AD7768_CH0_REG_OUT8; 
				break;
			case 1: 
				myCh = AD7768_CH0_REG_OUT9; 
				break;
			case 2: 
				myCh = AD7768_CH0_REG_OUT10; 
				break;
			case 3: 
				myCh = AD7768_CH0_REG_OUT11; 
				break;
			case 4: 
				myCh = AD7768_CH0_REG_OUT12; 
				break;
			case 5: 
				myCh = AD7768_CH0_REG_OUT13; 
				break;
			case 6: 
				myCh = AD7768_CH0_REG_OUT14; 
				break;
			case 7: 
				myCh = AD7768_CH0_REG_OUT15; 
				break;
			default: break;
		}
	}
	
	if(ch == 1)
	{
		switch(subCh)
		{
			case 0: 
				myCh = AD7768_CH1_REG_OUT8; 
				break;
			case 1: 
				myCh = AD7768_CH1_REG_OUT9; 
				break;
			case 2: 
				myCh = AD7768_CH1_REG_OUT10; 
				break;
			case 3: 
				myCh = AD7768_CH1_REG_OUT11; 
				break;
			case 4: 
				myCh = AD7768_CH1_REG_OUT12; 
				break;
			case 5: 
				myCh = AD7768_CH1_REG_OUT13; 
				break;
			case 6: 
				myCh = AD7768_CH1_REG_OUT14; 
				break;
			case 7: 
				myCh = AD7768_CH1_REG_OUT15; 
				break;
			default: break;
		}
	}
	read_reg(myCh, &data);
	
	pAD7768FiFoStat->fifoCount = data & 0x000003FF;
	pAD7768FiFoStat->full = (data & 0x40000000)>>30;
	pAD7768FiFoStat->empty = (data & 0x80000000)>>31;	
}

void ad7768_fifo_read_en(unsigned int ch, unsigned int subCh)
{
	unsigned int data;
	if(ch == 0)
	{
		switch(subCh)
		{
			case 0: 
				data = bit0; 
				break;
			case 1: 
				data = bit1; 
				break;
			case 2: 
				data = bit2; 
				break;
			case 3: 
				data = bit3; 
				break;
			case 4: 
				data = bit4; 
				break;
			case 5: 
				data = bit5; 
				break;
			case 6: 
				data = bit6; 
				break;
			case 7: 
				data = bit7; 
				break;
			default: break;
		}
		write_reg(AD7768_CH0_REG_IN2, data);
		write_reg(AD7768_CH0_REG_IN2, 0x00);
	}
	
	if(ch == 1)
	{
		switch(subCh)
		{
			case 0: 
				data = bit0; 
				break;
			case 1: 
				data = bit1; 
				break;
			case 2: 
				data = bit2; 
				break;
			case 3: 
				data = bit3; 
				break;
			case 4: 
				data = bit4; 
				break;
			case 5: 
				data = bit5; 
				break;
			case 6: 
				data = bit6; 
				break;
			case 7: 
				data = bit7; 
				break;
			default: break;
		}
		write_reg(AD7768_CH1_REG_IN2, data);
		write_reg(AD7768_CH1_REG_IN2, 0x00);
	}
}

void ad7768_rfifo_buffer(unsigned int ch, unsigned int subCh, unsigned int *ad7768Buffer, unsigned int num)
{
	unsigned int i = 0;
	unsigned int data;
	
	for(i = 0; i <= num - 1; i++)
	{
		ad7768_fifo_read_en(ch, subCh);
		//read_ad7768_fifo_data(ch, &data);
		//ad7768Buffer[i] = data;
		read_ad7768_fifo_data(ch, subCh, ad7768Buffer + i);
	}
}

void read_ad7768_fifo_data(unsigned int ch, unsigned int subCh, unsigned int *pdata)
{
	unsigned int myCh;
	
	if(ch == 0)
	{
		switch(subCh)
		{
			case 0: 
				myCh = AD7768_CH0_REG_OUT16; 
				break;
			case 1: 
				myCh = AD7768_CH0_REG_OUT17; 
				break;
			case 2: 
				myCh = AD7768_CH0_REG_OUT18; 
				break;
			case 3: 
				myCh = AD7768_CH0_REG_OUT19; 
				break;
			case 4: 
				myCh = AD7768_CH0_REG_OUT20; 
				break;
			case 5: 
				myCh = AD7768_CH0_REG_OUT21; 
				break;
			case 6: 
				myCh = AD7768_CH0_REG_OUT22; 
				break;
			case 7: 
				myCh = AD7768_CH0_REG_OUT23; 
				break;
			default: break;
		}
	}
	
	if(ch == 1)
	{
		switch(subCh)
		{
			case 0: 
				myCh = AD7768_CH1_REG_OUT16; 
				break;
			case 1: 
				myCh = AD7768_CH1_REG_OUT17; 
				break;
			case 2: 
				myCh = AD7768_CH1_REG_OUT18; 
				break;
			case 3: 
				myCh = AD7768_CH1_REG_OUT19; 
				break;
			case 4: 
				myCh = AD7768_CH1_REG_OUT20; 
				break;
			case 5: 
				myCh = AD7768_CH1_REG_OUT21; 
				break;
			case 6: 
				myCh = AD7768_CH1_REG_OUT22; 
				break;
			case 7: 
				myCh = AD7768_CH1_REG_OUT23; 
				break;
			default: break;
		}
	}
	read_reg(myCh, pdata);
}

void ad7768_get_sample_rate(unsigned int ch, unsigned int *pdata)
{
	unsigned int myCh;

	switch(ch)
	{
		case 0: 
			myCh = AD7768_CH0_REG_OUT24; 
			break;
		case 1: 
			myCh = AD7768_CH1_REG_OUT24; 
			break;
		default: break;
	}
	read_reg(myCh, pdata);
	
}

void ad7768_set_mclk(unsigned int ch, unsigned int data)
{
	unsigned int myCh;

	switch(ch)
	{
		case 0:
			myCh = AD7768_CH0_REG_IN3;
			break;
		case 1:
			myCh = AD7768_CH1_REG_IN3;
			break;
		default: break;
	}
	write_reg(myCh, data);
}

void read_ad7768_data(unsigned int ch, AD7768Data_p pAD7768Data)
{
	switch(ch)
	{
		case 0: 
			read_reg(AD7768_CH0_REG_OUT0, &pAD7768Data->ch0);
			read_reg(AD7768_CH0_REG_OUT1, &pAD7768Data->ch1);
			read_reg(AD7768_CH0_REG_OUT2, &pAD7768Data->ch2);
			read_reg(AD7768_CH0_REG_OUT3, &pAD7768Data->ch3);
			read_reg(AD7768_CH0_REG_OUT4, &pAD7768Data->ch4);
			read_reg(AD7768_CH0_REG_OUT5, &pAD7768Data->ch5);
			read_reg(AD7768_CH0_REG_OUT6, &pAD7768Data->ch6);
			read_reg(AD7768_CH0_REG_OUT7, &pAD7768Data->ch7);
			break;
		case 1: 
			read_reg(AD7768_CH1_REG_OUT0, &pAD7768Data->ch0);
			read_reg(AD7768_CH1_REG_OUT1, &pAD7768Data->ch1);
			read_reg(AD7768_CH1_REG_OUT2, &pAD7768Data->ch2);
			read_reg(AD7768_CH1_REG_OUT3, &pAD7768Data->ch3);
			read_reg(AD7768_CH1_REG_OUT4, &pAD7768Data->ch4);
			read_reg(AD7768_CH1_REG_OUT5, &pAD7768Data->ch5);
			read_reg(AD7768_CH1_REG_OUT6, &pAD7768Data->ch6);
			read_reg(AD7768_CH1_REG_OUT7, &pAD7768Data->ch7);
			break;
		default: break;
	}

}

