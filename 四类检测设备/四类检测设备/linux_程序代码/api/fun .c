    /*
 * fun.c
 *
 *  Created on: 2023年5月22日
 *      Author: hit
 */

#include "fun.h"
#include <stdio.h>

//#define PS_IP_ADDRESS "192.168.1.10"
#define PS_PORT 5001
//#define PL_IP_ADDRESS "192.168.1.11"
#define PL_PORT 5002

#define CMD_SEARCH_FILES 5004
#define CMD_EXPORT_AND_TRANSFER_FILE 5003
#define CMD_DELETE_OLD_FILES 5005
#define CMD_QUERY_DISK_USAGE 5006
#define CMD_START 5007
#define CMD_END 5008
#define CMD_STORAGE_SETTING 5009
#define CMD_TEST 0xAA
#define CMD_TEST1 0x55
#define FILE_TRANSFER_COMPLETE 0xFFFF
#define PACKAGE_SIZE 8200                 //每包一百帧共8200字节

#define PS_TEMP_FILE "/sys/bus/iio/devices/iio:device0/in_temp0_ps_temp_raw"
/****************************************************************************************/

//#define AD7915_DBG_PRINT	
#define TCP_DBG_PRINT	
#define FUN_DBG_PRINT	
#define BUFFER_SIZE 1024
#define FILENAME_LENGTH1 1024
/****************************************************************************************/
#define LED_REG_IN0    			((unsigned int)0)



#define AD7915_CH     	2
#define AD7768_CH     	1

//0x00 存储功能关；	
//0x01 存到SD"/mnt/file";	
//0x02 存到固态里"/run/media/nvme0n1p1/file/";
//0x03 存到固态里"/run/media/nvme0n1p1/file/iepe/"; 
//0x04 存到固态里"/run/media/nvme0n1p1/file/normal/"; 
#define      SAVE_MODE				1								
						

#define LIMIT_TIME_1S    			((unsigned int)10)

/////////////////////////////////////////////////////////////////////////////////////////////超声部分
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h> 
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h> // === 【必须包含 pthread 库】 ===
#include <time.h>
#include <math.h>

#include "uti_sdk.h"

#include <arpa/inet.h>

#define MAX_FILE_SIZE (10 * 1024 * 1024)

// 定义一个结构体，用于关联文件名和它的修改时间
typedef struct {
    char filename[64];
    time_t mtime;
} FileInfo_t;



// 在 fun.c 中
pthread_t g_utThreadId = 0;
volatile bool keep_acquiring = true;
volatile bool start_config_trigger = false;
AcqParams g_utParams = { .ip_address = "192.168.3.12" }; // 默认指向 PL 端板卡

static int waiting_for_config_payload = 0;
HardwareConfig g_hwConfig;

//volatile int g_master_data_len = 256; // 默认值，确保绝不为 0



// 通道配置结构体保持不变，但我们要定义一个数组
HardwareConfig g_hwConfigs[MAX_CH]; 

// 存储控制标志
volatile bool start_storage_flag = false;
// 记录哪些通道被配置过了（被配置过的才开启存储）
volatile bool ch_active_mask[MAX_CH] = {false};

// 【新增】：网络推送控制标志
volatile bool start_upload_flag = false;

#define MAX_POINTS 4096

// 二维数组：行是通道号，列是波形数据
// new_data 保留原始采集数据；algo_data 保存转换后的算法输入数据。
int new_data[MAX_CH][MAX_POINTS] = {0};
float algo_data[MAX_CH][MAX_POINTS] = {0.0f};

/* ===================== 算法输入转换层 =====================
 * 说明：
 * - new_data 始终保存原始采集值，供调试、TCP 原始数据推送和原始文件保存使用。
 * - algo_data 保存送入 Python/ONNX 算法前的物理量/模型输入值。
 * - 当前对逻辑电压通道 4~11 保留原代码中的电压/振动换算公式；
 *   其他通道默认使用原始值。后续只需要在这里替换真实公式即可。
 */ 
static float algo_raw_to_voltage(int ch, int raw)
{
    if (ch >= 4 && ch < 12) {
        return (float)raw * 0.000375f;
    }

    return (float)raw;
}

static float algo_voltage_to_value(int ch, float voltage)
{
    if (ch >= 4 && ch < 12) {
        const float v_min = -0.807f;
        const float v_max = 2.085f;
        return (voltage / 8.0f) * (v_max - v_min) + v_min;
    }

    return voltage;
}

static float algo_convert_raw_to_value(int ch, int raw)
{
    return algo_voltage_to_value(ch, algo_raw_to_voltage(ch, raw));
}

/* 高频突变捕获模式下，各算法单包所需的浮点数数量。 */
static unsigned int algo_fast_window_points(int algo_id)
{
    switch (algo_id) {
    case 1:
    case 4:
    case 5:
        return 256U;

    case 6:
    case 7:
        return 512U;

    default:
        /* 进入高频分支的算法2/3及其他未单独配置算法保持1024点。 */
        return 1024U;
    }
}

/*
 * 高频算法输入换算。
 * 所有新公式都先使用 actual_voltage = raw * 0.000375，再反算模型所需物理量。
 * 默认分支保持算法2/3及其他高频算法原有换算，不改变既有行为。
 */
static float algo_convert_algo67_voltage_to_value(float actual_voltage)
{
    float clamped_voltage = actual_voltage;
    float y;
    float abs_y;
    float sign_y;

    if (clamped_voltage < 0.0f) {
        clamped_voltage = 0.0f;
    } else if (clamped_voltage > 10.0f) {
        clamped_voltage = 10.0f;
    }

    y = clamped_voltage / 5.0f - 1.0f;
    abs_y = fabsf(y);
    sign_y = y < 0.0f ? -1.0f : 1.0f;

    return sign_y * (2000.0f / 255.0f) *
           (expf(abs_y * 5.5452f) - 1.0f);
}

static float algo_convert_fast_raw_for_id(int algo_id, int ch, int raw)
{
    const float actual_voltage = (float)raw * 0.000375f;

    switch (algo_id) {
    case 1:
    {
        float clamped_voltage = actual_voltage;
        float v_norm;
        float adc_value;

        if (clamped_voltage < 0.0f) {
            clamped_voltage = 0.0f;
        } else if (clamped_voltage > 8.0f) {
            clamped_voltage = 8.0f;
        }

        v_norm = clamped_voltage / 8.0f;
        adc_value = (expf(v_norm * 5.5452f) - 1.0f) *
                    3239.0f / 255.0f;

        return adc_value;
    }

    case 4:
    case 5:
    {
        float clamped_voltage = actual_voltage;
        float v_norm;
        float x_norm;

        if (clamped_voltage < 0.0f) {
            clamped_voltage = 0.0f;
        } else if (clamped_voltage > 8.0f) {
            clamped_voltage = 8.0f;
        }

        v_norm = clamped_voltage / 8.0f;
        x_norm = (expf(v_norm * 5.5452f) - 1.0f) / 255.0f;

        return x_norm * 2403.0f;
    }

    case 6:
    case 7:
        return algo_convert_algo67_voltage_to_value(actual_voltage);

    default:
        return algo_convert_raw_to_value(ch, raw);
    }
}

// 定义互斥锁
pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ===================== 11 个 Python 算法统一管理 =====================
 * 命令规则：
 * 311/321/.../391 -> 启动对应算法的 watch 连续检测，并在串口打印 Python 日志/结果
 * 312/322/.../392 -> 同样启动/保持对应算法的 watch 连续检测
 * 313/323/.../393 -> 停止对应算法的 watch 连续检测
 * 401/402/403 -> 启动/保持/停止算法10-漏油检测
 * 411/412/413 -> 启动/保持/停止算法11-漏油诊断
 * 310 -> 停止全部算法
 *
 * 说明：
 * - Python 仍然在后台运行，C 端不会被 watch 模式长时间阻塞。
 * - C 端会启动一个日志监听线程，把每个算法 log_file 中新增内容转发到串口。
 */
#define ALGO_COUNT 11
#define ALGO_CMD_BUF_SIZE 4096
#define ALGO_RESULT_MAX_CLASSES 5U
#define ALGO_RESULT_POLL_US 20000U
#define ALGO_RESULT_TYPE_DETECT 0x10U
#define ALGO_RESULT_TYPE_DIAG 0x11U


typedef struct {
    int id;                         /* 算法编号: 1~11 */
    const char *name;               /* 串口打印名称 */
    const char *py_path;            /* Python 程序路径 */
    const char *model_path;         /* ONNX 模型路径，没有就填 NULL */
    const char *metadata_path;      /* metadata.json 路径，没有就填 NULL */
    const char *scaler_path;        /* scaler.json 路径，没有就填 NULL */
    const char *data_file;          /* C 写给 Python 的数据文件 */
    const char *trigger_file;       /* 连续检测触发文件 */
    const char *stop_file;          /* 连续检测停止文件 */
    const char *log_file;           /* Python 输出日志文件，后台运行时使用 */
    const char *result_file;        /* Python 保存的最后一次结果 */
    const char *result_stream_file; /* Python 追加写入的完整结果流 */
    const char *algo_type;          /* DETECT 或 DIAG */
    unsigned char result_frame_type;/* 上位机结果帧类型: 0x10/0x11 */
    unsigned int class_count;       /* 有效结果类别数 */
    unsigned int vote_target;       /* 本算法累计多少次后投票 */
    int channel;                    /* Python 读取的通道号 */
    int offset;                     /* Python 读取偏移 */
    volatile int running;           /* 连续检测开关: 1=开启, 0=关闭 */
    pthread_t monitor_thread;       /* 串口日志监听线程 */
    volatile int monitor_running;   /* 串口日志监听开关: 1=开启, 0=关闭 */
    long log_offset;                /* 已转发到串口的日志偏移 */
    volatile int result_monitor_running;
    unsigned long result_monitor_generation;
    long result_stream_offset;
    unsigned long long run_id;
    unsigned long long trigger_seq;
    unsigned int vote_received;
    unsigned int vote_counts[ALGO_RESULT_MAX_CLASSES];
    unsigned int vote_last_seen[ALGO_RESULT_MAX_CLASSES];
    int result_completed;
} AlgoConfig;

static AlgoConfig g_algos[ALGO_COUNT] = {
    {.id = 1, .name = "算法1-胶条检测", .py_path = "/home/root/model/bearing_jc/test.py",
     .model_path = "/home/root/model/jt/model/sae_model.onnx",
     .metadata_path = "/home/root/model/jt/model/metadata.json",
     .scaler_path = "/home/root/model/jt/model/scaler.json",
     .data_file = "/tmp/algo1_jt_jc_data.bin", .trigger_file = "/tmp/algo1_jt_jc_trigger.flag",
     .stop_file = "/tmp/algo1_jt_jc_stop.flag", .log_file = "/tmp/algo1_jt_jc.log",
     .result_file = "/tmp/algo1_jt_jc_result.txt", .result_stream_file = "/tmp/algo1_jt_jc_result.stream",
     .algo_type = "DETECT", .result_frame_type = ALGO_RESULT_TYPE_DETECT,
     .class_count = 2, .vote_target = 20, .channel = 4},

    {.id = 2, .name = "算法2-艉轴承检测", .py_path = "/home/root/model/bearing_jc/test.py",
     .model_path = "/home/root/model/bearing_jc/model/sae_model.onnx",
     .metadata_path = "/home/root/model/bearing_jc/model/metadata.json",
     .scaler_path = "/home/root/model/bearing_jc/model/scaler.json",
     .data_file = "/tmp/algo2_bearing_jc_data.bin", .trigger_file = "/tmp/algo2_bearing_jc_trigger.flag",
     .stop_file = "/tmp/algo2_bearing_jc_stop.flag", .log_file = "/tmp/algo2_bearing_jc.log",
     .result_file = "/tmp/algo2_bearing_jc_result.txt", .result_stream_file = "/tmp/algo2_bearing_jc_result.stream",
     .algo_type = "DETECT", .result_frame_type = ALGO_RESULT_TYPE_DETECT,
     .class_count = 2, .vote_target = 100, .channel = 4},

    {.id = 3, .name = "算法3-艉轴承诊断", .py_path = "/home/root/model/bearing_zd/test_cnn_1024_classify.py",
     .model_path = "/home/root/model/bearing_zd/model/multiscale_cnn.onnx",
     .metadata_path = "/home/root/model/bearing_zd/model/metadata.json",
     .scaler_path = "/home/root/model/bearing_zd/model/scaler.json",
     .data_file = "/tmp/algo3_bearing_zd_data.bin", .trigger_file = "/tmp/algo3_bearing_zd_trigger.flag",
     .stop_file = "/tmp/algo3_bearing_zd_stop.flag", .log_file = "/tmp/algo3_bearing_zd.log",
     .result_file = "/tmp/algo3_bearing_zd_result.txt", .result_stream_file = "/tmp/algo3_bearing_zd_result.stream",
     .algo_type = "DIAG", .result_frame_type = ALGO_RESULT_TYPE_DIAG,
     .class_count = 4, .vote_target = 100, .channel = 4},

    {.id = 4, .name = "算法4-腐蚀板检测", .py_path = "/home/root/model/fs_jc/FS_jc.py",
     .model_path = "/home/root/model/fs_jc/model/pca_svm_model.onnx",
     .metadata_path = "/home/root/model/fs_jc/model/metadata.json",
     .data_file = "/tmp/algo4_fs_jc_data.bin", .trigger_file = "/tmp/algo4_fs_jc_trigger.flag",
     .stop_file = "/tmp/algo4_fs_jc_stop.flag", .log_file = "/tmp/algo4_fs_jc.log",
     .result_file = "/tmp/algo4_fs_jc_result.txt", .result_stream_file = "/tmp/algo4_fs_jc_result.stream",
     .algo_type = "DETECT", .result_frame_type = ALGO_RESULT_TYPE_DETECT,
     .class_count = 2, .vote_target = 10, .channel = 4},

    {.id = 5, .name = "算法5-腐蚀板诊断", .py_path = "/home/root/model/fs_zd/test_fs_256_classify.py",
     .model_path = "/home/root/model/fs_zd/model/mscnn_model.onnx",
     .metadata_path = "/home/root/model/fs_zd/model/metadata.json",
     .scaler_path = "/home/root/model/fs_zd/model/scaler.json",
     .data_file = "/tmp/algo5_fs_zd_data.bin", .trigger_file = "/tmp/algo5_fs_zd_trigger.flag",
     .stop_file = "/tmp/algo5_fs_zd_stop.flag", .log_file = "/tmp/algo5_fs_zd.log",
     .result_file = "/tmp/algo5_fs_zd_result.txt", .result_stream_file = "/tmp/algo5_fs_zd_result.stream",
     .algo_type = "DIAG", .result_frame_type = ALGO_RESULT_TYPE_DIAG,
     .class_count = 4, .vote_target = 20, .channel = 4},

    {.id = 6, .name = "算法6-舰炮撞针检测", .py_path = "/home/root/model/bearing_jc/test.py",
     .model_path = "/home/root/model/zz_jc/model/sae_model.onnx",
     .metadata_path = "/home/root/model/zz_jc/model/metadata.json",
     .scaler_path = "/home/root/model/zz_jc/model/scaler.json",
     .data_file = "/tmp/algo6_zz_jc_data.bin", .trigger_file = "/tmp/algo6_zz_jc_trigger.flag",
     .stop_file = "/tmp/algo6_zz_jc_stop.flag", .log_file = "/tmp/algo6_zz_jc.log",
     .result_file = "/tmp/algo6_zz_jc_result.txt", .result_stream_file = "/tmp/algo6_zz_jc_result.stream",
     .algo_type = "DETECT", .result_frame_type = ALGO_RESULT_TYPE_DETECT,
     .class_count = 2, .vote_target = 40, .channel = 4},

    {.id = 7, .name = "算法7-舰炮撞针诊断", .py_path = "/home/root/model/zz_zd/test.py",
     .model_path = "/home/root/model/zz_zd/model/mscnn_model.onnx",
     .metadata_path = "/home/root/model/zz_zd/model/metadata.json",
     .scaler_path = "/home/root/model/zz_zd/model/scaler.json",
     .data_file = "/tmp/algo7_zz_zd_data.bin", .trigger_file = "/tmp/algo7_zz_zd_trigger.flag",
     .stop_file = "/tmp/algo7_zz_zd_stop.flag", .log_file = "/tmp/algo7_zz_zd.log",
     .result_file = "/tmp/algo7_zz_zd_result.txt", .result_stream_file = "/tmp/algo7_zz_zd_result.stream",
     .algo_type = "DIAG", .result_frame_type = ALGO_RESULT_TYPE_DIAG,
     .class_count = 4, .vote_target = 100, .channel = 4},

    {.id = 8, .name = "算法8-艉轴密封装置检测", .py_path = "/home/root/model/bearing_jc/test.py",
     .model_path = "/home/root/model/mf_jc/model/sae_detector.onnx",
     .metadata_path = "/home/root/model/mf_jc/model/metadata.json",
     .data_file = "/tmp/algo8_mf_jc_data.bin", .trigger_file = "/tmp/algo8_mf_jc_trigger.flag",
     .stop_file = "/tmp/algo8_mf_jc_stop.flag", .log_file = "/tmp/algo8_mf_jc.log",
     .result_file = "/tmp/algo8_mf_jc_result.txt", .result_stream_file = "/tmp/algo8_mf_jc_result.stream",
     .algo_type = "DETECT", .result_frame_type = ALGO_RESULT_TYPE_DETECT,
     .class_count = 2, .vote_target = 40, .channel = 3},

    {.id = 9, .name = "算法9-艉轴密封装置诊断", .py_path = "/home/root/model/mf_zd/test_cnn_8ch32_classify.py",
     .model_path = "/home/root/model/mf_zd/model/multiscale_cnn_fault_diagnosis.onnx",
     .metadata_path = "/home/root/model/mf_zd/model/metadata.json",
     .data_file = "/tmp/algo9_mf_zd_data.bin", .trigger_file = "/tmp/algo9_mf_zd_trigger.flag",
     .stop_file = "/tmp/algo9_mf_zd_stop.flag", .log_file = "/tmp/algo9_mf_zd.log",
     .result_file = "/tmp/algo9_mf_zd_result.txt", .result_stream_file = "/tmp/algo9_mf_zd_result.stream",
     .algo_type = "DIAG", .result_frame_type = ALGO_RESULT_TYPE_DIAG,
     .class_count = 5, .vote_target = 1, .channel = 3},//投票次数需要改小，一包数32秒//改回1

    {.id = 10, .name = "算法10-漏油检测", .py_path = "/home/root/model/ly_jc/bearing_detection_24d.py",
     .model_path = "/home/root/model/ly_jc/model/ae_model.onnx",
     .metadata_path = "/home/root/model/ly_jc/model/metadata.json",
     .scaler_path = "NULL",
     .data_file = "/tmp/algo10_ly_jc_data.bin", .trigger_file = "/tmp/algo10_ly_jc_trigger.flag",
     .stop_file = "/tmp/algo10_ly_jc_stop.flag", .log_file = "/tmp/algo10_ly_jc.log",
     .result_file = "/tmp/algo10_ly_jc_result.txt", .result_stream_file = "/tmp/algo10_ly_jc_result.stream",
     .algo_type = "DETECT", .result_frame_type = ALGO_RESULT_TYPE_DETECT,
     .class_count = 2, .vote_target = 10, .channel = 5},

    {.id = 11, .name = "算法11-漏油诊断", .py_path = "/home/root/model/ly_zd/bearing_diagnosis_24d.py",
     .model_path = "/home/root/model/ly_zd/model/diagnosis_model.onnx",
     .metadata_path = "/home/root/model/ly_zd/model/metadata.json",
     .scaler_path = "/home/root/model/ly_zd/model/scaler.json",
     .data_file = "/tmp/algo11_ly_zd_data.bin", .trigger_file = "/tmp/algo11_ly_zd_trigger.flag",
     .stop_file = "/tmp/algo11_ly_zd_stop.flag", .log_file = "/tmp/algo11_ly_zd.log",
     .result_file = "/tmp/algo11_ly_zd_result.txt", .result_stream_file = "/tmp/algo11_ly_zd_result.stream",
     .algo_type = "DIAG", .result_frame_type = ALGO_RESULT_TYPE_DIAG,
     .class_count = 4, .vote_target = 10, .channel = 5},
};

typedef struct {
    AlgoConfig *algo;
    unsigned long generation;
} AlgoResultMonitorArgs;

static pthread_mutex_t g_algo_result_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 函数原型声明 */
static int write_algo_data_for_algo(AlgoConfig *algo);
static void *algo_serial_log_thread(void *arg);
static void algo_clear_log(const AlgoConfig *algo);
static void algo_start_serial_monitor(AlgoConfig *algo, int from_start);
static void algo_stop_serial_monitor(AlgoConfig *algo);
static void algo_test_once(AlgoConfig *algo);
static void algo_start_watch(AlgoConfig *algo);
static void algo_stop_watch(AlgoConfig *algo);
static void algo_stop_all(void);
static int handle_algo_cmd(unsigned int cmd);
static int algo_start_result_monitor(AlgoConfig *algo);
static void algo_stop_result_monitor(AlgoConfig *algo);



// // 请根据实际需求修改这 256 个数值
// static const int FIXED_WAVEFORM[256] = {
//     2827, 3223, 2934, 1881, 535, 367, 565, 540, 421, 207,
//     121, 147, 129, 95, 121, 116, 223, 236, 193, 107,
//     149, 142, 111, 151, 145, 113, 79, 88, 94, 96,
//     92, 78, 59, 49, 28, 33, 45, 51, 46, 16,
//     35, 77, 78, 36, 46, 73, 77, 73, 49, 14,
//     19, 24, 29, 31, 28, 15, 16, 31, 36, 36,
//     32, 19, 10, 13, 14, 10, 17, 31, 31, 23,
//     14, 10, 8, 5, 15, 21, 21, 22, 20, 14,
//     12, 10, 6, 4, 10, 11, 11, 8, 7, 11,
//     11, 11, 12, 12, 7, 5, 6, 3, 6, 9,
//     9, 10, 16, 18, 21, 23, 22, 17, 11, 15,
//     15, 13, 11, 6, 6, 8, 6, 8, 8, 8,
//     6, 10, 11, 10, 9, 9, 5, 8, 8, 6,
//     6, 10, 10, 9, 7, 6, 7, 8, 7, 4,
//     9, 13, 13, 12, 11, 11, 10, 4, 9, 9,
//     10, 11, 10, 7, 5, 9, 8, 7, 6, 8,
//     10, 8, 4, 7, 7, 5, 5, 11, 13, 11,
//     5, 4, 5, 7, 6, 4, 3, 4, 6, 6,
//     2, 1, 2, 5, 8, 10, 7, 10, 9, 4,
//     4, 6, 6, 7, 8, 10, 15, 18, 14, 8,
//     9, 23, 29, 29, 19, 14, 14, 14, 14, 13,
//     9, 7, 25, 56, 56, 41, 65, 277, 397, 417,
//     310, 174, 116, 333, 840, 903, 809, 446, 200, 110,
//     176, 389, 565, 543, 314, 157, 28, 55, 110, 118,
//     111, 101, 80, 46, 54, 71, 72, 64, 40, 17,
//     15, 11, 26, 35, 31, 18
// };




//////////////////////////////////////////////////////////////////////////////////////////////////////超声部分
/****************************************************************************************/

unsigned char g_saveFileEn = SAVE_MODE;	//0x00 存储功能关；	0x01 存到SD;	0x02 存到固态里
/****************************************************************************************/
unsigned int g_tcpStart;
/****************************************************************************************/
void cis_init_file(void);
void cis_search_files(void);
void cis_transfer_file(void);
void cis_delete_files(void);
void cis_query_disk_usage(void);
void cis_start(void);
void cis_storage_setting(void) ;
void cis_end(void);
/****************************************************************************************/
FileStruct_t g_tFileIepe0Struct;
FileStruct_t g_tFileIepe1Struct;
FileStruct_t g_tFileIepe2Struct;
FileStruct_t g_tFileIepe3Struct;
FileStruct_t g_tFileCurrent0Struct;
FileStruct_t g_tFileCurrent1Struct;
FileStruct_t g_tFileCurrent2Struct;
FileStruct_t g_tFileCurrent3Struct;
FileStruct_t g_tFileCurrent4Struct;
FileStruct_t g_tFileCurrent5Struct;
FileStruct_t g_tFileCurrent6Struct;
FileStruct_t g_tFileCurrent7Struct;
FileStruct_t g_tFileVoltage0Struct;
FileStruct_t g_tFileVoltage1Struct;
FileStruct_t g_tFileVoltage2Struct;
FileStruct_t g_tFileVoltage3Struct;
FileStruct_t g_tFileVoltage4Struct;
FileStruct_t g_tFileVoltage5Struct;
FileStruct_t g_tFileVoltage6Struct;
FileStruct_t g_tFileVoltage7Struct;
FileStruct_t g_tFilePsTempStruct;
/****************************************************************************************/
DeviceStruct_t g_tDeviceStruct = {0};
/****************************************************************************************/
void fun_ad7768_init(void);
void fun_ad7915_init(void);
void fun_time_init(void);
void deal_ad7915(void);
void deal_ad7768(void);
void deal_clock(void);
void deal_temperature(void);
void renew_file_time(void);
/****************************************************************************************/
void* thread_deal_client_ps(void *args);
void* thread_deal_client_pl(void *args);
char g_tcpClientRcvBuffer[BUFFER_SIZE];
char g_tcpClientRcvBuffer1[BUFFER_SIZE];
int g_socketFd;
int g_clientSocket;
int g_clientSocket1;
unsigned int g_tcpRecvBuffer[BUFFER_SIZE];
unsigned int g_tcpRecvBuffer1[BUFFER_SIZE];
unsigned char g_tcpSendBuffer[BUFFER_SIZE];
unsigned char g_recvFifoBackTcpBuffer[BUFFER_SIZE];

unsigned int g_tcpRecvLen;
unsigned int g_tcpSendLen;

/* ===================== 算法2/3高频1024点数据包共享上传 =====================///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
 * 3001: 开启上传；3002: 停止上传。
 * 上传内容复用算法2/3高频突变采样器生成的同一包1024个float。
 * 3001单独开启时也会驱动算法2/3共享采样器，但不会触发Python。
 *
 * 采样线程只提交完整数据包到双缓冲，不执行任何socket操作。
 * 发送线程独立完成组帧和TCP发送，因此不阻塞算法采集。
 */
#define HF_TCP_UPLOAD_CMD_START       3001U
#define HF_TCP_UPLOAD_CMD_STOP        3002U
#define HF_TCP_UPLOAD_LOGICAL_CH      4
#define HF_TCP_UPLOAD_SAMPLE_COUNT    1024U
#define HF_TCP_CONSUMER_ALGO2          0x01U
#define HF_TCP_CONSUMER_ALGO3          0x02U
#define HF_TCP_UPLOAD_PAYLOAD_BYTES   (HF_TCP_UPLOAD_SAMPLE_COUNT * sizeof(float))
#define HF_TCP_UPLOAD_HEADER_BYTES    24U
#define HF_TCP_UPLOAD_FOOTER_BYTES    2U
#define HF_TCP_UPLOAD_FRAME_BYTES     \
    (HF_TCP_UPLOAD_HEADER_BYTES + HF_TCP_UPLOAD_PAYLOAD_BYTES + \
     HF_TCP_UPLOAD_FOOTER_BYTES)

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    volatile int enabled;
    int thread_started;
    int shutdown_requested;
    int client_fd;
    int logical_channel;
    float buffers[2][HF_TCP_UPLOAD_SAMPLE_COUNT];
    unsigned int write_slot;
    unsigned int write_index;
    int ready_slot;
    unsigned char ready_source;
    uint32_t next_sequence;
    uint32_t ready_sequence;
    uint64_t ready_timestamp_us;
    unsigned long long dropped_samples;
    unsigned long long dropped_packets;
} HfTcpUploadState;

static HfTcpUploadState g_hf_tcp_upload = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .enabled = 0,
    .thread_started = 0,
    .shutdown_requested = 0,
    .client_fd = -1,
    .logical_channel = HF_TCP_UPLOAD_LOGICAL_CH,
    .write_slot = 0,
    .write_index = 0,
    .ready_slot = -1,
    .ready_source = 0,
    .next_sequence = 0
};

/* 只保护socket帧发送；采集线程不会获取这个锁。 */
static pthread_mutex_t g_tcp_socket_send_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t hf_tcp_timestamp_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
}

static void hf_tcp_put_u16_le(unsigned char *dst, uint16_t value)
{
    dst[0] = (unsigned char)(value & 0xFFU);
    dst[1] = (unsigned char)((value >> 8) & 0xFFU);
}

static void hf_tcp_put_u32_le(unsigned char *dst, uint32_t value)
{
    dst[0] = (unsigned char)(value & 0xFFU);
    dst[1] = (unsigned char)((value >> 8) & 0xFFU);
    dst[2] = (unsigned char)((value >> 16) & 0xFFU);
    dst[3] = (unsigned char)((value >> 24) & 0xFFU);
}

static void hf_tcp_put_u64_le(unsigned char *dst, uint64_t value)
{
    for (unsigned int i = 0; i < 8U; i++) {
        dst[i] = (unsigned char)((value >> (i * 8U)) & 0xFFU);
    }
}

static int hf_tcp_send_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = (const unsigned char *)buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t ret = send(fd, cursor + sent, length - sent, MSG_NOSIGNAL);

        if (ret > 0) {
            sent += (size_t)ret;
            continue;
        }
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }

    return 0;
}

/*
 * 独立的完整波形帧发送函数。
 * 帧布局（全部多字节整数和float有效载荷均为小端）：
 *  0..1   AA 55
 *  2      协议版本，当前为1
 *  3      帧类型，1表示高频物理量波形
 *  4      逻辑通道号
 *  5      保留
 *  6..9   包序号
 * 10..11  采样点数，固定1024
 * 12..15  有效载荷字节数，固定4096
 * 16..23  完成采集时的Unix时间戳，单位微秒
 * 24..    1024个IEEE754 float32
 * 最后2字节 FF FF
 */
static int hf_tcp_upload_send_frame(int fd, int logical_channel,
                                    unsigned char source_mask,
                                    uint32_t sequence, uint64_t timestamp_us,
                                    const float samples[HF_TCP_UPLOAD_SAMPLE_COUNT])
{
    unsigned char frame[HF_TCP_UPLOAD_FRAME_BYTES];
    size_t payload_offset = HF_TCP_UPLOAD_HEADER_BYTES;

    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[2] = 0x01;
    frame[3] = 0x01;
    frame[4] = (unsigned char)logical_channel;
    frame[5] = source_mask;
    hf_tcp_put_u32_le(frame + 6, sequence);
    hf_tcp_put_u16_le(frame + 10, (uint16_t)HF_TCP_UPLOAD_SAMPLE_COUNT);
    hf_tcp_put_u32_le(frame + 12, (uint32_t)HF_TCP_UPLOAD_PAYLOAD_BYTES);
    hf_tcp_put_u64_le(frame + 16, timestamp_us);

    for (unsigned int i = 0; i < HF_TCP_UPLOAD_SAMPLE_COUNT; i++) {
        uint32_t bits;
        memcpy(&bits, &samples[i], sizeof(bits));
        hf_tcp_put_u32_le(frame + payload_offset + i * sizeof(bits), bits);
    }

    frame[HF_TCP_UPLOAD_FRAME_BYTES - 2U] = 0xFF;
    frame[HF_TCP_UPLOAD_FRAME_BYTES - 1U] = 0xFF;

    pthread_mutex_lock(&g_tcp_socket_send_mutex);
    int ret = hf_tcp_send_all(fd, frame, sizeof(frame));
    pthread_mutex_unlock(&g_tcp_socket_send_mutex);

    return ret;
}

static void *hf_tcp_upload_thread(void *arg)
{
    float local_samples[HF_TCP_UPLOAD_SAMPLE_COUNT];
    (void)arg;

    for (;;) {
        int fd;
        int logical_channel;
        int slot;
        unsigned char source_mask;
        uint32_t sequence;
        uint64_t timestamp_us;

        pthread_mutex_lock(&g_hf_tcp_upload.mutex);
        while (g_hf_tcp_upload.ready_slot < 0 &&
               !g_hf_tcp_upload.shutdown_requested) {
            pthread_cond_wait(&g_hf_tcp_upload.cond,
                              &g_hf_tcp_upload.mutex);
        }

        if (g_hf_tcp_upload.shutdown_requested) {
            pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
            break;
        }

        slot = g_hf_tcp_upload.ready_slot;
        source_mask = g_hf_tcp_upload.ready_source;
        sequence = g_hf_tcp_upload.ready_sequence;
        timestamp_us = g_hf_tcp_upload.ready_timestamp_us;
        fd = g_hf_tcp_upload.client_fd;
        logical_channel = g_hf_tcp_upload.logical_channel;
        memcpy(local_samples, g_hf_tcp_upload.buffers[slot],
               sizeof(local_samples));
        g_hf_tcp_upload.ready_slot = -1;
        pthread_mutex_unlock(&g_hf_tcp_upload.mutex);

        if (hf_tcp_upload_send_frame(fd, logical_channel, source_mask,
                                     sequence, timestamp_us, local_samples) != 0) {
            printf("[HF TCP] 波形帧发送失败，停止独立上传。\n");
            pthread_mutex_lock(&g_hf_tcp_upload.mutex);
            g_hf_tcp_upload.enabled = 0;
            pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
            shutdown(fd, SHUT_RDWR);
            break;
        }

        printf("[Algo2/3 TCP] 已发送第%u包，通道=%d，点数=%u，消费者掩码=0x%02X。\n",
               sequence, logical_channel, HF_TCP_UPLOAD_SAMPLE_COUNT,
               source_mask);
    }

    return NULL;
}

static int hf_tcp_upload_attach_client(int client_fd)
{
    int ret;

    pthread_mutex_lock(&g_hf_tcp_upload.mutex);
    if (g_hf_tcp_upload.thread_started) {
        pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
        return -1;
    }

    g_hf_tcp_upload.client_fd = client_fd;
    g_hf_tcp_upload.enabled = 0;
    g_hf_tcp_upload.shutdown_requested = 0;
    g_hf_tcp_upload.write_slot = 0;
    g_hf_tcp_upload.write_index = 0;
    g_hf_tcp_upload.ready_slot = -1;
    g_hf_tcp_upload.ready_source = 0;
    g_hf_tcp_upload.next_sequence = 0;
    g_hf_tcp_upload.thread_started = 1;
    pthread_mutex_unlock(&g_hf_tcp_upload.mutex);

    ret = pthread_create(&g_hf_tcp_upload.thread, NULL,
                         hf_tcp_upload_thread, NULL);
    if (ret != 0) {
        pthread_mutex_lock(&g_hf_tcp_upload.mutex);
        g_hf_tcp_upload.thread_started = 0;
        g_hf_tcp_upload.client_fd = -1;
        pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
        printf("[HF TCP] 创建独立上传线程失败: %d\n", ret);
        return -1;
    }

    return 0;
}

static void hf_tcp_upload_detach_client(void)
{
    int should_join;
    int client_fd;

    pthread_mutex_lock(&g_hf_tcp_upload.mutex);
    should_join = g_hf_tcp_upload.thread_started;
    client_fd = g_hf_tcp_upload.client_fd;
    g_hf_tcp_upload.enabled = 0;
    g_hf_tcp_upload.shutdown_requested = 1;
    g_hf_tcp_upload.ready_slot = -1;
    pthread_cond_signal(&g_hf_tcp_upload.cond);
    pthread_mutex_unlock(&g_hf_tcp_upload.mutex);

    if (should_join) {
        /* 解除可能阻塞在send()中的上传线程，再等待它退出。 */
        if (client_fd >= 0) {
            shutdown(client_fd, SHUT_RDWR);
        }
        pthread_join(g_hf_tcp_upload.thread, NULL);
    }

    pthread_mutex_lock(&g_hf_tcp_upload.mutex);
    g_hf_tcp_upload.thread_started = 0;
    g_hf_tcp_upload.client_fd = -1;
    pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
}

static void hf_tcp_upload_start(int logical_channel)
{
    pthread_mutex_lock(&g_hf_tcp_upload.mutex);
    if (!g_hf_tcp_upload.thread_started ||
        g_hf_tcp_upload.client_fd < 0) {
        pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
        printf("[HF TCP] 尚无可用的TCP上传线程。\n");
        return;
    }

    if (g_hf_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
        printf("[Algo2/3 TCP] 共享数据包上传已经开启。\n");
        return;
    }

    g_hf_tcp_upload.logical_channel = logical_channel;
    g_hf_tcp_upload.write_slot = 0;
    g_hf_tcp_upload.write_index = 0;
    g_hf_tcp_upload.ready_slot = -1;
    g_hf_tcp_upload.ready_source = 0;
    g_hf_tcp_upload.next_sequence = 0;
    g_hf_tcp_upload.dropped_samples = 0;
    g_hf_tcp_upload.dropped_packets = 0;
    g_hf_tcp_upload.enabled = 1;
    pthread_mutex_unlock(&g_hf_tcp_upload.mutex);

    printf("[Algo2/3 TCP] 3001: 算法2/3共享1024点数据包上传已开启，逻辑通道=%d。\n",
           logical_channel);
}

static void hf_tcp_upload_stop(void)
{
    unsigned long long dropped_samples;
    unsigned long long dropped_packets;

    pthread_mutex_lock(&g_hf_tcp_upload.mutex);
    g_hf_tcp_upload.enabled = 0;
    g_hf_tcp_upload.write_index = 0;
    g_hf_tcp_upload.ready_slot = -1;
    dropped_samples = g_hf_tcp_upload.dropped_samples;
    dropped_packets = g_hf_tcp_upload.dropped_packets;
    pthread_mutex_unlock(&g_hf_tcp_upload.mutex);

    printf("[Algo2/3 TCP] 3002: 算法2/3共享数据包上传已停止，丢点=%llu，丢包=%llu。\n",
           dropped_samples, dropped_packets);
}


static int hf_tcp_upload_is_enabled(void)
{
    int enabled;

    pthread_mutex_lock(&g_hf_tcp_upload.mutex);
    enabled = g_hf_tcp_upload.enabled;
    pthread_mutex_unlock(&g_hf_tcp_upload.mutex);

    return enabled;
}

