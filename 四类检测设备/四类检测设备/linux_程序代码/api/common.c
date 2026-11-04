/*
 * common.c
 *
 *  Created on: 2022��1��25��
 *      Author: hit
 */

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>





void read_reg(unsigned int ch, unsigned int *pData)
{
	//*pData = MOTORBOAT_mReadReg(BASE_ADDRESS, REG0 + ch << 2);
    // 给 ch << 2 加上括号！
    *pData = MOTORBOAT_mReadReg(BASE_ADDRESS, REG0 + (ch << 2));
}


// void write_reg(unsigned int ch, unsigned int  mData)
// {
// 	MOTORBOAT_mWriteReg(BASE_ADDRESS, REG0 + ch << 2, mData);
// }

// 找到你的 write_reg 函数，改成下面这样：
void write_reg(unsigned int ch, unsigned int data)
{
    // 同样，必须给 ch << 2 加上括号！
    MOTORBOAT_mWriteReg(BASE_ADDRESS, REG0 + (ch << 2), data);
}



/*********************************************
* ������         :myip_init()
* ��������		 :���ļ��������ڴ�ӳ��
* ����           : ��
* ���         	 : 0���ɹ� -1��ʧ��
*********************************************/

int myip_init(void)
{
    buf_phy_addr = BASE_ADDRESS;
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd == -1)
    {
        perror("Failed to open /dev/mem");
        return -1;
    }
    //fprintf(stderr,"[DEBUG] The physical buffer address is at %08x\n", buf_phy_addr);


    pmyip = mmap(NULL, BA_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, (off_t)buf_phy_addr);
    if (NULL == pmyip)
    {
        perror("Failed to mmap the rec buffer");
        if (mem_fd >= 0) close(mem_fd);
        return -1;
    }
    return 0 ;
}
/************************************
* ������         :my_exit()
* ��������		 :�ر��ļ���ȡ���ڴ�ӳ��
* ����           : ��
* ���         	 : ��
************************************/
int my_exit(void)
{
    close (mem_fd);
    munmap (pmyip, BA_LEN);
}

