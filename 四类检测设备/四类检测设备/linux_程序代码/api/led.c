/*
 * fun.c
 *
 *  Created on: 2023Äê5ÔÂ22ÈÕ
 *      Author: hit
 */


#include "led.h"
#include "common.h"

#define LED_REG_IN0    ((unsigned int)100)



void fun_led_int(void)
{
	write_reg(LED_REG_IN0, 0x00);

}
unsigned int fun_set_led(unsigned int data)
{
	write_reg(LED_REG_IN0, data);
}