/* 算法2/3共享采样器采满1024点后调用；这里只复制4KB数据，不执行send。 */
static void hf_tcp_upload_submit_packet(unsigned int consumer_mask,
                                        int logical_channel,
                                        const float samples[HF_TCP_UPLOAD_SAMPLE_COUNT])
{
    unsigned int completed_slot;
    uint32_t completed_sequence;

    if (!g_hf_tcp_upload.enabled ||
        logical_channel != g_hf_tcp_upload.logical_channel) {
        return;
    }

    if (pthread_mutex_trylock(&g_hf_tcp_upload.mutex) != 0) {
        __sync_fetch_and_add(&g_hf_tcp_upload.dropped_packets, 1ULL);
        return;
    }

    if (!g_hf_tcp_upload.enabled ||
        logical_channel != g_hf_tcp_upload.logical_channel) {
        pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
        return;
    }

    if (g_hf_tcp_upload.ready_slot >= 0) {
        g_hf_tcp_upload.dropped_packets++;
        pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
        return;
    }

    completed_slot = g_hf_tcp_upload.write_slot;
    completed_sequence = g_hf_tcp_upload.next_sequence++;
    memcpy(g_hf_tcp_upload.buffers[completed_slot], samples,
           HF_TCP_UPLOAD_PAYLOAD_BYTES);

    g_hf_tcp_upload.ready_slot = (int)completed_slot;
    g_hf_tcp_upload.ready_source = (unsigned char)(consumer_mask & 0xFFU);
    g_hf_tcp_upload.ready_sequence = completed_sequence;
    g_hf_tcp_upload.ready_timestamp_us = hf_tcp_timestamp_us();
    g_hf_tcp_upload.write_slot ^= 1U;
    g_hf_tcp_upload.write_index = 0;
    pthread_cond_signal(&g_hf_tcp_upload.cond);
    pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
}

/* 旧的独立逐点上传入口已不再由deal_ad7915()调用，保留函数体避免影响其它引用。 */
static void hf_tcp_upload_collect_sample(int logical_channel, int raw_value)
{
    float physical_value;
    unsigned int completed_slot;
    uint32_t completed_sequence;

    if (!g_hf_tcp_upload.enabled ||
        logical_channel != g_hf_tcp_upload.logical_channel) {
        return;
    }

    physical_value = algo_convert_raw_to_value(logical_channel, raw_value);

    if (pthread_mutex_trylock(&g_hf_tcp_upload.mutex) != 0) {
        __sync_fetch_and_add(&g_hf_tcp_upload.dropped_samples, 1ULL);
        return;
    }

    if (!g_hf_tcp_upload.enabled ||
        logical_channel != g_hf_tcp_upload.logical_channel) {
        pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
        return;
    }

    g_hf_tcp_upload.buffers[g_hf_tcp_upload.write_slot]
                           [g_hf_tcp_upload.write_index] = physical_value;
    g_hf_tcp_upload.write_index++;

    if (g_hf_tcp_upload.write_index >= HF_TCP_UPLOAD_SAMPLE_COUNT) {
        completed_slot = g_hf_tcp_upload.write_slot;
        completed_sequence = g_hf_tcp_upload.next_sequence++;

        if (g_hf_tcp_upload.ready_slot >= 0) {
            /* 网络还未取走上一包：丢当前网络包，但绝不阻塞采集线程。 */
            g_hf_tcp_upload.dropped_packets++;
            g_hf_tcp_upload.write_index = 0;
        } else {
            g_hf_tcp_upload.ready_slot = (int)completed_slot;
            g_hf_tcp_upload.ready_source = 0;
            g_hf_tcp_upload.ready_sequence = completed_sequence;
            g_hf_tcp_upload.ready_timestamp_us = hf_tcp_timestamp_us();
            g_hf_tcp_upload.write_slot ^= 1U;
            g_hf_tcp_upload.write_index = 0;
            pthread_cond_signal(&g_hf_tcp_upload.cond);
        }
    }

    pthread_mutex_unlock(&g_hf_tcp_upload.mutex);
}

/* ===================== 算法8/9模式A每秒8x1共享上传 =====================//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
 * 3003: 开启上传；3004: 停止上传。
 * 上传内容优先复用算法8/9模式A采样器产生的8通道物理量。
 * 3003单独开启时仍按每秒一组8x1生成TCP上传包，但不会触发Python。
 */
#define MODE_A_TCP_UPLOAD_CMD_START       3003U
#define MODE_A_TCP_UPLOAD_CMD_STOP        3004U
#define MODE_A_TCP_UPLOAD_CHANNEL_COUNT   8U
#define MODE_A_TCP_UPLOAD_PAYLOAD_BYTES   \
    (MODE_A_TCP_UPLOAD_CHANNEL_COUNT * sizeof(float))
#define MODE_A_TCP_UPLOAD_HEADER_BYTES    24U
#define MODE_A_TCP_UPLOAD_FOOTER_BYTES    2U
#define MODE_A_TCP_UPLOAD_FRAME_BYTES     \
    (MODE_A_TCP_UPLOAD_HEADER_BYTES + MODE_A_TCP_UPLOAD_PAYLOAD_BYTES + \
     MODE_A_TCP_UPLOAD_FOOTER_BYTES)

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    volatile int enabled;
    int thread_started;
    int shutdown_requested;
    int client_fd;
    int raw_frame[MODE_A_TCP_UPLOAD_CHANNEL_COUNT];
    unsigned char frame_mask;
    float buffers[2][MODE_A_TCP_UPLOAD_CHANNEL_COUNT];
    unsigned int write_slot;
    int ready_slot;
    unsigned char ready_source;
    uint32_t next_sequence;
    uint32_t ready_sequence;
    uint64_t ready_timestamp_us;
    struct timespec next_output_time;
    unsigned long long dropped_samples;
    unsigned long long dropped_frames;
} ModeATcpUploadState;

static ModeATcpUploadState g_mode_a_tcp_upload = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .enabled = 0,
    .thread_started = 0,
    .shutdown_requested = 0,
    .client_fd = -1,
    .frame_mask = 0,
    .write_slot = 0,
    .ready_slot = -1,
    .ready_source = 0,
    .next_sequence = 0
};

static const float g_mode_a_p_min[MODE_A_TCP_UPLOAD_CHANNEL_COUNT] = {
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f
};

static const float g_mode_a_p_max[MODE_A_TCP_UPLOAD_CHANNEL_COUNT] = {
    30.0f, 50.0f, 0.1f, 40.0f,
    50.0f, 600.0f, 300.0f, 40.0f
};

static int mode_a_tcp_time_reached(const struct timespec *now,
                                   const struct timespec *deadline)
{
    if (now->tv_sec != deadline->tv_sec) {
        return now->tv_sec > deadline->tv_sec;
    }
    return now->tv_nsec >= deadline->tv_nsec;
}

static void mode_a_tcp_schedule_next(struct timespec *deadline,
                                     const struct timespec *now)
{
    *deadline = *now;
    deadline->tv_sec += 1;
}

static float mode_a_tcp_raw_to_physical(unsigned int channel, int raw_value)
{
    const float voltage = (float)raw_value * 0.000375f;

    /* 不钳位，保留输入超出0~8V时对应的真实超量程物理量。 */
    return g_mode_a_p_min[channel] +
           (voltage / 8.0f) *
           (g_mode_a_p_max[channel] - g_mode_a_p_min[channel]);
}

/*
 * 模式A独立上传帧：
 *  0..1   AA 55
 *  2      协议版本1
 *  3      帧类型2，表示模式A八通道物理量
 *  4      FF，表示通道组
 *  5      保留
 *  6..9   包序号
 * 10..11  数值数量，固定8
 * 12..15  有效载荷长度，固定32字节
 * 16..23  完成组包时的Unix时间戳，单位微秒
 * 24..55  通道0~7的8个IEEE754 float32，按小端排列
 * 56..57  FF FF
 */
static int mode_a_tcp_upload_send_frame(
    int fd, unsigned char source, uint32_t sequence, uint64_t timestamp_us,
    const float values[MODE_A_TCP_UPLOAD_CHANNEL_COUNT])
{
    unsigned char frame[MODE_A_TCP_UPLOAD_FRAME_BYTES];
    int ret;

    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[2] = 0x01;
    frame[3] = 0x02;
    frame[4] = 0xFF;
    frame[5] = source;
    hf_tcp_put_u32_le(frame + 6, sequence);
    hf_tcp_put_u16_le(frame + 10,
                      (uint16_t)MODE_A_TCP_UPLOAD_CHANNEL_COUNT);
    hf_tcp_put_u32_le(frame + 12,
                      (uint32_t)MODE_A_TCP_UPLOAD_PAYLOAD_BYTES);
    hf_tcp_put_u64_le(frame + 16, timestamp_us);

    for (unsigned int i = 0; i < MODE_A_TCP_UPLOAD_CHANNEL_COUNT; i++) {
        uint32_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        hf_tcp_put_u32_le(frame + MODE_A_TCP_UPLOAD_HEADER_BYTES +
                          i * sizeof(bits), bits);
    }

    frame[MODE_A_TCP_UPLOAD_FRAME_BYTES - 2U] = 0xFF;
    frame[MODE_A_TCP_UPLOAD_FRAME_BYTES - 1U] = 0xFF;

    pthread_mutex_lock(&g_tcp_socket_send_mutex);
    ret = hf_tcp_send_all(fd, frame, sizeof(frame));
    pthread_mutex_unlock(&g_tcp_socket_send_mutex);

    return ret;
}

static void *mode_a_tcp_upload_thread(void *arg)
{
    float local_values[MODE_A_TCP_UPLOAD_CHANNEL_COUNT];
    (void)arg;

    for (;;) {
        int fd;
        int slot;
        unsigned char source;
        uint32_t sequence;
        uint64_t timestamp_us;

        pthread_mutex_lock(&g_mode_a_tcp_upload.mutex);
        while (g_mode_a_tcp_upload.ready_slot < 0 &&
               !g_mode_a_tcp_upload.shutdown_requested) {
            pthread_cond_wait(&g_mode_a_tcp_upload.cond,
                              &g_mode_a_tcp_upload.mutex);
        }

        if (g_mode_a_tcp_upload.shutdown_requested) {
            pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
            break;
        }

        slot = g_mode_a_tcp_upload.ready_slot;
        source = g_mode_a_tcp_upload.ready_source;
        sequence = g_mode_a_tcp_upload.ready_sequence;
        timestamp_us = g_mode_a_tcp_upload.ready_timestamp_us;
        fd = g_mode_a_tcp_upload.client_fd;
        memcpy(local_values, g_mode_a_tcp_upload.buffers[slot],
               sizeof(local_values));
        g_mode_a_tcp_upload.ready_slot = -1;
        pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);

        if (mode_a_tcp_upload_send_frame(fd, source, sequence, timestamp_us,
                                         local_values) != 0) {
            printf("[ModeA TCP] 物理量帧发送失败，停止独立上传。\n");
            pthread_mutex_lock(&g_mode_a_tcp_upload.mutex);
            g_mode_a_tcp_upload.enabled = 0;
            pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
            shutdown(fd, SHUT_RDWR);
            break;
        }

        printf("[ModeA TCP] 已发送第%u包，8通道物理量，来源=%u。\n",
               sequence, source);
    }

    return NULL;
}

static int mode_a_tcp_upload_attach_client(int client_fd)
{
    int ret;

    pthread_mutex_lock(&g_mode_a_tcp_upload.mutex);
    if (g_mode_a_tcp_upload.thread_started) {
        pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
        return -1;
    }

    g_mode_a_tcp_upload.client_fd = client_fd;
    g_mode_a_tcp_upload.enabled = 0;
    g_mode_a_tcp_upload.shutdown_requested = 0;
    g_mode_a_tcp_upload.frame_mask = 0;
    g_mode_a_tcp_upload.write_slot = 0;
    g_mode_a_tcp_upload.ready_slot = -1;
    g_mode_a_tcp_upload.ready_source = 0;
    g_mode_a_tcp_upload.next_sequence = 0;
    g_mode_a_tcp_upload.thread_started = 1;
    pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);

    ret = pthread_create(&g_mode_a_tcp_upload.thread, NULL,
                         mode_a_tcp_upload_thread, NULL);
    if (ret != 0) {
        pthread_mutex_lock(&g_mode_a_tcp_upload.mutex);
        g_mode_a_tcp_upload.thread_started = 0;
        g_mode_a_tcp_upload.client_fd = -1;
        pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
        printf("[ModeA TCP] 创建独立上传线程失败: %d\n", ret);
        return -1;
    }

    return 0;
}

static void mode_a_tcp_upload_detach_client(void)
{
    int should_join;
    int client_fd;

    pthread_mutex_lock(&g_mode_a_tcp_upload.mutex);
    should_join = g_mode_a_tcp_upload.thread_started;
    client_fd = g_mode_a_tcp_upload.client_fd;
    g_mode_a_tcp_upload.enabled = 0;
    g_mode_a_tcp_upload.shutdown_requested = 1;
    g_mode_a_tcp_upload.ready_slot = -1;
    pthread_cond_signal(&g_mode_a_tcp_upload.cond);
    pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);

    if (should_join) {
        if (client_fd >= 0) {
            shutdown(client_fd, SHUT_RDWR);
        }
        pthread_join(g_mode_a_tcp_upload.thread, NULL);
    }

    pthread_mutex_lock(&g_mode_a_tcp_upload.mutex);
    g_mode_a_tcp_upload.thread_started = 0;
    g_mode_a_tcp_upload.client_fd = -1;
    pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
}

static void mode_a_tcp_upload_start(void)
{
    struct timespec now;

    pthread_mutex_lock(&g_mode_a_tcp_upload.mutex);
    if (!g_mode_a_tcp_upload.thread_started ||
        g_mode_a_tcp_upload.client_fd < 0) {
        pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
        printf("[ModeA TCP] 尚无可用的TCP上传线程。\n");
        return;
    }

    if (g_mode_a_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
        printf("[ModeA TCP] 算法8/9共享8x1上传已经开启。\n");
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    g_mode_a_tcp_upload.frame_mask = 0;
    g_mode_a_tcp_upload.write_slot = 0;
    g_mode_a_tcp_upload.ready_slot = -1;
    g_mode_a_tcp_upload.ready_source = 0;
    g_mode_a_tcp_upload.next_sequence = 0;
    g_mode_a_tcp_upload.dropped_samples = 0;
    g_mode_a_tcp_upload.dropped_frames = 0;
    mode_a_tcp_schedule_next(&g_mode_a_tcp_upload.next_output_time, &now);
    g_mode_a_tcp_upload.enabled = 1;
    pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);

    printf("[ModeA TCP] 3003: 算法8/9模式A每秒8x1共享上传已开启。\n");
}

static void mode_a_tcp_upload_stop(void)
{
    unsigned long long dropped_samples;
    unsigned long long dropped_frames;

    pthread_mutex_lock(&g_mode_a_tcp_upload.mutex);
    g_mode_a_tcp_upload.enabled = 0;
    g_mode_a_tcp_upload.frame_mask = 0;
    g_mode_a_tcp_upload.ready_slot = -1;
    dropped_samples = g_mode_a_tcp_upload.dropped_samples;
    dropped_frames = g_mode_a_tcp_upload.dropped_frames;
    pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);

    printf("[ModeA TCP] 3004: 算法8/9模式A 8x1共享上传已停止，丢点=%llu，丢帧=%llu。\n",
           dropped_samples, dropped_frames);
}


static int mode_a_tcp_upload_is_enabled(void)
{
    int enabled;

    pthread_mutex_lock(&g_mode_a_tcp_upload.mutex);
    enabled = g_mode_a_tcp_upload.enabled;
    pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);

    return enabled;
}

/* 模式A共享采样器生成一组8x1后调用；这里只复制32字节，不执行send。 */
static void mode_a_tcp_upload_submit_packet(
    unsigned char source,
    const float values[MODE_A_TCP_UPLOAD_CHANNEL_COUNT])
{
    unsigned int completed_slot;
    uint32_t completed_sequence;

    if (!g_mode_a_tcp_upload.enabled) {
        return;
    }

    if (pthread_mutex_trylock(&g_mode_a_tcp_upload.mutex) != 0) {
        __sync_fetch_and_add(&g_mode_a_tcp_upload.dropped_frames, 1ULL);
        return;
    }

    if (!g_mode_a_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
        return;
    }

    if (g_mode_a_tcp_upload.ready_slot >= 0) {
        g_mode_a_tcp_upload.dropped_frames++;
        pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
        return;
    }

    completed_slot = g_mode_a_tcp_upload.write_slot;
    completed_sequence = g_mode_a_tcp_upload.next_sequence++;
    memcpy(g_mode_a_tcp_upload.buffers[completed_slot], values,
           MODE_A_TCP_UPLOAD_PAYLOAD_BYTES);

    g_mode_a_tcp_upload.ready_slot = (int)completed_slot;
    g_mode_a_tcp_upload.ready_source = source;
    g_mode_a_tcp_upload.ready_sequence = completed_sequence;
    g_mode_a_tcp_upload.ready_timestamp_us = hf_tcp_timestamp_us();
    g_mode_a_tcp_upload.write_slot ^= 1U;
    pthread_cond_signal(&g_mode_a_tcp_upload.cond);
    pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
}

/* 旧的模式A独立逐样本上传入口已不再由deal_ad7915()调用，保留函数体避免影响其它引用。 */
static void mode_a_tcp_upload_collect_sample(unsigned int ad7915_index,
                                             unsigned int physical_channel,
                                             int raw_value)
{
    struct timespec now;
    unsigned int completed_slot;
    uint32_t completed_sequence;

    if (!g_mode_a_tcp_upload.enabled ||
        ad7915_index != 0U ||
        physical_channel >= MODE_A_TCP_UPLOAD_CHANNEL_COUNT) {
        return;
    }

    if (pthread_mutex_trylock(&g_mode_a_tcp_upload.mutex) != 0) {
        __sync_fetch_and_add(&g_mode_a_tcp_upload.dropped_samples, 1ULL);
        return;
    }

    if (!g_mode_a_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
        return;
    }

    /* 通道0作为一次新扫描的起点，避免缺通道时跨扫描周期拼帧。 */
    if (physical_channel == 0U) {
        g_mode_a_tcp_upload.frame_mask = 0;
    }

    g_mode_a_tcp_upload.raw_frame[physical_channel] = raw_value;
    g_mode_a_tcp_upload.frame_mask |=
        (unsigned char)(1U << physical_channel);

    if (g_mode_a_tcp_upload.frame_mask != 0xFFU) {
        pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
        return;
    }

    g_mode_a_tcp_upload.frame_mask = 0;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!mode_a_tcp_time_reached(&now,
                                 &g_mode_a_tcp_upload.next_output_time)) {
        pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
        return;
    }

    completed_slot = g_mode_a_tcp_upload.write_slot;
    completed_sequence = g_mode_a_tcp_upload.next_sequence++;
    mode_a_tcp_schedule_next(&g_mode_a_tcp_upload.next_output_time, &now);

    if (g_mode_a_tcp_upload.ready_slot >= 0) {
        g_mode_a_tcp_upload.dropped_frames++;
        pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
        return;
    }

    for (unsigned int ch = 0; ch < MODE_A_TCP_UPLOAD_CHANNEL_COUNT; ch++) {
        g_mode_a_tcp_upload.buffers[completed_slot][ch] =
            mode_a_tcp_raw_to_physical(
                ch, g_mode_a_tcp_upload.raw_frame[ch]);
    }

    g_mode_a_tcp_upload.ready_slot = (int)completed_slot;
    g_mode_a_tcp_upload.ready_source = 0;
    g_mode_a_tcp_upload.ready_sequence = completed_sequence;
    g_mode_a_tcp_upload.ready_timestamp_us = hf_tcp_timestamp_us();
    g_mode_a_tcp_upload.write_slot ^= 1U;
    pthread_cond_signal(&g_mode_a_tcp_upload.cond);
    pthread_mutex_unlock(&g_mode_a_tcp_upload.mutex);
}

/* ===================== 腐蚀板256点float数据包独立上传 =====================/////////////////////////////////////////////////////////////////////////////
 * 3005: 开启上传；3006: 停止上传。
 * 上传内容复用算法4/5腐蚀板采样器生成的同一包256个float，
 * 采样线程只提交完整数据包到双缓冲，不直接执行socket发送。
 */
#define CORROSION_TCP_UPLOAD_CMD_START       3005U
#define CORROSION_TCP_UPLOAD_CMD_STOP        3006U
#define CORROSION_TCP_UPLOAD_LOGICAL_CH      4
#define CORROSION_TCP_UPLOAD_SAMPLE_COUNT    256U
#define CORROSION_TCP_UPLOAD_PAYLOAD_BYTES   \
    (CORROSION_TCP_UPLOAD_SAMPLE_COUNT * sizeof(float))
#define CORROSION_TCP_UPLOAD_HEADER_BYTES    24U
#define CORROSION_TCP_UPLOAD_FOOTER_BYTES    2U
#define CORROSION_TCP_UPLOAD_FRAME_BYTES     \
    (CORROSION_TCP_UPLOAD_HEADER_BYTES + CORROSION_TCP_UPLOAD_PAYLOAD_BYTES + \
     CORROSION_TCP_UPLOAD_FOOTER_BYTES)

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    volatile int enabled;
    int thread_started;
    int shutdown_requested;
    int client_fd;
    float buffers[2][CORROSION_TCP_UPLOAD_SAMPLE_COUNT];
    unsigned int write_slot;
    int ready_slot;
    uint32_t next_sequence;
    uint32_t ready_sequence;
    uint64_t ready_timestamp_us;
    int ready_algo_id;
    int ready_channel;
    unsigned long long submitted_packets;
    unsigned long long dropped_packets;
} CorrosionTcpUploadState;

static CorrosionTcpUploadState g_corrosion_tcp_upload = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .enabled = 0,
    .thread_started = 0,
    .shutdown_requested = 0,
    .client_fd = -1,
    .write_slot = 0,
    .ready_slot = -1,
    .next_sequence = 0,
    .ready_algo_id = 0,
    .ready_channel = CORROSION_TCP_UPLOAD_LOGICAL_CH
};

static int corrosion_tcp_upload_is_enabled(void)
{
    return g_corrosion_tcp_upload.enabled != 0;
}

/*
 * 腐蚀板256点上传帧：
 *  0..1   AA 55
 *  2      协议版本1
 *  3      帧类型3，表示腐蚀板256点float包
 *  4      逻辑通道号，当前为4
 *  5      数据来源算法号：0=仅3005上传，4=算法4，5=算法5，45=算法4/5同时开启
 *  6..9   包序号
 * 10..11  数值数量，固定256
 * 12..15  有效载荷长度，固定1024字节
 * 16..23  完成组包时的Unix时间戳，单位微秒
 * 24..    256个IEEE754 float32，按小端排列
 * 最后2字节 FF FF
 */
static int corrosion_tcp_upload_send_frame(
    int fd, int algo_id, int channel, uint32_t sequence,
    uint64_t timestamp_us,
    const float values[CORROSION_TCP_UPLOAD_SAMPLE_COUNT])
{
    unsigned char frame[CORROSION_TCP_UPLOAD_FRAME_BYTES];
    int ret;

    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[2] = 0x01;
    frame[3] = 0x03;
    frame[4] = (unsigned char)channel;
    frame[5] = (unsigned char)algo_id;
    hf_tcp_put_u32_le(frame + 6, sequence);
    hf_tcp_put_u16_le(frame + 10,
                      (uint16_t)CORROSION_TCP_UPLOAD_SAMPLE_COUNT);
    hf_tcp_put_u32_le(frame + 12,
                      (uint32_t)CORROSION_TCP_UPLOAD_PAYLOAD_BYTES);
    hf_tcp_put_u64_le(frame + 16, timestamp_us);

    for (unsigned int i = 0; i < CORROSION_TCP_UPLOAD_SAMPLE_COUNT; i++) {
        uint32_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        hf_tcp_put_u32_le(frame + CORROSION_TCP_UPLOAD_HEADER_BYTES +
                          i * sizeof(bits), bits);
    }

    frame[CORROSION_TCP_UPLOAD_FRAME_BYTES - 2U] = 0xFF;
    frame[CORROSION_TCP_UPLOAD_FRAME_BYTES - 1U] = 0xFF;

    pthread_mutex_lock(&g_tcp_socket_send_mutex);
    ret = hf_tcp_send_all(fd, frame, sizeof(frame));
    pthread_mutex_unlock(&g_tcp_socket_send_mutex);

    return ret;
}

static void *corrosion_tcp_upload_thread(void *arg)
{
    float local_values[CORROSION_TCP_UPLOAD_SAMPLE_COUNT];
    (void)arg;

    for (;;) {
        int fd;
        int slot;
        int algo_id;
        int channel;
        uint32_t sequence;
        uint64_t timestamp_us;

        pthread_mutex_lock(&g_corrosion_tcp_upload.mutex);
        while (g_corrosion_tcp_upload.ready_slot < 0 &&
               !g_corrosion_tcp_upload.shutdown_requested) {
            pthread_cond_wait(&g_corrosion_tcp_upload.cond,
                              &g_corrosion_tcp_upload.mutex);
        }

        if (g_corrosion_tcp_upload.shutdown_requested) {
            pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);
            break;
        }

        slot = g_corrosion_tcp_upload.ready_slot;
        sequence = g_corrosion_tcp_upload.ready_sequence;
        timestamp_us = g_corrosion_tcp_upload.ready_timestamp_us;
        fd = g_corrosion_tcp_upload.client_fd;
        algo_id = g_corrosion_tcp_upload.ready_algo_id;
        channel = g_corrosion_tcp_upload.ready_channel;
        memcpy(local_values, g_corrosion_tcp_upload.buffers[slot],
               sizeof(local_values));
        g_corrosion_tcp_upload.ready_slot = -1;
        pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);

        if (corrosion_tcp_upload_send_frame(fd, algo_id, channel, sequence,
                                            timestamp_us, local_values) != 0) {
            printf("[Corrosion TCP] 腐蚀板256点帧发送失败，停止上传。\n");
            pthread_mutex_lock(&g_corrosion_tcp_upload.mutex);
            g_corrosion_tcp_upload.enabled = 0;
            pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);
            shutdown(fd, SHUT_RDWR);
            break;
        }

        printf("[Corrosion TCP] 已发送第%u包，来源=%d，通道=%d，点数=%u。\n",
               sequence, algo_id, channel, CORROSION_TCP_UPLOAD_SAMPLE_COUNT);
    }

    return NULL;
}

static int corrosion_tcp_upload_attach_client(int client_fd)
{
    int ret;

    pthread_mutex_lock(&g_corrosion_tcp_upload.mutex);
    if (g_corrosion_tcp_upload.thread_started) {
        pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);
        return -1;
    }

    g_corrosion_tcp_upload.client_fd = client_fd;
    g_corrosion_tcp_upload.enabled = 0;
    g_corrosion_tcp_upload.shutdown_requested = 0;
    g_corrosion_tcp_upload.write_slot = 0;
    g_corrosion_tcp_upload.ready_slot = -1;
    g_corrosion_tcp_upload.next_sequence = 0;
    g_corrosion_tcp_upload.thread_started = 1;
    pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);

    ret = pthread_create(&g_corrosion_tcp_upload.thread, NULL,
                         corrosion_tcp_upload_thread, NULL);
    if (ret != 0) {
        pthread_mutex_lock(&g_corrosion_tcp_upload.mutex);
        g_corrosion_tcp_upload.thread_started = 0;
        g_corrosion_tcp_upload.client_fd = -1;
        pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);
        printf("[Corrosion TCP] 创建独立上传线程失败: %d\n", ret);
        return -1;
    }

    return 0;
}

static void corrosion_tcp_upload_detach_client(void)
{
    int should_join;
    int client_fd;

    pthread_mutex_lock(&g_corrosion_tcp_upload.mutex);
    should_join = g_corrosion_tcp_upload.thread_started;
    client_fd = g_corrosion_tcp_upload.client_fd;
    g_corrosion_tcp_upload.enabled = 0;
    g_corrosion_tcp_upload.shutdown_requested = 1;
    g_corrosion_tcp_upload.ready_slot = -1;
    pthread_cond_signal(&g_corrosion_tcp_upload.cond);
    pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);

    if (should_join) {
        if (client_fd >= 0) {
            shutdown(client_fd, SHUT_RDWR);
        }
        pthread_join(g_corrosion_tcp_upload.thread, NULL);
    }

    pthread_mutex_lock(&g_corrosion_tcp_upload.mutex);
    g_corrosion_tcp_upload.thread_started = 0;
    g_corrosion_tcp_upload.client_fd = -1;
    pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);
}

static void corrosion_tcp_upload_start(void)
{
    pthread_mutex_lock(&g_corrosion_tcp_upload.mutex);
    if (!g_corrosion_tcp_upload.thread_started ||
        g_corrosion_tcp_upload.client_fd < 0) {
        pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);
        printf("[Corrosion TCP] 尚无可用的TCP上传线程。\n");
        return;
    }

    if (g_corrosion_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);
        printf("[Corrosion TCP] 腐蚀板256点上传已经开启。\n");
        return;
    }

    g_corrosion_tcp_upload.write_slot = 0;
    g_corrosion_tcp_upload.ready_slot = -1;
    g_corrosion_tcp_upload.next_sequence = 0;
    g_corrosion_tcp_upload.submitted_packets = 0;
    g_corrosion_tcp_upload.dropped_packets = 0;
    g_corrosion_tcp_upload.enabled = 1;
    pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);

    printf("[Corrosion TCP] 3005: 腐蚀板256点float上传已开启，可独立驱动采样器。\n");
}

static void corrosion_tcp_upload_stop(void)
{
    unsigned long long submitted_packets;
    unsigned long long dropped_packets;

    pthread_mutex_lock(&g_corrosion_tcp_upload.mutex);
    g_corrosion_tcp_upload.enabled = 0;
    g_corrosion_tcp_upload.ready_slot = -1;
    submitted_packets = g_corrosion_tcp_upload.submitted_packets;
    dropped_packets = g_corrosion_tcp_upload.dropped_packets;
    pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);

    printf("[Corrosion TCP] 3006: 腐蚀板256点上传已停止，提交包=%llu，丢包=%llu。\n",
           submitted_packets, dropped_packets);
}

static void corrosion_tcp_upload_submit(
    int source_algo_id, int channel,
    const float values[CORROSION_TCP_UPLOAD_SAMPLE_COUNT])
{
    unsigned int completed_slot;
    uint32_t completed_sequence;

    if (!g_corrosion_tcp_upload.enabled) {
        return;
    }

    if (pthread_mutex_trylock(&g_corrosion_tcp_upload.mutex) != 0) {
        __sync_fetch_and_add(&g_corrosion_tcp_upload.dropped_packets, 1ULL);
        return;
    }

    if (!g_corrosion_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);
        return;
    }

    if (g_corrosion_tcp_upload.ready_slot >= 0) {
        g_corrosion_tcp_upload.dropped_packets++;
        pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);
        return;
    }

    completed_slot = g_corrosion_tcp_upload.write_slot;
    completed_sequence = g_corrosion_tcp_upload.next_sequence++;
    memcpy(g_corrosion_tcp_upload.buffers[completed_slot], values,
           sizeof(g_corrosion_tcp_upload.buffers[completed_slot]));

    g_corrosion_tcp_upload.ready_slot = (int)completed_slot;
    g_corrosion_tcp_upload.ready_sequence = completed_sequence;
    g_corrosion_tcp_upload.ready_timestamp_us = hf_tcp_timestamp_us();
    g_corrosion_tcp_upload.ready_algo_id = source_algo_id;
    g_corrosion_tcp_upload.ready_channel = channel;
    g_corrosion_tcp_upload.write_slot ^= 1U;
    g_corrosion_tcp_upload.submitted_packets++;
    pthread_cond_signal(&g_corrosion_tcp_upload.cond);
    pthread_mutex_unlock(&g_corrosion_tcp_upload.mutex);
}


/* ===================== 漏油12点float数据包独立上传 =====================/////////////////////////////////////////////////////////////////////////////
 * 3007: 开启上传；3008: 停止上传。
 * 上传内容复用算法10/11漏油采样器生成的同一包12个实际电压float，
 * 采样线程只提交完整数据包到双缓冲，不直接执行socket发送。
 */
#define LEAK_TCP_UPLOAD_CMD_START       3007U
#define LEAK_TCP_UPLOAD_CMD_STOP        3008U
#define LEAK_TCP_UPLOAD_LOGICAL_CH      5
#define LEAK_TCP_UPLOAD_SAMPLE_COUNT    12U
#define LEAK_TCP_UPLOAD_PAYLOAD_BYTES   \
    (LEAK_TCP_UPLOAD_SAMPLE_COUNT * sizeof(float))
#define LEAK_TCP_UPLOAD_HEADER_BYTES    24U
#define LEAK_TCP_UPLOAD_FOOTER_BYTES    2U
#define LEAK_TCP_UPLOAD_FRAME_BYTES     \
    (LEAK_TCP_UPLOAD_HEADER_BYTES + LEAK_TCP_UPLOAD_PAYLOAD_BYTES + \
     LEAK_TCP_UPLOAD_FOOTER_BYTES)

#define LEAK_TCP_CONSUMER_ALGO10        0x01U
#define LEAK_TCP_CONSUMER_ALGO11        0x02U

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    volatile int enabled;
    int thread_started;
    int shutdown_requested;
    int client_fd;
    float buffers[2][LEAK_TCP_UPLOAD_SAMPLE_COUNT];
    unsigned int write_slot;
    int ready_slot;
    uint32_t next_sequence;
    uint32_t ready_sequence;
    uint64_t ready_timestamp_us;
    uint8_t ready_consumer_mask;
    int ready_channel;
    unsigned long long submitted_packets;
    unsigned long long dropped_packets;
} LeakTcpUploadState;

static LeakTcpUploadState g_leak_tcp_upload = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .enabled = 0,
    .thread_started = 0,
    .shutdown_requested = 0,
    .client_fd = -1,
    .write_slot = 0,
    .ready_slot = -1,
    .next_sequence = 0,
    .ready_consumer_mask = 0,
    .ready_channel = LEAK_TCP_UPLOAD_LOGICAL_CH
};

static int leak_tcp_upload_is_enabled(void)
{
    return g_leak_tcp_upload.enabled != 0;
}

/*
 * 漏油12点上传帧：
 *  0..1   AA 55
 *  2      协议版本1
 *  3      帧类型4，表示算法10/11漏油12点float包
 *  4      逻辑通道号，当前为5
 *  5      消费者掩码：bit0=算法10，bit1=算法11，0=仅3007上传
 *  6..9   包序号
 * 10..11  数值数量，固定12
 * 12..15  有效载荷长度，固定48字节
 * 16..23  完成组包时的Unix时间戳，单位微秒
 * 24..    12个IEEE754 float32，按小端排列
 * 最后2字节 FF FF
 */
static int leak_tcp_upload_send_frame(
    int fd, uint8_t consumer_mask, int channel, uint32_t sequence,
    uint64_t timestamp_us,
    const float values[LEAK_TCP_UPLOAD_SAMPLE_COUNT])
{
    unsigned char frame[LEAK_TCP_UPLOAD_FRAME_BYTES];
    int ret;

    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[2] = 0x01;
    frame[3] = 0x04;
    frame[4] = (unsigned char)channel;
    frame[5] = consumer_mask;
    hf_tcp_put_u32_le(frame + 6, sequence);
    hf_tcp_put_u16_le(frame + 10,
                      (uint16_t)LEAK_TCP_UPLOAD_SAMPLE_COUNT);
    hf_tcp_put_u32_le(frame + 12,
                      (uint32_t)LEAK_TCP_UPLOAD_PAYLOAD_BYTES);
    hf_tcp_put_u64_le(frame + 16, timestamp_us);

    for (unsigned int i = 0; i < LEAK_TCP_UPLOAD_SAMPLE_COUNT; i++) {
        uint32_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        hf_tcp_put_u32_le(frame + LEAK_TCP_UPLOAD_HEADER_BYTES +
                          i * sizeof(bits), bits);
    }

    frame[LEAK_TCP_UPLOAD_FRAME_BYTES - 2U] = 0xFF;
    frame[LEAK_TCP_UPLOAD_FRAME_BYTES - 1U] = 0xFF;

    pthread_mutex_lock(&g_tcp_socket_send_mutex);
    ret = hf_tcp_send_all(fd, frame, sizeof(frame));
    pthread_mutex_unlock(&g_tcp_socket_send_mutex);

    return ret;
}

static void *leak_tcp_upload_thread(void *arg)
{
    float local_values[LEAK_TCP_UPLOAD_SAMPLE_COUNT];
    (void)arg;

    for (;;) {
        int fd;
        int slot;
        int channel;
        uint8_t consumer_mask;
        uint32_t sequence;
        uint64_t timestamp_us;

        pthread_mutex_lock(&g_leak_tcp_upload.mutex);
        while (g_leak_tcp_upload.ready_slot < 0 &&
               !g_leak_tcp_upload.shutdown_requested) {
            pthread_cond_wait(&g_leak_tcp_upload.cond,
                              &g_leak_tcp_upload.mutex);
        }

        if (g_leak_tcp_upload.shutdown_requested) {
            pthread_mutex_unlock(&g_leak_tcp_upload.mutex);
            break;
        }

        slot = g_leak_tcp_upload.ready_slot;
        sequence = g_leak_tcp_upload.ready_sequence;
        timestamp_us = g_leak_tcp_upload.ready_timestamp_us;
        fd = g_leak_tcp_upload.client_fd;
        consumer_mask = g_leak_tcp_upload.ready_consumer_mask;
        channel = g_leak_tcp_upload.ready_channel;
        memcpy(local_values, g_leak_tcp_upload.buffers[slot],
               sizeof(local_values));
        g_leak_tcp_upload.ready_slot = -1;
        pthread_mutex_unlock(&g_leak_tcp_upload.mutex);

        if (leak_tcp_upload_send_frame(fd, consumer_mask, channel, sequence,
                                       timestamp_us, local_values) != 0) {
            printf("[Leak TCP] 漏油12点帧发送失败，停止上传。\n");
            pthread_mutex_lock(&g_leak_tcp_upload.mutex);
            g_leak_tcp_upload.enabled = 0;
            pthread_mutex_unlock(&g_leak_tcp_upload.mutex);
            shutdown(fd, SHUT_RDWR);
            break;
        }

        printf("[Leak TCP] 已发送第%u包，消费者掩码=0x%02X，通道=%d，点数=%u。\n",
               sequence, consumer_mask, channel, LEAK_TCP_UPLOAD_SAMPLE_COUNT);
    }

    return NULL;
}

