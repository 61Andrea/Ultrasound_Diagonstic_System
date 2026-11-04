/*
 * A422.c
 *
 *  Created on: 2022Äê1ÔÂ25ÈÕ
 *      Author: hit
 */

#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <dirent.h>             
#include <time.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <ctype.h>
#include <signal.h>

unsigned int cal_cpu_occupy(CpuOccupy_p o, CpuOccupy_p n)
{
	unsigned int od, nd;
	unsigned int id, sd;
	unsigned int cpuUse;

	od = (o->user + o->nice + o->system + o->idle + o->softIrq + o->ioWait + o->irq);
	nd = (n->user + n->nice + n->system + n->idle + n->softIrq + n->ioWait + n->irq);
	id = (n->idle);
	sd = (o->idle);

	if((nd - od) != 0)
	{
		cpuUse = 100 - (id - sd) * 100/(nd - od);
	}
	else
	{
		cpuUse = 0;
	}

	return cpuUse;
}


void get_cpu_occupy(CpuOccupy_p cpuSt)
{
	FILE *fd;
	int n;
	char buff[256];
	CpuOccupy_p pCpuOccupy;
	
	pCpuOccupy = cpuSt;
	
	fd = fopen("/proc/stat", "r");

	if(fd == NULL)
	{
		perror("fopen:");
		exit(0);

	}

	fgets(buff, sizeof(buff), fd);

	sscanf(buff, "%s %u %u %u %u %u %u %u", pCpuOccupy->name, &pCpuOccupy->user, &pCpuOccupy->nice, &pCpuOccupy->system, &pCpuOccupy->idle, &pCpuOccupy->ioWait, &pCpuOccupy->irq, &pCpuOccupy->softIrq);
	
	fclose(fd);
}


unsigned int get_sys_cpu_usage(void)
{
	CpuOccupy_t tCpuStat1;
	CpuOccupy_t tCpuStat2;
	unsigned int cpu;
	get_cpu_occupy((CpuOccupy_p)&tCpuStat1);

	sleep(1);

	get_cpu_occupy((CpuOccupy_p)&tCpuStat2);

	cpu = cal_cpu_occupy((CpuOccupy_p)&tCpuStat1, (CpuOccupy_p)&tCpuStat2);

	return cpu;


}