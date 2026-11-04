/******************************************************************************
*
* Copyright (C) 2009 - 2014 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/

/*
 * helloworld.c: simple test application
 *
 * This application configures UART 16550 to baud rate 9600.
 * PS7 UART (Zynq) is not initialized by this application, since
 * bootrom/bsp configures it to baud rate 115200
 *
 * ------------------------------------------------
 * | UART TYPE   BAUD RATE                        |
 * ------------------------------------------------
 *   uartns550   9600
 *   uartlite    Configurable only in HW design
 *   ps7_uart    115200 (configured by bootrom/bsp)
 */


/****************************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "signal.h"
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <dirent.h>              //文件夹操作
#include <arpa/inet.h>//htons() sockaddr_in
#include <string.h>//bzero()
#include <sys/socket.h>//socket
#include <stdlib.h>//exit()
#include<sys/wait.h>//waitpid();
#include <pthread.h>
#include "../api/fun.h"
#include <time.h>
/****************************************************************************************/
#define MAIN_DBG_PRINT
//#define MAIN_DBG_PRINT2
#define BUFFER_SIZE 1024


/****************************************************************************************/
int main(void)
{
	if(0 != myip_init())
	{
		perror("Failed to myip_can_init");
		exit(-1);
	}
/****************************************************************************************/
	char command[256];
 	char interface_name[] = "eth0"; // 以太网接口名字
    char ip_address[] = PS_IP_ADDRESS; // 设置的IP地址
    char netmask[] = "255.255.255.0"; // 子网掩码
    char gateway[] = "192.168.1.1"; // 默认网关

    sprintf(command, "ifconfig %s %s netmask %s up", interface_name, ip_address, netmask);
    system(command);

    // 配置默认网关
    sprintf(command, "route add default gw %s %s", gateway, interface_name);
    system(command);


    char interface_name1[] = "eth1"; // 以太网接口名字
    char ip_address1[] = PL_IP_ADDRESS; // 设置的IP地址
    char netmask1[] = "255.255.255.0"; // 子网掩码
    char gateway1[] = "192.168.3.1"; // 默认网关

    sprintf(command, "ifconfig %s %s netmask %s up", interface_name1, ip_address1, netmask1);
    system(command);
	
	// 配置默认网关
    sprintf(command, "route add default gw %s %s", gateway1, interface_name1);
    system(command);

	// system("ifconfig eth0 down");
	// system("ifconfig eth0 hw ether 00:0a:35:00:22:02");
	// system("ifconfig eth0 up");
	
	// system("ifconfig eth1 down");
	// system("ifconfig eth1 hw ether 00:0a:35:00:22:03");
	// system("ifconfig eth1 up");


/////////////////////////////////////////////////////////////////////////////////////////////新增	
	// --- eth0 配置 ---
    system("ifconfig eth0 down");
    system("ifconfig eth0 hw ether 00:0a:35:00:22:02");
    // 修改这一行：启动的同时分配 1 网段 IP
    system("ifconfig eth0 192.168.1.10 netmask 255.255.255.0 up");
    
    // --- eth1 配置 (连接超声板卡)  ---
    system("ifconfig eth1 down");
    system("ifconfig eth1 hw ether 00:0a:35:00:22:03");
    //  eth1 用的是 3网段
    system("ifconfig eth1 192.168.3.20 netmask 255.255.255.0 up");

    // 【关键】增加延迟：给硬件 PHY 芯片 2 秒钟时间完成链路协商（Link Up）
    // 否则网卡还没“跳”起来，后面的线程就开始 connect，还是会失败
    printf("[Main] Waiting for Ethernet link up...\n");
    sleep(5);
/////////////////////////////////////////////////////////////////////////////////////////////////////

/****************************************************************************************/
	// system("ifconfig eth0 down");
	// system("ifconfig eth0 hw ether 00:0a:35:00:22:02");
	// system("ifconfig eth0 up");
	
	// system("ifconfig eth1 down");
	// system("ifconfig eth1 hw ether 00:0a:35:00:22:03");
	// system("ifconfig eth1 up");
/*********************************thread*************************************************/
	pthread_t thTcpServerPs;
	pthread_t thTcpServerPl;
	pthread_create(&thTcpServerPs, NULL, thread_tcp_server_ps, NULL);
	//pthread_create(&thTcpServerPl, NULL, thread_tcp_server_pl, NULL);
/****************************************************************************************/	
/////////////////////////////////////////////////////////////////////////////////超声部分线程
// =================================================================
	// pthread_t acq_thread;
	// AcqParams params;
    // strcpy(params.ip_address, "192.168.3.12");
    // if (pthread_create(&acq_thread, NULL, Thread_Uti_ctrl, &params) != 0) {
    //     printf("Error: Failed to create acquisition thread.\n");
    //     return -1;
    // }

    // 2. 直接进入你的业务主循环 (删掉 for循环睡眠10秒 和 pthread_join)
    //printf("--- All Threads Started, entering main loop ---\n");
    
    fun_init(); 
    printf("start\n");
///////////////////////////////////////////////////////////////////////////////////////////////
	
	fun_init();	
	printf("start\n");

	while(1)
	{	
		deal_fun();
		usleep(10);	
	}

}