static int leak_tcp_upload_attach_client(int client_fd)
{
    int ret;

    pthread_mutex_lock(&g_leak_tcp_upload.mutex);
    if (g_leak_tcp_upload.thread_started) {
        pthread_mutex_unlock(&g_leak_tcp_upload.mutex);
        return -1;
    }

    g_leak_tcp_upload.client_fd = client_fd;
    g_leak_tcp_upload.enabled = 0;
    g_leak_tcp_upload.shutdown_requested = 0;
    g_leak_tcp_upload.write_slot = 0;
    g_leak_tcp_upload.ready_slot = -1;
    g_leak_tcp_upload.next_sequence = 0;
    g_leak_tcp_upload.thread_started = 1;
    pthread_mutex_unlock(&g_leak_tcp_upload.mutex);

    ret = pthread_create(&g_leak_tcp_upload.thread, NULL,
                         leak_tcp_upload_thread, NULL);
    if (ret != 0) {
        pthread_mutex_lock(&g_leak_tcp_upload.mutex);
        g_leak_tcp_upload.thread_started = 0;
        g_leak_tcp_upload.client_fd = -1;
        pthread_mutex_unlock(&g_leak_tcp_upload.mutex);
        printf("[Leak TCP] 创建独立上传线程失败: %d\n", ret);
        return -1;
    }

    return 0;
}

static void leak_tcp_upload_detach_client(void)
{
    int should_join;
    int client_fd;

    pthread_mutex_lock(&g_leak_tcp_upload.mutex);
    should_join = g_leak_tcp_upload.thread_started;
    client_fd = g_leak_tcp_upload.client_fd;
    g_leak_tcp_upload.enabled = 0;
    g_leak_tcp_upload.shutdown_requested = 1;
    g_leak_tcp_upload.ready_slot = -1;
    pthread_cond_signal(&g_leak_tcp_upload.cond);
    pthread_mutex_unlock(&g_leak_tcp_upload.mutex);

    if (should_join) {
        if (client_fd >= 0) {
            shutdown(client_fd, SHUT_RDWR);
        }
        pthread_join(g_leak_tcp_upload.thread, NULL);
    }

    pthread_mutex_lock(&g_leak_tcp_upload.mutex);
    g_leak_tcp_upload.thread_started = 0;
    g_leak_tcp_upload.client_fd = -1;
    pthread_mutex_unlock(&g_leak_tcp_upload.mutex);
}

static void leak_tcp_upload_start(void)
{
    pthread_mutex_lock(&g_leak_tcp_upload.mutex);
    if (!g_leak_tcp_upload.thread_started ||
        g_leak_tcp_upload.client_fd < 0) {
        pthread_mutex_unlock(&g_leak_tcp_upload.mutex);
        printf("[Leak TCP] 尚无可用的TCP上传线程。\n");
        return;
    }

    if (g_leak_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_leak_tcp_upload.mutex);
        printf("[Leak TCP] 漏油12点上传已经开启。\n");
        return;
    }

    g_leak_tcp_upload.write_slot = 0;
    g_leak_tcp_upload.ready_slot = -1;
    g_leak_tcp_upload.next_sequence = 0;
    g_leak_tcp_upload.submitted_packets = 0;
    g_leak_tcp_upload.dropped_packets = 0;
    g_leak_tcp_upload.enabled = 1;
    pthread_mutex_unlock(&g_leak_tcp_upload.mutex);

    printf("[Leak TCP] 3007: 漏油12点float上传已开启，可独立驱动算法10/11采样器。\n");
}

static void leak_tcp_upload_stop(void)
{
    unsigned long long submitted_packets;
    unsigned long long dropped_packets;

    pthread_mutex_lock(&g_leak_tcp_upload.mutex);
    g_leak_tcp_upload.enabled = 0;
    g_leak_tcp_upload.ready_slot = -1;
    submitted_packets = g_leak_tcp_upload.submitted_packets;
    dropped_packets = g_leak_tcp_upload.dropped_packets;
    pthread_mutex_unlock(&g_leak_tcp_upload.mutex);

    printf("[Leak TCP] 3008: 漏油12点上传已停止，提交包=%llu，丢包=%llu。\n",
           submitted_packets, dropped_packets);
}

static void leak_tcp_upload_submit(
    uint8_t consumer_mask, int channel,
    const float values[LEAK_TCP_UPLOAD_SAMPLE_COUNT])
{
    unsigned int completed_slot;
    uint32_t completed_sequence;

    if (!g_leak_tcp_upload.enabled) {
        return;
    }

    if (pthread_mutex_trylock(&g_leak_tcp_upload.mutex) != 0) {
        __sync_fetch_and_add(&g_leak_tcp_upload.dropped_packets, 1ULL);
        return;
    }

    if (!g_leak_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_leak_tcp_upload.mutex);
        return;
    }

    if (g_leak_tcp_upload.ready_slot >= 0) {
        g_leak_tcp_upload.dropped_packets++;
        pthread_mutex_unlock(&g_leak_tcp_upload.mutex);
        return;
    }

    completed_slot = g_leak_tcp_upload.write_slot;
    completed_sequence = g_leak_tcp_upload.next_sequence++;
    memcpy(g_leak_tcp_upload.buffers[completed_slot], values,
           sizeof(g_leak_tcp_upload.buffers[completed_slot]));

    g_leak_tcp_upload.ready_slot = (int)completed_slot;
    g_leak_tcp_upload.ready_sequence = completed_sequence;
    g_leak_tcp_upload.ready_timestamp_us = hf_tcp_timestamp_us();
    g_leak_tcp_upload.ready_consumer_mask = consumer_mask;
    g_leak_tcp_upload.ready_channel = channel;
    g_leak_tcp_upload.write_slot ^= 1U;
    g_leak_tcp_upload.submitted_packets++;
    pthread_cond_signal(&g_leak_tcp_upload.cond);
    pthread_mutex_unlock(&g_leak_tcp_upload.mutex);
}


/* ===================== 算法6/7 512点float数据包独立上传 =====================/////////////////////////////////////////////////////////////////////////////
 * 3009: 开启上传；3010: 停止上传。
 * 上传内容复用算法6/7五点平均同步采样器生成的同一包512个float，
 * 采样线程只提交完整数据包到双缓冲，不直接执行socket发送。
 * 每一包开始前仍然必须重新确认低电平<0.5V，再等待>3V包头。
 */
#define ALGO67_TCP_UPLOAD_CMD_START       3009U
#define ALGO67_TCP_UPLOAD_CMD_STOP        3010U
#define ALGO67_TCP_UPLOAD_LOGICAL_CH      4
#define ALGO67_TCP_UPLOAD_SAMPLE_COUNT    512U
#define ALGO67_TCP_UPLOAD_PAYLOAD_BYTES   \
    (ALGO67_TCP_UPLOAD_SAMPLE_COUNT * sizeof(float))
#define ALGO67_TCP_UPLOAD_HEADER_BYTES    24U
#define ALGO67_TCP_UPLOAD_FOOTER_BYTES    2U
#define ALGO67_TCP_UPLOAD_FRAME_BYTES     \
    (ALGO67_TCP_UPLOAD_HEADER_BYTES + ALGO67_TCP_UPLOAD_PAYLOAD_BYTES + \
     ALGO67_TCP_UPLOAD_FOOTER_BYTES)

#define ALGO67_TCP_CONSUMER_ALGO6         0x01U
#define ALGO67_TCP_CONSUMER_ALGO7         0x02U

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    volatile int enabled;
    int thread_started;
    int shutdown_requested;
    int client_fd;
    float buffers[2][ALGO67_TCP_UPLOAD_SAMPLE_COUNT];
    unsigned int write_slot;
    int ready_slot;
    uint32_t next_sequence;
    uint32_t ready_sequence;
    uint64_t ready_timestamp_us;
    uint8_t ready_consumer_mask;
    int ready_channel;
    unsigned long long submitted_packets;
    unsigned long long dropped_packets;
} Algo67TcpUploadState;

static Algo67TcpUploadState g_algo67_tcp_upload = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .enabled = 0,
    .thread_started = 0,
    .shutdown_requested = 0,
    .client_fd = -1,
    .write_slot = 0,
    .ready_slot = -1,
    .next_sequence = 0,
    .ready_consumer_mask = 0,
    .ready_channel = ALGO67_TCP_UPLOAD_LOGICAL_CH
};

static int algo67_tcp_upload_is_enabled(void)
{
    return g_algo67_tcp_upload.enabled != 0;
}

/*
 * 算法6/7 512点上传帧：
 *  0..1   AA 55
 *  2      协议版本1
 *  3      帧类型5，表示算法6/7 512点float包
 *  4      逻辑通道号，当前为4
 *  5      消费者掩码：bit0=算法6，bit1=算法7，0=仅3009上传
 *  6..9   包序号
 * 10..11  数值数量，固定512
 * 12..15  有效载荷长度，固定2048字节
 * 16..23  完成组包时的Unix时间戳，单位微秒
 * 24..    512个IEEE754 float32，按小端排列
 * 最后2字节 FF FF
 */
static int algo67_tcp_upload_send_frame(
    int fd, uint8_t consumer_mask, int channel, uint32_t sequence,
    uint64_t timestamp_us,
    const float values[ALGO67_TCP_UPLOAD_SAMPLE_COUNT])
{
    unsigned char frame[ALGO67_TCP_UPLOAD_FRAME_BYTES];
    int ret;

    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[2] = 0x01;
    frame[3] = 0x05;
    frame[4] = (unsigned char)channel;
    frame[5] = consumer_mask;
    hf_tcp_put_u32_le(frame + 6, sequence);
    hf_tcp_put_u16_le(frame + 10,
                      (uint16_t)ALGO67_TCP_UPLOAD_SAMPLE_COUNT);
    hf_tcp_put_u32_le(frame + 12,
                      (uint32_t)ALGO67_TCP_UPLOAD_PAYLOAD_BYTES);
    hf_tcp_put_u64_le(frame + 16, timestamp_us);

    for (unsigned int i = 0; i < ALGO67_TCP_UPLOAD_SAMPLE_COUNT; i++) {
        uint32_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        hf_tcp_put_u32_le(frame + ALGO67_TCP_UPLOAD_HEADER_BYTES +
                          i * sizeof(bits), bits);
    }

    frame[ALGO67_TCP_UPLOAD_FRAME_BYTES - 2U] = 0xFF;
    frame[ALGO67_TCP_UPLOAD_FRAME_BYTES - 1U] = 0xFF;

    pthread_mutex_lock(&g_tcp_socket_send_mutex);
    ret = hf_tcp_send_all(fd, frame, sizeof(frame));
    pthread_mutex_unlock(&g_tcp_socket_send_mutex);

    return ret;
}

static void *algo67_tcp_upload_thread(void *arg)
{
    float local_values[ALGO67_TCP_UPLOAD_SAMPLE_COUNT];
    (void)arg;

    for (;;) {
        int fd;
        int slot;
        int channel;
        uint8_t consumer_mask;
        uint32_t sequence;
        uint64_t timestamp_us;

        pthread_mutex_lock(&g_algo67_tcp_upload.mutex);
        while (g_algo67_tcp_upload.ready_slot < 0 &&
               !g_algo67_tcp_upload.shutdown_requested) {
            pthread_cond_wait(&g_algo67_tcp_upload.cond,
                              &g_algo67_tcp_upload.mutex);
        }

        if (g_algo67_tcp_upload.shutdown_requested) {
            pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);
            break;
        }

        slot = g_algo67_tcp_upload.ready_slot;
        sequence = g_algo67_tcp_upload.ready_sequence;
        timestamp_us = g_algo67_tcp_upload.ready_timestamp_us;
        fd = g_algo67_tcp_upload.client_fd;
        consumer_mask = g_algo67_tcp_upload.ready_consumer_mask;
        channel = g_algo67_tcp_upload.ready_channel;
        memcpy(local_values, g_algo67_tcp_upload.buffers[slot],
               sizeof(local_values));
        g_algo67_tcp_upload.ready_slot = -1;
        pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);

        if (algo67_tcp_upload_send_frame(fd, consumer_mask, channel,
                                         sequence, timestamp_us,
                                         local_values) != 0) {
            printf("[Algo6/7 TCP] 512点帧发送失败，停止上传。\n");
            pthread_mutex_lock(&g_algo67_tcp_upload.mutex);
            g_algo67_tcp_upload.enabled = 0;
            pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);
            shutdown(fd, SHUT_RDWR);
            break;
        }

        printf("[Algo6/7 TCP] 已发送第%u包，消费者掩码=0x%02X，通道=%d，点数=%u。\n",
               sequence, consumer_mask, channel,
               ALGO67_TCP_UPLOAD_SAMPLE_COUNT);
    }

    return NULL;
}

static int algo67_tcp_upload_attach_client(int client_fd)
{
    int ret;

    pthread_mutex_lock(&g_algo67_tcp_upload.mutex);
    if (g_algo67_tcp_upload.thread_started) {
        pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);
        return -1;
    }

    g_algo67_tcp_upload.client_fd = client_fd;
    g_algo67_tcp_upload.enabled = 0;
    g_algo67_tcp_upload.shutdown_requested = 0;
    g_algo67_tcp_upload.write_slot = 0;
    g_algo67_tcp_upload.ready_slot = -1;
    g_algo67_tcp_upload.next_sequence = 0;
    g_algo67_tcp_upload.thread_started = 1;
    pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);

    ret = pthread_create(&g_algo67_tcp_upload.thread, NULL,
                         algo67_tcp_upload_thread, NULL);
    if (ret != 0) {
        pthread_mutex_lock(&g_algo67_tcp_upload.mutex);
        g_algo67_tcp_upload.thread_started = 0;
        g_algo67_tcp_upload.client_fd = -1;
        pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);
        printf("[Algo6/7 TCP] 创建独立上传线程失败: %d\n", ret);
        return -1;
    }

    return 0;
}

static void algo67_tcp_upload_detach_client(void)
{
    int should_join;
    int client_fd;

    pthread_mutex_lock(&g_algo67_tcp_upload.mutex);
    should_join = g_algo67_tcp_upload.thread_started;
    client_fd = g_algo67_tcp_upload.client_fd;
    g_algo67_tcp_upload.enabled = 0;
    g_algo67_tcp_upload.shutdown_requested = 1;
    g_algo67_tcp_upload.ready_slot = -1;
    pthread_cond_signal(&g_algo67_tcp_upload.cond);
    pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);

    if (should_join) {
        if (client_fd >= 0) {
            shutdown(client_fd, SHUT_RDWR);
        }
        pthread_join(g_algo67_tcp_upload.thread, NULL);
    }

    pthread_mutex_lock(&g_algo67_tcp_upload.mutex);
    g_algo67_tcp_upload.thread_started = 0;
    g_algo67_tcp_upload.client_fd = -1;
    pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);
}

static void algo67_tcp_upload_start(void)
{
    pthread_mutex_lock(&g_algo67_tcp_upload.mutex);
    if (!g_algo67_tcp_upload.thread_started ||
        g_algo67_tcp_upload.client_fd < 0) {
        pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);
        printf("[Algo6/7 TCP] 尚无可用的TCP上传线程。\n");
        return;
    }

    if (g_algo67_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);
        printf("[Algo6/7 TCP] 512点上传已经开启。\n");
        return;
    }

    g_algo67_tcp_upload.write_slot = 0;
    g_algo67_tcp_upload.ready_slot = -1;
    g_algo67_tcp_upload.next_sequence = 0;
    g_algo67_tcp_upload.submitted_packets = 0;
    g_algo67_tcp_upload.dropped_packets = 0;
    g_algo67_tcp_upload.enabled = 1;
    pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);

    printf("[Algo6/7 TCP] 3009: 算法6/7 512点float上传已开启，可独立驱动采样器。\n");
}

static void algo67_tcp_upload_stop(void)
{
    unsigned long long submitted_packets;
    unsigned long long dropped_packets;

    pthread_mutex_lock(&g_algo67_tcp_upload.mutex);
    g_algo67_tcp_upload.enabled = 0;
    g_algo67_tcp_upload.ready_slot = -1;
    submitted_packets = g_algo67_tcp_upload.submitted_packets;
    dropped_packets = g_algo67_tcp_upload.dropped_packets;
    pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);

    printf("[Algo6/7 TCP] 3010: 算法6/7 512点上传已停止，提交包=%llu，丢包=%llu。\n",
           submitted_packets, dropped_packets);
}

static void algo67_tcp_upload_submit(
    uint8_t consumer_mask, int channel,
    const float values[ALGO67_TCP_UPLOAD_SAMPLE_COUNT])
{
    unsigned int completed_slot;
    uint32_t completed_sequence;

    if (!g_algo67_tcp_upload.enabled) {
        return;
    }

    if (pthread_mutex_trylock(&g_algo67_tcp_upload.mutex) != 0) {
        __sync_fetch_and_add(&g_algo67_tcp_upload.dropped_packets, 1ULL);
        return;
    }

    if (!g_algo67_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);
        return;
    }

    if (g_algo67_tcp_upload.ready_slot >= 0) {
        g_algo67_tcp_upload.dropped_packets++;
        pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);
        return;
    }

    completed_slot = g_algo67_tcp_upload.write_slot;
    completed_sequence = g_algo67_tcp_upload.next_sequence++;
    memcpy(g_algo67_tcp_upload.buffers[completed_slot], values,
           sizeof(g_algo67_tcp_upload.buffers[completed_slot]));

    g_algo67_tcp_upload.ready_slot = (int)completed_slot;
    g_algo67_tcp_upload.ready_sequence = completed_sequence;
    g_algo67_tcp_upload.ready_timestamp_us = hf_tcp_timestamp_us();
    g_algo67_tcp_upload.ready_consumer_mask = consumer_mask;
    g_algo67_tcp_upload.ready_channel = channel;
    g_algo67_tcp_upload.write_slot ^= 1U;
    g_algo67_tcp_upload.submitted_packets++;
    pthread_cond_signal(&g_algo67_tcp_upload.cond);
    pthread_mutex_unlock(&g_algo67_tcp_upload.mutex);
}


/* ===================== 算法1 256点float数据包独立上传 =====================/////////////////////////////////////////////////////////////////////////////
 * 3011: 开启上传；3012: 停止上传。
 * 上传内容复用算法1快速包同步采样器生成的同一包256个float，
 * 采样线程只提交完整数据包到双缓冲，不直接执行socket发送。
 * 采样规则保持算法1原逻辑：>7V触发、50ms首点、100ms间隔、<=1V复位。
 */
#define ALGO1_TCP_UPLOAD_CMD_START       3011U
#define ALGO1_TCP_UPLOAD_CMD_STOP        3012U
#define ALGO1_TCP_UPLOAD_LOGICAL_CH      4
#define ALGO1_TCP_UPLOAD_SAMPLE_COUNT    256U
#define ALGO1_TCP_UPLOAD_PAYLOAD_BYTES   \
    (ALGO1_TCP_UPLOAD_SAMPLE_COUNT * sizeof(float))
#define ALGO1_TCP_UPLOAD_HEADER_BYTES    24U
#define ALGO1_TCP_UPLOAD_FOOTER_BYTES    2U
#define ALGO1_TCP_UPLOAD_FRAME_BYTES     \
    (ALGO1_TCP_UPLOAD_HEADER_BYTES + ALGO1_TCP_UPLOAD_PAYLOAD_BYTES + \
     ALGO1_TCP_UPLOAD_FOOTER_BYTES)

#define ALGO1_TCP_CONSUMER_ALGO1         0x01U

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    volatile int enabled;
    int thread_started;
    int shutdown_requested;
    int client_fd;
    float buffers[2][ALGO1_TCP_UPLOAD_SAMPLE_COUNT];
    unsigned int write_slot;
    int ready_slot;
    uint32_t next_sequence;
    uint32_t ready_sequence;
    uint64_t ready_timestamp_us;
    uint8_t ready_consumer_mask;
    int ready_channel;
    unsigned long long submitted_packets;
    unsigned long long dropped_packets;
} Algo1TcpUploadState;

static Algo1TcpUploadState g_algo1_tcp_upload = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .enabled = 0,
    .thread_started = 0,
    .shutdown_requested = 0,
    .client_fd = -1,
    .write_slot = 0,
    .ready_slot = -1,
    .next_sequence = 0,
    .ready_consumer_mask = 0,
    .ready_channel = ALGO1_TCP_UPLOAD_LOGICAL_CH
};

static int algo1_tcp_upload_is_enabled(void)
{
    return g_algo1_tcp_upload.enabled != 0;
}

/*
 * 算法1 256点上传帧：
 *  0..1   AA 55
 *  2      协议版本1
 *  3      帧类型6，表示算法1 256点float包
 *  4      逻辑通道号，当前为4
 *  5      消费者掩码：bit0=算法1，0=仅3011上传
 *  6..9   包序号
 * 10..11  数值数量，固定256
 * 12..15  有效载荷长度，固定1024字节
 * 16..23  完成组包时的Unix时间戳，单位微秒
 * 24..    256个IEEE754 float32，按小端排列
 * 最后2字节 FF FF
 */
static int algo1_tcp_upload_send_frame(
    int fd, uint8_t consumer_mask, int channel, uint32_t sequence,
    uint64_t timestamp_us,
    const float values[ALGO1_TCP_UPLOAD_SAMPLE_COUNT])
{
    unsigned char frame[ALGO1_TCP_UPLOAD_FRAME_BYTES];
    int ret;

    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[2] = 0x01;
    frame[3] = 0x06;
    frame[4] = (unsigned char)channel;
    frame[5] = consumer_mask;
    hf_tcp_put_u32_le(frame + 6, sequence);
    hf_tcp_put_u16_le(frame + 10,
                      (uint16_t)ALGO1_TCP_UPLOAD_SAMPLE_COUNT);
    hf_tcp_put_u32_le(frame + 12,
                      (uint32_t)ALGO1_TCP_UPLOAD_PAYLOAD_BYTES);
    hf_tcp_put_u64_le(frame + 16, timestamp_us);

    for (unsigned int i = 0; i < ALGO1_TCP_UPLOAD_SAMPLE_COUNT; i++) {
        uint32_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        hf_tcp_put_u32_le(frame + ALGO1_TCP_UPLOAD_HEADER_BYTES +
                          i * sizeof(bits), bits);
    }

    frame[ALGO1_TCP_UPLOAD_FRAME_BYTES - 2U] = 0xFF;
    frame[ALGO1_TCP_UPLOAD_FRAME_BYTES - 1U] = 0xFF;

    pthread_mutex_lock(&g_tcp_socket_send_mutex);
    ret = hf_tcp_send_all(fd, frame, sizeof(frame));
    pthread_mutex_unlock(&g_tcp_socket_send_mutex);

    return ret;
}

static void *algo1_tcp_upload_thread(void *arg)
{
    float local_values[ALGO1_TCP_UPLOAD_SAMPLE_COUNT];
    (void)arg;

    for (;;) {
        int fd;
        int slot;
        int channel;
        uint8_t consumer_mask;
        uint32_t sequence;
        uint64_t timestamp_us;

        pthread_mutex_lock(&g_algo1_tcp_upload.mutex);
        while (g_algo1_tcp_upload.ready_slot < 0 &&
               !g_algo1_tcp_upload.shutdown_requested) {
            pthread_cond_wait(&g_algo1_tcp_upload.cond,
                              &g_algo1_tcp_upload.mutex);
        }

        if (g_algo1_tcp_upload.shutdown_requested) {
            pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);
            break;
        }

        slot = g_algo1_tcp_upload.ready_slot;
        sequence = g_algo1_tcp_upload.ready_sequence;
        timestamp_us = g_algo1_tcp_upload.ready_timestamp_us;
        fd = g_algo1_tcp_upload.client_fd;
        consumer_mask = g_algo1_tcp_upload.ready_consumer_mask;
        channel = g_algo1_tcp_upload.ready_channel;
        memcpy(local_values, g_algo1_tcp_upload.buffers[slot],
               sizeof(local_values));
        g_algo1_tcp_upload.ready_slot = -1;
        pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);

        if (algo1_tcp_upload_send_frame(fd, consumer_mask, channel,
                                        sequence, timestamp_us,
                                        local_values) != 0) {
            printf("[Algo1 TCP] 256点帧发送失败，停止上传。\n");
            pthread_mutex_lock(&g_algo1_tcp_upload.mutex);
            g_algo1_tcp_upload.enabled = 0;
            pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);
            shutdown(fd, SHUT_RDWR);
            break;
        }

        printf("[Algo1 TCP] 已发送第%u包，消费者掩码=0x%02X，通道=%d，点数=%u。\n",
               sequence, consumer_mask, channel,
               ALGO1_TCP_UPLOAD_SAMPLE_COUNT);
    }

    return NULL;
}

static int algo1_tcp_upload_attach_client(int client_fd)
{
    int ret;

    pthread_mutex_lock(&g_algo1_tcp_upload.mutex);
    if (g_algo1_tcp_upload.thread_started) {
        pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);
        return -1;
    }

    g_algo1_tcp_upload.client_fd = client_fd;
    g_algo1_tcp_upload.enabled = 0;
    g_algo1_tcp_upload.shutdown_requested = 0;
    g_algo1_tcp_upload.write_slot = 0;
    g_algo1_tcp_upload.ready_slot = -1;
    g_algo1_tcp_upload.next_sequence = 0;
    g_algo1_tcp_upload.thread_started = 1;
    pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);

    ret = pthread_create(&g_algo1_tcp_upload.thread, NULL,
                         algo1_tcp_upload_thread, NULL);
    if (ret != 0) {
        pthread_mutex_lock(&g_algo1_tcp_upload.mutex);
        g_algo1_tcp_upload.thread_started = 0;
        g_algo1_tcp_upload.client_fd = -1;
        pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);
        printf("[Algo1 TCP] 创建独立上传线程失败: %d\n", ret);
        return -1;
    }

    return 0;
}

static void algo1_tcp_upload_detach_client(void)
{
    int should_join;
    int client_fd;

    pthread_mutex_lock(&g_algo1_tcp_upload.mutex);
    should_join = g_algo1_tcp_upload.thread_started;
    client_fd = g_algo1_tcp_upload.client_fd;
    g_algo1_tcp_upload.enabled = 0;
    g_algo1_tcp_upload.shutdown_requested = 1;
    g_algo1_tcp_upload.ready_slot = -1;
    pthread_cond_signal(&g_algo1_tcp_upload.cond);
    pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);

    if (should_join) {
        if (client_fd >= 0) {
            shutdown(client_fd, SHUT_RDWR);
        }
        pthread_join(g_algo1_tcp_upload.thread, NULL);
    }

    pthread_mutex_lock(&g_algo1_tcp_upload.mutex);
    g_algo1_tcp_upload.thread_started = 0;
    g_algo1_tcp_upload.client_fd = -1;
    pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);
}

static void algo1_tcp_upload_start(void)
{
    pthread_mutex_lock(&g_algo1_tcp_upload.mutex);
    if (!g_algo1_tcp_upload.thread_started ||
        g_algo1_tcp_upload.client_fd < 0) {
        pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);
        printf("[Algo1 TCP] 尚无可用的TCP上传线程。\n");
        return;
    }

    if (g_algo1_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);
        printf("[Algo1 TCP] 256点上传已经开启。\n");
        return;
    }

    g_algo1_tcp_upload.write_slot = 0;
    g_algo1_tcp_upload.ready_slot = -1;
    g_algo1_tcp_upload.next_sequence = 0;
    g_algo1_tcp_upload.submitted_packets = 0;
    g_algo1_tcp_upload.dropped_packets = 0;
    g_algo1_tcp_upload.enabled = 1;
    pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);

    printf("[Algo1 TCP] 3011: 算法1 256点float上传已开启，可独立驱动采样器。\n");
}

static void algo1_tcp_upload_stop(void)
{
    unsigned long long submitted_packets;
    unsigned long long dropped_packets;

    pthread_mutex_lock(&g_algo1_tcp_upload.mutex);
    g_algo1_tcp_upload.enabled = 0;
    g_algo1_tcp_upload.ready_slot = -1;
    submitted_packets = g_algo1_tcp_upload.submitted_packets;
    dropped_packets = g_algo1_tcp_upload.dropped_packets;
    pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);

    printf("[Algo1 TCP] 3012: 算法1 256点上传已停止，提交包=%llu，丢包=%llu。\n",
           submitted_packets, dropped_packets);
}

static void algo1_tcp_upload_submit(
    uint8_t consumer_mask, int channel,
    const float values[ALGO1_TCP_UPLOAD_SAMPLE_COUNT])
{
    unsigned int completed_slot;
    uint32_t completed_sequence;

    if (!g_algo1_tcp_upload.enabled) {
        return;
    }

    if (pthread_mutex_trylock(&g_algo1_tcp_upload.mutex) != 0) {
        __sync_fetch_and_add(&g_algo1_tcp_upload.dropped_packets, 1ULL);
        return;
    }

    if (!g_algo1_tcp_upload.enabled) {
        pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);
        return;
    }

    if (g_algo1_tcp_upload.ready_slot >= 0) {
        g_algo1_tcp_upload.dropped_packets++;
        pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);
        return;
    }

    completed_slot = g_algo1_tcp_upload.write_slot;
    completed_sequence = g_algo1_tcp_upload.next_sequence++;
    memcpy(g_algo1_tcp_upload.buffers[completed_slot], values,
           sizeof(g_algo1_tcp_upload.buffers[completed_slot]));

    g_algo1_tcp_upload.ready_slot = (int)completed_slot;
    g_algo1_tcp_upload.ready_sequence = completed_sequence;
    g_algo1_tcp_upload.ready_timestamp_us = hf_tcp_timestamp_us();
    g_algo1_tcp_upload.ready_consumer_mask = consumer_mask;
    g_algo1_tcp_upload.ready_channel = channel;
    g_algo1_tcp_upload.write_slot ^= 1U;
    g_algo1_tcp_upload.submitted_packets++;
    pthread_cond_signal(&g_algo1_tcp_upload.cond);
    pthread_mutex_unlock(&g_algo1_tcp_upload.mutex);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/****************************************************************************************/
FileNameInfo_t g_tFileList[BUFFER_SIZE];
int g_fileCount = 0;

/****************************************************************************************/
typedef union{
	unsigned long long u64Data;
	unsigned int u32Data[2];
	unsigned char u8Data[8];
}U64_n;

U64_n g_clkData;
/****************************************************************************************/
unsigned int g_ledData;
/****************************************************************************************/

void led_out(unsigned data)
{
	write_reg(LED_REG_IN0, data);
}

/****************************************************************************************/
int temp_change_byte1(int ps_temp)
{
	int ps_temp1;		
	ps_temp1 = (int)(((((float)(ps_temp)/65536.0f)/0.00196342531f ) - 280.2309f) * 100);
	
	return ps_temp1;
}
/****************************************************************************************/
unsigned char time_communication_status(unsigned long long currentTime, unsigned long long lastTime, unsigned int limitTime)
{
	unsigned char mBool = 0x01;
	
	if(lastTime == 0x00)
	{
		mBool = 0x00;
	}
	else if((currentTime - lastTime) > limitTime)
	{
		mBool = 0x00;
	}
	
	return mBool;
}
/****************************************************************************************/
void fun_init(void)
{
	g_clkData.u64Data = 0;
	
	g_ledData = 0x00;

	//memset(&g_tDeviceStruct, 0, sizeof(struct g_tDeviceStruct));
	fun_time_init();
	fun_ad7768_init();
	fun_ad7915_init();
}

/////////////////////////////////////////////////////////////////////////////////////////////超声部分函数





void cis_start_thread() 
{
    if (g_utThreadId != 0) 
	{
        printf("[TCP] 线程已在后台，无需重复启动。\n");
        return;
    }
    keep_acquiring = true;
    start_config_trigger = false; 
    pthread_create(&g_utThreadId, NULL, Thread_Uti_ctrl, (void*)&g_utParams);
}

// 触发函数,对应302
void cis_trigger_config() 
{
    // 这里的逻辑可以为空，或者打印一行日志
    printf("[TCP] 触发配置流程，等待下一步数据包...\n");
}

// 确保 HardwareConfig 结构体里的变量名和这里对应
int cis_parse_config_packet(unsigned char* buf, int len) 
{
    if (len < 13) return -1;
    if (buf[0] != 0xAA || buf[1] != 0x55 || buf[len-1] != 0xFF) 
    {
        printf("[Error] 配置包校验失败！\n");
        return -1;
    }

    int ch = (int)buf[2]; // 第 3 位是通道号
    if (ch < 0 || ch >= MAX_CH) { 
        printf("[Error] 非法通道号: %d\n", ch);
        return -1;
    }

    // --- 解析到对应通道的配置中 ---
    g_hwConfigs[ch].delay         = (float)buf[3];
    g_hwConfigs[ch].range         = (int)buf[4];
    g_hwConfigs[ch].filter        = (int)buf[5];
    g_hwConfigs[ch].detect_method = (int)buf[6];
    g_hwConfigs[ch].pulse_width   = (int)buf[7];
    g_hwConfigs[ch].data_len      = (int)buf[8]*2;
    g_hwConfigs[ch].tgc           = (int)buf[9]; 
    g_hwConfigs[ch].gain          = (int)buf[10]; 
    g_hwConfigs[ch].trigger_mode  = (int)buf[11]; 

    // 仅仅标记该通道已配置好参数
    ch_active_mask[ch] = true; 
    
    // 【重要修改】：删除 start_config_trigger = true; 这行代码！
    // 不在这里触发硬件启动，等待 203 指令统一触发

    printf("[TCP] 通道 %d 配置参数已缓存 (等待203指令下发)\n", ch);

    return 0;
}

// --- 对应 206
// 在 fun.c 中修改这个函数
void cis_stop_everything() 
{
    start_config_trigger = false;  // 停止采集逻辑
    start_storage_flag = false;    // 停止存储逻辑
    
    // --- 放在这里，替换掉 memset ---
    for (int i = 0; i < MAX_CH; i++) {
        ch_active_mask[i] = false;
    }
    
    printf("[TCP] 206 指令：已停止采集并清除通道激活状态，回到待命模式。\n");
}

