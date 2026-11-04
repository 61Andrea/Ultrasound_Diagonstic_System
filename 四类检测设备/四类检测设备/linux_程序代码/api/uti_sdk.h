/******************************************************************************************
* Copyright (c) 2025,北京鸿棠电子技术有限公司 All rights reserved.
* 接口版本：V5.0.0
* 摘要：适用UTI-V5.0.0以上
* 首次发布日期：2025-04-XX
*
* 修改日志：

*******************************************************************************************/
#ifndef __UTI_SDK_API_H__
#define __UTI_SDK_API_H__


#ifdef __cplusplus
extern "C" {
#endif

	typedef struct 
	{
		int channelNo;      //通道号
		int Encoder1_cyc;
		int Encoder1_pulse;
		int Encoder2;       //编码器，  X坐标
		int Analog1;
		int Analog2;
		short* WaveData;     //波形数据，注意其长度为DataSize, 通过UTI_GetDataSize()获得
	}UTData;

	typedef struct 
	{
		short rev_fpga_err;                      
		int time_from_start;   //程序启动到现在的时间，单位为“秒”
		int rev_fpga_count;                
		int send_win_count;             
		int data_err_count;            
		short send_unfinished_count;
		int arm_version;       //ARM程序（本程序）版本号
		int fpga_version;      //FPGA程序版本号
		int ut_sn;             //仪器序列号
	}UTState;

// 操作函数
int UTI_OpenDevice(const char* IP);
void UTI_CloseDevice();

int UTI_SetTriggerMode(int triggerMode);
int UTI_SetRPF(int RPF);
int UTI_SetSequ(const char* channelSequence);
int UTI_SetChDelay(int channelNumber, double delayTime);
int UTI_SetChSampleTime(int channelNumber, double scope, double speed);
int UTI_SetChDACCurve(int channelNumber, const char* dac);
int UTI_SetChGain(int channelNumber, double gain);
int UTI_SetChPGC(int channelNumber, double PGain);
int UTI_SetChFilter(int channelNumber, int filter);
int UTI_SetChDetectionType(int channelNumber, int detectionType);
int UTI_SetChPulseWidth(int channelNumber, int pulseWidth);
int UTI_SetDataSize(int size);
int UTI_GetDataSize();   //UTI_SetDataSize()后必须通过此函数获取实际的长度
int UTI_StartWork();
int UTI_StopWork();

UTData* UTI_ReadData();
UTState UTI_GetUTState();

int UTI_GetSN();
void UTI_GetVersion(char* version);  

#ifdef __cplusplus
}
#endif

#endif /* __UTI_SDK_API_H__ */