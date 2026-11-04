/*
 * common.h
 *
 *  Created on: 2022��1��25��
 *      Author: hit
 */


//#include <pthread.h>


#define BASE_ADDRESS							0x80050000
//#define BASE_ADDRESS							0x80002000
#define REG0								0



#define HEAD_NUM 14
/*********************************************/
int mem_fd;  // �豸���
unsigned int buf_phy_addr; // ������ַ����

int myip_init(void);
int my_exit(void);
void *pmyip;  //�ڴ�ӳ��Ļ���ַ
#define BA_LEN                			0x1000

#define MOTORBOAT_mReadReg(BaseAddress, RegOffset) (((unsigned int *)pmyip)[(RegOffset)>>2])

#define MOTORBOAT_mWriteReg(BaseAddress, RegOffset, Data)  ((unsigned int*)pmyip)[(RegOffset)>>2] = Data



void read_reg(unsigned int ch, unsigned int *pData);
void write_reg(unsigned int ch, unsigned int  mData);