////////////
void* Thread_Uti_ctrl(void* arg)
{
    AcqParams* params = (AcqParams*)arg;

    FILE *fps[MAX_CH] = {NULL};      
    int fileIndexes[MAX_CH] = {0};   
    int packCounts[MAX_CH] = {0};    
    char fileName[128];              

    printf("[Acq Thread] 线程启动成功，进入待命状态...\n");

	// ================= 新增：必须先建立设备连接 =================
    while (keep_acquiring) {
        if (0 == UTI_OpenDevice(params->ip_address)) {
            printf("[Acq Thread] 硬件连接成功 (IP: %s)，进入初始待命态。\n", params->ip_address);
            break;
        }
        printf("[Acq Thread] 正在尝试连接硬件...\n");
        sleep(2);
    }


    while (keep_acquiring) {
        // A. 待命：等待 203 指令解析成功触发 start_config_trigger
        while (keep_acquiring && !start_config_trigger) {
            usleep(100000);
        }


        if (!keep_acquiring) break;

        // B. 硬件配置环节 (这一部分建议给硬件一点稳定时间)
        int first_ch = -1;
        for(int i=0; i<MAX_CH; i++) { 
            if(ch_active_mask[i]) { first_ch = i; break; } 
        }
        
if (first_ch != -1) 
        {
            printf("[Acq Thread] 检测到配置，正在下发硬件参数...\n");
            // (1) 基础设置
            UTI_SetTriggerMode(0);
            UTI_SetRPF(100);

            // === 核心修复 1：加上 static，防止指针被回收引发 Bad address ===
            static char seq_str[256] = {0}; 
            memset(seq_str, 0, sizeof(seq_str)); // 每次配置前先清空旧字符串
            
            for (int i = 0; i < MAX_CH; i++) {
                if (ch_active_mask[i]) {
                    char temp[32];
                    sprintf(temp, "%d,%d,50;", i, i); 
                    strcat(seq_str, temp);
                }
            }
            UTI_SetSequ(seq_str);
            printf("[Acq Thread] 动态下发采集序列: %s\n", seq_str);
            // ==============================================================

            // (3) 设置所有激活通道的：延迟
            for (int i = 0; i < MAX_CH; i++) {
                if (ch_active_mask[i]) UTI_SetChDelay(i, g_hwConfigs[i].delay);
            }
            // (4) 设置所有激活通道的：采样时间
            for (int i = 0; i < MAX_CH; i++) {
                if (ch_active_mask[i]) UTI_SetChSampleTime(i, g_hwConfigs[i].range, 5.92);
            }
            // (5) 设置 DAC
            for (int i = 0; i < MAX_CH; i++) {
                if (ch_active_mask[i]) UTI_SetChDACCurve(i, "0,0.0;");
            }
            // (6) 设置增益
            for (int i = 0; i < MAX_CH; i++) {
                if (ch_active_mask[i]) UTI_SetChGain(i, g_hwConfigs[i].gain);
            }
            // (7) 设置 PGC
            for (int i = 0; i < MAX_CH; i++) {
                if (ch_active_mask[i]) UTI_SetChPGC(i, g_hwConfigs[i].tgc);
            }
            // (8) 设置滤波器
            for (int i = 0; i < MAX_CH; i++) {
                if (ch_active_mask[i]) UTI_SetChFilter(i, g_hwConfigs[i].filter);
            }
            // (9) 设置检波方式
            for (int i = 0; i < MAX_CH; i++) {
                if (ch_active_mask[i]) UTI_SetChDetectionType(i, g_hwConfigs[i].detect_method);
            }
            // (10) 设置脉冲宽度
            for (int i = 0; i < MAX_CH; i++) {
                if (ch_active_mask[i]) UTI_SetChPulseWidth(i, g_hwConfigs[i].pulse_width);
            }

            // === 核心修复 2：不再写死 256，使用你上位机下发的实际长度 ===
            // 取第一个激活通道的 data_len 作为全局设定 (必须为 4 的整数倍)
            int global_data_size = g_hwConfigs[first_ch].data_len; 
            UTI_SetDataSize(global_data_size); 
            printf("[Acq Thread] 设置全局数据长度 DataSize: %d\n", global_data_size);

            usleep(50000); // 等待硬件稳定
        }
            



		UTI_StartWork();
		// --- 加这行打印 ---
        printf("[DEBUG] 硬件配置尝试完毕，准备进入数据读取循环...\n");
		

		// C. 采集与保存循环
        printf("[Acq Thread] 已经进入 While 循环，正在尝试读取第一包数据...\n");
        
        int empty_count = 0; // 用来记录空包次数，防止刷屏

        while (keep_acquiring && start_config_trigger) 
		{
            
            //调试打印：确认读取函数是否卡死
            //printf("Check: Calling UTI_ReadData...\n"); 
            
            UTData* data = UTI_ReadData();
            
            if (data != NULL)
            {
                int ch = data->channelNo;
                
                // 只要拿到数据，就打印一次，确认硬件在跑
                static int receive_count = 0;
                if (receive_count++ % 1000 == 0) {
                    printf("[DEBUG] 硬件活跃中：已收到 %d 包数据 (当前通道: %d)\n", receive_count, ch);
                }

				// ==========================================================
				// ==========================================================
                // 【更新最新数据到全局数组】
                if (ch >= 0 && ch < MAX_CH) 
                {
                    int actual_data_size = g_hwConfigs[ch].data_len;
                    if (actual_data_size > MAX_POINTS) {
                        actual_data_size = MAX_POINTS; 
                    }

                    // 【上锁】：同时保存原始数据和转换后的算法输入数据
                    pthread_mutex_lock(&data_mutex);
                    for (int i = 0; i < actual_data_size; i++) {
                        int raw = (int)data->WaveData[i];
                        new_data[ch][i] = raw;
                        algo_data[ch][i] = algo_convert_raw_to_value(ch, raw);
                    }
                    pthread_mutex_unlock(&data_mutex);


					// pthread_mutex_lock(&data_mutex);
					// // 复制长度 = min(actual_data_size, 256)
					// int copy_len = (actual_data_size < 256) ? actual_data_size : 256;
					// for (int i = 0; i < copy_len; i++) {
					// 	new_data[ch][i] = FIXED_WAVEFORM[i];   // 使用固定数组
					// }
					// // 如果 actual_data_size > 256，多出的部分不处理；如果小于 256，则只填充前 actual_data_size 个
					// pthread_mutex_unlock(&data_mutex);


                    // ==========================================================
                    // 每 10000 包抽样打印一次数组内容，验证是否存入
                    // ==========================================================
                    // 定义一个静态数组，独立记录每个通道更新进内存的次数
                    static int array_update_cnt[MAX_CH] = {0};
                    array_update_cnt[ch]++;
                    
                    if (array_update_cnt[ch] % 3000 == 0)
                    {
                        printf("[Verify] 通道 %d 内存数组已刷新 %d 次。抽样前10个数: ", ch, array_update_cnt[ch]);

                        // 只打印前 10 个数
                        int print_len = (actual_data_size < 10) ? actual_data_size : 10;
                        for (int k = 0; k < print_len; k++) {
                            printf("%d ", new_data[ch][k]);
                        }
                        printf("...\n");

                        /* 自动连续检测：遍历 9 个算法，只有处于 running 状态且通道匹配才触发 */
                        for (int ai = 0; ai < ALGO_COUNT; ai++) {
                            if (g_algos[ai].running && ch == g_algos[ai].channel) {
                                printf("[Algo] 自动触发 %s 检测\n", g_algos[ai].name);
                                write_algo_data_for_algo(&g_algos[ai]);
                            }
                        }
                    }
                    // ==========================================================
                }

				// 【核心检查点】：只要拿到数据，并且允许存盘，且通道已激活
                if (start_storage_flag && ch >= 0 && ch < MAX_CH && ch_active_mask[ch])
                {
                    // 1. 文件尺寸检查与分卷机制 (每 500 包检查一次大小)
                    if (fps[ch] != NULL && packCounts[ch] > 0 && (packCounts[ch] % 500 == 0)) 
                    {
                        fseek(fps[ch], 0, SEEK_END);
                        if (ftell(fps[ch]) >= MAX_FILE_SIZE) {
                            fclose(fps[ch]);
                            fps[ch] = NULL;      // 置空，触发下方重新创建文件
                            fileIndexes[ch]++;   // 文件分卷序号 +1
                        }
                    }

                    // 2. 首次进入或当前通道的文件还未打开时，按特定命名规则创建文件
                    if (fps[ch] == NULL) 
                    {
                        time_t t = time(NULL);
                        struct tm *tm_info = localtime(&t);
                        char fileTimeStr[32];
                        
                        // 格式化时间为 年月日_时分秒，例如：20231024_153000
                        strftime(fileTimeStr, sizeof(fileTimeStr), "%Y%m%d_%H%M%S", tm_info);
                        
                        // 拼装文件路径：结合时间戳和分卷序号，例如 S0_20231024_153000_part0.txt
                        sprintf(fileName, "/mnt/file/S%d_%s_part%d.txt", ch, fileTimeStr, fileIndexes[ch]);
                        
                        fps[ch] = fopen(fileName, "w"); // 改为 "w" 文本写入模式
                        
                        if (fps[ch] == NULL) {
                            static int err_cnt = 0;
                            if (err_cnt++ % 1000 == 0) {
                                printf("[Error] 创建文件失败: %s (请确认板子上存在 /mnt/file 目录！)\n", fileName);
                            }
                        } else {
                            printf("[Acq] 成功创建通道 %d 数据文件: %s, 开始落盘！\n", ch, fileName);
                        }
                    }

                    // 3. 如果文件打开成功，且波形数据不为空，执行格式化写入
                    if (fps[ch] != NULL && data->WaveData != NULL) 
                    {
                        // ---- 记录行首的时间戳 ----
                        time_t now = time(NULL);
                        // 使用静态数组保存每个通道的上一秒时间，避免同一秒内重复调用 strftime 消耗性能
                        static time_t lastTime[MAX_CH] = {0};
                        static char timeStr[MAX_CH][64] = {0};
                        
                        if (now != lastTime[ch]) {
                            struct tm *timeInfo = localtime(&now);
                            strftime(timeStr[ch], sizeof(timeStr[ch]), "%Y/%m/%d %H:%M:%S: ", timeInfo);
                            lastTime[ch] = now;
                        }

                        // 先写入时间戳前缀
                        fprintf(fps[ch], "%s", timeStr[ch]); 
                        
                        // ---- 写入波形数据 ----
                        //int data_size = 256; // 对应你之前的 UTI_SetDataSize(256)
						// 将原来的 int data_size = 256; 替换为动态获取：
                        int actual_data_size = g_hwConfigs[ch].data_len; 
                        for (int i = 0; i < actual_data_size; i++) {
                            fprintf(fps[ch], "%d ", (int)data->WaveData[i]); 
                        }
                        fprintf(fps[ch], "\n");

                        
                        packCounts[ch]++;
                        
                        // ---- 定期打印进度并刷盘 ----
                        // 每存 2000 包打印一次进度，并强制 fflush 写入物理磁盘防止断电丢失
                        if (packCounts[ch] % 2000 == 0) {
                            printf("[Acq] 通道 %d 已持续写入 %d 包数据...\n", ch, packCounts[ch]);
                            fflush(fps[ch]); 
                        }
                    }
                }
                else 
                {
                    // 如果拿到数据但不存盘，打印原因（每5000次打印一次，防止刷屏）
                    static int skip_count = 0;
                    if (skip_count++ % 5000 == 0) {
                        printf("[SKIP] 数据被过滤: flag=%d, ch=%d, mask=%d\n", 
                               start_storage_flag, ch, (ch>=0 && ch<MAX_CH)?ch_active_mask[ch]:-1);
                    }
                }

                if (data->WaveData) free(data->WaveData);
                free(data);
            } 
            else {
                // 如果 data 是 NULL
                empty_count++;
                if (empty_count % 10000 == 0) {
                    //printf("[DEBUG] UTI_ReadData 返回 NULL (硬件可能未产生数据)\n");
                }
                usleep(100);
            }
        }

        // D. 停止清理：当执行 206 指令时会运行到这里
        UTI_StopWork();
        for(int i=0; i<MAX_CH; i++)
        {
            if(fps[i]) { 
                fclose(fps[i]); 
                fps[i] = NULL; 
            }
            packCounts[i] = 0; // 重置计数器
        }
        printf("[Acq Thread] 采集循环已退出，重置状态，等待下一次配置。\n");
    }

    UTI_CloseDevice();
    return NULL;
}






// qsort 排序回调函数：按时间降序排列 (最新的排在前面)
int compare_file_time(const void *a, const void *b) {
    FileInfo_t *f1 = (FileInfo_t *)a;
    FileInfo_t *f2 = (FileInfo_t *)b;
    if (f1->mtime < f2->mtime) return 1;
    if (f1->mtime > f2->mtime) return -1;
    return 0;
}






void uti_search_files(void)
{
    unsigned char response[6 + 20 * 64] = {0}; // 6字节头 + 最多20个文件(每个固定占64字节)
    
    // 1. 构造响应头前 4 个字节 (0x55, 0x00, 原始指令头)
    response[0] = 0x55;
    response[1] = 0x00;
    response[2] = g_tcpClientRcvBuffer[0];
    response[3] = g_tcpClientRcvBuffer[1];

    DIR *dir;
    struct dirent *ent;
    FileInfo_t files[1000]; // 缓冲区：最多扫 1000 个文件防止内存溢出
    int total_files = 0;

    // 2. 打开文件夹并扫描
    if ((dir = opendir("/mnt/file")) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            // 只过滤出以 S 开头且以 .txt 结尾的数据文件
            if (ent->d_name[0] == 'S' && strstr(ent->d_name, ".txt") != NULL) {
                char fullPath[512];
                snprintf(fullPath, sizeof(fullPath), "/mnt/file/%s", ent->d_name);
                
                // 获取文件的属性（修改时间）
                struct stat st;
                if (stat(fullPath, &st) == 0) {
                    strncpy(files[total_files].filename, ent->d_name, 63);
                    files[total_files].filename[63] = '\0';
                    files[total_files].mtime = st.st_mtime;
                    total_files++;
                    
                    if (total_files >= 1000) break; // 达到扫描上限则停止
                }
            }
        }
        closedir(dir);
    } else {
        // 如果目录打开失败，发送错误响应
        perror("[Search Files] 无法打开 /mnt/file 目录");
        response[4] = 0xFF; // 失败标志
        response[5] = 0x00;
        send(g_clientSocket, response, 6, 0);
        return;
    }

    // 3. 对收集到的文件按时间进行降序排序
    qsort(files, total_files, sizeof(FileInfo_t), compare_file_time);

    // 4. 提取前 20 个文件 (如果总数不够20个，则取实际数量)
    int send_count = (total_files > 20) ? 20 : total_files;

    // 5. 构造成功的响应报文
    response[4] = 0x00;           // 成功标志
    response[5] = send_count;     // 实际发送的文件数量 (0~20)

    // 将文件名填入报文的负载区域，每个文件名固定占用 64 字节空间
    for (int i = 0; i < send_count; i++) {
        int offset = 6 + (i * 64);
        // 使用 strncpy 将字符串安全地放进去，其余未使用的空间由于数组初始化为 0，天然带有 \0 结尾
        strncpy((char*)&response[offset], files[i].filename, 63);
        
        printf("[Search Files] 准备下发列表项 %d: %s\n", i + 1, files[i].filename);
    }

    // 6. 发送数据
    int total_length = 6 + (send_count * 64);
    send(g_clientSocket, response, total_length, 0);

    printf("[Search Files] 文件搜索完成，共下发 %d 个最新文件。\n", send_count);
}




void uti_transfer_selected_file(void)
{
    char targetFileName[128] = {0};
    char fullFilePath[256] = {0};
    unsigned char response[BUFFER_SIZE];

    // 1. 构造响应头前 4 个字节 (0x55, 0x00, 原始指令头)
    response[0] = 0x55;
    response[1] = 0x00;
    response[2] = g_tcpClientRcvBuffer[0];
    response[3] = g_tcpClientRcvBuffer[1];

    // 2. 解析上位机请求的文件名
    int nameLen = (unsigned char)g_tcpClientRcvBuffer[2];
    
    // 安全性校验：防止长度不合法或导致缓冲区溢出
    if (nameLen <= 0 || nameLen >= sizeof(targetFileName)) {
        printf("[Error] 无效的文件名长度: %d\n", nameLen);
        
        // 【协议对齐】：发送失败响应，固定 6 字节 (末尾 0xFF 0x00 表示失败)
        response[4] = 0xFF;
        response[5] = 0x00;
        send(g_clientSocket, response, 6, 0);
        return;
    }

    // 从接收缓冲区安全拷贝文件名 (从第 4 个字节开始)
    memcpy(targetFileName, g_tcpClientRcvBuffer + 3, nameLen);
    targetFileName[nameLen] = '\0'; // 确保字符串以 \0 结尾

    // 3. 拼接完整路径
    snprintf(fullFilePath, sizeof(fullFilePath), "/mnt/file/%s", targetFileName);
    printf("[File Transfer] 收到上位机请求，准备发送文件: %s\n", fullFilePath);

    // 4. 尝试打开文件
    FILE *file = fopen(fullFilePath, "rb"); 
    if (file == NULL) {
        perror("[File Transfer] 文件打开失败 (可能文件不存在)");
        
        // 【协议对齐】：发送失败响应，固定 6 字节 (末尾 0xFF 0x00 表示失败)
        response[4] = 0xFF;
        response[5] = 0x00;
        send(g_clientSocket, response, 6, 0);
        return;
    }

    // ==========================================
    // 【修改点 1】：统一成功响应为 6 个字节
    // ==========================================
    response[4] = 0x00; // 0x00 0x00 表示成功找到了文件
    response[5] = 0x00;
    send(g_clientSocket, response, 6, 0);

    // 5. 分块读取并发送文件数据
    size_t bytesRead;
    char buffer[BUFFER_SIZE];
    
    while ((bytesRead = fread(buffer, 1, BUFFER_SIZE, file)) > 0) 
    {
        // 转换为 32 位无符号整数，并转为网络字节序(大端)
        uint32_t netLength = htonl((uint32_t)bytesRead); 
        
        // 发送数据包长度 (固定 4 个字节)
        send(g_clientSocket, (char*)&netLength, sizeof(netLength), 0); 
        
        // 发送实际波形/文本数据
        send(g_clientSocket, buffer, bytesRead, 0);
    }

    // ==========================================
    // 【修改点 2】：统一结束标志为 4 个字节
    // ==========================================
    // 发送 0xFFFFFFFF 代表文件传输结束。上位机读到这个长度就会停止接收。
    // (0xFFFFFFFF 不管是大端还是小端，在内存里的排列都是全 1，所以这里可以直接发)
    uint32_t eofMarker = 0xFFFFFFFF; 
    send(g_clientSocket, (char*)&eofMarker, sizeof(eofMarker), 0);

    // 7. 清理资源
    fclose(file);
    printf("[File Transfer] 文件 %s 发送完成！\n", targetFileName);
}




void uti_delete_files(void)
{
    char targetFileName[128] = {0};
    char fullFilePath[256] = {0};
    unsigned char response[6]; // 固定 6 字节响应

    // 1. 构造响应头前 4 个字节 (0x55, 0x00, 原始指令头)
    response[0] = 0x55;
    response[1] = 0x00;
    response[2] = g_tcpClientRcvBuffer[0];
    response[3] = g_tcpClientRcvBuffer[1];

    // 2. 解析上位机请求的文件名
    // 提取第3个字节作为文件名长度
    int nameLen = (unsigned char)g_tcpClientRcvBuffer[2];
    
    // 安全性校验：防止长度不合法
    if (nameLen <= 0 || nameLen >= sizeof(targetFileName)) {
        printf("[Error] 删除请求中无效的文件名长度: %d\n", nameLen);
        
        // 发送失败响应 (0xFF 0x00 表示请求格式错误)
        response[4] = 0xFF;
        response[5] = 0x00;
        send(g_clientSocket, response, 6, 0);
        return;
    }

    // 从接收缓冲区安全拷贝文件名 (从第 4 个字节开始)
    memcpy(targetFileName, g_tcpClientRcvBuffer + 3, nameLen);
    targetFileName[nameLen] = '\0'; // 确保字符串以 \0 结尾

    // 3. 拼接完整路径 (固定为 /mnt/file 目录)
    snprintf(fullFilePath, sizeof(fullFilePath), "/mnt/file/%s", targetFileName);
    printf("[File Delete] 收到上位机请求，准备删除文件: %s\n", fullFilePath);

    // 4. 直接执行删除操作
    // C 标准库 remove 函数：如果成功返回 0，失败返回非 0
    if (remove(fullFilePath) == 0) {
        printf("[File Delete] 文件 %s 删除成功！\n", targetFileName);
        
        // 删除成功，发送 0x00 0x00
        response[4] = 0x00;
        response[5] = 0x00;
    } else {
        perror("[File Delete] 文件删除失败 (可能文件不存在或权限不足)");
        
        // 删除失败，发送 0xFF 0x01 区分错误类型
        response[4] = 0xFF;
        response[5] = 0x01; 
    }

    // 5. 发送 6 字节的响应结果给上位机
    send(g_clientSocket, response, 6, 0);
}



void* thread_tcp_server_ps(void *args)
{
	pthread_t th;
	
	struct sockaddr_in clientAddr;
	int clilen = sizeof(clientAddr);
    int ret = -1; 
	IPData_t tIpData;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PS_PORT);   // 大端端口
    // INADDR_ANY代表本机的所有IP, 假设有三个网卡就有三个IP地址
    // 这个宏可以代表任意一个IP地址
    // 这个宏一般用于本地的绑定操作
    addr.sin_addr.s_addr = inet_addr(PS_IP_ADDRESS);  
    
    do
	{
        ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &ret, sizeof(int));
    }while(ret == -1);
	
    while(bind(fd, (struct sockaddr*)&addr, sizeof(addr))== -1)
	{
 
    };
    // 3. 设置监听
	while(listen(fd, 128)==-1)
	{

	};
    //4. 阻塞等待并接受客户端连接
    while(1)
	{
        int fd1 = accept(fd, (struct sockaddr*)&clientAddr, &clilen);
        // 打印客户端的地址信息
        char ip[24] = {0};
#ifdef TCP_DBG_PRINT
        printf("TCP协议连接成功 客户端的IP地址: %s, 端口: %d\n",inet_ntop(AF_INET, &clientAddr.sin_addr.s_addr, ip, sizeof(ip)),ntohs(clientAddr.sin_port));
#endif		
    // 5. 和客户端通信
		//if(strcmp(inet_ntop(AF_INET, &clientAddr.sin_addr.s_addr, ip, sizeof(ip)), "192.168.1.31") == 0)
		if(1)
		{
			tIpData.fd = fd1;
			tIpData.clientAddr = clientAddr;
			pthread_create(&th, NULL, thread_deal_client_ps, (void*)(&tIpData));
		}
		else
		{
			close(fd1);
		}
		usleep(100);
    }
	
}

// void* thread_tcp_server_pl(void *args)
// {
// 	pthread_t th;
	
// 	struct sockaddr_in clientAddr;
// 	int clilen = sizeof(clientAddr);
//     int ret = -1; 
// 	IPData_t tIpData;
//     int fd = socket(AF_INET, SOCK_STREAM, 0);
//     struct sockaddr_in addr;
//     addr.sin_family = AF_INET;
//     addr.sin_port = htons(PL_PORT);   // 大端端口
//     // INADDR_ANY代表本机的所有IP, 假设有三个网卡就有三个IP地址
//     // 这个宏可以代表任意一个IP地址
//     // 这个宏一般用于本地的绑定操作
//     addr.sin_addr.s_addr = inet_addr(PL_IP_ADDRESS);  
    
//     do
// 	{
//         ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &ret, sizeof(int));
//     }while(ret == -1);
	
//     while(bind(fd, (struct sockaddr*)&addr, sizeof(addr))== -1)
// 	{
 
//     };
//     // 3. 设置监听
// 	while(listen(fd, 128)==-1)
// 	{

// 	};
//     //4. 阻塞等待并接受客户端连接
//     while(1)
// 	{
//         int fd1 = accept(fd, (struct sockaddr*)&clientAddr, &clilen);
//         // 打印客户端的地址信息
//         char ip[24] = {0};
// #ifdef TCP_DBG_PRINT
//         printf("TCP协议连接成功 客户端的IP地址: %s, 端口: %d\n",inet_ntop(AF_INET, &clientAddr.sin_addr.s_addr, ip, sizeof(ip)),ntohs(clientAddr.sin_port));
// #endif		
//     // 5. 和客户端通信
// 		//if(strcmp(inet_ntop(AF_INET, &clientAddr.sin_addr.s_addr, ip, sizeof(ip)), "192.168.1.31") == 0)
// 		if(1)
// 		{
// 			tIpData.fd = fd1;
// 			tIpData.clientAddr = clientAddr;
// 			pthread_create(&th, NULL, thread_deal_client_pl, (void*)(&tIpData));
// 		}
// 		else
// 		{
// 			close(fd1);
// 		}
// 		usleep(100);
//     }
	
// }


// =========================================================================
// 【新增】：专门用于向 TCP 客户端（上位机）每秒推送一次实时数据的线程//UTI
// =========================================================================
void* thread_tcp_upload_ps(void *args)
{
    IPData_t tIpData = *(IPData_t*)args;
    int client_fd = tIpData.fd;
    
    printf("[TCP Upload] 实时数据推送线程已启动，等待 303 指令触发...\n");

    while(1)
    {
        // 严格休眠 1 秒，达到 1Hz 的发送频率
        sleep(1); 
        
		// 【修改】：只有当允许网络推送时，才发送数据
        if (start_upload_flag)
        {
            for (int ch = 0; ch < MAX_CH; ch++) 
            {
                // 只发送被激活配置的通道数据
                if (ch_active_mask[ch]) 
                {
                    int actual_data_size = g_hwConfigs[ch].data_len;
                    if (actual_data_size <= 0 || actual_data_size > MAX_POINTS) continue;

                    // 1. 光速上锁，把数据拷贝到线程的局部缓存中，然后立即解锁，不卡采集线程
                    int local_buffer[MAX_POINTS];
                    pthread_mutex_lock(&data_mutex);
                    memcpy(local_buffer, new_data[ch], actual_data_size * sizeof(int));
                    pthread_mutex_unlock(&data_mutex);

					// === 在这里补上这行定义！ ===
					int byte_len = actual_data_size * sizeof(int);

                    // 2. 组装【帧头】和辅助信息 (AA 55 + 通道号 + 数据长度)
                    // 强烈建议加上通道号和长度，否则上位机解包时容易错位粘包
                    unsigned char header[5];
                    header[0] = 0xAA;
                    header[1] = 0x55;
                    header[2] = (unsigned char)ch;                               // 几号通道
					header[3] = (unsigned char)(byte_len & 0xFF);        // 【修复】：发送字节长度低8位
					header[4] = (unsigned char)((byte_len >> 8) & 0xFF); // 【修复】：发送字节长度高8位

					pthread_mutex_lock(&g_tcp_socket_send_mutex);
					int ret1 = send(client_fd, header, 5, MSG_NOSIGNAL);
					int ret2 = send(client_fd, local_buffer, byte_len, MSG_NOSIGNAL); // 使用 byte_len

                    // 4. 组装并发送【帧尾】(FF FF)
                    unsigned char footer[2] = {0xFF, 0xFF};
					int ret3 = send(client_fd, footer, 2, MSG_NOSIGNAL);
					pthread_mutex_unlock(&g_tcp_socket_send_mutex);
                    
                    // 检查网络连接状态，如果发送失败说明断开了
                    if (ret1 <= 0 || ret2 <= 0 || ret3 <= 0) {
                        printf("[TCP Upload] 上位机连接断开，推送线程安全退出。\n");
                        return NULL; 
                    }
                }
            }
        }
    }
    return NULL;
}



/* ===================== Python 日志转串口 =====================
 * Python watch 进程仍然后台运行，stdout/stderr 继续写入 log_file。
 * C 端为每个算法启动一个轻量监听线程，把 log_file 的新增内容实时打印到串口，
 * 这样既不会阻塞主业务线程，也能在串口看到 watch 检测结果。
 */
static void algo_clear_log(const AlgoConfig *algo)
{
    FILE *fp;

    if (algo == NULL || algo->log_file == NULL) {
        return;
    }

    fp = fopen(algo->log_file, "w");
    if (fp != NULL) {
        fclose(fp);
    }
}

static void *algo_serial_log_thread(void *arg)
{
    AlgoConfig *algo = (AlgoConfig *)arg;
    char line[1024];

    if (algo == NULL || algo->log_file == NULL) {
        return NULL;
    }

    while (algo->monitor_running) {
        FILE *fp = fopen(algo->log_file, "r");
        if (fp != NULL) {
            long end_pos;

            fseek(fp, 0, SEEK_END);
            end_pos = ftell(fp);

            /* log 被清空或重新创建时，偏移回到 0。 */
            if (algo->log_offset < 0) {
                algo->log_offset = end_pos;
            } else if (algo->log_offset > end_pos) {
                algo->log_offset = 0;
            }

            fseek(fp, algo->log_offset, SEEK_SET);
            while (fgets(line, sizeof(line), fp) != NULL) {
                printf("[SERIAL][%s] %s", algo->name, line);
                if (strchr(line, '\n') == NULL) {
                    printf("\n");
                }
                fflush(stdout);
            }
            algo->log_offset = ftell(fp);
            fclose(fp);
        }

        usleep(200000);  /* 200ms 扫描一次日志新增内容 */
    }

    return NULL;
}

static void algo_start_serial_monitor(AlgoConfig *algo, int from_start)
{
    int ret;

    if (algo == NULL || algo->log_file == NULL) {
        return;
    }

    if (algo->monitor_running) {
        return;
    }

    algo->log_offset = from_start ? 0 : -1;
    algo->monitor_running = 1;

    ret = pthread_create(&algo->monitor_thread, NULL, algo_serial_log_thread, (void *)algo);
    if (ret != 0) {
        algo->monitor_running = 0;
        fprintf(stderr, "%s: 创建串口日志监听线程失败\n", algo->name);
        return;
    }

    pthread_detach(algo->monitor_thread);
    printf("%s: 串口日志监听已启动，log=%s\n", algo->name, algo->log_file);
}

static void algo_stop_serial_monitor(AlgoConfig *algo)
{
    if (algo == NULL) {
        return;
    }

    if (algo->monitor_running) {
        algo->monitor_running = 0;
        printf("%s: 串口日志监听已停止\n", algo->name);
    }
}

static int algo_remove_old_result_file(const char *path)
{
    if (path == NULL) {
        return 0;
    }

    if (remove(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "无法删除旧文件 %s: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

static int algo_reset_result_run(AlgoConfig *algo)
{
    int ret = 0;
    struct timespec now = {0, 0};
    unsigned long long new_run_id;

    if (algo == NULL || algo->trigger_file == NULL ||
        algo->result_file == NULL || algo->result_stream_file == NULL) {
        return -1;
    }

    ret |= algo_remove_old_result_file(algo->trigger_file);
    ret |= algo_remove_old_result_file(algo->result_file);
    ret |= algo_remove_old_result_file(algo->result_stream_file);
    if (ret != 0) {
        return -1;
    }

    clock_gettime(CLOCK_REALTIME, &now);
    new_run_id = (unsigned long long)now.tv_sec * 1000000000ULL +
                 (unsigned long long)now.tv_nsec;

    pthread_mutex_lock(&g_algo_result_mutex);
    if (new_run_id == 0ULL || new_run_id <= algo->run_id) {
        new_run_id = algo->run_id + 1ULL;
    }
    algo->run_id = new_run_id;
    algo->trigger_seq = 0ULL;
    algo->result_stream_offset = 0;
    algo->vote_received = 0U;
    memset(algo->vote_counts, 0, sizeof(algo->vote_counts));
    memset(algo->vote_last_seen, 0, sizeof(algo->vote_last_seen));
    algo->result_completed = 0;
    pthread_mutex_unlock(&g_algo_result_mutex);

    return 0;
}

static int algo_send_result_frame(const AlgoConfig *algo,
                                  unsigned int result_code)
{
    unsigned char frame[4];
    int ret;

    if (algo == NULL || result_code >= algo->class_count) {
        return -1;
    }

    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[2] = algo->result_frame_type;
    frame[3] = (unsigned char)result_code;

    pthread_mutex_lock(&g_tcp_socket_send_mutex);
    ret = hf_tcp_send_all(g_clientSocket, frame, sizeof(frame));
    pthread_mutex_unlock(&g_tcp_socket_send_mutex);

    if (ret == 0) {
        printf("[Algo Result] %s 返回 AA 55 %02X %02X\n",
               algo->name, frame[2], frame[3]);
    } else {
        fprintf(stderr, "[Algo Result] %s 结果帧发送失败\n", algo->name);
    }

    return ret;
}

static int algo_add_vote(AlgoConfig *algo, unsigned long generation,
                         unsigned long long run_id, int algo_id,
                         const char *algo_type, int result_code,
                         unsigned int *final_code)
{
    unsigned int selected = 0U;
    unsigned int max_count = 0U;
    int completed = 0;

    if (algo == NULL || algo_type == NULL || final_code == NULL) {
        return 0;
    }

    pthread_mutex_lock(&g_algo_result_mutex);

    if (!algo->result_monitor_running ||
        algo->result_monitor_generation != generation ||
        algo->result_completed || run_id != algo->run_id ||
        algo_id != algo->id || strcmp(algo_type, algo->algo_type) != 0 ||
        result_code < 0 || (unsigned int)result_code >= algo->class_count ||
        algo->class_count == 0U ||
        algo->class_count > ALGO_RESULT_MAX_CLASSES ||
        algo->vote_target == 0U) {
        pthread_mutex_unlock(&g_algo_result_mutex);
        return 0;
    }

    algo->vote_received++;
    algo->vote_counts[result_code]++;
    algo->vote_last_seen[result_code] = algo->vote_received;

    printf("[Algo Result] %s 收到第 %u/%u 次结果，code=%d\n",
           algo->name, algo->vote_received, algo->vote_target, result_code);

    if (algo->vote_received >= algo->vote_target) {
        for (unsigned int i = 0; i < algo->class_count; i++) {
            if (algo->vote_counts[i] > max_count ||
                (algo->vote_counts[i] == max_count &&
                 algo->vote_last_seen[i] > algo->vote_last_seen[selected])) {
                selected = i;
                max_count = algo->vote_counts[i];
            }
        }

        algo->result_completed = 1;
        *final_code = selected;
        completed = 1;
    }

    pthread_mutex_unlock(&g_algo_result_mutex);
    return completed;
}

static void *algo_result_monitor_thread(void *arg)
{
    AlgoResultMonitorArgs *thread_args = (AlgoResultMonitorArgs *)arg;
    AlgoConfig *algo;
    unsigned long generation;
    char line[256];

    if (thread_args == NULL) {
        return NULL;
    }

    algo = thread_args->algo;
    generation = thread_args->generation;
    free(thread_args);

    if (algo == NULL || algo->result_stream_file == NULL) {
        return NULL;
    }

    for (;;) {
        long offset;
        FILE *fp;

        pthread_mutex_lock(&g_algo_result_mutex);
        if (!algo->result_monitor_running ||
            algo->result_monitor_generation != generation) {
            pthread_mutex_unlock(&g_algo_result_mutex);
            break;
        }
        offset = algo->result_stream_offset;
        pthread_mutex_unlock(&g_algo_result_mutex);

        fp = fopen(algo->result_stream_file, "r");
        if (fp != NULL) {
            long end_pos;

            if (fseek(fp, 0, SEEK_END) == 0) {
                end_pos = ftell(fp);
                if (end_pos >= 0 && offset > end_pos) {
                    offset = 0;
                }
            }
            fseek(fp, offset, SEEK_SET);

            for (;;) {
                long line_start = ftell(fp);
                long line_end;
                unsigned long long line_run_id;
                unsigned long long line_seq;
                long long line_timestamp;
                int line_algo_id;
                int line_result_code;
                char line_algo_type[16];
                unsigned int final_code = 0U;
                int parsed;

                if (fgets(line, sizeof(line), fp) == NULL) {
                    break;
                }

                line_end = ftell(fp);
                if (strchr(line, '\n') == NULL) {
                    offset = line_start;
                    break;
                }

                parsed = sscanf(line, "%llu,%d,%15[^,],%d,%lld,%llu",
                                &line_run_id, &line_algo_id, line_algo_type,
                                &line_result_code, &line_timestamp, &line_seq);

                pthread_mutex_lock(&g_algo_result_mutex);
                if (!algo->result_monitor_running ||
                    algo->result_monitor_generation != generation) {
                    pthread_mutex_unlock(&g_algo_result_mutex);
                    fclose(fp);
                    return NULL;
                }
                algo->result_stream_offset = line_end;
                pthread_mutex_unlock(&g_algo_result_mutex);
                offset = line_end;

                if (parsed != 6) {
                    fprintf(stderr, "[Algo Result] %s 忽略非法结果行: %s",
                            algo->name, line);
                    continue;
                }

                (void)line_timestamp;
                (void)line_seq;

                if (algo_add_vote(algo, generation, line_run_id,
                                  line_algo_id, line_algo_type,
                                  line_result_code, &final_code)) {
                    fclose(fp);
                    algo_send_result_frame(algo, final_code);
                    algo_stop_watch(algo);
                    return NULL;
                }
            }

            fclose(fp);
        }

        usleep(ALGO_RESULT_POLL_US);
    }

    return NULL;
}

static int algo_start_result_monitor(AlgoConfig *algo)
{
    AlgoResultMonitorArgs *thread_args;
    pthread_t thread;
    unsigned long generation;
    int ret;

    if (algo == NULL || algo->result_stream_file == NULL) {
        return -1;
    }

    thread_args = (AlgoResultMonitorArgs *)malloc(sizeof(*thread_args));
    if (thread_args == NULL) {
        return -1;
    }

    pthread_mutex_lock(&g_algo_result_mutex);
    algo->result_monitor_generation++;
    generation = algo->result_monitor_generation;
    algo->result_monitor_running = 1;
    pthread_mutex_unlock(&g_algo_result_mutex);

    thread_args->algo = algo;
    thread_args->generation = generation;

    ret = pthread_create(&thread, NULL, algo_result_monitor_thread,
                         (void *)thread_args);
    if (ret != 0) {
        free(thread_args);
        pthread_mutex_lock(&g_algo_result_mutex);
        if (algo->result_monitor_generation == generation) {
            algo->result_monitor_running = 0;
        }
        pthread_mutex_unlock(&g_algo_result_mutex);
        fprintf(stderr, "%s: 创建结果监听线程失败: %d\n",
                algo->name, ret);
        return -1;
    }

    pthread_detach(thread);
    printf("%s: 结果监听已启动，stream=%s, vote_target=%u\n",
           algo->name, algo->result_stream_file, algo->vote_target);
    return 0;
}

static void algo_stop_result_monitor(AlgoConfig *algo)
{
    if (algo == NULL) {
        return;
    }

    pthread_mutex_lock(&g_algo_result_mutex);
    algo->result_monitor_running = 0;
    algo->result_monitor_generation++;
    pthread_mutex_unlock(&g_algo_result_mutex);
}

static int write_algo_data_for_algo(AlgoConfig *algo)
{
    FILE *fp;
    size_t written;
    char tmp_file[256];
    char trigger_tmp_file[256];
    unsigned long long trigger_seq;
    unsigned long long run_id;

    if (algo == NULL || algo->data_file == NULL || algo->trigger_file == NULL) {
        fprintf(stderr, "write_algo_data_for_algo: invalid algo config\n");
        return -1;
    }

    snprintf(tmp_file, sizeof(tmp_file), "%s.tmp", algo->data_file);

    pthread_mutex_lock(&data_mutex);

    fp = fopen(tmp_file, "wb");
    if (fp == NULL) {
        pthread_mutex_unlock(&data_mutex);
        perror("write_algo_data_for_algo: fopen tmp failed");
        return -1;
    }

    written = fwrite(algo_data, sizeof(float), MAX_CH * MAX_POINTS, fp);
    fflush(fp);
    fclose(fp);

    pthread_mutex_unlock(&data_mutex);

    if (written != (size_t)(MAX_CH * MAX_POINTS)) {
        fprintf(stderr, "write_algo_data_for_algo: %s write count mismatch, expected %d, got %zu\n",
                algo->name, MAX_CH * MAX_POINTS, written);
        return -1;
    }

    if (rename(tmp_file, algo->data_file) != 0) {
        perror("write_algo_data_for_algo: rename failed");
        return -1;
    }

    pthread_mutex_lock(&g_algo_result_mutex);
    algo->trigger_seq++;
    trigger_seq = algo->trigger_seq;
    run_id = algo->run_id;
    pthread_mutex_unlock(&g_algo_result_mutex);

    snprintf(trigger_tmp_file, sizeof(trigger_tmp_file),
             "%s.tmp", algo->trigger_file);
    fp = fopen(trigger_tmp_file, "w");
    if (fp != NULL) {
        int write_failed =
            fprintf(fp, "%llu,%llu\n", trigger_seq, run_id) < 0;
        int close_failed = fclose(fp) != 0;
        if (write_failed || close_failed) {
            perror("write_algo_data_for_algo: write trigger failed");
            remove(trigger_tmp_file);
            return -1;
        }
    } else {
        perror("write_algo_data_for_algo: fopen trigger failed");
        return -1;
    }

    if (rename(trigger_tmp_file, algo->trigger_file) != 0) {
        perror("write_algo_data_for_algo: rename trigger failed");
        remove(trigger_tmp_file);
        return -1;
    }

    return 0;
}

static void build_python_cmd(char *cmd, size_t cmd_size, const AlgoConfig *algo,
                             const char *mode, int background)
{
    char extra[2048] = {0};
    char redirect[512] = {0};

    if (algo->model_path != NULL) {
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 " --model %s", algo->model_path);
    }
    if (algo->metadata_path != NULL) {
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 " --metadata %s", algo->metadata_path);
    }
    if (algo->scaler_path != NULL) {
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 " --scaler %s", algo->scaler_path);
    }
    if (algo->result_file != NULL && algo->result_stream_file != NULL &&
        algo->algo_type != NULL) {
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 " --result-file %s --result-stream-file %s"
                 " --algo-id %d --algo-type %s",
                 algo->result_file, algo->result_stream_file,
                 algo->id, algo->algo_type);
    }

    if (algo->id == 8) {
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 " --scaler none --bin-format window");
    }

    if (background) {
        if (algo->log_file != NULL) {
            snprintf(redirect, sizeof(redirect), " >> %s 2>&1 &", algo->log_file);
        } else {
            snprintf(redirect, sizeof(redirect), " > /dev/null 2>&1 &");
        }
    } else {
        snprintf(redirect, sizeof(redirect), " 2>&1");
    }

    if (strcmp(mode, "watch") == 0) {
        snprintf(cmd, cmd_size,
                 "python3 -u %s --mode watch%s --data %s --trigger %s --stop-file %s "
                 "--max-ch %d --max-points %d --channel %d --offset %d --interval 0.05%s",
                 algo->py_path, extra, algo->data_file, algo->trigger_file, algo->stop_file,
                 MAX_CH, MAX_POINTS, algo->channel, algo->offset, redirect);
    } else {
        snprintf(cmd, cmd_size,
                 "python3 -u %s --mode once%s --data %s "
                 "--max-ch %d --max-points %d --channel %d --offset %d%s",
                 algo->py_path, extra, algo->data_file,
                 MAX_CH, MAX_POINTS, algo->channel, algo->offset, redirect);
    }
}

