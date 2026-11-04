/*
 * A422.h
 *
 *  Created on: 2022Äê1ÔÂ25ÈÕ
 *      Author: hit
 */

typedef struct 
{
	char name[20];
	unsigned int user;
	unsigned int nice;
	unsigned int system;
	unsigned int idle;
	unsigned int ioWait;	
	unsigned int irq;
	unsigned int softIrq;

}CpuOccupy_t, *CpuOccupy_p;


unsigned int cal_cpu_occupy(CpuOccupy_p o, CpuOccupy_p n);
void get_cpu_occupy(CpuOccupy_p cpuSt);
unsigned int get_sys_cpu_usage(void);