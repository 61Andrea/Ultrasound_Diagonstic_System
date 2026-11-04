/*
 * fun.h
 *
 *  Created on: 2023��5��22��
 *      Author: hit
 */

#ifndef FUN_H
#define FUN_H

#include <pthread.h>
#include "common.h"
#include "ad7915.h"
#include "ad7768.h"
#include "api_file.h"
#include "cpu.h"
#include "parse_file.h"
#include "net.h"
#include "clock.h"

#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#define PS_IP_ADDRESS "192.168.1.10"
#define PL_IP_ADDRESS "192.168.3.20"


/////////////////////////////////////////////////////////////////////////////////////////////超声部分


#include <stdbool.h> // 必须包含这个才能用 bool

// =====================================================================
// 1. 结构体与宏定义 (确保 AcqParams 在这里定义)
// =====================================================================
typedef struct {
    char ip_address[32];
    // 这里可以添加其他配置参数，如端口号等
} AcqParams;

// =====================================================================
// 2. 全局变量的外部声明 (使用 extern)
// 这里的 extern 告诉编译器：这些变量在别处（fun.c）定义了，这里只是引用
// =====================================================================
extern pthread_t g_utThreadId;
extern volatile bool keep_acquiring;
extern volatile bool start_config_trigger;
extern AcqParams g_utParams;

// =====================================================================
// 3. 函数原型声明
// =====================================================================

// 启动采集线程 (对应 201)
void cis_start_thread(void);

// 触发配置与运行 (对应 202)
void cis_trigger_config(void);

// 停止一切并销毁线程 (对应 206)
void cis_stop_everything(void);

// 采集线程函数本身 (如果需要在其他地方手动管理)
void* Thread_Uti_ctrl(void* arg);

#define CMD_UT_THREAD_INIT    201  // 启动后台线程（待命）
#define CMD_UT_CONFIG_RUN     202  // 配置参数并正式开工
#define CMD_UT_CLOSE_DEVICE   206  // 停止采集并销毁线程

// fun.h

#define MAX_CH 8

typedef struct {
    float delay;            // 延迟时间 (4 bytes)
    int range;              // 监测范围 (4 bytes)
    int filter;             // 滤波器 (4 bytes)
    int detect_method;      // 检波方法 (4 bytes)
    int pulse_width;        // 发射脉宽 (4 bytes)
    int data_len;           // 数据长度 (4 bytes)
    int tgc;                // TGC (如果是一个数据用int，两个建议用int tgc[2])
    int gain;               // 增益
    int trigger_mode;       // 触发模式
} __attribute__((packed)) HardwareConfig; 

// 声明全局配置数组（供各个 .c 文件使用）
extern HardwareConfig g_hwConfigs[MAX_CH]; 

// 顺便在这里声明你的通道激活标志，逻辑更统一
extern volatile bool ch_active_mask[MAX_CH];

// fun.h
extern volatile int g_master_data_len;



/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/****************************************************************************************/
typedef struct
{	
	unsigned char iepe0Status;			
	unsigned char iepe1Status;					
	unsigned char iepe2Status;					
	unsigned char iepe3Status;					
	unsigned char current0Status;				
	unsigned char current1Status;				
	unsigned char current2Status;				
	unsigned char current3Status;				
	unsigned char current4Status;		
	unsigned char current5Status;
	unsigned char current6Status;
	unsigned char current7Status;
	unsigned char voltage0Status;
	unsigned char voltage1Status;
	unsigned char voltage2Status;
	unsigned char voltage3Status;
	unsigned char voltage4Status;
	unsigned char voltage5Status;
	unsigned char voltage6Status;
	unsigned char voltage7Status;
}DeviceStruct_t, *DeviceStruct_p;
/****************************************************************************************/
void deal_fun(void);
void fun_init(void);
void* thread_tcp_server_ps(void *args);
//void* thread_tcp_server_pl(void *args);
#endif // 结束 FUN_H