static void algo_test_once(AlgoConfig *algo)
{
    if (algo == NULL) {
        return;
    }

    /*
     * 为了满足串口现场使用习惯，*1 命令不再执行 once 后退出，
     * 而是等同于启动 watch：Python 后台常驻，C 端实时把日志/结果转发到串口。
     */
    printf("%s: command *1 received, switch to watch mode with serial output\n", algo->name);
    algo_start_watch(algo);
}

static void algo_start_watch(AlgoConfig *algo)
{
    char cmd[ALGO_CMD_BUF_SIZE];
    char check_cmd[ALGO_CMD_BUF_SIZE];
    char kill_cmd[ALGO_CMD_BUF_SIZE];
    int ret;

    if (algo == NULL) {
        return;
    }

    printf("%s: start watch in background, serial output enabled\n", algo->name);

    algo_stop_result_monitor(algo);

    if (algo->stop_file != NULL) {
        remove(algo->stop_file);
    }

    if (algo_reset_result_run(algo) != 0) {
        fprintf(stderr, "%s: 初始化本轮结果文件失败\n", algo->name);
        return;
    }

    /* Some algorithms share the same Python file, so match both py_path and data_file. */
    snprintf(check_cmd, sizeof(check_cmd),
             "ps | grep '%s' | grep 'mode watch' | grep '%s' | "
             "grep 'result-stream-file %s' | grep -v grep > /dev/null 2>&1",
             algo->py_path, algo->data_file, algo->result_stream_file);
    ret = system(check_cmd);

    if (ret != 0) {
        /* 旧版 watch 没有结果流参数，先结束，避免两个进程同时消费 trigger。 */
        snprintf(kill_cmd, sizeof(kill_cmd),
                 "ps | grep '%s' | grep 'mode watch' | grep '%s' | "
                 "grep -v grep | awk '{print $1}' | xargs kill 2>/dev/null",
                 algo->py_path, algo->data_file);
        system(kill_cmd);

        /* 新启动 watch 时清空旧日志，并从日志开头转发，避免串口混入上一次结果。 */
        algo_clear_log(algo);
        algo_start_serial_monitor(algo, 1);

        build_python_cmd(cmd, sizeof(cmd), algo, "watch", 1);
        printf("%s: background watch cmd: %s\n", algo->name, cmd);
        system(cmd);
    } else {
        /* Python 已经在 watch，继续保持；串口监听从当前日志末尾开始。 */
        printf("%s: python watch is already running\n", algo->name);
        algo_start_serial_monitor(algo, 0);
    }

    algo->running = 1;

    if (algo_start_result_monitor(algo) != 0) {
        fprintf(stderr, "%s: 结果监听启动失败，停止算法\n", algo->name);
        algo_stop_watch(algo);
        return;
    }

    /*
     * 所有算法都等待真实数据组包后再创建trigger：
     * 1~7等待高频突变窗口，8/9等待模式A组包，
     * 10/11等待自适应信号确认后每200ms采一点形成1x12实际电压包。
     */

    if (algo->log_file != NULL) {
        printf("%s: python log: %s\n", algo->name, algo->log_file);
    }
}

static void algo_stop_watch(AlgoConfig *algo)
{
    char cmd[ALGO_CMD_BUF_SIZE];

    if (algo == NULL) {
        return;
    }

    printf("%s: 停止连续检测\n", algo->name);

    algo->running = 0;
    algo_stop_result_monitor(algo);

    if (algo->stop_file != NULL) {
        snprintf(cmd, sizeof(cmd), "touch %s", algo->stop_file);
        system(cmd);
    }

    snprintf(cmd, sizeof(cmd),
             "ps | grep '%s' | grep 'mode watch' | grep '%s' | grep -v grep | awk '{print $1}' | xargs kill 2>/dev/null",
             algo->py_path, algo->data_file);
    system(cmd);

    algo_stop_serial_monitor(algo);
}

static void algo_stop_all(void)
{
    for (int i = 0; i < ALGO_COUNT; i++) {
        algo_stop_watch(&g_algos[i]);
    }
}

static int handle_algo_cmd(unsigned int cmd)
{
    int algo_id;
    int op;
    AlgoConfig *algo;

    if (cmd == 310) {
        printf("310: 停止全部算法\n");
        algo_stop_all();
        return 1;
    }

    if ((cmd < 311 || cmd > 393) && (cmd < 401 || cmd > 413)) {
        return 0;
    }

    algo_id = ((int)cmd / 10) - 30;
    op = (int)cmd % 10;

    if (algo_id < 1 || algo_id > ALGO_COUNT) {
        return 0;
    }

    algo = &g_algos[algo_id - 1];

    switch (op) {
    case 1:
        algo_test_once(algo);
        break;
    case 2:
        algo_start_watch(algo);
        break;
    case 3:
        algo_stop_watch(algo);
        break;
    default:
        printf("无效算法操作命令: %u，有效操作个位为 1/2/3\n", cmd);
        break;
    }

    return 1;
}


void* thread_deal_client_ps(void *args)
{
	char ip[24] = {0};
	unsigned int i;
	
    IPData_t tIpData = *(IPData_t*)args;
    g_clientSocket = tIpData.fd;
    struct sockaddr_in clientAddr = tIpData.clientAddr;
	unsigned int cmd;
	
	unsigned int dataBuffer[100];
    unsigned int count = 100;
    unsigned int len = 0;

    /* 每个TCP连接只创建一个独立高频浮点上传线程。 */
    hf_tcp_upload_attach_client(g_clientSocket);
    /* 模式A八通道物理量使用独立线程，可与高频上传分别启停。 */
    mode_a_tcp_upload_attach_client(g_clientSocket);
    /* 腐蚀板256点float数据包上传线程，3005/3006独立控制。 */
    corrosion_tcp_upload_attach_client(g_clientSocket);
    /* 漏油12点float数据包上传线程，3007/3008独立控制。 */
    leak_tcp_upload_attach_client(g_clientSocket);
    /* 算法6/7 512点float数据包上传线程，3009/3010独立控制。 */
    algo67_tcp_upload_attach_client(g_clientSocket);
    /* 算法1 256点float数据包上传线程，3011/3012独立控制。 */
    algo1_tcp_upload_attach_client(g_clientSocket);
	
	// ==============================================================
    // 【新增】：当客户端连上时，顺便启动每秒推送数据的后台线程
    pthread_t upload_th;
    pthread_create(&upload_th, NULL, thread_tcp_upload_ps, args);
    pthread_detach(upload_th); // 分离线程，让它自己管理生命周期
    // ==============================================================


    while(1)
	{
		ssize_t numBytesRcvd = recv(g_clientSocket, g_tcpClientRcvBuffer, BUFFER_SIZE, 0);
		if (numBytesRcvd < 0) 
		{
			perror("recv() failed");
			break;
		}
		else if (numBytesRcvd == 0) 
		{
			printf("Connection closed by client\n");
			break; // 如果客户端关闭连接，退出循环
		}
        if (numBytesRcvd < 2)
        {
            printf("[TCP] 命令长度不足: %zd\n", numBytesRcvd);
            continue;
        }

		g_tcpClientRcvBuffer[numBytesRcvd] = '\0';  // Null-terminate the received data
        //memcpy(g_tcpRecvBuffer, g_tcpClientRcvBuffer, numBytesRcvd);
		for(i = 0; i <= numBytesRcvd - 1; i++)
		{
			//g_tcpRecvBuffer[i] = (unsigned int)g_tcpClientRcvBuffer[i];
            g_tcpRecvBuffer[i] =
            (unsigned int)(unsigned char)g_tcpClientRcvBuffer[i];
			printf("%x,", g_tcpRecvBuffer[i]);
		}
		printf("\n");
        //cmd = g_tcpRecvBuffer[0] + (g_tcpRecvBuffer[1] << 8);
        cmd = g_tcpRecvBuffer[0] |
        (g_tcpRecvBuffer[1] << 8);

        printf("[TCP] cmd=%u, hex=0x%04X\n", cmd, cmd);
		len = numBytesRcvd  | 0x10000;
		g_tcpRecvBuffer[numBytesRcvd - 1] = 0x20000 | g_tcpRecvBuffer[numBytesRcvd - 1];
        
        // Main switch-case block to handle commands
        switch (cmd) {
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			case HF_TCP_UPLOAD_CMD_START:
				hf_tcp_upload_start(HF_TCP_UPLOAD_LOGICAL_CH);
				break;

			case HF_TCP_UPLOAD_CMD_STOP:
				hf_tcp_upload_stop();
				break;

			case MODE_A_TCP_UPLOAD_CMD_START:
				mode_a_tcp_upload_start();
				break;

			case MODE_A_TCP_UPLOAD_CMD_STOP:
				mode_a_tcp_upload_stop();
				break;

			case CORROSION_TCP_UPLOAD_CMD_START:
				corrosion_tcp_upload_start();
				break;

			case CORROSION_TCP_UPLOAD_CMD_STOP:
				corrosion_tcp_upload_stop();
				break;

			case LEAK_TCP_UPLOAD_CMD_START:
				leak_tcp_upload_start();
				break;

			case LEAK_TCP_UPLOAD_CMD_STOP:
				leak_tcp_upload_stop();
				break;

			case ALGO67_TCP_UPLOAD_CMD_START:
				algo67_tcp_upload_start();
				break;

			case ALGO67_TCP_UPLOAD_CMD_STOP:
				algo67_tcp_upload_stop();
				break;

			case ALGO1_TCP_UPLOAD_CMD_START:
				algo1_tcp_upload_start();
				break;

			case ALGO1_TCP_UPLOAD_CMD_STOP:
				algo1_tcp_upload_stop();
				break;

			case 301: // 假设 201 对应你的启动宏
				printf("A\n");
                // 你可以从 TCP 报文里解析 IP，或者用默认 IP
                strcpy(g_utParams.ip_address, "192.168.3.12"); 
                cis_start_thread();
            	break;

			case 302: 
                cis_trigger_config();
                waiting_for_config_payload = 1; // 开启拦截模式
            	break;

			// ====================================================
            // 模式 A：仅采集 + 本地存盘 (无 1Hz 推送)
            // ====================================================
			case 303: 
				printf("303: 硬件启动 (仅本地存盘，不推送)\n");
                waiting_for_config_payload = 0; 
                start_config_trigger = true; // 硬件开
                start_storage_flag = true;   // 存盘开
                start_upload_flag = false;   // 【推送关】
				break;

			// ====================================================
            // 模式 B：仅采集 + 1Hz 推送 (无本地存盘)
            // ====================================================
            case 304:
                printf("304: 硬件启动 (仅 1Hz 推送，不落盘)\n");
                waiting_for_config_payload = 0; 
                start_config_trigger = true; // 硬件开
                start_storage_flag = false;  // 【存盘关】
                start_upload_flag = true;    // 推送开
                break;
			

			case 305: 
				printf("305\n");
				cis_stop_everything();
				start_storage_flag = false; // 停止保存
				for (int i = 0; i < MAX_CH; i++) 
				{
    			ch_active_mask[i] = false;
				}
				waiting_for_config_payload = 0; 
    			break;

			case 306:
#ifdef TCP_DBG_PRINT
				printf("306\n");
#endif
                uti_search_files();
                break;

			case 307:
#ifdef TCP_DBG_PRINT
				printf("307\n");
#endif
                uti_transfer_selected_file();
                break;

			case 308:
#ifdef TCP_DBG_PRINT
				printf("308\n");
#endif
                uti_delete_files();
                break;
            case 310:
            case 311:
            case 312:
            case 313:
            case 321:
            case 322:
            case 323:
            case 331:
            case 332:
            case 333:
            case 341:
            case 342:
            case 343:
            case 351:
            case 352:
            case 353:
            case 361:
            case 362:
            case 363:
            case 371:
            case 372:
            case 373:
            case 381:
            case 382:
            case 383:
            case 391:
            case 392:
            case 393:
            case 401:
            case 402:
            case 403:
            case 411:
            case 412:
            case 413:
                handle_algo_cmd(cmd);
                break;



/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
			case CMD_EXPORT_AND_TRANSFER_FILE:
#ifdef TCP_DBG_PRINT
				printf("5003\n");
#endif
                cis_transfer_file();
                break;

            case CMD_SEARCH_FILES:
#ifdef TCP_DBG_PRINT
				printf("5004\n");
#endif
                cis_search_files();
                break;

            case CMD_DELETE_OLD_FILES:
#ifdef TCP_DBG_PRINT
				printf("5005\n");		
#endif				
                cis_delete_files();
                break;

            case CMD_QUERY_DISK_USAGE:
#ifdef TCP_DBG_PRINT
				printf("5006\n");	
#endif				
                cis_query_disk_usage();
                break;

            case CMD_START:
#ifdef TCP_DBG_PRINT
				printf("5007\n");	
#endif				
                cis_start();
                break;	
				
			case CMD_END:
#ifdef TCP_DBG_PRINT
				printf("5008\n");	
#endif
				cis_end();
				break;	
				
            case CMD_STORAGE_SETTING:
#ifdef TCP_DBG_PRINT
				printf("5009\n");	
#endif
				cis_storage_setting();
				break;	
				
#ifdef TCP_DBG_PRINT2
			case CMD_TEST1:
				break;	
#endif			
//             default:
// #ifdef TCP_DBG_PRINT2			
// #endif	
// 				send(g_clientSocket, "Error: Unknown command", 22, 0);
//             break;

// ////////////////////////////////////////////////////////////////////////////////////////
// 			default:
// 			// --- 关键点：处理配置载荷 ---
// 			// 直接使用强制转换为无符号字符后的数据来判断包头，最安全！
// 			if (waiting_for_config_payload && 
// 				(unsigned char)g_tcpClientRcvBuffer[0] == 0xAA && 
// 				(unsigned char)g_tcpClientRcvBuffer[1] == 0x55) 
// 			{
// 				printf("[TCP] 检测到 AA 55 配置包，开始解析...\n");
				
// 				unsigned char raw_packet[128]; // 稍微给大一点缓存
// 				int k = 0; // 使用局部变量 k 循环，防止和外部 i 冲突
				
// 				for(k = 0; k < numBytesRcvd; k++) 
// 				{
// 					raw_packet[k] = (unsigned char)g_tcpClientRcvBuffer[k];
// 				}

// 				// 调用在 fun.c 中编写的解析函数
// 				if (0 == cis_parse_config_packet(raw_packet, numBytesRcvd)) 
// 				{
// 					// 解析成功后，标志位归零
// 					// 注意：start_config_trigger = true 已经在解析函数内部设置了
// 					waiting_for_config_payload = 0; 
// 				}
// 			} 
// 			else 
// 			{
// 				// 既不是已知指令，也不是合法的配置包
// 				#ifdef TCP_DBG_PRINT
// 				printf("[TCP] 收到未知指令或未处于等待配置状态: 0x%X\n", cmd);
// 				#endif
// 			}
//             break;
			default:
			// --- 只有在配置模式下，且包头是 AA 55 时才解析 ---
			if (waiting_for_config_payload && 
				(unsigned char)g_tcpClientRcvBuffer[0] == 0xAA &&
				(unsigned char)g_tcpClientRcvBuffer[1] == 0x55) 
			{
				unsigned char raw_packet[128];
				for(int k = 0; k < numBytesRcvd; k++) {
					raw_packet[k] = (unsigned char)g_tcpClientRcvBuffer[k];
				}

				if (0 == cis_parse_config_packet(raw_packet, numBytesRcvd)) 
				{
					// 这里不再清零 waiting_for_config_payload
					// 这样你可以连续发 AA 55 03..., AA 55 04...
					printf("[TCP] 通道配置已记录，可继续配置其他通道或发 203 开始保存。\n");
				}
			}
			// --- 第二步：既不是已知指令，也不是合法的配置包，报错 ---
			else 
			{
				#ifdef TCP_DBG_PRINT
				printf("[TCP] 收到未知指令或非法包。Cmd: 0x%X, Waiting: %d\n", 
						cmd, waiting_for_config_payload);
				#endif
				
				// 给客户端回传一个报错信息
				send(g_clientSocket, "Error: Unknown command", 22, 0);
			}
            break; // 整个 default 结束

		}
	}
	algo1_tcp_upload_detach_client();
	algo67_tcp_upload_detach_client();
	leak_tcp_upload_detach_client();
	corrosion_tcp_upload_detach_client();
	mode_a_tcp_upload_detach_client();
	hf_tcp_upload_detach_client();
	close(g_clientSocket); // Close client socket
}

// void* thread_deal_client_pl(void *args)
// {
// 	char ip[24] = {0};
// 	unsigned int i;
	
//     IPData_t tIpData = *(IPData_t*)args;
//     g_clientSocket1 = tIpData.fd;
//     struct sockaddr_in clientAddr = tIpData.clientAddr;
// 	unsigned int cmd;
	
// 	unsigned int dataBuffer[100];
// 	unsigned int count = 100;
// 	unsigned int len = 0;		
//     while(1)
// 	{
// 		ssize_t numBytesRcvd = recv(g_clientSocket1, g_tcpClientRcvBuffer1, BUFFER_SIZE, 0);
// 		if (numBytesRcvd < 0) 
// 		{
// 			perror("recv() failed");
// 			break;
// 		}
// 		else if (numBytesRcvd == 0) 
// 		{
// 			printf("Connection closed by client\n");
// 			break; // 如果客户端关闭连接，退出循环
// 		}
// 		g_tcpClientRcvBuffer1[numBytesRcvd] = '\0';  // Null-terminate the received data
//         //memcpy(g_tcpRecvBuffer, g_tcpClientRcvBuffer, numBytesRcvd);
// 		for(i = 0; i <= numBytesRcvd - 1; i++)
// 		{
// 			g_tcpRecvBuffer1[i] = (unsigned int)g_tcpClientRcvBuffer1[i];
// 			printf("%x,", g_tcpRecvBuffer1[i]);
// 		}
// 		printf("\n");
//         cmd = g_tcpRecvBuffer1[0] + (g_tcpRecvBuffer1[1] << 8);
// 		len = numBytesRcvd  | 0x10000;
// 		g_tcpRecvBuffer1[numBytesRcvd - 1] = 0x20000 | g_tcpRecvBuffer1[numBytesRcvd - 1];
        
// 		// Main switch-case block to handle commands
//         //switch (cmd) 
// 	}
// 	close(g_clientSocket1); // Close client socket
// }


void cis_search_files(void)
{
	FileNameInfo_t buffer[BUFFER_SIZE];
	unsigned char functionNo = 0xff;
    int count = 0;
	unsigned char response[BUFFER_SIZE];
	unsigned int responseLength = 0;
	TimeParams_t tStart, tEnd;
	
	receive_time(g_tcpClientRcvBuffer + 2 , &tStart);
    receive_time(g_tcpClientRcvBuffer + 14, &tEnd);
	
	TimeRange_t timeRange = {tStart, tEnd};
	
	response[0] = 0x55;
	response[1] = 0x00;
    response[2] = g_tcpClientRcvBuffer[0];
    response[3] = g_tcpClientRcvBuffer[1];
	
	
    if (search_and_sort_files(&timeRange, buffer, &count) != 0) {
        response[4] = 0xFF;
        response[5] = 0x00;
		responseLength = 6; 
        send(g_clientSocket, response, responseLength, 0);
        return;
    }
	
	response[4] = count;
    response[5] = count >> 8;
	
    for (int i = 0; i < count; i++) {
		if(strcmp(buffer[i].function, "0") == 0) 
		{
			functionNo = 0;
		}
		else if(strcmp(buffer[i].function, "1") == 0) 
		{
			functionNo = 1;
		}
		else if(strcmp(buffer[i].function, "2") == 0) 
		{
			functionNo = 2;
		}
		else if(strcmp(buffer[i].function, "3") == 0) 
		{
			functionNo = 3;
		}
		else if(strcmp(buffer[i].function, "4") == 0) 
		{
			functionNo = 4;
		}
		else if(strcmp(buffer[i].function, "5") == 0) 
		{
			functionNo = 5;
		}
		else if(strcmp(buffer[i].function, "6") == 0) 
		{
			functionNo = 6;
		}
		else if(strcmp(buffer[i].function, "7") == 0) 
		{
			functionNo = 7;
		}
		else if(strcmp(buffer[i].function, "8") == 0) 
		{
			functionNo = 8;
		}
		else if(strcmp(buffer[i].function, "9") == 0) 
		{
			functionNo = 9;
		}
		else if(strcmp(buffer[i].function, "10") == 0) 
		{
			functionNo = 10;
		}
		else if(strcmp(buffer[i].function, "11") == 0) 
		{
			functionNo = 11;
		}
		else if(strcmp(buffer[i].function, "12") == 0) 
		{
			functionNo = 12;
		}
		else if(strcmp(buffer[i].function, "13") == 0) 
		{
			functionNo = 13;
		}
		else if(strcmp(buffer[i].function, "14") == 0) 
		{
			functionNo = 14;
		}
		else if(strcmp(buffer[i].function, "15") == 0) 
		{
			functionNo = 15;
		}
		else
		{
			functionNo = 0xff;
		}
		
		response[i * 15 + 6] = i;
		response[i * 15 + 7] = i >> 8;
		response[i * 15 + 8] = functionNo & 0xFF;
		response[i * 15 + 9] = functionNo >> 8;
	
		response[i * 15 + 10] = buffer[i].year;
		response[i * 15 + 11] = buffer[i].year >> 8;
		response[i * 15 + 12] = buffer[i].month;
		response[i * 15 + 13] = buffer[i].month >> 8;
		response[i * 15 + 14] = buffer[i].day;
		response[i * 15 + 15] = buffer[i].day >> 8;
		response[i * 15 + 16] = buffer[i].hour;
		response[i * 15 + 17] = buffer[i].hour >> 8;
		response[i * 15 + 18] = buffer[i].minute;
		response[i * 15 + 19] = buffer[i].minute >> 8;
		response[i * 15 + 20] = buffer[i].second;
		response[i * 15 + 21] = buffer[i].second >> 8;
    }
	send(g_clientSocket, response, (count - 1) * 15 + 22, 0);		
    // Store the search results for later use in file export
    memcpy(g_tFileList, buffer, count * sizeof(FileNameInfo_t));
    g_fileCount = count;
}


void cis_transfer_file(void)
{
	int fileIndex;
	char fullFilePath[FILENAME_LENGTH1];
	unsigned char response[BUFFER_SIZE];
    unsigned int responseLength = 0;
	
	response[0] = 0x55;
	response[1] = 0x00;
	response[2] = g_tcpClientRcvBuffer[0];
	response[3] = g_tcpClientRcvBuffer[1];
		
	memcpy(&fileIndex, g_tcpClientRcvBuffer + 2, sizeof(unsigned char));
	
	printf("%d, %d\n", fileIndex, g_fileCount);
	
    if (fileIndex >= 1 && fileIndex <= g_fileCount) {
        FileNameInfo_t *fileInfo = &g_tFileList[fileIndex - 1];
		if(g_saveFileEn == 1)
		{
			snprintf(fullFilePath, sizeof(fullFilePath), "%s/%s_%04hu_%02hhu_%02hhu_%02hhu_%02hhu_%02hhu.bin",
                 "/mnt/file", fileInfo->function,
                 fileInfo->year, fileInfo->month, fileInfo->day,
                 fileInfo->hour, fileInfo->minute, fileInfo->second);
		}
		else if(g_saveFileEn == 2)
		{
			snprintf(fullFilePath, sizeof(fullFilePath), "%s/%s_%04hu_%02hhu_%02hhu_%02hhu_%02hhu_%02hhu.bin",
                 "/run/media/nvme0n1p1/file", fileInfo->function,
                 fileInfo->year, fileInfo->month, fileInfo->day,
                 fileInfo->hour, fileInfo->minute, fileInfo->second);
		}
    } else {
        perror("Invalid file index");
        return;
    }

    printf("fullFilePath = %s\n", fullFilePath);
    FILE *file = fopen(fullFilePath, "rb");

    if (file == NULL) {
        perror("File open error");
        
        response[4] = 0xFF;
		response[5] = 0x00;
        responseLength = 6;
        send(g_clientSocket, response, responseLength, 0);
        return;
    } else {
        responseLength = 4;
        send(g_clientSocket, response, responseLength, 0);
    }

    size_t bytesRead;
    char buffer[BUFFER_SIZE];
    while ((bytesRead = fread(buffer, 1, BUFFER_SIZE, file)) > 0) 
	{
        // 发送包长度
        size_t packageLength = bytesRead; // 这里是实际读取的长度
        send(g_clientSocket, (char*)&packageLength, sizeof(packageLength), 0); // 发送长度
        
        // 发送包数据
        send(g_clientSocket, buffer, bytesRead, 0);
    }

    // Send file transfer complete flag
    unsigned char endFlagBuffer[2] = {0xFF, 0xFF};
    send(g_clientSocket, endFlagBuffer, 2, 0);

    fclose(file);
}
	



void cis_delete_files(void)
{
	//char timeStr[20];
	int fileIndex;
	char fullFilePath[FILENAME_LENGTH1];
	unsigned char response[BUFFER_SIZE];
	unsigned int responseLength = 0;
	
	
/* 	TimeParams_t tStart;
    tStart.year = 1900;
    tStart.month = 1;
    tStart.day = 1;
    tStart.hour = 0;
    tStart.minute = 0;
    tStart.second = 0; 
	
	TimeParams_t tEnd;
	receive_time(g_tcpClientRcvBuffer + 2, &tEnd);*/
	
	response[0] = 0x55;
	response[1] = 0x00;
	response[2] = g_tcpClientRcvBuffer[0];
	response[3] = g_tcpClientRcvBuffer[1];
		
	//memcpy(&fileIndex, g_tcpClientRcvBuffer + 2, sizeof(unsigned char));
	fileIndex = g_tcpClientRcvBuffer[2] + (g_tcpClientRcvBuffer[3] << 8);
	
	printf("%d, %d\n", fileIndex, g_fileCount);
    //按照时间删除
	/*DIR *dir;
	if(g_saveFileEn == 1)
	{
		dir = opendir("/mnt/file");
	}
	else if(g_saveFileEn == 2)
	{
		dir = opendir("/run/media/nvme0n1p1/file");
	}

    if (!dir) {
        perror("无法打开目录"); 
        response[4] = 0xFF;
        response[5] = 0x00;
		responseLength = 6;
        send(g_clientSocket, response, responseLength, 0);
        return;
    }
	
	struct dirent *dp;
    while ((dp = readdir(dir)) != NULL) {
        if (dp->d_type == DT_REG) {
            const char *dot = strrchr(dp->d_name, '.');
            if (dot && strcmp(dot, ".bin") == 0) {
                FileNameInfo_t info;
                if (parse_filename(dp->d_name, &info)) {
                    if (time_in_range(&info, &tStart, &tEnd)) {
						char filePath[FILENAME_LENGTH1];
						if(g_saveFileEn == 1)
						{
							snprintf(filePath, sizeof(filePath), "%s/%s", "/mnt/file", dp->d_name);
						}
						else if(g_saveFileEn == 2)
						{
							snprintf(filePath, sizeof(filePath), "%s/%s", "/run/media/nvme0n1p1/file", dp->d_name);
						}
						
						remove(filePath);
						system("sync");
					}
				}
			}
		}
	}

    closedir(dir);
	*/
    
        if (fileIndex >= 1 && fileIndex <= g_fileCount) {
        FileNameInfo_t *fileInfo = &g_tFileList[fileIndex - 1];
		if(g_saveFileEn == 1)
		{
			snprintf(fullFilePath, sizeof(fullFilePath), "%s/%s_%04hu_%02hhu_%02hhu_%02hhu_%02hhu_%02hhu.bin",
                 "/mnt/file", fileInfo->function,
                 fileInfo->year, fileInfo->month, fileInfo->day,
                 fileInfo->hour, fileInfo->minute, fileInfo->second);
		}
		else if(g_saveFileEn == 2)
		{
			snprintf(fullFilePath, sizeof(fullFilePath), "%s/%s_%04hu_%02hhu_%02hhu_%02hhu_%02hhu_%02hhu.bin",
                 "/run/media/nvme0n1p1/file", fileInfo->function,
                 fileInfo->year, fileInfo->month, fileInfo->day,
                 fileInfo->hour, fileInfo->minute, fileInfo->second);
		}
    } else {
        perror("Invalid file index");
        return;
    }

    printf("fullFilePath = %s\n", fullFilePath);
    // 打开文件
    FILE *file = fopen(fullFilePath, "rb");
    
    if (file == NULL) {
        perror("File open error");
        
        response[4] = 0xFF;
        response[5] = 0x00;
        responseLength = 6;
        send(g_clientSocket, response, responseLength, 0);
        return;
    }
    fclose(file);
    // 删除文件
    if (remove(fullFilePath) != 0) {
        perror("File delete error");
        
        response[4] = 0xFF;
        response[5] = 0x01; // 错误标志
        responseLength = 6;
        send(g_clientSocket, response, responseLength, 0);
    } else {
        // 删除成功，发送成功标志
        response[4] = 0x00; // 成功标志
        response[5] = 0x00;
        responseLength = 6;
        send(g_clientSocket, response, responseLength, 0);
    }
}
	
	
void cis_query_disk_usage(void)
{
	unsigned char response[BUFFER_SIZE];
	unsigned int responseLength = 0;
	
	response[0] = 0x55;
	response[1] = 0x00;
	response[2] = g_tcpClientRcvBuffer[0];
	response[3] = g_tcpClientRcvBuffer[1];
	
    struct statvfs stat;
	
	if(g_saveFileEn == 1)
	{
		if (statvfs("/mnt/file", &stat) != 0) {
			perror("statvfs() failed");
			response[4] = 0xFF;
			response[5] = 0x00;
			responseLength = 6;
			send(g_clientSocket, response, responseLength, 0);
			return;
		}
	}
	else if(g_saveFileEn == 2)
	{
		if (statvfs("/run/media/nvme0n1p1/file", &stat) != 0) {
			perror("statvfs() failed");
			response[4] = 0xFF;
			response[5] = 0x00;
			responseLength = 6;
			send(g_clientSocket, response, responseLength, 0);
			return;
		}
		
	}

    unsigned long total = stat.f_blocks * stat.f_frsize;
    unsigned long available = stat.f_bavail * stat.f_frsize;

    
    snprintf((unsigned char *)(response + 4), BUFFER_SIZE - 4 * sizeof(unsigned char),
             "Total: %lu bytes, Available: %lu bytes", total, total - available);
			 
    send(g_clientSocket, response, strlen((unsigned char *)(response + 4)) + 4 * sizeof(unsigned char), 0);
}

