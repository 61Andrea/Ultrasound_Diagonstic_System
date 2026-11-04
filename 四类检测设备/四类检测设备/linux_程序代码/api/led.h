/*
 * fun.h
 *
 *  Created on: 2023Äê5ÔÂ22ÈÕ
 *      Author: hit
 */



typedef struct {
	unsigned int led0 : 1;
	unsigned int led1 : 1;
	unsigned int led2 : 1;
	unsigned int led3 : 1;
	unsigned int led4 : 1;
	unsigned int led5 : 1;
	unsigned int rsvd1 : 24;
} LedReg_t;

typedef union
{
	unsigned int myWord;
	LedReg_t myBit;
} LedReg_n;



void fun_led_int(void);
unsigned int fun_set_led(unsigned int data);






