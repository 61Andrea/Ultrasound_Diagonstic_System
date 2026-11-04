 
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
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
#include <dirent.h>              //文件夹操作
#include <pthread.h>

#define FILE_LIMIT_BYTE  104857600			  //单个文本存储容量上限  
												  //100M = 104857600字节

#pragma pack (4)
typedef struct
{	
	char fileType[64];						//文件类型
	char fileOldFolderTime[64];			//文件夹日期
	char fileNewFolderTime[64];			//文件夹日期
	char fileOldMakeTime[64];				//文件旧的创建时间
	char fileNewMakeTime[64];				//文件更新创建时间
	unsigned int fileFolderNo;			//文件夹数量
	unsigned char fileNo;					//文件号码
	unsigned char fileOpenStatus;			//文件打开状态 1:打开 0：关闭			
	unsigned int fileNum;					//文件数量
	unsigned int fileOpenTime;				//文件打开时间
	unsigned int fileOpenByte;				//文件打开保存数量
	unsigned int fileFlushSum;				//文件Flush字节数量
	unsigned int fileByteSum;				//文件总字节数量
	unsigned int fileFlushByteCount;		//文件刷新字节数量；
	unsigned int fileCloseByteCount;		//文件关闭字节数量；
	unsigned int lastTime;
	FILE *fp; 
	pthread_mutex_t fileNameLock;
}FileStruct_t, *FileStruct_p;

#pragma pack ()


unsigned int init_folder(unsigned char saveFileType);
void create_folder(unsigned int fileFolderNo, char * fileFolderTime, unsigned char saveFileType);

void init_file(FileStruct_p pFileStruct,unsigned int fileFolderNo, unsigned char fileNo, unsigned int fileFlushByteCount, unsigned int fileCloseByteCount, unsigned char saveFileType, char *ptime0, char *ptime1);
unsigned int init_num_file(char* str);
unsigned int read_num_file(char* str);
void write_num_file(char* str,int a);
unsigned char write_pro_data_file_u8(FileStruct_p pFileStruct, unsigned char *buffer, unsigned int num, unsigned char saveFileEn);
unsigned char write_pro_data_file_u16(FileStruct_p pFileStruct, unsigned short int *buffer, unsigned int num, unsigned char saveFileEn);
unsigned char write_pro_data_file_u32(FileStruct_p pFileStruct, unsigned int *buffer, unsigned int num, unsigned char saveFileEn);