void cis_storage_setting(void) {
	
	unsigned char response[BUFFER_SIZE];
	unsigned int responseLength = 0;
	unsigned int function = 0;
    unsigned int period = 0;
	response[0] = 0x55;
	response[1] = 0x00;
	response[2] = g_tcpClientRcvBuffer[0];
	response[3] = g_tcpClientRcvBuffer[1];
			
    AD7915RegIn1_t tAD7915RegIn1;
    
	function = g_tcpClientRcvBuffer[2] + (g_tcpClientRcvBuffer[3] << 8);
    period   = (g_tcpClientRcvBuffer[4] + (g_tcpClientRcvBuffer[5] << 8))*100000;
	
#ifdef FUN_DBG_PRINT
	printf("function == %d\n", function);
    printf("period == %d\n", period);
#endif	
	switch (function) 
	{
		case 0:

#ifdef FUN_DBG_PRINT
				printf("iepe channel %d period = %d \n" ,function ,period);
#endif
			break;
            
		case 1:

#ifdef FUN_DBG_PRINT
				printf("iepe channel %d period = %d \n" ,function ,period);
#endif
			break;
            
		case 2:

#ifdef FUN_DBG_PRINT
				printf("iepe channel %d period = %d \n" ,function ,period);
#endif
			break;
            
		case 3:

#ifdef FUN_DBG_PRINT
				printf("iepe channel %d period = %d \n" ,function ,period);
#endif
			break;
            
		case 4:

#ifdef FUN_DBG_PRINT
				printf("current channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(1, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(1, &tAD7915RegIn1);
            break;

		case 5:

#ifdef FUN_DBG_PRINT
				printf("current channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(1, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(1, &tAD7915RegIn1);
            break;
            
		case 6:

#ifdef FUN_DBG_PRINT
				printf("current channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(1, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(1, &tAD7915RegIn1);
            break;
            
		case 7:

#ifdef FUN_DBG_PRINT
				printf("current channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(1, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(1, &tAD7915RegIn1);
            break;
            
		case 8:

#ifdef FUN_DBG_PRINT
				printf("current channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(1, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(1, &tAD7915RegIn1);
            break;
            
		case 9:

#ifdef FUN_DBG_PRINT
				printf("current channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(1, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(1, &tAD7915RegIn1);
            break;
            
		case 10:

#ifdef FUN_DBG_PRINT
				printf("current channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(1, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(1, &tAD7915RegIn1);
            break;
            
		case 11:

#ifdef FUN_DBG_PRINT
				printf("current channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(1, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(1, &tAD7915RegIn1);
            break;
            
            
		case 12:
#ifdef FUN_DBG_PRINT
				printf("voltage channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(0, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(0, &tAD7915RegIn1);
			break;		
		
		case 13:
#ifdef FUN_DBG_PRINT
				printf("voltage channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(0, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(0, &tAD7915RegIn1);
			break;		

		case 14:
#ifdef FUN_DBG_PRINT
				printf("voltage channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(0, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(0, &tAD7915RegIn1);
			break;		

		case 15:
#ifdef FUN_DBG_PRINT
				printf("voltage channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(0, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(0, &tAD7915RegIn1);
			break;		
			
		case 16:
#ifdef FUN_DBG_PRINT
				printf("voltage channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(0, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(0, &tAD7915RegIn1);
			break;		
			
		case 17:
#ifdef FUN_DBG_PRINT
				printf("voltage channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(0, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(0, &tAD7915RegIn1);
			break;
		
		case 18:
#ifdef FUN_DBG_PRINT
				printf("voltage channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(0, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(0, &tAD7915RegIn1);
			break;		

		case 19:
#ifdef FUN_DBG_PRINT
				printf("voltage channel %d period = %d \n" ,function ,period);
#endif
                tAD7915RegIn1.period = period;
                tAD7915RegIn1.resetnSys = 0;
                init_ad7915(0, &tAD7915RegIn1);

                
                tAD7915RegIn1.resetnSys = 1;
                init_ad7915(0, &tAD7915RegIn1);
			break;		
			
		default:

            break;
	}
	
	

finish:	
    response[4] = 0xFF; // Success
	response[5] = 0x00;
	responseLength = 6;
	
    send(g_clientSocket, response, responseLength, 0);
}

void cis_start(void) {
	
	unsigned char response[BUFFER_SIZE];
	unsigned int responseLength = 0;
	unsigned int function = 0;
	response[0] = 0x55;
	response[1] = 0x00;
	response[2] = g_tcpClientRcvBuffer[0];
	response[3] = g_tcpClientRcvBuffer[1];
			
    struct tm timeInfo = {0};
	TimeParams_t tEnd;
	TimeParams_t tInitTime;
	
	receive_time(g_tcpClientRcvBuffer + 2, &tInitTime);
	// 使用 tInitTime 来初始化时间
    timeInfo.tm_year = tInitTime.year - 1900;  // struct tm 年份是从 1900 年开始的
    timeInfo.tm_mon = tInitTime.month - 1;     // struct tm 月份是从 0 开始的 (0 = January)
    timeInfo.tm_mday = tInitTime.day;
    timeInfo.tm_hour = tInitTime.hour;
    timeInfo.tm_min = tInitTime.minute;
    timeInfo.tm_sec = tInitTime.second;  // 添加秒
	
    time_t initTime = mktime(&timeInfo);
	if (initTime == (time_t)(-1)) {
        perror("mktime() failed");
        response[4] = 0xFF; // Error
		response[5] = 0x00;
		responseLength = 6;
        send(g_clientSocket, response, responseLength, 0);
        return;
    }
	
	char ptime[64];
	char ptime1[64];
	
	
    struct timeval tv;
    tv.tv_sec = initTime;
    tv.tv_usec = 0;

    if (settimeofday(&tv, NULL) != 0) {
        perror("settimeofday() failed");
        
        response[4] = 0xFF; // Error
		response[5] = 0x00;
		responseLength = 6;
        send(g_clientSocket, response, responseLength, 0);
        return;
    }

    system("hwclock -w");
	fflush(stdout);
	
				  
	sprintf(ptime, "%04hu_%02hhu_%02hhu", 
                  tInitTime.year, tInitTime.month, tInitTime.day);
	printf("ptime %s\n", ptime);
	sprintf(ptime1, "%04hu_%02hhu_%02hhu_%02hhu_%02hhu_%02hhu", 
                  tInitTime.year, tInitTime.month, tInitTime.day,
                  tInitTime.hour, tInitTime.minute, tInitTime.second);
	printf("ptime1 %s\n", ptime1);
	
	unsigned int fileFlushByteCount = 100;
	unsigned int fileCloseByteCount = 10000;
	
/*
	function = g_tcpClientRcvBuffer[14] + (g_tcpClientRcvBuffer[15] << 8);
	
#ifdef FUN_DBG_PRINT
	printf("function == %d\n", function);
#endif	
*/
	if(g_tcpStart == 0)
	{
		g_tcpStart = 1;
		init_folder(g_saveFileEn);
		strcpy(g_tFilePsTempStruct.fileType, "temp");
		init_file(&g_tFilePsTempStruct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			
	}

    // 定义 20 个通道的状态数组
    unsigned short channelStatus[20] = {0};
    
    // 从 g_tcpClientRcvBuffer[14] 到 g_tcpClientRcvBuffer[54] 提取 20 个通道的状态
    for (int i = 0; i < 20; i++) 
    {
        channelStatus[i] = g_tcpClientRcvBuffer[14 + (i * 2)] + (g_tcpClientRcvBuffer[15 + (i * 2)] << 8);
    }

// 前面解析 channelStatus 的 for 循环保持不变...

    for (int i = 0; i < 20; i++) 
    {
switch (i) 
        {
		// ================= 振动通道 IEPE (0~3) =================
		case 0:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.iepe0Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting IEPE 0...\n");
			#endif
				g_tDeviceStruct.iepe0Status = 0x01;
				strcpy(g_tFileIepe0Struct.fileType, "0");
				init_file(&g_tFileIepe0Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;
		
		case 1:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.iepe1Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting IEPE 1...\n");
			#endif
				g_tDeviceStruct.iepe1Status = 0x01;
				strcpy(g_tFileIepe1Struct.fileType, "1");
				init_file(&g_tFileIepe1Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 2:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.iepe2Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting IEPE 2...\n");
			#endif
				g_tDeviceStruct.iepe2Status = 0x01;
				strcpy(g_tFileIepe2Struct.fileType, "2");
				init_file(&g_tFileIepe2Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 3:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.iepe3Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting IEPE 3...\n");
			#endif
				g_tDeviceStruct.iepe3Status = 0x01;
				strcpy(g_tFileIepe3Struct.fileType, "3");
				init_file(&g_tFileIepe3Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;
		
		// ================= 电流通道 Current (4~11) =================
		case 4:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.current0Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Current 0 (Ch 4)...\n");
			#endif
				g_tDeviceStruct.current0Status = 0x01;
				strcpy(g_tFileCurrent0Struct.fileType, "4");
				init_file(&g_tFileCurrent0Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 5:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.current1Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Current 1 (Ch 5)...\n");
			#endif
				g_tDeviceStruct.current1Status = 0x01;
				strcpy(g_tFileCurrent1Struct.fileType, "5");
				init_file(&g_tFileCurrent1Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 6:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.current2Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Current 2 (Ch 6)...\n");
			#endif
				g_tDeviceStruct.current2Status = 0x01;
				strcpy(g_tFileCurrent2Struct.fileType, "6");
				init_file(&g_tFileCurrent2Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 7:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.current3Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Current 3 (Ch 7)...\n");
			#endif
				g_tDeviceStruct.current3Status = 0x01;
				strcpy(g_tFileCurrent3Struct.fileType, "7");
				init_file(&g_tFileCurrent3Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 8:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.current4Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Current 4 (Ch 8)...\n");
			#endif
				g_tDeviceStruct.current4Status = 0x01;
				strcpy(g_tFileCurrent4Struct.fileType, "8");
				init_file(&g_tFileCurrent4Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 9:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.current5Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Current 5 (Ch 9)...\n");
			#endif
				g_tDeviceStruct.current5Status = 0x01;
				strcpy(g_tFileCurrent5Struct.fileType, "9");
				init_file(&g_tFileCurrent5Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 10:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.current6Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Current 6 (Ch 10)...\n");
			#endif
				g_tDeviceStruct.current6Status = 0x01;
				strcpy(g_tFileCurrent6Struct.fileType, "10");
				init_file(&g_tFileCurrent6Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;              

		case 11:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.current7Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Current 7 (Ch 11)...\n");
			#endif
				g_tDeviceStruct.current7Status = 0x01;
				strcpy(g_tFileCurrent7Struct.fileType, "11");
				init_file(&g_tFileCurrent7Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		// ================= 电压通道 Voltage (12~19) =================
		case 12:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.voltage0Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Voltage 0 (Ch 12)...\n");
			#endif
				g_tDeviceStruct.voltage0Status = 0x01;
				strcpy(g_tFileVoltage0Struct.fileType, "12");
				init_file(&g_tFileVoltage0Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 13:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.voltage1Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Voltage 1 (Ch 13)...\n");
			#endif
				g_tDeviceStruct.voltage1Status = 0x01;
				strcpy(g_tFileVoltage1Struct.fileType, "13");
				init_file(&g_tFileVoltage1Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 14:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.voltage2Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Voltage 2 (Ch 14)...\n");
			#endif
				g_tDeviceStruct.voltage2Status = 0x01;
				strcpy(g_tFileVoltage2Struct.fileType, "14");
				init_file(&g_tFileVoltage2Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 15:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.voltage3Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Voltage 3 (Ch 15)...\n");
			#endif
				g_tDeviceStruct.voltage3Status = 0x01;
				strcpy(g_tFileVoltage3Struct.fileType, "15");
				init_file(&g_tFileVoltage3Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 16:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.voltage4Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Voltage 4 (Ch 16)...\n");
			#endif
				g_tDeviceStruct.voltage4Status = 0x01;
				strcpy(g_tFileVoltage4Struct.fileType, "16");
				init_file(&g_tFileVoltage4Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 17:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.voltage5Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Voltage 5 (Ch 17)...\n");
			#endif
				g_tDeviceStruct.voltage5Status = 0x01;
				strcpy(g_tFileVoltage5Struct.fileType, "17");
				init_file(&g_tFileVoltage5Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 18:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.voltage6Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Voltage 6 (Ch 18)...\n");
			#endif
				g_tDeviceStruct.voltage6Status = 0x01;
				strcpy(g_tFileVoltage6Struct.fileType, "18");
				init_file(&g_tFileVoltage6Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		case 19:
			if(channelStatus[i] == 0x01 && g_tDeviceStruct.voltage7Status == 0x00)
			{
			#ifdef FUN_DBG_PRINT
				printf("Starting Voltage 7 (Ch 19)...\n");
			#endif
				g_tDeviceStruct.voltage7Status = 0x01;
				strcpy(g_tFileVoltage7Struct.fileType, "19");
				init_file(&g_tFileVoltage7Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
			}
			break;

		default:
			break;
        }
    }

finish: 
    response[4] = 0xFF; // Success
    response[5] = 0x00;
    responseLength = 6;
    send(g_clientSocket, response, responseLength, 0);
}




//上面进行修改的
//原先的
//     for (int i = 0; i < 20; i++) 
//     {
//         	switch (i) 
//         {
//             case 0:g_tDeviceStruct.iepe0Status == channelStatus[i];
//                 if(g_tDeviceStruct.iepe0Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.iepe0Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.iepe0Status = 0x01;
//                     strcpy(g_tFileIepe0Struct.fileType, "0");
//                     init_file(&g_tFileIepe0Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
                
//             case 1:g_tDeviceStruct.iepe1Status == channelStatus[i];
//                 if(g_tDeviceStruct.iepe1Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.iepe1Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.iepe1Status = 0x01;
//                     strcpy(g_tFileIepe1Struct.fileType, "1");
//                     init_file(&g_tFileIepe1Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
                
//             case 2:g_tDeviceStruct.iepe2Status == channelStatus[i];
//                 if(g_tDeviceStruct.iepe2Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.iepe2Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.iepe2Status = 0x01;
//                     strcpy(g_tFileIepe2Struct.fileType, "2");
//                     init_file(&g_tFileIepe2Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;		
            
//             case 3:g_tDeviceStruct.iepe3Status == channelStatus[i];
//                 if(g_tDeviceStruct.iepe3Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.iepe3Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.iepe3Status = 0x01;
//                     strcpy(g_tFileIepe3Struct.fileType, "3");
//                     init_file(&g_tFileIepe3Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
    
//             case 4:g_tDeviceStruct.current0Status == channelStatus[i];
//                 if(g_tDeviceStruct.current0Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.current0Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.current0Status = 0x01;
//                     strcpy(g_tFileCurrent0Struct.fileType, "4");
//                     init_file(&g_tFileCurrent0Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
    
//             case 5:g_tDeviceStruct.current1Status == channelStatus[i];
//                 if(g_tDeviceStruct.current1Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.current1Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.current1Status = 0x01;
//                     strcpy(g_tFileCurrent1Struct.fileType, "5");
//                     init_file(&g_tFileCurrent1Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
                
//             case 6:g_tDeviceStruct.current2Status == channelStatus[i];
//                 if(g_tDeviceStruct.current2Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.current2Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.current2Status = 0x01;
//                     strcpy(g_tFileCurrent2Struct.fileType, "6");
//                     init_file(&g_tFileCurrent2Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
                
//             case 7:g_tDeviceStruct.current3Status == channelStatus[i];
//                 if(g_tDeviceStruct.current3Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.current3Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.current3Status = 0x01;
//                     strcpy(g_tFileCurrent3Struct.fileType, "7");
//                     init_file(&g_tFileCurrent3Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;		
            
//             case 8:g_tDeviceStruct.current4Status == channelStatus[i];
//                 if(g_tDeviceStruct.current4Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.current4Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.current4Status = 0x01;
//                     strcpy(g_tFileCurrent4Struct.fileType, "8");
//                     init_file(&g_tFileCurrent4Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
    
//             case 9:g_tDeviceStruct.current5Status == channelStatus[i];
//                 if(g_tDeviceStruct.current5Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.current5Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.current5Status = 0x01;
//                     strcpy(g_tFileCurrent5Struct.fileType, "9");
//                     init_file(&g_tFileCurrent5Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
    
//             case 10:g_tDeviceStruct.current6Status == channelStatus[i];
//                 if(g_tDeviceStruct.current6Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.current6Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.current6Status = 0x01;
//                     strcpy(g_tFileCurrent6Struct.fileType, "10");
//                     init_file(&g_tFileCurrent6Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
                
//             case 11:g_tDeviceStruct.current7Status == channelStatus[i];
//                 if(g_tDeviceStruct.current7Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.current7Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.current7Status = 0x01;
//                     strcpy(g_tFileCurrent7Struct.fileType, "11");
//                     init_file(&g_tFileCurrent7Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
                
//             case 12:g_tDeviceStruct.voltage0Status == channelStatus[i];
//                 if(g_tDeviceStruct.voltage0Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.voltage0Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.voltage0Status = 0x01;
//                     strcpy(g_tFileVoltage0Struct.fileType, "12");
//                     init_file(&g_tFileVoltage0Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;		
            
//             case 13:g_tDeviceStruct.voltage1Status == channelStatus[i];
//                 if(g_tDeviceStruct.voltage1Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.voltage1Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.voltage1Status = 0x01;
//                     strcpy(g_tFileVoltage1Struct.fileType, "13");
//                     init_file(&g_tFileVoltage1Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
    
//             case 14:g_tDeviceStruct.voltage2Status == channelStatus[i];
//                 if(g_tDeviceStruct.voltage2Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.voltage2Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.voltage2Status = 0x01;
//                     strcpy(g_tFileVoltage2Struct.fileType, "14");
//                     init_file(&g_tFileVoltage2Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
    
//             case 15:g_tDeviceStruct.voltage3Status == channelStatus[i];
//                 if(g_tDeviceStruct.voltage3Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.voltage3Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.voltage3Status = 0x01;
//                     strcpy(g_tFileVoltage3Struct.fileType, "15");
//                     init_file(&g_tFileVoltage3Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
                
//             case 16:g_tDeviceStruct.voltage4Status == channelStatus[i];
//                 if(g_tDeviceStruct.voltage4Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.voltage4Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.voltage4Status = 0x01;
//                     strcpy(g_tFileVoltage4Struct.fileType, "16");
//                     init_file(&g_tFileVoltage4Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
                
//             case 17:g_tDeviceStruct.voltage5Status == channelStatus[i];
//                 if(g_tDeviceStruct.voltage5Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.voltage5Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.voltage5Status = 0x01;
//                     strcpy(g_tFileVoltage5Struct.fileType, "17");
//                     init_file(&g_tFileVoltage5Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;		
            
//             case 18:g_tDeviceStruct.voltage6Status == channelStatus[i];
//                 if(g_tDeviceStruct.voltage6Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.voltage6Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.voltage6Status = 0x01;
//                     strcpy(g_tFileVoltage6Struct.fileType, "18");
//                     init_file(&g_tFileVoltage6Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
    
//             case 19:g_tDeviceStruct.voltage7Status == channelStatus[i];
//                 if(g_tDeviceStruct.voltage7Status == 0x00)
//                 {
// #ifdef FUN_DBG_PRINT
//                     printf("g_tDeviceStruct.voltage7Status == 0x01\n");
// #endif
//                     g_tDeviceStruct.voltage7Status = 0x01;
//                     strcpy(g_tFileVoltage7Struct.fileType, "19");
//                     init_file(&g_tFileVoltage7Struct, 0, 0, fileFlushByteCount, fileCloseByteCount, SAVE_MODE, ptime, ptime1);
//                 }
//                 else
//                 {
//                     goto finish;
//                 }
//                 break;
                
//             default:
    
//                 break;
//         }
        
        
//     }
	

// finish:	
//     response[4] = 0xFF; // Success
// 	response[5] = 0x00;
// 	responseLength = 6;
	
//     send(g_clientSocket, response, responseLength, 0);
// }


void cis_end(void)
{
	unsigned char response[BUFFER_SIZE];
	unsigned int responseLength = 0;
	unsigned int function = 0;
	response[0] = 0x55;
	response[1] = 0x00;
	response[2] = g_tcpClientRcvBuffer[0];
	response[3] = g_tcpClientRcvBuffer[1];
	
	function = g_tcpClientRcvBuffer[2] + (g_tcpClientRcvBuffer[3] << 8);
	
	switch (function) 
	{
		case 0:
			if(g_tDeviceStruct.iepe0Status == 0x01)
			{
				g_tDeviceStruct.iepe0Status = 0x00;
				fclose(g_tFileIepe0Struct.fp);
				system("sync");
			}
			break;
			
		case 1:
			if(g_tDeviceStruct.iepe1Status == 0x01)
			{
				g_tDeviceStruct.iepe1Status = 0x00;
				fclose(g_tFileIepe1Struct.fp);
				system("sync");
			}
			break;
			
		case 2:
			if(g_tDeviceStruct.iepe2Status == 0x01)
			{
				g_tDeviceStruct.iepe2Status = 0x00;
				fclose(g_tFileIepe2Struct.fp);
				system("sync");
			}
			break;		
		
		case 3:
			if(g_tDeviceStruct.iepe3Status == 0x01)
			{
				g_tDeviceStruct.iepe3Status = 0x00;
				fclose(g_tFileIepe3Struct.fp);
				system("sync");
			}
			break;

		case 4:
			if(g_tDeviceStruct.current0Status == 0x01)
			{
				g_tDeviceStruct.current0Status = 0x00;
				fclose(g_tFileCurrent0Struct.fp);
				system("sync");
			}
			break;

		case 5:
			if(g_tDeviceStruct.current1Status == 0x01)
			{
				g_tDeviceStruct.current1Status = 0x00;
				fclose(g_tFileCurrent1Struct.fp);
				system("sync");
			}
			break;
			
		case 6:
			if(g_tDeviceStruct.current2Status == 0x01)
			{
				g_tDeviceStruct.current2Status = 0x00;
				fclose(g_tFileCurrent2Struct.fp);
				system("sync");
			}
			break;
			
		case 7:
			if(g_tDeviceStruct.current3Status == 0x01)
			{
				g_tDeviceStruct.current3Status = 0x00;
				fclose(g_tFileCurrent3Struct.fp);
				system("sync");
			}
			break;		
		
		case 8:
			if(g_tDeviceStruct.current4Status == 0x01)
			{
				g_tDeviceStruct.current4Status = 0x00;
				fclose(g_tFileCurrent4Struct.fp);
				system("sync");
			}
			break;

		case 9:
			if(g_tDeviceStruct.current5Status == 0x01)
			{
				g_tDeviceStruct.current5Status = 0x00;
				fclose(g_tFileCurrent5Struct.fp);
				system("sync");
			}
			break;

		case 10:
			if(g_tDeviceStruct.current6Status == 0x01)
			{
				g_tDeviceStruct.current6Status = 0x00;
				fclose(g_tFileCurrent6Struct.fp);
				system("sync");
			}
			break;
			
		case 11:
			if(g_tDeviceStruct.current7Status == 0x01)
			{
				g_tDeviceStruct.current7Status = 0x00;
				fclose(g_tFileCurrent7Struct.fp);
				system("sync");
			}
			break;
			
		case 12:
			if(g_tDeviceStruct.voltage0Status == 0x01)
			{
				g_tDeviceStruct.voltage0Status = 0x00;
				fclose(g_tFileVoltage0Struct.fp);
				system("sync");
			}
			break;		
		
		case 13:
			if(g_tDeviceStruct.voltage1Status == 0x01)
			{
				g_tDeviceStruct.voltage1Status = 0x00;
				fclose(g_tFileVoltage1Struct.fp);
				system("sync");
			}
			break;

		case 14:
			if(g_tDeviceStruct.voltage2Status == 0x01)
			{
				g_tDeviceStruct.voltage2Status = 0x00;
				fclose(g_tFileVoltage2Struct.fp);
				system("sync");
			}
			break;

		case 15:
			if(g_tDeviceStruct.voltage3Status == 0x01)
			{
				g_tDeviceStruct.voltage3Status = 0x00;
				fclose(g_tFileVoltage3Struct.fp);
				system("sync");
			}
			break;
			
		case 16:
			if(g_tDeviceStruct.voltage4Status == 0x01)
			{
				g_tDeviceStruct.voltage4Status = 0x00;
				fclose(g_tFileVoltage4Struct.fp);
				system("sync");
			}
			break;
			
		case 17:
			if(g_tDeviceStruct.voltage5Status == 0x01)
			{
				g_tDeviceStruct.voltage5Status = 0x00;
				fclose(g_tFileVoltage5Struct.fp);
				system("sync");
			}
			break;		
		
		case 18:
			if(g_tDeviceStruct.voltage6Status == 0x01)
			{
				g_tDeviceStruct.voltage6Status = 0x00;
				fclose(g_tFileVoltage6Struct.fp);
				system("sync");
			}
			break;

		case 19:
			if(g_tDeviceStruct.voltage7Status == 0x01)
			{
				g_tDeviceStruct.voltage7Status = 0x00;
				fclose(g_tFileVoltage7Struct.fp);
				system("sync");
			}
			break;
			
		default:

            break;
	}
	
	response[4] = 0xFF; // Success
	response[5] = 0x00;
	responseLength = 6;
	
    send(g_clientSocket, response, responseLength, 0);
}


void deal_fun(void)
{

	deal_ad7768();
	deal_ad7915();
	deal_clock();
}



void deal_clock(void)
{
	unsigned int dataL, dataH;
	U64_n currentClkData;
		
	get_time(&dataL, &dataH);
	currentClkData.u32Data[0] = dataL;
	currentClkData.u32Data[1] = dataH;

	// // ==============================================================
	// // 【调试新增】：抽样打印硬件时钟的值
	// // 使用 static 变量，每进这个函数 10000 次才打印一次，防止串口刷屏卡死
	// static unsigned int clock_debug_cnt = 0;
	// if (clock_debug_cnt++ % 50000 == 0)
	// {
	//     // 打印高32位，低32位，以及组合后的64位完整值
	// 	printf("[Debug] Clock: H=0x%08X, L=0x%08X, u64=%llu\n", 
	//            dataH,  dataL, currentClkData.u64Data);
	// }
	// // ==============================================================


	
	if((currentClkData.u64Data < g_clkData.u64Data))
	{
		g_clkData.u64Data = 0;
	}
	else if(currentClkData.u64Data >= g_clkData.u64Data + 10000)
	{
		g_ledData = g_ledData | 0x01;
		g_clkData.u64Data = g_clkData.u64Data + 10000;
		
		if(g_tcpStart == 1)
		{
			deal_temperature();
		}
		led_out(g_ledData);
		renew_file_time();
		//printf("fun_cpu0\n");
	}
}

void deal_temperature(void)
{
	int psTemp;
	FILE *fp; 
	unsigned char psTempBuffer[10];
	static unsigned int s_count = 0;
	
	fp = fopen(PS_TEMP_FILE, "r");
	if(fp == NULL)
	{
		printf("open %s failed!\n", PS_TEMP_FILE);
	}
	fscanf(fp,"%d", &psTemp); 
	
	//psTempByte = temp_change_byte(psTemp);
	psTempBuffer[0] = (psTemp >> 8) & 0XFF;
	psTempBuffer[1] = psTemp;
	
	fclose(fp);
	s_count++;
	write_pro_data_file_u32(&g_tFilePsTempStruct, &s_count, 1, g_saveFileEn);
	write_pro_data_file_u8(&g_tFilePsTempStruct, psTempBuffer, 2, g_saveFileEn);
	//printf("ps temp OK!\n");
}

void renew_file_time(void)
{
	char fileMakeTime[64];
	
	struct tm t;
	time_t sec;
	
	sec = time(NULL);
	
	if(-1 == sec)
	{
		perror("time error");
		exit(-1);
	}
	localtime_r(&sec, &t);
	
	sprintf(fileMakeTime, "%04hu_%02hhu_%02hhu_%02hhu_%02hhu_%02hhu", 
						t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
						t.tm_hour, t.tm_min, t.tm_sec);
	
	if(g_tDeviceStruct.iepe0Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileIepe0Struct.fileNameLock);
		strcpy(g_tFileIepe0Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileIepe0Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.iepe1Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileIepe1Struct.fileNameLock);
		strcpy(g_tFileIepe1Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileIepe1Struct.fileNameLock);	
	}	
	
	if(g_tDeviceStruct.iepe2Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileIepe2Struct.fileNameLock);
		strcpy(g_tFileIepe2Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileIepe2Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.iepe3Status == 0x01)
	{	
		pthread_mutex_lock(&g_tFileIepe3Struct.fileNameLock);
		strcpy(g_tFileIepe3Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileIepe3Struct.fileNameLock);	
	}
		
	if(g_tDeviceStruct.current0Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileCurrent0Struct.fileNameLock);
		strcpy(g_tFileCurrent0Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileCurrent0Struct.fileNameLock);	
	}
		
	if(g_tDeviceStruct.current1Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileCurrent1Struct.fileNameLock);
		strcpy(g_tFileCurrent1Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileCurrent1Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.current2Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileCurrent2Struct.fileNameLock);
		strcpy(g_tFileCurrent2Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileCurrent2Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.current3Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileCurrent3Struct.fileNameLock);
		strcpy(g_tFileCurrent3Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileCurrent3Struct.fileNameLock);	
	}
		
	if(g_tDeviceStruct.current4Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileCurrent4Struct.fileNameLock);
		strcpy(g_tFileCurrent4Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileCurrent4Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.current5Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileCurrent5Struct.fileNameLock);
		strcpy(g_tFileCurrent5Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileCurrent5Struct.fileNameLock);	
	}
		
	if(g_tDeviceStruct.current6Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileCurrent6Struct.fileNameLock);
		strcpy(g_tFileCurrent6Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileCurrent6Struct.fileNameLock);	
	}
		
	if(g_tDeviceStruct.current7Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileCurrent7Struct.fileNameLock);
		strcpy(g_tFileCurrent7Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileCurrent7Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.voltage0Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileVoltage0Struct.fileNameLock);
		strcpy(g_tFileVoltage0Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileVoltage0Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.voltage1Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileVoltage1Struct.fileNameLock);
		strcpy(g_tFileVoltage1Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileVoltage1Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.voltage2Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileVoltage2Struct.fileNameLock);
		strcpy(g_tFileVoltage2Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileVoltage2Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.voltage3Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileVoltage3Struct.fileNameLock);
		strcpy(g_tFileVoltage3Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileVoltage3Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.voltage4Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileVoltage4Struct.fileNameLock);
		strcpy(g_tFileVoltage4Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileVoltage4Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.voltage5Status == 0x01)
	{	
		pthread_mutex_lock(&g_tFileVoltage5Struct.fileNameLock);
		strcpy(g_tFileVoltage5Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileVoltage5Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.voltage6Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileVoltage6Struct.fileNameLock);
		strcpy(g_tFileVoltage6Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileVoltage6Struct.fileNameLock);	
	}
	
	if(g_tDeviceStruct.voltage7Status == 0x01)
	{
		pthread_mutex_lock(&g_tFileVoltage7Struct.fileNameLock);
		strcpy(g_tFileVoltage7Struct.fileNewMakeTime, fileMakeTime);
		pthread_mutex_unlock(&g_tFileVoltage7Struct.fileNameLock);	
	}
	
}

void fun_ad7768_init(void)
{
	unsigned int i;

	AD7768RegIn1_t tAD7768RegIn1;
	tAD7768RegIn1.resetn = 1;
	tAD7768RegIn1.resetnSys = 1;
	
	for(i = 0; i <= AD7768_NUM - 1; i++)
	{
		ad7768_set_mclk(i, 4);
	}

	for(i = 0; i <= AD7768_NUM - 1; i++)
	{
		ad7768_pin_resetn(i, &tAD7768RegIn1);
	}

	for(i = 0; i <= AD7768_NUM - 1; i++)
	{
		ad7768_sys_resetn(i, &tAD7768RegIn1);
	}
}


void fun_ad7915_init(void)
{
	unsigned int i;
	unsigned int ad7915Ch = 0;
	unsigned int ad7915Buffer[1024] = {0};
	unsigned int data = 0;

	AD7915RegIn1_t tAD7915RegIn1;
	AD7915FiFoStat_t tAD7915FiFoStat;

	AD7915RegIn3_n nAD7915RegIn3;
	nAD7915RegIn3.myBit.convstTime = 0x80;
	nAD7915RegIn3.myBit.enTime = 0x09;
	nAD7915RegIn3.myBit.sclkTime = 0x09;
	
	for(i = 0; i <= AD7915_NUM - 1; i++)
	{
		ad7915_set_time(i, nAD7915RegIn3.u32);
	}

	tAD7915RegIn1.period = 12500;
	tAD7915RegIn1.resetnSys = 0;
	for(i = 0; i <= AD7915_NUM - 1; i++)
	{
		init_ad7915(i, &tAD7915RegIn1);
	}
	
	tAD7915RegIn1.resetnSys = 1;
	for(i = 0; i <= AD7915_NUM - 1; i++)
	{
		init_ad7915(i, &tAD7915RegIn1);
	}

	//ad7915Ch = 1; //vol: 0, cur: 1
}

void fun_time_init(void)
{
	unsigned int lData, hData;
	TimeReg_n nTimeReg;
	
	init_time(&nTimeReg.myBit);
	time_div(&nTimeReg.myBit, 10000);
	time_reset(&nTimeReg.myBit, 1);
	time_reset(&nTimeReg.myBit, 0);
}


void deal_ad7768(void)
{
	unsigned int rDataCount = 0;
	
	unsigned int ad7768Ch = 0;
	unsigned int ad7768Buffer[1024] = {0};
	unsigned int subCh = 0;
	AD7768FiFoStat_t tAD7768FiFoStat;
	unsigned int adFifoFlag = 0;
	
	for(ad7768Ch = 0; ad7768Ch <= AD7768_NUM - 1; ad7768Ch++)
	{
		for(subCh = 0; subCh <= 4 - 1; subCh++)
		{
			read_ad7768_fifo_stat(ad7768Ch, subCh, &tAD7768FiFoStat);
			if((rDataCount = tAD7768FiFoStat.fifoCount) > 0)
			{
				adFifoFlag = 1;
			}
			else if(tAD7768FiFoStat.full == 1)
			{
				adFifoFlag = 1;
				rDataCount = 1024;
			}

			if(adFifoFlag == 1)
			{
				adFifoFlag = 0;
				ad7768_rfifo_buffer(ad7768Ch, subCh, ad7768Buffer, rDataCount);				
				
				if(ad7768Ch == 0)
				{
					if(subCh == 0)
					{
						if(g_tDeviceStruct.iepe0Status == 0x01)
						{
#ifdef AD7768_DBG_PRINT
							printf("subCh = 0\n");
#endif							
							write_pro_data_file_u32(&g_tFileIepe0Struct, ad7768Buffer, rDataCount, g_saveFileEn);
						}							
					}
					
					if(subCh == 1)
					{
						if(g_tDeviceStruct.iepe1Status == 0x01)
						{
#ifdef AD7768_DBG_PRINT
							printf("subCh = 1\n");
#endif
							write_pro_data_file_u32(&g_tFileIepe1Struct, ad7768Buffer, rDataCount, g_saveFileEn);	
						}
					}
					
					if(subCh == 2)
					{
						if(g_tDeviceStruct.iepe2Status == 0x01)
						{
#ifdef AD7768_DBG_PRINT
							printf("subCh = 2\n");
#endif
							write_pro_data_file_u32(&g_tFileIepe2Struct, ad7768Buffer, rDataCount, g_saveFileEn);
						}
					}
					
					if(subCh == 3)
					{
						if(g_tDeviceStruct.iepe3Status == 0x01)
						{
#ifdef AD7768_DBG_PRINT
							printf("subCh = 3\n");
#endif
							write_pro_data_file_u32(&g_tFileIepe3Struct, ad7768Buffer, rDataCount, g_saveFileEn);
						}
					}
				}
			}
		}
	}
}



/* ===================== 算法2/3共享1x1024高频突变采样器 =====================
 * 消费者包括：算法2、算法3、3001 TCP上传。
 * 触发和换算沿用原高频分支：原码相对基线差超过140后进入连续采集，
 * 每个AD样本生成1个float，满1024点形成一包。
 */
#define ALGO_23_SAMPLE_COUNT          HF_TCP_UPLOAD_SAMPLE_COUNT
#define ALGO_23_TRIGGER_THRESHOLD_RAW 140

enum {
    ALGO_23_STATE_IDLE = 0,
    ALGO_23_STATE_WAIT_TRIGGER,
    ALGO_23_STATE_COLLECTING
};

typedef struct {
    int active;
    int state;
    unsigned int sample_index;
    int baseline;
    int baseline_init;
    int raw_values[ALGO_23_SAMPLE_COUNT];
    float value_values[ALGO_23_SAMPLE_COUNT];
} Algo23SamplerState;

static Algo23SamplerState g_algo_23_sampler = {0};

static AlgoConfig *algo_23_find_algo(int algo_id)
{
    for (int i = 0; i < ALGO_COUNT; i++) {
        if (g_algos[i].id == algo_id) {
            return &g_algos[i];
        }
    }

    return NULL;
}

static int algo_23_has_running_algo(int algo_id)
{
    AlgoConfig *algo = algo_23_find_algo(algo_id);
    return algo != NULL && algo->running;
}

static int algo_23_has_consumer(void)
{
    return hf_tcp_upload_is_enabled() ||
           algo_23_has_running_algo(2) ||
           algo_23_has_running_algo(3);
}

static int algo_23_target_channel(void)
{
    AlgoConfig *algo2 = algo_23_find_algo(2);

    if (algo2 != NULL && algo2->channel >= 0 && algo2->channel < MAX_CH) {
        return algo2->channel;
    }

    return HF_TCP_UPLOAD_LOGICAL_CH;
}

static uint8_t algo_23_consumer_mask(void)
{
    uint8_t mask = 0;

    if (algo_23_has_running_algo(2)) {
        mask |= HF_TCP_CONSUMER_ALGO2;
    }
    if (algo_23_has_running_algo(3)) {
        mask |= HF_TCP_CONSUMER_ALGO3;
    }

    return mask;
}

static void algo_23_reset_sampler(int active)
{
    g_algo_23_sampler.active = active;
    g_algo_23_sampler.state =
        active ? ALGO_23_STATE_WAIT_TRIGGER : ALGO_23_STATE_IDLE;
    g_algo_23_sampler.sample_index = 0;
    g_algo_23_sampler.baseline = 0;
    g_algo_23_sampler.baseline_init = 0;
    memset(g_algo_23_sampler.raw_values, 0,
           sizeof(g_algo_23_sampler.raw_values));
    memset(g_algo_23_sampler.value_values, 0,
           sizeof(g_algo_23_sampler.value_values));
}

static void algo_23_publish_to_algo(AlgoConfig *algo)
{
    int write_ch;

    if (algo == NULL || !algo->running) {
        return;
    }

    write_ch = algo->channel;
    if (write_ch < 0 || write_ch >= MAX_CH) {
        printf("[Algo2/3] %s 的目标通道%d越界。\n",
               algo->name, write_ch);
        return;
    }

    pthread_mutex_lock(&data_mutex);
    memset(new_data[write_ch], 0, sizeof(new_data[write_ch]));
    memset(algo_data[write_ch], 0, sizeof(algo_data[write_ch]));
    for (unsigned int i = 0; i < ALGO_23_SAMPLE_COUNT; i++) {
        new_data[write_ch][i] = g_algo_23_sampler.raw_values[i];
        algo_data[write_ch][i] = g_algo_23_sampler.value_values[i];
    }
    pthread_mutex_unlock(&data_mutex);

    if (write_algo_data_for_algo(algo) == 0) {
        printf("[Algo2/3] 1x1024高频共享包采集完成,触发 %s。\n",
               algo->name);
    } else {
        fprintf(stderr, "[Algo2/3] %s 数据写入或触发失败。\n",
                algo->name);
    }
}

static void algo_23_collect_sample(int target_ch, int raw_value)
{
    int write_ch = algo_23_target_channel();
    int ac_diff;
    float physical_value;
    uint8_t consumer_mask;

    if (!algo_23_has_consumer()) {
        if (g_algo_23_sampler.active) {
            algo_23_reset_sampler(0);
        }
        return;
    }

    if (target_ch != write_ch) {
        return;
    }

    if (!g_algo_23_sampler.active) {
        algo_23_reset_sampler(1);
        printf("[Algo2/3] 共享高频采样器开启: 等待通道%d原码突变超过%d，消费者=算法2/3/3001。\n",
               write_ch, ALGO_23_TRIGGER_THRESHOLD_RAW);
    }

    physical_value = algo_convert_fast_raw_for_id(2, target_ch, raw_value);

    if (g_algo_23_sampler.state == ALGO_23_STATE_WAIT_TRIGGER) {
        if (!g_algo_23_sampler.baseline_init) {
            g_algo_23_sampler.baseline = raw_value;
            g_algo_23_sampler.baseline_init = 1;
            return;
        }

        g_algo_23_sampler.baseline =
            (g_algo_23_sampler.baseline * 1023 + raw_value) / 1024;
        ac_diff = raw_value - g_algo_23_sampler.baseline;

        if (ac_diff <= ALGO_23_TRIGGER_THRESHOLD_RAW &&
            ac_diff >= -ALGO_23_TRIGGER_THRESHOLD_RAW) {
            return;
        }

        g_algo_23_sampler.state = ALGO_23_STATE_COLLECTING;
        g_algo_23_sampler.sample_index = 0;
        printf("[Algo2/3] 检测到高频突变: raw=%d, baseline=%d, diff=%d，开始1024点连续采集。\n",
               raw_value, g_algo_23_sampler.baseline, ac_diff);
    }

    if (g_algo_23_sampler.state != ALGO_23_STATE_COLLECTING) {
        return;
    }

    g_algo_23_sampler.raw_values[g_algo_23_sampler.sample_index] = raw_value;
    g_algo_23_sampler.value_values[g_algo_23_sampler.sample_index] =
        physical_value;
    g_algo_23_sampler.sample_index++;

    if (g_algo_23_sampler.sample_index < ALGO_23_SAMPLE_COUNT) {
        return;
    }

    consumer_mask = algo_23_consumer_mask();

    /* 先把完整包提交给TCP发送线程；这里只复制4KB，不执行send。 */
    hf_tcp_upload_submit_packet(consumer_mask, write_ch,
                                g_algo_23_sampler.value_values);

    /* 同一包数据分发给算法2和算法3，二者同时running时拿到完全相同的数据。 */
    algo_23_publish_to_algo(algo_23_find_algo(2));
    algo_23_publish_to_algo(algo_23_find_algo(3));

    g_algo_23_sampler.sample_index = 0;
    g_algo_23_sampler.state = ALGO_23_STATE_COLLECTING;
}

/* ===================== 算法10/11共享1x12自适应触发采集 =====================
 * 算法10、算法11和3007 TCP上传共用同一个漏油采样器。
 * 先学习1秒空载噪声，变化持续100ms后进入采集；
 * 每200ms采一个实际电压float，12点约2.4秒形成一包。
 */
#define ALGO_10_11_SAMPLE_COUNT       12U
#define ALGO_10_11_INTERVAL_NS        200000000L
#define ALGO_10_11_BASELINE_NS        1000000000LL
#define ALGO_10_11_CONFIRM_NS         100000000LL
#define ALGO_10_11_MIN_BASELINE_SAMPLES 100U
#define ALGO_10_11_MIN_THRESHOLD_V    0.01f
#define ALGO_10_11_NOISE_MULTIPLIER   3.0f
#define ALGO_10_11_FILTER_DIVISOR     20.0f

enum {
    ALGO_10_11_STATE_IDLE = 0,
    ALGO_10_11_STATE_BASELINE,
    ALGO_10_11_STATE_ARMED,
    ALGO_10_11_STATE_CONFIRM,
    ALGO_10_11_STATE_COLLECTING
};

typedef struct {
    int active_algo_id;
    int state;
    unsigned int sample_index;
    int raw_values[ALGO_10_11_SAMPLE_COUNT];
    float voltage_values[ALGO_10_11_SAMPLE_COUNT];
    unsigned int baseline_count;
    double baseline_mean;
    double baseline_m2;
    float baseline_voltage;
    float noise_std_voltage;
    float trigger_threshold_voltage;
    float filtered_voltage;
    int filter_initialized;
    struct timespec baseline_start_time;
    struct timespec confirm_start_time;
    struct timespec next_sample_time;
} Algo1011SamplerState;

static Algo1011SamplerState g_algo_10_11_sampler = {0};

static int algo_10_11_time_reached(const struct timespec *now,
                                   const struct timespec *deadline)
{
    if (now->tv_sec != deadline->tv_sec) {
        return now->tv_sec > deadline->tv_sec;
    }
    return now->tv_nsec >= deadline->tv_nsec;
}

static long long algo_10_11_elapsed_ns(const struct timespec *now,
                                       const struct timespec *start)
{
    return (long long)(now->tv_sec - start->tv_sec) * 1000000000LL +
           (long long)(now->tv_nsec - start->tv_nsec);
}

static float algo_10_11_absf(float value)
{
    return value < 0.0f ? -value : value;
}

/* 避免为一个平方根额外引入libm链接依赖。 */
static float algo_10_11_sqrtf(float value)
{
    float estimate;

    if (value <= 0.0f) {
        return 0.0f;
    }

    estimate = value > 1.0f ? value : 1.0f;
    for (int i = 0; i < 12; i++) {
        estimate = 0.5f * (estimate + value / estimate);
    }
    return estimate;
}

static void algo_10_11_add_interval(struct timespec *value)
{
    value->tv_nsec += ALGO_10_11_INTERVAL_NS;
    if (value->tv_nsec >= 1000000000L) {
        value->tv_sec += value->tv_nsec / 1000000000L;
        value->tv_nsec %= 1000000000L;
    }
}

static AlgoConfig *algo_10_11_find_algo(int algo_id)
{
    for (int i = 0; i < ALGO_COUNT; i++) {
        if (g_algos[i].id == algo_id) {
            return &g_algos[i];
        }
    }

    return NULL;
}

static int algo_10_11_has_running_algo(int algo_id)
{
    AlgoConfig *algo = algo_10_11_find_algo(algo_id);
    return algo != NULL && algo->running;
}

static int algo_10_11_has_consumer(void)
{
    return leak_tcp_upload_is_enabled() ||
           algo_10_11_has_running_algo(10) ||
           algo_10_11_has_running_algo(11);
}

static int algo_10_11_target_channel(void)
{
    AlgoConfig *algo10 = algo_10_11_find_algo(10);

    if (algo10 != NULL && algo10->channel >= 0 &&
        algo10->channel < MAX_CH) {
        return algo10->channel;
    }

    return LEAK_TCP_UPLOAD_LOGICAL_CH;
}

static uint8_t algo_10_11_consumer_mask(void)
{
    uint8_t mask = 0;

    if (algo_10_11_has_running_algo(10)) {
        mask |= LEAK_TCP_CONSUMER_ALGO10;
    }
    if (algo_10_11_has_running_algo(11)) {
        mask |= LEAK_TCP_CONSUMER_ALGO11;
    }

    return mask;
}

static void algo_10_11_reset_sampler(int active,
                                     const struct timespec *now)
{
    g_algo_10_11_sampler.active_algo_id = active ? 1 : 0;
    g_algo_10_11_sampler.state =
        active ? ALGO_10_11_STATE_BASELINE : ALGO_10_11_STATE_IDLE;
    g_algo_10_11_sampler.sample_index = 0;
    g_algo_10_11_sampler.baseline_count = 0;
    g_algo_10_11_sampler.baseline_mean = 0.0;
    g_algo_10_11_sampler.baseline_m2 = 0.0;
    g_algo_10_11_sampler.baseline_voltage = 0.0f;
    g_algo_10_11_sampler.noise_std_voltage = 0.0f;
    g_algo_10_11_sampler.trigger_threshold_voltage =
        ALGO_10_11_MIN_THRESHOLD_V;
    g_algo_10_11_sampler.filtered_voltage = 0.0f;
    g_algo_10_11_sampler.filter_initialized = 0;
    memset(g_algo_10_11_sampler.raw_values, 0,
           sizeof(g_algo_10_11_sampler.raw_values));
    memset(g_algo_10_11_sampler.voltage_values, 0,
           sizeof(g_algo_10_11_sampler.voltage_values));

    if (active && now != NULL) {
        g_algo_10_11_sampler.baseline_start_time = *now;
        memset(&g_algo_10_11_sampler.confirm_start_time, 0,
               sizeof(g_algo_10_11_sampler.confirm_start_time));
        memset(&g_algo_10_11_sampler.next_sample_time, 0,
               sizeof(g_algo_10_11_sampler.next_sample_time));
    } else {
        memset(&g_algo_10_11_sampler.baseline_start_time, 0,
               sizeof(g_algo_10_11_sampler.baseline_start_time));
        memset(&g_algo_10_11_sampler.confirm_start_time, 0,
               sizeof(g_algo_10_11_sampler.confirm_start_time));
        memset(&g_algo_10_11_sampler.next_sample_time, 0,
               sizeof(g_algo_10_11_sampler.next_sample_time));
    }
}

static void algo_10_11_publish_to_algo(AlgoConfig *algo)
{
    int write_ch;

    if (algo == NULL || !algo->running) {
        return;
    }

    write_ch = algo->channel;
    if (write_ch < 0 || write_ch >= MAX_CH) {
        printf("[Algo10/11] %s 的目标通道%d越界。\n",
               algo->name, write_ch);
        return;
    }

    pthread_mutex_lock(&data_mutex);
    memset(new_data[write_ch], 0, sizeof(new_data[write_ch]));
    memset(algo_data[write_ch], 0, sizeof(algo_data[write_ch]));
    for (unsigned int i = 0; i < ALGO_10_11_SAMPLE_COUNT; i++) {
        new_data[write_ch][i] = g_algo_10_11_sampler.raw_values[i];
        algo_data[write_ch][i] =
            g_algo_10_11_sampler.voltage_values[i];
    }
    pthread_mutex_unlock(&data_mutex);

    if (write_algo_data_for_algo(algo) == 0) {
        printf("[Algo10/11] 1x12实际电压共享包完成,触发 %s。\n",
               algo->name);
    } else {
        fprintf(stderr, "[Algo10/11] %s 数据写入或触发失败。\n",
                algo->name);
    }
}

static void algo_10_11_collect_sample(int target_ch, int raw_value)
{
    struct timespec now;
    unsigned int index;
    int write_ch = algo_10_11_target_channel();
    float actual_voltage;
    float difference;
    uint8_t consumer_mask;

    if (!algo_10_11_has_consumer()) {
        if (g_algo_10_11_sampler.active_algo_id != 0) {
            algo_10_11_reset_sampler(0, NULL);
        }
        return;
    }

    if (target_ch != write_ch) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);

    if (g_algo_10_11_sampler.active_algo_id == 0) {
        algo_10_11_reset_sampler(1, &now);
        printf("[Algo10/11] 共享采样器开启: 开始学习通道%d空载噪声,持续1秒,消费者=算法10/11/3007。\n",
               write_ch);
    }

    actual_voltage = (float)raw_value * 0.000375f;

    if (!g_algo_10_11_sampler.filter_initialized) {
        g_algo_10_11_sampler.filtered_voltage = actual_voltage;
        g_algo_10_11_sampler.filter_initialized = 1;
    } else {
        g_algo_10_11_sampler.filtered_voltage +=
            (actual_voltage - g_algo_10_11_sampler.filtered_voltage) /
            ALGO_10_11_FILTER_DIVISOR;
    }

    if (g_algo_10_11_sampler.state == ALGO_10_11_STATE_BASELINE) {
        double delta;
        double variance;
        float adaptive_threshold;

        g_algo_10_11_sampler.baseline_count++;
        delta = (double)actual_voltage -
                g_algo_10_11_sampler.baseline_mean;
        g_algo_10_11_sampler.baseline_mean +=
            delta / (double)g_algo_10_11_sampler.baseline_count;
        g_algo_10_11_sampler.baseline_m2 +=
            delta * ((double)actual_voltage -
                     g_algo_10_11_sampler.baseline_mean);

        if (algo_10_11_elapsed_ns(
                &now, &g_algo_10_11_sampler.baseline_start_time) <
                ALGO_10_11_BASELINE_NS ||
            g_algo_10_11_sampler.baseline_count <
                ALGO_10_11_MIN_BASELINE_SAMPLES) {
            return;
        }

        variance = g_algo_10_11_sampler.baseline_m2 /
                   (double)(g_algo_10_11_sampler.baseline_count - 1U);
        if (variance < 0.0) {
            variance = 0.0;
        }

        g_algo_10_11_sampler.baseline_voltage =
            (float)g_algo_10_11_sampler.baseline_mean;
        g_algo_10_11_sampler.noise_std_voltage =
            algo_10_11_sqrtf((float)variance);
        adaptive_threshold =
            ALGO_10_11_NOISE_MULTIPLIER *
            g_algo_10_11_sampler.noise_std_voltage;
        g_algo_10_11_sampler.trigger_threshold_voltage =
            adaptive_threshold > ALGO_10_11_MIN_THRESHOLD_V ?
                adaptive_threshold : ALGO_10_11_MIN_THRESHOLD_V;
        g_algo_10_11_sampler.state = ALGO_10_11_STATE_ARMED;

        printf("[Algo10/11] 基线完成: mean=%.6fV, noise_std=%.6fV, threshold=%.6fV。\n",
               g_algo_10_11_sampler.baseline_voltage,
               g_algo_10_11_sampler.noise_std_voltage,
               g_algo_10_11_sampler.trigger_threshold_voltage);
        return;
    }

    difference = algo_10_11_absf(
        g_algo_10_11_sampler.filtered_voltage -
        g_algo_10_11_sampler.baseline_voltage);

    if (g_algo_10_11_sampler.state == ALGO_10_11_STATE_ARMED) {
        if (difference >=
            g_algo_10_11_sampler.trigger_threshold_voltage) {
            g_algo_10_11_sampler.confirm_start_time = now;
            g_algo_10_11_sampler.state = ALGO_10_11_STATE_CONFIRM;
            printf("[Algo10/11] 检测到候选信号,开始100ms连续确认。\n");
        }
        return;
    }

    if (g_algo_10_11_sampler.state == ALGO_10_11_STATE_CONFIRM) {
        if (difference <
            g_algo_10_11_sampler.trigger_threshold_voltage) {
            g_algo_10_11_sampler.state = ALGO_10_11_STATE_ARMED;
            return;
        }

        if (algo_10_11_elapsed_ns(
                &now, &g_algo_10_11_sampler.confirm_start_time) <
                ALGO_10_11_CONFIRM_NS) {
            return;
        }

        g_algo_10_11_sampler.state = ALGO_10_11_STATE_COLLECTING;
        g_algo_10_11_sampler.sample_index = 0;
        g_algo_10_11_sampler.next_sample_time = now;
        algo_10_11_add_interval(
            &g_algo_10_11_sampler.next_sample_time);
        printf("[Algo10/11] 有效信号确认完成,开始每200ms采集1点。\n");
        return;
    }

    if (g_algo_10_11_sampler.state != ALGO_10_11_STATE_COLLECTING ||
        !algo_10_11_time_reached(
            &now, &g_algo_10_11_sampler.next_sample_time)) {
        return;
    }

    /* 延迟时只取当前最新值，并将下一个采样时刻推进到未来。 */
    do {
        algo_10_11_add_interval(
            &g_algo_10_11_sampler.next_sample_time);
    } while (algo_10_11_time_reached(
                 &now, &g_algo_10_11_sampler.next_sample_time));

    index = g_algo_10_11_sampler.sample_index;
    g_algo_10_11_sampler.raw_values[index] = raw_value;
    g_algo_10_11_sampler.voltage_values[index] = actual_voltage;
    g_algo_10_11_sampler.sample_index++;

    if (g_algo_10_11_sampler.sample_index < ALGO_10_11_SAMPLE_COUNT) {
        return;
    }

    consumer_mask = algo_10_11_consumer_mask();

    /* 先把完整包提交给TCP发送线程；这里只复制48字节，不执行send。 */
    leak_tcp_upload_submit(consumer_mask, write_ch,
                           g_algo_10_11_sampler.voltage_values);

    /* 同一包数据分发给算法10和算法11，二者同时running时拿到完全相同的数据。 */
    algo_10_11_publish_to_algo(algo_10_11_find_algo(10));
    algo_10_11_publish_to_algo(algo_10_11_find_algo(11));

    g_algo_10_11_sampler.sample_index = 0;
}

/* ===================== 算法6/7：5点平均包同步采样 =====================
 * 模拟器建议按“低电平空档 + >3V包头 + 512个5ms有效点 + 低电平空档”输出。
 * 设备端先确认低于0.5V，再等待第一个大于3V的点；从该点开始，
 * 每5个AD采样原码先求平均，再按算法6/7公式转换成一个float。
 * 采满512个float后触发当前监听的算法6或7，随后必须重新检测低电平。
 */
#define ALGO_67_SAMPLE_COUNT          512U
#define ALGO_67_AVG_GROUP_SAMPLES    5U
#define ALGO_67_LOW_THRESHOLD_V      0.5f
#define ALGO_67_START_THRESHOLD_V    3.0f
#define ALGO_67_LOW_CONFIRM_SAMPLES  5U

enum {
    ALGO_67_STATE_IDLE = 0,
    ALGO_67_STATE_WAIT_LOW,
    ALGO_67_STATE_WAIT_RISE,
    ALGO_67_STATE_COLLECTING
};

typedef struct {
    int active_algo_id;
    int state;
    unsigned int low_count;
    unsigned int sample_index;
    unsigned int group_count;
    long group_raw_sum;
    int raw_values[ALGO_67_SAMPLE_COUNT];
    float value_values[ALGO_67_SAMPLE_COUNT];
} Algo67PacketSamplerState;

static Algo67PacketSamplerState g_algo_67_sampler = {0};

static AlgoConfig *algo_67_find_algo(int algo_id)
{
    for (int i = 0; i < ALGO_COUNT; i++) {
        if (g_algos[i].id == algo_id) {
            return &g_algos[i];
        }
    }

    return NULL;
}

static int algo_67_has_running_algo(int algo_id)
{
    AlgoConfig *algo = algo_67_find_algo(algo_id);
    return algo != NULL && algo->running;
}

static int algo_67_has_consumer(void)
{
    return algo67_tcp_upload_is_enabled() ||
           algo_67_has_running_algo(6) ||
           algo_67_has_running_algo(7);
}

static int algo_67_target_channel(void)
{
    AlgoConfig *algo6 = algo_67_find_algo(6);

    if (algo6 != NULL && algo6->channel >= 0 &&
        algo6->channel < MAX_CH) {
        return algo6->channel;
    }

    return ALGO67_TCP_UPLOAD_LOGICAL_CH;
}

static uint8_t algo_67_consumer_mask(void)
{
    uint8_t mask = 0;

    if (algo_67_has_running_algo(6)) {
        mask |= ALGO67_TCP_CONSUMER_ALGO6;
    }
    if (algo_67_has_running_algo(7)) {
        mask |= ALGO67_TCP_CONSUMER_ALGO7;
    }

    return mask;
}

static void algo_67_reset_sampler(int active)
{
    g_algo_67_sampler.active_algo_id = active ? 1 : 0;
    g_algo_67_sampler.state =
        active ? ALGO_67_STATE_WAIT_LOW : ALGO_67_STATE_IDLE;
    g_algo_67_sampler.low_count = 0;
    g_algo_67_sampler.sample_index = 0;
    g_algo_67_sampler.group_count = 0;
    g_algo_67_sampler.group_raw_sum = 0;
    memset(g_algo_67_sampler.raw_values, 0,
           sizeof(g_algo_67_sampler.raw_values));
    memset(g_algo_67_sampler.value_values, 0,
           sizeof(g_algo_67_sampler.value_values));
}

static int algo_67_round_raw(float value)
{
    if (value >= 0.0f) {
        return (int)(value + 0.5f);
    }

    return (int)(value - 0.5f);
}

static void algo_67_publish_to_algo(AlgoConfig *algo)
{
    int write_ch;

    if (algo == NULL || !algo->running) {
        return;
    }

    write_ch = algo->channel;
    if (write_ch < 0 || write_ch >= MAX_CH) {
        printf("[Algo6/7] %s 的目标通道%d越界。\n",
               algo->name, write_ch);
        return;
    }

    pthread_mutex_lock(&data_mutex);
    memset(new_data[write_ch], 0, sizeof(new_data[write_ch]));
    memset(algo_data[write_ch], 0, sizeof(algo_data[write_ch]));
    for (unsigned int i = 0; i < ALGO_67_SAMPLE_COUNT; i++) {
        new_data[write_ch][i] = g_algo_67_sampler.raw_values[i];
        algo_data[write_ch][i] = g_algo_67_sampler.value_values[i];
    }
    pthread_mutex_unlock(&data_mutex);

    if (write_algo_data_for_algo(algo) == 0) {
        printf("[Algo6/7] 5点平均1x512共享包采集完成,触发 %s。\n",
               algo->name);
    } else {
        fprintf(stderr, "[Algo6/7] %s 数据写入或触发失败。\n",
                algo->name);
    }
}

static void algo_67_collect_sample(int target_ch, int raw_value)
{
    float actual_voltage;
    int write_ch = algo_67_target_channel();
    uint8_t consumer_mask;

    if (!algo_67_has_consumer()) {
        if (g_algo_67_sampler.active_algo_id != 0) {
            algo_67_reset_sampler(0);
        }
        return;
    }

    if (target_ch != write_ch) {
        return;
    }

    if (g_algo_67_sampler.active_algo_id == 0) {
        algo_67_reset_sampler(1);
        printf("[Algo6/7] 共享采样器开启: 先等待通道%d电压<%.2fV，消费者=算法6/7/3009。\n",
               write_ch, ALGO_67_LOW_THRESHOLD_V);
    }

    actual_voltage = (float)raw_value * 0.000375f;

    if (g_algo_67_sampler.state == ALGO_67_STATE_WAIT_LOW) {
        if (actual_voltage < ALGO_67_LOW_THRESHOLD_V) {
            g_algo_67_sampler.low_count++;
            if (g_algo_67_sampler.low_count >=
                ALGO_67_LOW_CONFIRM_SAMPLES) {
                g_algo_67_sampler.state = ALGO_67_STATE_WAIT_RISE;
                printf("[Algo6/7] 已确认低电平,等待第一个>%.2fV包头。\n",
                       ALGO_67_START_THRESHOLD_V);
            }
        } else {
            g_algo_67_sampler.low_count = 0;
        }
        return;
    }

    if (g_algo_67_sampler.state == ALGO_67_STATE_WAIT_RISE) {
        if (actual_voltage < ALGO_67_LOW_THRESHOLD_V) {
            g_algo_67_sampler.low_count =
                ALGO_67_LOW_CONFIRM_SAMPLES;
            return;
        }

        if (actual_voltage <= ALGO_67_START_THRESHOLD_V) {
            return;
        }

        g_algo_67_sampler.state = ALGO_67_STATE_COLLECTING;
        g_algo_67_sampler.sample_index = 0;
        g_algo_67_sampler.group_count = 0;
        g_algo_67_sampler.group_raw_sum = 0;
        printf("[Algo6/7] 检测到%.3fV包头,开始5点平均采集。\n",
               actual_voltage);
    }

    if (g_algo_67_sampler.state != ALGO_67_STATE_COLLECTING) {
        return;
    }

    g_algo_67_sampler.group_raw_sum += raw_value;
    g_algo_67_sampler.group_count++;

    if (g_algo_67_sampler.group_count < ALGO_67_AVG_GROUP_SAMPLES) {
        return;
    }

    {
        float average_raw =
            (float)g_algo_67_sampler.group_raw_sum /
            (float)ALGO_67_AVG_GROUP_SAMPLES;
        int rounded_raw = algo_67_round_raw(average_raw);
        float average_voltage = average_raw * 0.000375f;
        float physical_value =
            algo_convert_algo67_voltage_to_value(average_voltage);
        unsigned int index = g_algo_67_sampler.sample_index;

        g_algo_67_sampler.raw_values[index] = rounded_raw;
        g_algo_67_sampler.value_values[index] = physical_value;
        g_algo_67_sampler.sample_index++;
        g_algo_67_sampler.group_count = 0;
        g_algo_67_sampler.group_raw_sum = 0;
    }

    if (g_algo_67_sampler.sample_index < ALGO_67_SAMPLE_COUNT) {
        return;
    }

    if (write_ch < 0 || write_ch >= MAX_CH) {
        printf("[Algo6/7] 目标通道%d越界。\n", write_ch);
        algo_67_reset_sampler(1);
        return;
    }

    consumer_mask = algo_67_consumer_mask();

    /* 先把完整包提交给TCP发送线程；这里只复制2048字节，不执行send。 */
    algo67_tcp_upload_submit(consumer_mask, write_ch,
                             g_algo_67_sampler.value_values);

    /* 同一包数据分发给算法6和算法7，二者同时running时拿到完全相同的数据。 */
    algo_67_publish_to_algo(algo_67_find_algo(6));
    algo_67_publish_to_algo(algo_67_find_algo(7));

    /* 保留测试数据格式要求：每一包完成后都重新等待<0.5V低电平。 */
    g_algo_67_sampler.state = ALGO_67_STATE_WAIT_LOW;
    g_algo_67_sampler.low_count = 0;
    g_algo_67_sampler.sample_index = 0;
    g_algo_67_sampler.group_count = 0;
    g_algo_67_sampler.group_raw_sum = 0;
}


// /* ===================== 算法1：0.5秒步进包同步采样 =====================
//  * 模拟器按“0V空档 + 256个有效点 + 0.5秒0V空档”输出。
//  * 设备端只在等待状态检测5V包头跳变，随后从跳变时刻+250ms开始，
//  * 每500ms采1个点，采满256点触发算法1，再回到等待下一包。
//  */
// #define ALGO_145_SAMPLE_COUNT        256U
// #define ALGO_145_TRIGGER_THRESHOLD_V 5.0f
// // #define ALGO_145_FIRST_DELAY_NS      50000000L
// // #define ALGO_145_INTERVAL_NS         100000000L
// #define ALGO_145_FIRST_DELAY_NS      250000000L
// #define ALGO_145_INTERVAL_NS         500000000L
// #define ALGO_145_BASELINE_DIVISOR    128.0f

// enum {
//     ALGO_145_STATE_IDLE = 0,
//     ALGO_145_STATE_WAIT_TRIGGER,
//     ALGO_145_STATE_COLLECTING
// };

// typedef struct {
//     int active_algo_id;
//     int state;
//     int baseline_initialized;
//     float baseline_voltage;
//     unsigned int sample_index;
//     int raw_values[ALGO_145_SAMPLE_COUNT];   
//     float value_values[ALGO_145_SAMPLE_COUNT];
//     struct timespec next_sample_time;
// } Algo145PacketSamplerState;

// static Algo145PacketSamplerState g_algo_145_sampler = {0};

// static int algo_145_is_packet_algo(int algo_id)
// {
//     (void)algo_id;
//     return 0;
// }

// static float algo_145_absf(float value)
// {
//     return value < 0.0f ? -value : value;
// }

// static void algo_145_add_ns(struct timespec *value, long ns)
// {
//     value->tv_nsec += ns;
//     if (value->tv_nsec >= 1000000000L) {
//         value->tv_sec += value->tv_nsec / 1000000000L;
//         value->tv_nsec %= 1000000000L;
//     }
// }

// static int algo_145_time_reached(const struct timespec *now,
//                                  const struct timespec *deadline)
// {
//     if (now->tv_sec != deadline->tv_sec) {
//         return now->tv_sec > deadline->tv_sec;
//     }

//     return now->tv_nsec >= deadline->tv_nsec;
// }

// static AlgoConfig *algo_145_find_running(void)
// {
//     for (int i = 0; i < ALGO_COUNT; i++) {
//         if (g_algos[i].running && algo_145_is_packet_algo(g_algos[i].id)) {
//             return &g_algos[i];
//         }
//     }

//     return NULL;
// }

// static void algo_145_reset_sampler(int active_algo_id)
// {
//     g_algo_145_sampler.active_algo_id = active_algo_id;
//     g_algo_145_sampler.state =
//         active_algo_id != 0 ? ALGO_145_STATE_WAIT_TRIGGER :
//                               ALGO_145_STATE_IDLE;
//     g_algo_145_sampler.baseline_initialized = 0;
//     g_algo_145_sampler.baseline_voltage = 0.0f;
//     g_algo_145_sampler.sample_index = 0;
//     memset(g_algo_145_sampler.raw_values, 0,
//            sizeof(g_algo_145_sampler.raw_values));
//     memset(g_algo_145_sampler.value_values, 0,
//            sizeof(g_algo_145_sampler.value_values));
//     memset(&g_algo_145_sampler.next_sample_time, 0,
//            sizeof(g_algo_145_sampler.next_sample_time));
// }

// static void algo_145_collect_sample(int target_ch, int raw_value)
// {
//     AlgoConfig *algo = algo_145_find_running();
//     struct timespec now;
//     float actual_voltage;
//     float physical_value;
//     float difference;
//     int write_ch;

//     if (algo == NULL) {
//         if (g_algo_145_sampler.active_algo_id != 0) {
//             algo_145_reset_sampler(0);
//         }
//         return;
//     }

//     if (target_ch != algo->channel) {
//         return;
//     }

//     clock_gettime(CLOCK_MONOTONIC, &now);

//     if (g_algo_145_sampler.active_algo_id != algo->id) {
//         algo_145_reset_sampler(algo->id);
//         printf("[Algo1] %s 进入包同步采样: 等待>%.2fV包头跳变。\n",
//                algo->name, ALGO_145_TRIGGER_THRESHOLD_V);
//     }

//     actual_voltage = (float)raw_value * 0.000375f;
//     physical_value = algo_convert_fast_raw_for_id(algo->id, target_ch,
//                                                   raw_value);

//     if (g_algo_145_sampler.state == ALGO_145_STATE_WAIT_TRIGGER) {
//         if (!g_algo_145_sampler.baseline_initialized) {
//             g_algo_145_sampler.baseline_voltage = actual_voltage;
//             g_algo_145_sampler.baseline_initialized = 1;
//             return;
//         }

//         difference = algo_145_absf(actual_voltage -
//                                    g_algo_145_sampler.baseline_voltage);
//         if (difference < ALGO_145_TRIGGER_THRESHOLD_V) {
//             g_algo_145_sampler.baseline_voltage +=
//                 (actual_voltage - g_algo_145_sampler.baseline_voltage) /
//                 ALGO_145_BASELINE_DIVISOR;
//             return;
//         }

//         g_algo_145_sampler.state = ALGO_145_STATE_COLLECTING;
//         g_algo_145_sampler.sample_index = 0;
//         g_algo_145_sampler.next_sample_time = now;
//         algo_145_add_ns(&g_algo_145_sampler.next_sample_time,
//                         ALGO_145_FIRST_DELAY_NS);
//         printf("[Algo1] %s 检测到包头跳变 %.3fV, 250ms后开始256点采样。\n",
//                algo->name, difference);
//         return;
//     }

//     if (g_algo_145_sampler.state != ALGO_145_STATE_COLLECTING ||
//         !algo_145_time_reached(&now, &g_algo_145_sampler.next_sample_time)) {
//         return;
//     }

//     do {
//         algo_145_add_ns(&g_algo_145_sampler.next_sample_time,
//                         ALGO_145_INTERVAL_NS);
//     } while (algo_145_time_reached(
//                  &now, &g_algo_145_sampler.next_sample_time));

//     g_algo_145_sampler.raw_values[g_algo_145_sampler.sample_index] =
//         raw_value;
//     g_algo_145_sampler.value_values[g_algo_145_sampler.sample_index] =
//         physical_value;
//     g_algo_145_sampler.sample_index++;

//     if (g_algo_145_sampler.sample_index < ALGO_145_SAMPLE_COUNT) {
//         return;
//     }

//     write_ch = algo->channel;
//     if (write_ch < 0 || write_ch >= MAX_CH) {
//         printf("[Algo1] %s 的目标通道%d越界。\n",
//                algo->name, write_ch);
//         algo_145_reset_sampler(algo->id);
//         return;
//     }

//     pthread_mutex_lock(&data_mutex);
//     memset(new_data[write_ch], 0, sizeof(new_data[write_ch]));
//     memset(algo_data[write_ch], 0, sizeof(algo_data[write_ch]));
//     for (unsigned int i = 0; i < ALGO_145_SAMPLE_COUNT; i++) {
//         new_data[write_ch][i] = g_algo_145_sampler.raw_values[i];
//         algo_data[write_ch][i] = g_algo_145_sampler.value_values[i];
//     }
//     pthread_mutex_unlock(&data_mutex);

//     g_algo_145_sampler.state = ALGO_145_STATE_WAIT_TRIGGER;
//     g_algo_145_sampler.sample_index = 0;
//     g_algo_145_sampler.baseline_initialized = 0;

//     if (!algo->running || g_algo_145_sampler.active_algo_id != algo->id) {
//         return;
//     }

//     if (write_algo_data_for_algo(algo) == 0) {
//         printf("[Algo1] 1x256定时包采集完成,触发 %s。\n",
//                algo->name);
//     } else {
//         fprintf(stderr, "[Algo1] %s 数据写入或触发失败。\n",
//                 algo->name);
//     }
// }

/* ===================== 算法1：绝对阈值快速包同步共享采样 =====================
 * 等待实际电压超过7V作为包头，50ms后采第1点，之后每100ms采1点。
 * 采满256点后分发给算法1和3011 TCP上传；触发后先等电压低于1V，再允许下一包。
 */
#define ALGO_45_SAMPLE_COUNT        256U
#define ALGO_45_TRIGGER_V          7.0f
#define ALGO_45_RESET_V            1.0f
#define ALGO_45_FIRST_DELAY_NS     50000000L
#define ALGO_45_INTERVAL_NS        100000000L

enum {
    ALGO_45_STATE_IDLE = 0,
    ALGO_45_STATE_WAIT_LOW,
    ALGO_45_STATE_WAIT_TRIGGER,
    ALGO_45_STATE_COLLECTING
};

typedef struct {
    int active_algo_id;
    int state;
    unsigned int sample_index;
    int raw_values[ALGO_45_SAMPLE_COUNT];
    float value_values[ALGO_45_SAMPLE_COUNT];
    struct timespec next_sample_time;
} Algo45PacketSamplerState;

static Algo45PacketSamplerState g_algo_45_sampler = {0};

static int algo_45_is_packet_algo(int algo_id)
{
    /* 算法1保留原1x256快速包同步采样；腐蚀板4/5由独立共享采样器处理。 */
    return algo_id == 1;
}

static AlgoConfig *algo_45_find_algo(int algo_id)
{
    for (int i = 0; i < ALGO_COUNT; i++) {
        if (g_algos[i].id == algo_id) {
            return &g_algos[i];
        }
    }

    return NULL;
}

static AlgoConfig *algo_45_find_running(void)
{
    for (int i = 0; i < ALGO_COUNT; i++) {
        if (g_algos[i].running && algo_45_is_packet_algo(g_algos[i].id)) {
            return &g_algos[i];
        }
    }

    return NULL;
}

static int algo_45_has_consumer(void)
{
    return algo1_tcp_upload_is_enabled() ||
           algo_45_find_running() != NULL;
}

static int algo_45_target_channel(void)
{
    AlgoConfig *algo1 = algo_45_find_algo(1);

    if (algo1 != NULL && algo1->channel >= 0 &&
        algo1->channel < MAX_CH) {
        return algo1->channel;
    }

    return ALGO1_TCP_UPLOAD_LOGICAL_CH;
}

static uint8_t algo_45_consumer_mask(void)
{
    return algo_45_find_running() != NULL ? ALGO1_TCP_CONSUMER_ALGO1 : 0;
}

static void algo_45_reset_sampler(int active_algo_id)
{
    g_algo_45_sampler.active_algo_id = active_algo_id;
    g_algo_45_sampler.state =
        active_algo_id != 0 ? ALGO_45_STATE_WAIT_TRIGGER :
                              ALGO_45_STATE_IDLE;
    g_algo_45_sampler.sample_index = 0;
    memset(g_algo_45_sampler.raw_values, 0,
           sizeof(g_algo_45_sampler.raw_values));
    memset(g_algo_45_sampler.value_values, 0,
           sizeof(g_algo_45_sampler.value_values));
    memset(&g_algo_45_sampler.next_sample_time, 0,
           sizeof(g_algo_45_sampler.next_sample_time));
}

static void algo_45_publish_to_algo(AlgoConfig *algo)
{
    int write_ch;

    if (algo == NULL || !algo->running) {
        return;
    }

    write_ch = algo->channel;
    if (write_ch < 0 || write_ch >= MAX_CH) {
        printf("[Algo1] %s 的目标通道%d越界。\n",
               algo->name, write_ch);
        return;
    }

    pthread_mutex_lock(&data_mutex);
    memset(new_data[write_ch], 0, sizeof(new_data[write_ch]));
    memset(algo_data[write_ch], 0, sizeof(algo_data[write_ch]));
    for (unsigned int i = 0; i < ALGO_45_SAMPLE_COUNT; i++) {
        new_data[write_ch][i] = g_algo_45_sampler.raw_values[i];
        algo_data[write_ch][i] = g_algo_45_sampler.value_values[i];
    }
    pthread_mutex_unlock(&data_mutex);

    if (write_algo_data_for_algo(algo) == 0) {
        printf("[Algo1] 1x256快速定时包采集完成,触发 %s。\n",
               algo->name);
    } else {
        fprintf(stderr, "[Algo1] %s 数据写入或触发失败。\n",
                algo->name);
    }
}

static void algo_45_collect_sample(int target_ch, int raw_value)
{
    struct timespec now;
    float actual_voltage;
    float physical_value;
    int write_ch = algo_45_target_channel();
    uint8_t consumer_mask;

    if (!algo_45_has_consumer()) {
        if (g_algo_45_sampler.active_algo_id != 0) {
            algo_45_reset_sampler(0);
        }
        return;
    }

    if (target_ch != write_ch) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);

    if (g_algo_45_sampler.active_algo_id == 0) {
        algo_45_reset_sampler(1);
        printf("[Algo1] 共享采样器开启: 等待通道%d电压>%.2fV触发，消费者=算法1/3011。\n",
               write_ch, ALGO_45_TRIGGER_V);
    }

    actual_voltage = (float)raw_value * 0.000375f;
    physical_value = algo_convert_fast_raw_for_id(1, target_ch, raw_value);

    if (g_algo_45_sampler.state == ALGO_45_STATE_WAIT_LOW) {
        if (actual_voltage > ALGO_45_RESET_V) {
            return;
        }

        g_algo_45_sampler.state = ALGO_45_STATE_WAIT_TRIGGER;
        printf("[Algo1] 电压已低于%.2fV, 重新等待下一包>%.2fV触发。\n",
               ALGO_45_RESET_V, ALGO_45_TRIGGER_V);
        return;
    }

    if (g_algo_45_sampler.state == ALGO_45_STATE_WAIT_TRIGGER) {
        if (actual_voltage <= ALGO_45_TRIGGER_V) {
            return;
        }

        g_algo_45_sampler.state = ALGO_45_STATE_COLLECTING;
        g_algo_45_sampler.sample_index = 0;
        g_algo_45_sampler.next_sample_time = now;
        algo_145_add_ns(&g_algo_45_sampler.next_sample_time,
                        ALGO_45_FIRST_DELAY_NS);
        printf("[Algo1] 检测到%.3fV包头, 50ms后开始256点采样。\n",
               actual_voltage);
        return;
    }

    if (g_algo_45_sampler.state != ALGO_45_STATE_COLLECTING ||
        !algo_145_time_reached(&now, &g_algo_45_sampler.next_sample_time)) {
        return;
    }

    do {
        algo_145_add_ns(&g_algo_45_sampler.next_sample_time,
                        ALGO_45_INTERVAL_NS);
    } while (algo_145_time_reached(
                 &now, &g_algo_45_sampler.next_sample_time));

    g_algo_45_sampler.raw_values[g_algo_45_sampler.sample_index] =
        raw_value;
    g_algo_45_sampler.value_values[g_algo_45_sampler.sample_index] =
        physical_value;
    g_algo_45_sampler.sample_index++;

    if (g_algo_45_sampler.sample_index < ALGO_45_SAMPLE_COUNT) {
        return;
    }

    consumer_mask = algo_45_consumer_mask();

    /* 先把完整包提交给TCP发送线程；这里只复制1KB，不执行send。 */
    algo1_tcp_upload_submit(consumer_mask, write_ch,
                            g_algo_45_sampler.value_values);

    /* 同一包数据分发给算法1；3011单独开启时不会触发Python。 */
    algo_45_publish_to_algo(algo_45_find_algo(1));

    g_algo_45_sampler.state = ALGO_45_STATE_WAIT_LOW;
    g_algo_45_sampler.sample_index = 0;
}


/* ===================== 腐蚀板算法4/5共享256点采样器 =====================
 * 消费者包括：算法4、算法5、3005 TCP上传。
 * 采样器只运行一次，采满同一包后分发给所有正在启用的消费者，
 * 避免算法数据和TCP上传数据不一致。
 */
typedef struct {
    int active;
    int state;
    unsigned int sample_index;
    int raw_values[ALGO_45_SAMPLE_COUNT];
    float value_values[ALGO_45_SAMPLE_COUNT];
    struct timespec next_sample_time;
} Corrosion45PacketSamplerState;

static Corrosion45PacketSamplerState g_corrosion_45_sampler = {0};

static AlgoConfig *corrosion_45_find_algo(int algo_id)
{
    for (int i = 0; i < ALGO_COUNT; i++) {
        if (g_algos[i].id == algo_id) {
            return &g_algos[i];
        }
    }

    return NULL;
}

static int corrosion_45_has_running_algo(int algo_id)
{
    AlgoConfig *algo = corrosion_45_find_algo(algo_id);
    return algo != NULL && algo->running;
}

static int corrosion_45_has_consumer(void)
{
    return corrosion_tcp_upload_is_enabled() ||
           corrosion_45_has_running_algo(4) ||
           corrosion_45_has_running_algo(5);
}

static int corrosion_45_target_channel(void)
{
    AlgoConfig *algo4 = corrosion_45_find_algo(4);

    if (algo4 != NULL && algo4->channel >= 0 && algo4->channel < MAX_CH) {
        return algo4->channel;
    }

    return CORROSION_TCP_UPLOAD_LOGICAL_CH;
}

static int corrosion_45_source_id(void)
{
    int has_algo4 = corrosion_45_has_running_algo(4);
    int has_algo5 = corrosion_45_has_running_algo(5);

    if (has_algo4 && has_algo5) {
        return 45;
    }
    if (has_algo4) {
        return 4;
    }
    if (has_algo5) {
        return 5;
    }

    return 0;
}

static void corrosion_45_reset_sampler(int active)
{
    g_corrosion_45_sampler.active = active;
    g_corrosion_45_sampler.state =
        active ? ALGO_45_STATE_WAIT_TRIGGER : ALGO_45_STATE_IDLE;
    g_corrosion_45_sampler.sample_index = 0;
    memset(g_corrosion_45_sampler.raw_values, 0,
           sizeof(g_corrosion_45_sampler.raw_values));
    memset(g_corrosion_45_sampler.value_values, 0,
           sizeof(g_corrosion_45_sampler.value_values));
    memset(&g_corrosion_45_sampler.next_sample_time, 0,
           sizeof(g_corrosion_45_sampler.next_sample_time));
}

static void corrosion_45_publish_to_algo(AlgoConfig *algo)
{
    int write_ch;

    if (algo == NULL || !algo->running) {
        return;
    }

    write_ch = algo->channel;
    if (write_ch < 0 || write_ch >= MAX_CH) {
        printf("[Corrosion45] %s 的目标通道%d越界。\n",
               algo->name, write_ch);
        return;
    }

    pthread_mutex_lock(&data_mutex);
    memset(new_data[write_ch], 0, sizeof(new_data[write_ch]));
    memset(algo_data[write_ch], 0, sizeof(algo_data[write_ch]));
    for (unsigned int i = 0; i < ALGO_45_SAMPLE_COUNT; i++) {
        new_data[write_ch][i] = g_corrosion_45_sampler.raw_values[i];
        algo_data[write_ch][i] = g_corrosion_45_sampler.value_values[i];
    }
    pthread_mutex_unlock(&data_mutex);

    if (write_algo_data_for_algo(algo) == 0) {
        printf("[Corrosion45] 1x256腐蚀板共享包采集完成,触发 %s。\n",
               algo->name);
    } else {
        fprintf(stderr, "[Corrosion45] %s 数据写入或触发失败。\n",
                algo->name);
    }
}

static void corrosion_45_collect_sample(int target_ch, int raw_value)
{
    struct timespec now;
    float actual_voltage;
    float physical_value;
    int write_ch = corrosion_45_target_channel();

    if (!corrosion_45_has_consumer()) {
        if (g_corrosion_45_sampler.active) {
            corrosion_45_reset_sampler(0);
        }
        return;
    }

    if (target_ch != write_ch) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!g_corrosion_45_sampler.active) {
        corrosion_45_reset_sampler(1);
        printf("[Corrosion45] 共享采样器开启: 等待电压>%.2fV触发，消费者=算法4/5/3005。\n",
               ALGO_45_TRIGGER_V);
    }

    actual_voltage = (float)raw_value * 0.000375f;
    physical_value = algo_convert_fast_raw_for_id(4, target_ch, raw_value);

    if (g_corrosion_45_sampler.state == ALGO_45_STATE_WAIT_LOW) {
        if (actual_voltage > ALGO_45_RESET_V) {
            return;
        }

        g_corrosion_45_sampler.state = ALGO_45_STATE_WAIT_TRIGGER;
        printf("[Corrosion45] 电压已低于%.2fV, 重新等待下一包>%.2fV触发。\n",
               ALGO_45_RESET_V, ALGO_45_TRIGGER_V);
        return;
    }

    if (g_corrosion_45_sampler.state == ALGO_45_STATE_WAIT_TRIGGER) {
        if (actual_voltage <= ALGO_45_TRIGGER_V) {
            return;
        }

        g_corrosion_45_sampler.state = ALGO_45_STATE_COLLECTING;
        g_corrosion_45_sampler.sample_index = 0;
        g_corrosion_45_sampler.next_sample_time = now;
        algo_145_add_ns(&g_corrosion_45_sampler.next_sample_time,
                        ALGO_45_FIRST_DELAY_NS);
        printf("[Corrosion45] 检测到%.3fV包头, 50ms后开始256点腐蚀板采样。\n",
               actual_voltage);
        return;
    }

    if (g_corrosion_45_sampler.state != ALGO_45_STATE_COLLECTING ||
        !algo_145_time_reached(&now,
                               &g_corrosion_45_sampler.next_sample_time)) {
        return;
    }

    do {
        algo_145_add_ns(&g_corrosion_45_sampler.next_sample_time,
                        ALGO_45_INTERVAL_NS);
    } while (algo_145_time_reached(
                 &now, &g_corrosion_45_sampler.next_sample_time));

    g_corrosion_45_sampler.raw_values[g_corrosion_45_sampler.sample_index] =
        raw_value;
    g_corrosion_45_sampler.value_values[g_corrosion_45_sampler.sample_index] =
        physical_value;
    g_corrosion_45_sampler.sample_index++;

    if (g_corrosion_45_sampler.sample_index < ALGO_45_SAMPLE_COUNT) {
        return;
    }

    /* 先把完整包提交给TCP发送线程；这里只复制1KB，不执行send。 */
    corrosion_tcp_upload_submit(
        corrosion_45_source_id(), write_ch,
        g_corrosion_45_sampler.value_values);

    /* 同一包数据分发给算法4和算法5，二者同时running时拿到完全相同的数据。 */
    corrosion_45_publish_to_algo(corrosion_45_find_algo(4));
    corrosion_45_publish_to_algo(corrosion_45_find_algo(5));

    g_corrosion_45_sampler.state = ALGO_45_STATE_WAIT_LOW;
    g_corrosion_45_sampler.sample_index = 0;
}

void deal_ad7915(void)
{
    unsigned int ad7915Ch = 0;
    unsigned int ad7915Buffer[1024] = {0};
    unsigned int rDataCount = 0;
    unsigned int adFifoFlag = 0;
    //unsigned int data = 0;
    unsigned int i;
    unsigned int adData;
    AD7915FiFoStat_t tAD7915FiFoStat;
    
    /* 模式 A 使用电压 ADC（ad7915Ch == 0）的单通道采样率。 */
    //ad7915_get_sample_rate(0, &data);
    for(ad7915Ch = 0; ad7915Ch <= AD7915_NUM - 1; ad7915Ch++)
    {
        read_ad7915_fifo_stat(ad7915Ch, &tAD7915FiFoStat);
        if((rDataCount = tAD7915FiFoStat.fifoCount) > 0)
        {
            adFifoFlag = 1;
        }
        else if(tAD7915FiFoStat.full == 1)
        {
            adFifoFlag = 1;
            rDataCount = 1024;
        }

        if(adFifoFlag == 1)
        {
            adFifoFlag = 0;
            ad7915_rfifo_buffer(ad7915Ch, ad7915Buffer, rDataCount);
            
			for(i = 0; i <= rDataCount - 1; i++)
            {
                // ==============================================================
                // 1. 数据解析 (小端模式)
                // ==============================================================
                unsigned int raw_32 = ad7915Buffer[i];
                unsigned short ch_id = (raw_32 >> 16) & 0xFFFF; // 物理子通道号 (0~7)
                short raw_voltage    = raw_32 & 0xFFFF;         // 实际数值 (带符号)

                /* 3003/3004不再走独立逐样本上传，改为在模式A共享采样器产生8x1时提交TCP包。 */

                // ==============================================================
                // 2. 全局统一逻辑通道映射
                // 电压(0~7) -> 逻辑通道 4~11
                // 电流(0~7) -> 逻辑通道 12~19
                // ==============================================================
                int target_ch = -1;
                if (ad7915Ch == 0) target_ch = ch_id + 4;
                if (ad7915Ch == 1) target_ch = ch_id + 12;

                /* 算法2/3共享高频突变采样入口；算法运行或3001开启时匹配逻辑通道4。 */
                algo_23_collect_sample(target_ch, (int)raw_voltage);

                /* 算法10/11独立定时采样入口；仅匹配其配置的逻辑通道5。 */
                algo_10_11_collect_sample(target_ch, (int)raw_voltage);

                /* 算法6/7共享5点平均同步采样入口；算法运行或3009开启时匹配逻辑通道4。 */
                algo_67_collect_sample(target_ch, (int)raw_voltage);

                /* 算法1共享快速包同步采样入口；算法运行或3011开启时匹配逻辑通道4。 */
                algo_45_collect_sample(target_ch, (int)raw_voltage);

                /* 腐蚀板4/5共享采样器；3005开启时也可独立生成TCP上传包。 */
                corrosion_45_collect_sample(target_ch, (int)raw_voltage);

                /* 3001/3002不再走独立逐点波形上传，改为在算法2/3共享采样器满1024点时提交TCP包。 */

                // ==============================================================
                // 3. 动态识别算法并智能组包 (新增：双模式 - 单次抓拍 vs 连续监测)
                // ==============================================================
                if (target_ch >= 4 && target_ch < 20) 
                {
                    static int fast_idx[20] = {0};
                    static int slow_idx[20] = {0};
                    static int skip_cnt[20] = {0};
                    
                    static int record_state[20] = {0};
                    static int baseline[20] = {0};
                    static int baseline_init[20] = {0};
                    static int cooldown_timer[20] = {0};
                    static int fast_active_algo_id[20] = {0};

                    // 【核心配置】：采集模式选择
                    // 1 = 单次抓拍模式 (带冷却，专治模拟器尾巴，适合演示测一包)
                    // 0 = 连续监测模式 (无冷却，一旦触发绝不丢包，适合真实设备长运)
                    int is_single_shot_mode = 0; 

                    int has_listener = 0;

                    // ==============================================================
                    // 模式 A：8通道完整帧 + 软件触发 + 周期采样
                    //
                    // 电压物理通道 0~7 先组成一个完整扫描帧。
                    // 算法8：突变确认后等待0.5秒，平均16帧，之后按固定1秒时间点输出。
                    // 算法9：启动监听即开始单次采集，每秒1组，32组后输出一次。
                    // ==============================================================
                    enum {
                        LF_STATE_ARMED = 0,
                        LF_STATE_SETTLING,
                        LF_STATE_COLLECTING,
                        LF_STATE_PERIODIC_WAIT
                    };

                    static const float lf_p_min[8] = {
                        0.0f, 0.0f, 0.0f, 0.0f,
                        0.0f, 0.0f, 0.0f, 0.0f
                    };
                    static const float lf_p_max[8] = {
                        30.0f, 50.0f, 0.1f, 40.0f,
                        50.0f, 600.0f, 300.0f, 40.0f
                    };
                    const float lf_voltage_per_code = 0.000375f;
                    const float lf_trigger_threshold_v = 0.05f;
                    const unsigned int lf_confirm_frames = 3U;
                    const unsigned int lf_average_frames = 16U;
                    const double lf_first_delay_s = 0.5;
                    const long long lf_period_interval_ns = 1000000000LL;
                    const unsigned int lf9_window_size = 32U;
                    const double lf9_sample_interval_s = 1.0;

                    static int lf_state = LF_STATE_ARMED;
                    static unsigned char lf_frame_mask = 0;
                    static int lf_frame_raw[8] = {0};
                    static float lf_baseline_raw[8] = {0.0f};
                    static int lf_baseline_ready = 0;
                    static unsigned int lf_change_confirm = 0;
                    //static unsigned int lf_settle_remaining = 0;
                    static unsigned int lf_collect_count = 0;
                    //static unsigned int lf_periodic_frame_count = 0;
                    static double lf_collect_sum[8] = {0.0};
                    static int lf_packet_raw[8] = {0};
                    static float lf_packet_8ch[8] = {0.0f};
                    /////
                    static struct timespec lf_trigger_time = {0, 0};
                    static struct timespec lf_next_collect_time = {0, 0};

                    /* 算法9独立保存8通道x32秒窗口，排列为[channel][time]。 */
                    static int lf9_collecting = 0;
                    static int lf9_was_running = 0;
                    static int lf9_done = 0;
                    static unsigned int lf9_sample_index = 0;
                    static float lf9_window_8ch32[8][32] = {{0.0f}};
                    static struct timespec lf9_last_sample_time = {0, 0};

                    /* 3003单独开启时使用当前完整8通道帧每秒上传一次。 */
                    static int lf_upload_time_valid = 0;
                    static struct timespec lf_upload_next_time = {0, 0};

                    int has_slow_listener = 0;
                    int has_algo8_listener = 0;
                    int has_algo9_listener = 0;
                    int has_mode_a_upload = mode_a_tcp_upload_is_enabled();
                    for (int ai = 0; ai < ALGO_COUNT; ai++)
                    {
                        if (!g_algos[ai].running)
                        {
                            continue;
                        }

                        if (g_algos[ai].id == 8)
                        {
                            has_algo8_listener = 1;
                            has_slow_listener = 1;
                        }
                        else if (g_algos[ai].id == 9)
                        {
                            has_algo9_listener = 1;
                            has_slow_listener = 1;
                        }
                    }

                    if (has_mode_a_upload)
                    {
                        has_slow_listener = 1;
                    }
                    else
                    {
                        lf_upload_time_valid = 0;
                    }

                    if (!has_algo8_listener)
                    {
                        /* 算法8停止后，不保留它的0.5秒/1秒采集状态。 */
                        lf_state = LF_STATE_ARMED;
                        lf_collect_count = 0;
                        memset(&lf_next_collect_time, 0,
                               sizeof(lf_next_collect_time));
                        memset(lf_collect_sum, 0, sizeof(lf_collect_sum));
                    }

                    /*
                     * 算法9采用单次诊断：监听状态由0变1时立即开始32秒采集。
                     * 保持running=1不会再次启动，必须停止后重新开启才会采下一包。
                     */
                    if (has_algo9_listener && !lf9_was_running)
                    {
                        lf9_collecting = 1;
                        lf9_done = 0;
                        lf9_sample_index = 0;
                        memset(lf9_window_8ch32, 0,
                               sizeof(lf9_window_8ch32));
                        clock_gettime(CLOCK_MONOTONIC,
                                      &lf9_last_sample_time);
                        printf("[Algo9] 监听已开启，开始单次32秒窗口采集。\n");
                    }
                    else if (!has_algo9_listener && lf9_was_running)
                    {
                        lf9_collecting = 0;
                        lf9_done = 0;
                        lf9_sample_index = 0;
                        memset(lf9_window_8ch32, 0,
                               sizeof(lf9_window_8ch32));
                        printf("[Algo9] 监听已停止，单次采集状态已清空。\n");
                    }
                    lf9_was_running = has_algo9_listener;

                    if (!has_slow_listener)
                    {
                        /* 所有慢速算法停止后，清空状态并重新学习基线。 */
                        lf_state = LF_STATE_ARMED;
                        lf_frame_mask = 0;
                        lf_baseline_ready = 0;
                        lf_change_confirm = 0;
                        //lf_settle_remaining = 0;
                        lf_collect_count = 0;
                        //lf_periodic_frame_count = 0;
                        memset(&lf_next_collect_time, 0,
                               sizeof(lf_next_collect_time));
                        memset(lf_collect_sum, 0, sizeof(lf_collect_sum));
                        lf9_collecting = 0;
                        lf9_done = 0;
                        lf9_sample_index = 0;
                        memset(lf9_window_8ch32, 0, sizeof(lf9_window_8ch32));
                        lf_upload_time_valid = 0;
                    }
                    // else if (data == 0U)
                    // {
                    //     static unsigned int lf_rate_error_cnt = 0;
                    //     if (lf_rate_error_cnt++ % 5000U == 0U)
                    //     {
                    //         printf("[Algo] 8通道模式: 电压采样率为0,暂停采集!\n");
                    //     }
                    // }
                    else if (ad7915Ch == 0 && ch_id < 8)
                    {
                        /* 通道0代表一轮新扫描，避免跨扫描周期拼成伪同步帧。 */
                        // if (ch_id == 0)
                        // {
                        //     lf_frame_mask = 0;
                        // }

                        lf_frame_raw[ch_id] = (int)raw_voltage;
                        lf_frame_mask |= (unsigned char)(1U << ch_id);

                        if (lf_frame_mask == 0xFFU)
                        {
                            static unsigned int full_frame_count = 0;

                            lf_frame_mask = 0;
                            full_frame_count++;

                            if (full_frame_count % 1000U == 0U)
                            {
                                printf("[ModeA Debug] 完整8通道帧=%u, algo8_state=%d\n",
                                    full_frame_count, lf_state);
                            }

                            float lf_current_8ch[8];
                            for (int k = 0; k < 8; k++)
                            {
                                float voltage =
                                    (float)lf_frame_raw[k] * lf_voltage_per_code;
                                lf_current_8ch[k] =
                                    (voltage / 8.0f) *
                                    (lf_p_max[k] - lf_p_min[k]) +
                                    lf_p_min[k];
                            }

                            if (has_mode_a_upload &&
                                !has_algo8_listener && !has_algo9_listener)
                            {
                                struct timespec upload_now;

                                clock_gettime(CLOCK_MONOTONIC, &upload_now);
                                if (!lf_upload_time_valid)
                                {
                                    mode_a_tcp_schedule_next(
                                        &lf_upload_next_time, &upload_now);
                                    lf_upload_time_valid = 1;
                                }
                                else if (mode_a_tcp_time_reached(
                                             &upload_now,
                                             &lf_upload_next_time))
                                {
                                    mode_a_tcp_upload_submit_packet(
                                        0U, lf_current_8ch);
                                    mode_a_tcp_schedule_next(
                                        &lf_upload_next_time, &upload_now);
                                }
                            }

                            /*
                             * 算法9窗口独立于算法8：触发后每满1秒取当前完整帧，
                             * 32次后按通道优先展开到目标行的前256个float。
                             */
                            if (has_algo9_listener && lf9_collecting &&
                                !lf9_done)
                            {
                                struct timespec now;
                                double elapsed;

                                clock_gettime(CLOCK_MONOTONIC, &now);
                                elapsed =
                                    (double)(now.tv_sec - lf9_last_sample_time.tv_sec) +
                                    (double)(now.tv_nsec - lf9_last_sample_time.tv_nsec) /
                                    1000000000.0;

                                if (elapsed >= lf9_sample_interval_s)
                                {
                                    for (int k = 0; k < 8; k++)
                                    {
                                        lf9_window_8ch32[k][lf9_sample_index] =
                                            lf_current_8ch[k];
                                    }

                                    if (has_mode_a_upload)
                                    {
                                        mode_a_tcp_upload_submit_packet(
                                            9U, lf_current_8ch);
                                    }

                                    lf9_sample_index++;
                                    lf9_last_sample_time = now;
                                    printf("[Algo9] 已采集第%u/32组8通道物理量。\n",
                                           lf9_sample_index);

                                    if (lf9_sample_index >= lf9_window_size)
                                    {
                                        for (int ai9 = 0; ai9 < ALGO_COUNT; ai9++)
                                        {
                                            if (!g_algos[ai9].running ||
                                                g_algos[ai9].id != 9)
                                            {
                                                continue;
                                            }

                                            int write_ch = g_algos[ai9].channel;
                                            if (write_ch < 0 || write_ch >= MAX_CH)
                                            {
                                                printf("[Algo9] %s 的目标通道 %d 越界!\n",
                                                       g_algos[ai9].name, write_ch);
                                                continue;
                                            }

                                            pthread_mutex_lock(&data_mutex);
                                            for (int k = 0; k < 8; k++)
                                            {
                                                for (int t = 0; t < 32; t++)
                                                {
                                                    algo_data[write_ch][k * 32 + t] =
                                                        lf9_window_8ch32[k][t];
                                                }
                                            }
                                            pthread_mutex_unlock(&data_mutex);

                                            if (write_algo_data_for_algo(
                                                    &g_algos[ai9]) == 0)
                                            {
                                                printf("[Algo9] 8x32窗口组包完成，触发 %s !\n",
                                                       g_algos[ai9].name);
                                            }
                                        }

                                        lf9_collecting = 0;
                                        lf9_done = 1;
                                        lf9_sample_index = 0;
                                        memset(lf9_window_8ch32, 0,
                                               sizeof(lf9_window_8ch32));

                                        /* 以窗口末尾电平作为下一次突变检测基线。 */
                                        for (int k = 0; k < 8; k++)
                                        {
                                            lf_baseline_raw[k] =
                                                (float)lf_frame_raw[k];
                                        }
                                        lf_change_confirm = 0;
                                    }
                                }
                            }


                            if (!lf_baseline_ready)
                            {
                                for (int k = 0; k < 8; k++)
                                {
                                    lf_baseline_raw[k] = (float)lf_frame_raw[k];
                                }
                                lf_baseline_ready = 1;
                                lf_change_confirm = 0;
                                printf("[Algo] 8通道模式: 初始基线建立完成，进入实时监听。\n");
                            }
                            else
                            {
                                switch (lf_state)
                                {
                                case LF_STATE_ARMED:
                                case LF_STATE_PERIODIC_WAIT:
                                {
                                    int periodic_due = 0;
                                    int frame_changed = 0;
                                    struct timespec now;

                                    if (lf_state == LF_STATE_PERIODIC_WAIT)
                                    {
                                        double elapsed;

                                        clock_gettime(CLOCK_MONOTONIC, &now);
                                        elapsed = (double)(now.tv_sec - lf_next_collect_time.tv_sec) +
                                                (double)(now.tv_nsec - lf_next_collect_time.tv_nsec) /
                                                1000000000.0;

                                        if (elapsed >= 0.0)
                                        {
                                            periodic_due = 1;
                                        }
                                    }

                                    if (periodic_due)
                                    {
                                        lf_state = LF_STATE_COLLECTING;
                                        lf_collect_count = 0;
                                        memset(lf_collect_sum, 0, sizeof(lf_collect_sum));
                                        do
                                        {
                                            lf_next_collect_time.tv_sec +=
                                                (time_t)(lf_period_interval_ns /
                                                         1000000000LL);
                                            lf_next_collect_time.tv_nsec +=
                                                (long)(lf_period_interval_ns %
                                                       1000000000LL);
                                            if (lf_next_collect_time.tv_nsec >=
                                                1000000000L)
                                            {
                                                lf_next_collect_time.tv_sec +=
                                                    lf_next_collect_time.tv_nsec /
                                                    1000000000L;
                                                lf_next_collect_time.tv_nsec %=
                                                    1000000000L;
                                            }
                                        } while (
                                            now.tv_sec > lf_next_collect_time.tv_sec ||
                                            (now.tv_sec == lf_next_collect_time.tv_sec &&
                                             now.tv_nsec >= lf_next_collect_time.tv_nsec));
                                    }
                                    else
                                    {
                                        for (int k = 0; k < 8; k++)
                                        {
                                            float diff_v =
                                                ((float)lf_frame_raw[k] - lf_baseline_raw[k]) *
                                                lf_voltage_per_code;
                                            if (diff_v > lf_trigger_threshold_v ||
                                                diff_v < -lf_trigger_threshold_v)
                                            {
                                                frame_changed = 1;
                                            }
                                        }

                                        if (frame_changed)
                                        {
                                            lf_change_confirm++;
                                            if (lf_change_confirm >= lf_confirm_frames)
                                            {
                                                if (has_algo8_listener)
                                                {
                                                    lf_state = LF_STATE_SETTLING;
                                                    clock_gettime(
                                                        CLOCK_MONOTONIC,
                                                        &lf_trigger_time);
                                                    printf("[Algo8] 检测到稳定突变，等待0.5秒后采集。\n");
                                                }
                                                else
                                                {
                                                    /* 只有算法9运行时立即接纳新电平为基线。 */
                                                    for (int k = 0; k < 8; k++)
                                                    {
                                                        lf_baseline_raw[k] =
                                                            (float)lf_frame_raw[k];
                                                    }
                                                }

                                                lf_change_confirm = 0;
                                            }
                                        }
                                        else
                                        {
                                            lf_change_confirm = 0;
                                            for (int k = 0; k < 8; k++)
                                            {
                                                /* 浮点慢速基线避免原整数IIR的量化停滞。 */
                                                lf_baseline_raw[k] +=
                                                    ((float)lf_frame_raw[k] - lf_baseline_raw[k]) /
                                                    1024.0f;
                                            }
                                        }
                                    }
                                    break;
                                }

                                case LF_STATE_SETTLING:
                                {
                                    struct timespec now;
                                    double elapsed;

                                    clock_gettime(CLOCK_MONOTONIC, &now);

                                    elapsed = (double)(now.tv_sec - lf_trigger_time.tv_sec) +
                                            (double)(now.tv_nsec - lf_trigger_time.tv_nsec) /
                                            1000000000.0;

                                    if (elapsed >= lf_first_delay_s)
                                    {
                                        lf_state = LF_STATE_COLLECTING;
                                        lf_collect_count = 0;
                                        memset(lf_collect_sum, 0, sizeof(lf_collect_sum));
                                        lf_next_collect_time = now;
                                        lf_next_collect_time.tv_sec +=
                                            (time_t)(lf_period_interval_ns /
                                                     1000000000LL);
                                        lf_next_collect_time.tv_nsec +=
                                            (long)(lf_period_interval_ns %
                                                   1000000000LL);
                                        if (lf_next_collect_time.tv_nsec >=
                                            1000000000L)
                                        {
                                            lf_next_collect_time.tv_sec +=
                                                lf_next_collect_time.tv_nsec /
                                                1000000000L;
                                            lf_next_collect_time.tv_nsec %=
                                                1000000000L;
                                        }
                                    }
                                    break;
                                }

                                case LF_STATE_COLLECTING:
                                    for (int k = 0; k < 8; k++)
                                    {
                                        lf_collect_sum[k] += (double)lf_frame_raw[k];
                                    }
                                    lf_collect_count++;

                                    if (lf_collect_count >= lf_average_frames)
                                    {
                                        for (int k = 0; k < 8; k++)
                                        {
                                            float average_raw =
                                                (float)(lf_collect_sum[k] /
                                                        (double)lf_average_frames);
                                            float voltage = average_raw * lf_voltage_per_code;

                                            lf_packet_raw[k] = (int)average_raw;
                                            /* 不钳位，保留超出0~8V时的真实超量程结果。 */
                                            lf_packet_8ch[k] =
                                                (voltage / 8.0f) *
                                                (lf_p_max[k] - lf_p_min[k]) +
                                                lf_p_min[k];
                                            lf_baseline_raw[k] = average_raw;
                                        }

                                        if (has_mode_a_upload)
                                        {
                                            mode_a_tcp_upload_submit_packet(
                                                8U, lf_packet_8ch);
                                        }

                                        /* 算法8保持原来的8浮点平均包，不在这里触发算法9。 */
                                        for (int slow_ai = 0;
                                             slow_ai < ALGO_COUNT;
                                             slow_ai++)
                                        {
                                            if (!g_algos[slow_ai].running ||
                                                g_algos[slow_ai].id != 8)
                                            {
                                                continue;
                                            }

                                            int write_ch = g_algos[slow_ai].channel;
                                            if (write_ch < 0 || write_ch >= MAX_CH)
                                            {
                                                printf("[Algo] 8通道模式: %s 的目标通道 %d 越界!\n",
                                                       g_algos[slow_ai].name,
                                                       write_ch);
                                                continue;
                                            }

                                            pthread_mutex_lock(&data_mutex);
                                            for (int k = 0; k < 8; k++)
                                            {
                                                new_data[write_ch][k] = lf_packet_raw[k];
                                                algo_data[write_ch][k] = lf_packet_8ch[k];
                                            }
                                            pthread_mutex_unlock(&data_mutex);

                                            if (write_algo_data_for_algo(
                                                    &g_algos[slow_ai]) == 0)
                                            {
                                                printf("[Algo] 8通道模式: 物理量组包完成，触发 %s !\n",
                                                       g_algos[slow_ai].name);
                                            }
                                        }

                                        lf_state = LF_STATE_PERIODIC_WAIT;
                                        lf_collect_count = 0;
                                        lf_change_confirm = 0;
                                        memset(lf_collect_sum, 0, sizeof(lf_collect_sum));
                                    }
                                    break;

                                default:
                                    lf_state = LF_STATE_ARMED;
                                    lf_baseline_ready = 0;
                                    break;
                                }
                            }
                        }
                    }

                    for (int ai = 0; ai < ALGO_COUNT; ai++)
                    {
                        if (g_algos[ai].running && g_algos[ai].channel == target_ch)
                        {
                            int uses_separate_sampler =
                                (g_algos[ai].id == 1 ||
                                 g_algos[ai].id == 2 ||
                                 g_algos[ai].id == 3 ||
                                 g_algos[ai].id == 4 ||
                                 g_algos[ai].id == 5 ||
                                 g_algos[ai].id == 6 ||
                                 g_algos[ai].id == 7 ||
                                 g_algos[ai].id == 8 ||
                                 g_algos[ai].id == 9 ||
                                 g_algos[ai].id == 10 ||
                                 g_algos[ai].id == 11);

                            if (uses_separate_sampler)
                            {
                                /* 算法1、2/3、4/5、6/7、8/9、10/11均走各自共享或独立采样器。 */
                                continue;
                            }
                            else
                            {
                                has_listener = 1;
                                // ---- 高频波形精准捕获模式 ----
                                int THRESHOLD = 140; // 降低门槛到140
                                unsigned int required_points =
                                    algo_fast_window_points(g_algos[ai].id);

                                /* 算法切换时丢弃上一算法未完成的半包和旧基线。 */
                                if (fast_active_algo_id[target_ch] !=
                                    g_algos[ai].id)
                                {
                                    fast_active_algo_id[target_ch] =
                                        g_algos[ai].id;
                                    fast_idx[target_ch] = 0;
                                    record_state[target_ch] = 0;
                                    baseline[target_ch] = 0;
                                    baseline_init[target_ch] = 0;
                                    cooldown_timer[target_ch] = 0;
                                }

                                // 【物理量换算】：转换结果只进入 algo_data，new_data 继续保留原始值
                                float physical_value =
                                    algo_convert_fast_raw_for_id(
                                        g_algos[ai].id, target_ch,
                                        (int)raw_voltage);

                                // 只有在“单次抓拍模式”下，冷却器才生效
                                if (is_single_shot_mode && cooldown_timer[target_ch] > 0) 
                                {
                                    cooldown_timer[target_ch]--;
                                    continue; 
                                }

                                if (record_state[target_ch] == 0) 
                                {
                                    if (baseline_init[target_ch] == 0) {
                                        baseline[target_ch] = (int)raw_voltage; 
                                        baseline_init[target_ch] = 1;
                                    } else {
                                        // 触发判断依然使用原始数值，保证性能和基线稳定
                                        baseline[target_ch] = (baseline[target_ch] * 1023 + (int)raw_voltage) / 1024;
                                    }

                                    int ac_diff = (int)raw_voltage - baseline[target_ch];

                                    static int debug_cnt[20] = {0};
                                    if (debug_cnt[target_ch]++ % 5000 == 0) {
                                        printf("[Debug] 算法%d 通道%d监听中... 原码=%d, 物理量=%.4f, 点数=%u, ac_diff=%d (需>%d)\n",
                                               g_algos[ai].id, target_ch,
                                               raw_voltage, physical_value,
                                               required_points, ac_diff,
                                               THRESHOLD);
                                    }

                                    if (ac_diff > THRESHOLD || ac_diff < -THRESHOLD) 
                                    {
                                        record_state[target_ch] = 1; 
                                        fast_idx[target_ch] = 0;     
                                        
                                        pthread_mutex_lock(&data_mutex);
                                        // new_data 保存原始值，algo_data 保存转换后的算法输入值
                                        new_data[target_ch][fast_idx[target_ch]] = (int)raw_voltage;
                                        algo_data[target_ch][fast_idx[target_ch]] = physical_value;
                                        pthread_mutex_unlock(&data_mutex);
                                        fast_idx[target_ch]++;
                                    }
                                }
                                else if (record_state[target_ch] == 1)
                                {
                                    pthread_mutex_lock(&data_mutex);
                                    // new_data 保存原始值，algo_data 保存转换后的算法输入值
                                    new_data[target_ch][fast_idx[target_ch]] = (int)raw_voltage;
                                    algo_data[target_ch][fast_idx[target_ch]] = physical_value;
                                    pthread_mutex_unlock(&data_mutex);

                                    fast_idx[target_ch]++;
                                    if ((unsigned int)fast_idx[target_ch] >=
                                        required_points)
                                    {


                                        fast_idx[target_ch] = 0; 
                                        
                                        if (is_single_shot_mode) {
                                            record_state[target_ch] = 0;
                                            cooldown_timer[target_ch] = 20000;
                                            printf("[Algo] 单拍模式: 抓获%u点触发 %s ! (进入冷却)\n",
                                                   required_points,
                                                   g_algos[ai].name);
                                        } else {
                                            record_state[target_ch] = 1;
                                            printf("[Algo] 连续模式: 输出%u点触发 %s ! (无缝对接下一包)\n",
                                                   required_points,
                                                   g_algos[ai].name);
                                        }
                                        
                                        fflush(stdout); 
                                        write_algo_data_for_algo(&g_algos[ai]); 
                                    }
                                }
                            }
                        }
                    }

                    if (!has_listener) 
                    {
                        fast_idx[target_ch] = 0;
                        slow_idx[target_ch] = 0;
                        skip_cnt[target_ch] = 0;
                        record_state[target_ch] = 0; 
                        baseline_init[target_ch] = 0;
                        cooldown_timer[target_ch] = 0;
                        fast_active_algo_id[target_ch] = 0;
                    }
                }
				// ==============================================================
				//  4 . 原始数据落盘 (已修复指针 Bug：使用 &ad7915Buffer[i])
				// ==============================================================
				adData = ch_id; // 前面已经解析过了，直接复用
				
				if(ad7915Ch == 0) // 电压物理通道
				{
					switch(adData)
					{
						case 0x00:
							if(g_tDeviceStruct.voltage0Status == 0x01)
								write_pro_data_file_u32(&g_tFileVoltage0Struct, &ad7915Buffer[i], 1, g_saveFileEn);	
							break;
						case 0x01:
							if(g_tDeviceStruct.voltage1Status == 0x01)
								write_pro_data_file_u32(&g_tFileVoltage1Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x02:
							if(g_tDeviceStruct.voltage2Status == 0x01)
								write_pro_data_file_u32(&g_tFileVoltage2Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x03:
							if(g_tDeviceStruct.voltage3Status == 0x01)
								write_pro_data_file_u32(&g_tFileVoltage3Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x04:
							if(g_tDeviceStruct.voltage4Status == 0x01)
								write_pro_data_file_u32(&g_tFileVoltage4Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x05:
							if(g_tDeviceStruct.voltage5Status == 0x01)
								write_pro_data_file_u32(&g_tFileVoltage5Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x06:
							if(g_tDeviceStruct.voltage6Status == 0x01)
								write_pro_data_file_u32(&g_tFileVoltage6Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x07:
							if(g_tDeviceStruct.voltage7Status == 0x01)
								write_pro_data_file_u32(&g_tFileVoltage7Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
					}
				}
				
				if(ad7915Ch == 1) // 电流物理通道
				{
					switch(adData)
					{
						case 0x00:
							if(g_tDeviceStruct.current0Status == 0x01)
								write_pro_data_file_u32(&g_tFileCurrent0Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x01:
							if(g_tDeviceStruct.current1Status == 0x01)
								write_pro_data_file_u32(&g_tFileCurrent1Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x02:
							if(g_tDeviceStruct.current2Status == 0x01)
								write_pro_data_file_u32(&g_tFileCurrent2Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x03:
							if(g_tDeviceStruct.current3Status == 0x01)
								write_pro_data_file_u32(&g_tFileCurrent3Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x04:
							if(g_tDeviceStruct.current4Status == 0x01)
								write_pro_data_file_u32(&g_tFileCurrent4Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x05:
							if(g_tDeviceStruct.current5Status == 0x01)
								write_pro_data_file_u32(&g_tFileCurrent5Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x06:
							if(g_tDeviceStruct.current6Status == 0x01)
								write_pro_data_file_u32(&g_tFileCurrent6Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;
						case 0x07:
							if(g_tDeviceStruct.current7Status == 0x01)
								write_pro_data_file_u32(&g_tFileCurrent7Struct, &ad7915Buffer[i], 1, g_saveFileEn);
							break;  
					}
				}
			}
		}
	}

}

// 		if(adFifoFlag == 1)
// 		{
// 			adFifoFlag = 0;
// 			ad7915_rfifo_buffer(ad7915Ch, ad7915Buffer, rDataCount);
// 			for(i = 0; i <= rDataCount - 1; i++)
// 			{
// #ifdef AD7915_DBG_PRINT
// 				printf("ad7915Buffer[i] = %x", ad7915Buffer[i]);
// #endif				
// 				if(ad7915Ch == 0)
// 				{
// 					adData = ad7915Buffer[i] >> 16;
// #ifdef AD7915_DBG_PRINT					
// 					printf("adData = %x\n", adData);
// #endif					
// 					switch(adData)
// 					{
// 						case 0x00:
// 							if(g_tDeviceStruct.voltage0Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileVoltage0Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x01:
// 							if(g_tDeviceStruct.voltage1Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileVoltage1Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x02:
// 							if(g_tDeviceStruct.voltage2Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileVoltage2Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x03:
// 							if(g_tDeviceStruct.voltage3Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileVoltage3Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x04:
// 							if(g_tDeviceStruct.voltage4Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileVoltage4Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x05:
// 							if(g_tDeviceStruct.voltage5Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileVoltage5Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x06:
// 							if(g_tDeviceStruct.voltage6Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileVoltage6Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x07:
// 							if(g_tDeviceStruct.voltage7Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileVoltage7Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
							
// 					}
// 				}
				
// 				if(ad7915Ch == 1)
// 				{
// 					adData = ad7915Buffer[i] >> 16;
// 					switch(adData)
// 					{
// 						case 0x00:
// 							if(g_tDeviceStruct.current0Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileCurrent0Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x01:
// 							if(g_tDeviceStruct.current1Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileCurrent1Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x02:
// 							if(g_tDeviceStruct.current2Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileCurrent2Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x03:
// 							if(g_tDeviceStruct.current3Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileCurrent3Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x04:
// 							if(g_tDeviceStruct.current4Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileCurrent4Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x05:
// 							if(g_tDeviceStruct.current5Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileCurrent5Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x06:
// 							if(g_tDeviceStruct.current6Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileCurrent6Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;
						
// 						case 0x07:
// 							if(g_tDeviceStruct.current7Status == 0x01)
// 							{
// 								write_pro_data_file_u32(&g_tFileCurrent7Struct, ad7915Buffer, 1, g_saveFileEn);
// 							}
// 						break;	
// 					}
// 				}
// 			}
// 		}
// 	}
// }
