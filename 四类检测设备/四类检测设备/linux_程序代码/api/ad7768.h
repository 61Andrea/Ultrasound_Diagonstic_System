/*
 * led.h
 *
 *  Created on: 2023Äê5ÔÂ22ÈÕ
 *      Author: hit
 */

typedef struct {
	unsigned int period : 16;
	unsigned int reserved0 : 14;
	unsigned int resetnSys : 1;
	unsigned int resetn : 1;
} AD7768RegIn1_t, *AD7768RegIn1_p;

typedef union
{
	unsigned int u32;
	AD7768RegIn1_t myBit;
} AD7768RegIn1_n;


typedef struct{
	unsigned int fifoCount : 10;
	unsigned int reserved0 : 20;
	unsigned int full : 1;
	unsigned int empty : 1;
} AD7768FiFoStat_t, *AD7768FiFoStat_p;

typedef struct{
	unsigned int ch0;
	unsigned int ch1;
	unsigned int ch2;
	unsigned int ch3;
	unsigned int ch4;
	unsigned int ch5;
	unsigned int ch6;
	unsigned int ch7;
} AD7768Data_t, *AD7768Data_p;


#define AD7768_NUM    ((unsigned int)2)

void ad7768_sys_clk_div(unsigned int ch, unsigned int data);
void ad7768_pin_resetn(unsigned int ch, AD7768RegIn1_p pAD7768RegIn1);
void ad7768_sys_resetn(unsigned int ch, AD7768RegIn1_p pAD7768RegIn1);
void read_ad7768_fifo_stat(unsigned int ch, unsigned int subCh, AD7768FiFoStat_p pAD7768FiFoStat);
void ad7768_fifo_read_en(unsigned int ch, unsigned int subCh);
void ad7768_rfifo_buffer(unsigned int ch, unsigned int subCh, unsigned int *ad7768Buffer, unsigned int num);
void read_ad7768_fifo_data(unsigned int ch, unsigned int subCh, unsigned int *pdata);
void ad7768_get_sample_rate(unsigned int ch, unsigned int *pdata);
void ad7768_set_mclk(unsigned int ch, unsigned int data);
void read_ad7768_data(unsigned int ch, AD7768Data_p pAD7768Data);
