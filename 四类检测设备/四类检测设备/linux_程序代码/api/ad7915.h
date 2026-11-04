/*
 * led.h
 *
 *  Created on: 2023Äê5ÔÂ22ÈÕ
 *      Author: hit
 */
 
 
typedef struct {
	unsigned int period : 24;
	unsigned int reserved0 : 7;
	unsigned int resetnSys : 1;
} AD7915RegIn1_t, *AD7915RegIn1_p;

typedef union
{
	unsigned int u32;
	AD7915RegIn1_t myBit;
} AD7915RegIn1_n;


typedef struct{
	unsigned int fifoCount : 10;
	unsigned int reserved0 : 20;
	unsigned int full : 1;
	unsigned int empty : 1;
} AD7915FiFoStat_t, *AD7915FiFoStat_p;

typedef struct {
	unsigned int convstTime : 8;
	unsigned int enTime : 8;
	unsigned int sclkTime : 8;
	unsigned int reserved0 : 8;
} AD7915RegIn3_t, *AD7915RegIn3_p;

typedef union
{
	unsigned int u32;
	AD7915RegIn3_t myBit;
} AD7915RegIn3_n;

#define AD7915_NUM    ((unsigned int)2)

void ad7915_sys_clk_div(unsigned int ch, unsigned int data);
void ad7915_set_time(unsigned int ch, unsigned int data);
void init_ad7915(unsigned int ch, AD7915RegIn1_p pAD7915RegIn1);
void read_ad7915_fifo_stat(unsigned int ch, AD7915FiFoStat_p pAD7915FiFoStat);
void ad7915_fifo_read_en(unsigned int ch);
void ad7915_rfifo_buffer(unsigned int ch, unsigned int *ad7915Buffer, unsigned int num);
void read_ad7915_fifo_data(unsigned int ch, unsigned int *pdata);
void ad7915_get_sample_rate(unsigned int ch, unsigned int *pdata);
void ad7915_set_sample_rate(unsigned int ch, unsigned int frequency, AD7915RegIn1_p pAD7915RegIn1);

