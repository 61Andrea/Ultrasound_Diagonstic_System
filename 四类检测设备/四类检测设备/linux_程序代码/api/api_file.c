
#include "api_file.h"


struct dirent * dir_info;    //文件目录结构体


unsigned int init_folder(unsigned char saveFileType)
{
	DIR * dir_ptr;
	char dateFileName[64];
	char fileFolderName[64];
	unsigned int currentNo;
	unsigned int tempNo = 0;
	
	if(saveFileType <= 0x01)
	{
		mkdir("/mnt/file", 0777);	
	}
	else if(saveFileType == 0x02)
	{
		mkdir("/run/media/nvme0n1p1/file/", 0777);	
	}
	else if(saveFileType == 0x03)
	{
		mkdir("/run/media/nvme0n1p1/file/", 0777);	
		mkdir("/run/media/nvme0n1p1/file/iepe/", 0777);	
	}
	else if(saveFileType == 0x04)
	{
		mkdir("/run/media/nvme0n1p1/file/", 0777);	
		mkdir("/run/media/nvme0n1p1/file/normal/", 0777);	
	}
	return 0;
}



void create_folder(unsigned int fileFolderNo, char * fileFolderTime, unsigned char saveFileType)
{
	char fileFolderName[64];
	
	if(saveFileType <= 0x01)
	{
		sprintf(fileFolderName, "/mnt/file/%u_%s", fileFolderNo, fileFolderTime);
	}
	else if(saveFileType == 0x02)
	{
		sprintf(fileFolderName, "/run/media/nvme0n1p1/file/%u_%s", fileFolderNo, fileFolderTime);
	}
	else if(saveFileType == 0x03)
	{
		sprintf(fileFolderName, "/run/media/nvme0n1p1/file/iepe/%u_%s", fileFolderNo, fileFolderTime);
	}
	else if(saveFileType == 0x04)
	{
		sprintf(fileFolderName, "/run/media/nvme0n1p1/file/normal/%u_%s", fileFolderNo, fileFolderTime);
	}
	mkdir(fileFolderName, 0777);	
}



void init_file(FileStruct_p pFileStruct,unsigned int fileFolderNo, unsigned char fileNo, unsigned int fileFlushByteCount, unsigned int fileCloseByteCount, unsigned char saveFileType, char *ptime0, char *ptime1)
{
	/*
	strcpy(pFileStruct->fileOldFolderTime, "2000_01_01");
	strcpy(pFileStruct->fileNewFolderTime, "2000_01_01");
	strcpy(pFileStruct->fileOldMakeTime, "2000_01_01_00_00_00");
	strcpy(pFileStruct->fileNewMakeTime, "2000_01_01_00_00_00");
	*/
	strcpy(pFileStruct->fileOldFolderTime, ptime0);
	strcpy(pFileStruct->fileNewFolderTime, ptime0);
	strcpy(pFileStruct->fileOldMakeTime, ptime1);
	strcpy(pFileStruct->fileNewMakeTime, ptime1);
	
	
	pFileStruct->fileFolderNo = fileFolderNo;
	pFileStruct->fileOpenStatus = 0;
	pFileStruct->fileOpenTime = 0;
	pFileStruct->fileOpenByte = 0;
	pFileStruct->fileByteSum = 0;
	
	pFileStruct->fileNo = fileNo;
	pFileStruct->fileFlushByteCount = fileFlushByteCount;
	pFileStruct->fileCloseByteCount = fileCloseByteCount;
	pFileStruct->fileNum = 0;
	
	
	pthread_mutex_init(&pFileStruct->fileNameLock, NULL);
}


/*********************************************
* 函数名         :init_num_txt()
* 函数功能		 :创建计数文本
* 输入           : *str：需要创建的计数文本的名称
* 输出         	 : 无
*********************************************/
unsigned int init_num_file(char* str)            
{
    FILE* fp;
    int c;    
	unsigned int fileNum = 0;
	
    fp = fopen(str,"a+");    // 如果没有txt_num.txt这个文件则创建
    c = fgetc(fp);
    if (c == EOF)
    {
      fprintf(fp, "%d", 0);
      fclose(fp);
    }
    else                               //如果有则跳过
    {
	  fclose(fp);
	  fileNum = read_num_file(str);
	  fileNum = fileNum + 1;
	  write_num_file(str,fileNum);
    }
	return fileNum;
    
}


/****************************************************
* 函数名         :read_num_txt()
* 函数功能		 :从计数文本中获取文本计数值
* 输入           : *str：需要被获取计数值的计数文本的名称
* 输出         	 : d：文本计数值
****************************************************/
unsigned int read_num_file(char* str)                             
{
	FILE* fp;
	unsigned int d;     
	
	fp = fopen(str,"a+");
    fscanf(fp, "%d",  &d);
    //printf("当前文本记数值为: %d\n", txt_num);
    fclose(fp);
	return d; 
}


/**********************************************
* 函数名         :write_num_txt()
* 函数功能		 :用来更新 计数文本 中的文本计数值
* 输入           : *str：计数文本 a：更新的计数值
* 输出         	 : 无
**********************************************/
void write_num_file(char* str,int a)
{
	FILE* fp;
	int fd;
  	fp = fopen(str, "w");  
	if (fp == NULL)
	{
		//printf("file is error.");	
	}
	else
	{
    	fprintf(fp, "%d", a);  
        fclose(fp);
	}
}



/***********************************************************************************************************
* 函数名         :write_pro_data_txt_0()
* 函数功能		 :向指定通道的数据文本中写入数据，写入前检测待写入的数据文本容量，防止超出上限（10M）
* 输入           : *pro_type：通道类型（例：A422）txt_no：通道号（例：0）*buffer：需要写入的数据 num：写入的字节数 
*                  fp：文件指针 *f_open_close_en：文件开关使能 *text_en_byte：使能间隔计数（单位：字节）
* 输出         	 : 文件指针 fp（）
***********************************************************************************************************/
unsigned char write_pro_data_file_u8(FileStruct_p pFileStruct, unsigned char *buffer, unsigned int num, unsigned char saveFileEn)
{
	
	char dateFileName[256];
	if(saveFileEn >= 0x01)
	{

		if(pFileStruct->fileOpenStatus == 0x01)
		{
			fwrite(buffer, sizeof(unsigned char), sizeof(unsigned char)*num, pFileStruct->fp);
			pFileStruct->fileByteSum = pFileStruct->fileByteSum + num;
			pFileStruct->fileOpenByte = pFileStruct->fileOpenByte + num;
			pFileStruct->fileFlushSum = pFileStruct->fileFlushSum + num;
		}
		else
		{
			
			if(saveFileEn == 0x01)
			{
				//sprintf(dateFileName, "/mnt/file/%s_%u_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				//sprintf(dateFileName, "/mnt/file/%s_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/mnt/file/%s_%s.bin", pFileStruct->fileType, pFileStruct->fileOldMakeTime);
			}
			else if(saveFileEn == 0x02)
			{
				//sprintf(dateFileName, "/run/media/nvme0n1p1/file/%s_%u_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				//sprintf(dateFileName, "/run/media/nvme0n1p1/file/%s_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/run/media/nvme0n1p1/file/%s_%s.bin", pFileStruct->fileType, pFileStruct->fileOldMakeTime);
			}
			else if(saveFileEn == 0x03)
			{
				//sprintf(dateFileName, "/run/media/nvme0n1p1/file/iepe/%u_%s/%s_%u_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/run/media/nvme0n1p1/file/iepe/%u_%s/%s_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
			}
			else if(saveFileEn == 0x04)
			{
				//sprintf(dateFileName, "/run/media/nvme0n1p1/file/normal/%u_%s/%s_%u_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/run/media/nvme0n1p1/file/normal/%u_%s/%s_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
			}
			else
			{
				return 0;
			}
			pFileStruct->fp = fopen(dateFileName, "a+");  
			if (pFileStruct->fp == NULL)
			{
				printf("file1 is error. %s\n", pFileStruct->fileType);	
				pFileStruct->fileOpenStatus = 0x00;			
			}
			else
			{
				pFileStruct->fileOpenStatus = 0x01;
				fwrite(buffer, sizeof(unsigned char), sizeof(unsigned char)*num, pFileStruct->fp);
				pFileStruct->fileByteSum = pFileStruct->fileByteSum + num;
				pFileStruct->fileOpenByte = pFileStruct->fileOpenByte + num;
				pFileStruct->fileFlushSum = pFileStruct->fileFlushSum + num;
			}
		}   
		
		
		if(strcmp(pFileStruct->fileOldFolderTime, pFileStruct->fileNewFolderTime) != 0) 
		{
			pFileStruct->fileOpenStatus = 0x00;	
			pFileStruct->fileNum++;	
			pFileStruct->fileOpenTime = 0x00;	
			pFileStruct->fileOpenByte = 0x00;	
			pFileStruct->fileFlushSum = 0x00;	
			pFileStruct->fileByteSum = 0x00;
			pthread_mutex_lock(&pFileStruct->fileNameLock);
			strcpy(pFileStruct->fileOldMakeTime, pFileStruct->fileNewMakeTime);
			strcpy(pFileStruct->fileOldFolderTime, pFileStruct->fileNewFolderTime);
			pthread_mutex_unlock(&pFileStruct->fileNameLock);
			fclose(pFileStruct->fp);
		}
		else if(pFileStruct->fileByteSum >= FILE_LIMIT_BYTE)
		{
			pFileStruct->fileOpenStatus = 0x00;	
			pFileStruct->fileNum++;	
			pFileStruct->fileOpenTime = 0x00;	
			pFileStruct->fileOpenByte = 0x00;	
			pFileStruct->fileFlushSum = 0x00;	
			pFileStruct->fileByteSum = 0x00;
			pthread_mutex_lock(&pFileStruct->fileNameLock);
			strcpy(pFileStruct->fileOldMakeTime, pFileStruct->fileNewMakeTime);
			//strcpy(pFileStruct->fileOldFolderTime, pFileStruct->fileNewFolderTime);
			pthread_mutex_unlock(&pFileStruct->fileNameLock);
			fclose(pFileStruct->fp);
		}
		else if(pFileStruct->fileOpenByte >= pFileStruct->fileCloseByteCount)
		{
			pFileStruct->fileOpenStatus = 0x00;	
			pFileStruct->fileOpenTime = 0x00;
			pFileStruct->fileOpenByte = 0x00;			
			pFileStruct->fileFlushSum = 0x00;	
			fclose(pFileStruct->fp);
		}
		else if(pFileStruct->fileFlushSum >= pFileStruct->fileFlushByteCount)
		{
			pFileStruct->fileFlushSum = 0x00;	
			fflush(pFileStruct->fp); 
		}
	}
	return 1;
}



unsigned char write_pro_data_file_u16(FileStruct_p pFileStruct, unsigned short int *buffer, unsigned int num, unsigned char saveFileEn)
{
	
	char dateFileName[256];
	if(saveFileEn >= 0x01)
	{

		if(pFileStruct->fileOpenStatus == 0x01)
		{
			fwrite(buffer, sizeof(unsigned char), sizeof(unsigned char)*num*2, pFileStruct->fp);
			pFileStruct->fileByteSum = pFileStruct->fileByteSum + num*2;
			pFileStruct->fileOpenByte = pFileStruct->fileOpenByte + num*2;
			pFileStruct->fileFlushSum = pFileStruct->fileFlushSum + num*2;
		}
		else
		{
			
			if(saveFileEn == 0x01)
			{
				//sprintf(dateFileName, "/mnt/file/%s_%u_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				//sprintf(dateFileName, "/mnt/file/%s_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/mnt/file/%s_%s.bin", pFileStruct->fileType, pFileStruct->fileOldMakeTime);
			}
			else if(saveFileEn == 0x02)
			{
				//sprintf(dateFileName, "/run/media/nvme0n1p1/file/%s_%u_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/run/media/nvme0n1p1/file/%s_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/run/media/nvme0n1p1/file/%s_%s.bin", pFileStruct->fileType, pFileStruct->fileOldMakeTime);
			}
			else if(saveFileEn == 0x03)
			{
				//sprintf(dateFileName, "/run/media/nvme0n1p1/file/iepe/%u_%s/%s_%u_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/run/media/nvme0n1p1/file/iepe/%u_%s/%s_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
			}
			else if(saveFileEn == 0x04)
			{
				//sprintf(dateFileName, "/run/media/nvme0n1p1/file/normal/%u_%s/%s_%u_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/run/media/nvme0n1p1/file/normal/%u_%s/%s_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
			}
			else
			{
				return 0;
			}
			pFileStruct->fp = fopen(dateFileName, "a+");  
			if (pFileStruct->fp == NULL)
			{
				printf("file2 is error. %s\n", pFileStruct->fileType);	
				pFileStruct->fileOpenStatus = 0x00;			
			}
			else
			{
				pFileStruct->fileOpenStatus = 0x01;
				fwrite(buffer, sizeof(unsigned char), sizeof(unsigned char)*num*2, pFileStruct->fp);
				pFileStruct->fileByteSum = pFileStruct->fileByteSum + num*2;
				pFileStruct->fileOpenByte = pFileStruct->fileOpenByte + num*2;
				pFileStruct->fileFlushSum = pFileStruct->fileFlushSum + num*2;
			}
		}   
		
		
		if(strcmp(pFileStruct->fileOldFolderTime, pFileStruct->fileNewFolderTime) != 0) 
		{
			pFileStruct->fileOpenStatus = 0x00;	
			pFileStruct->fileNum++;	
			pFileStruct->fileOpenTime = 0x00;	
			pFileStruct->fileOpenByte = 0x00;	
			pFileStruct->fileFlushSum = 0x00;	
			pFileStruct->fileByteSum = 0x00;
			pthread_mutex_lock(&pFileStruct->fileNameLock);
			strcpy(pFileStruct->fileOldMakeTime, pFileStruct->fileNewMakeTime);
			strcpy(pFileStruct->fileOldFolderTime, pFileStruct->fileNewFolderTime);
			pthread_mutex_unlock(&pFileStruct->fileNameLock);
			fclose(pFileStruct->fp);
		}
		else if(pFileStruct->fileByteSum >= FILE_LIMIT_BYTE)
		{
			pFileStruct->fileOpenStatus = 0x00;	
			pFileStruct->fileNum++;	
			pFileStruct->fileOpenTime = 0x00;	
			pFileStruct->fileOpenByte = 0x00;	
			pFileStruct->fileFlushSum = 0x00;	
			pFileStruct->fileByteSum = 0x00;
			pthread_mutex_lock(&pFileStruct->fileNameLock);
			strcpy(pFileStruct->fileOldMakeTime, pFileStruct->fileNewMakeTime);
			//strcpy(pFileStruct->fileOldFolderTime, pFileStruct->fileNewFolderTime);
			pthread_mutex_unlock(&pFileStruct->fileNameLock);
			fclose(pFileStruct->fp);
		}
		else if(pFileStruct->fileOpenByte >= pFileStruct->fileCloseByteCount)
		{
			pFileStruct->fileOpenStatus = 0x00;	
			pFileStruct->fileOpenTime = 0x00;
			pFileStruct->fileOpenByte = 0x00;			
			pFileStruct->fileFlushSum = 0x00;	
			fclose(pFileStruct->fp);
		}
		else if(pFileStruct->fileFlushSum >= pFileStruct->fileFlushByteCount)
		{
			pFileStruct->fileFlushSum = 0x00;	
			fflush(pFileStruct->fp); 
		}
	}
	return 1;
}


unsigned char write_pro_data_file_u32(FileStruct_p pFileStruct, unsigned int *buffer, unsigned int num, unsigned char saveFileEn)
{
	
	char dateFileName[256];
	if(saveFileEn >= 0x01)
	{

		if(pFileStruct->fileOpenStatus == 0x01)
		{
			fwrite(buffer, sizeof(unsigned char), sizeof(unsigned char)*num*4, pFileStruct->fp);
			pFileStruct->fileByteSum = pFileStruct->fileByteSum + num*4;
			pFileStruct->fileOpenByte = pFileStruct->fileOpenByte + num*4;
			pFileStruct->fileFlushSum = pFileStruct->fileFlushSum + num*4;
		}
		else
		{
			
			if(saveFileEn == 0x01)
			{
				//sprintf(dateFileName, "/mnt/file/%s_%u_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				//sprintf(dateFileName, "/mnt/file/%s_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/mnt/file/%s_%s.bin", pFileStruct->fileType, pFileStruct->fileOldMakeTime);
			}
			else if(saveFileEn == 0x02)
			{
				//sprintf(dateFileName, "/run/media/nvme0n1p1/file/%s_%u_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				//sprintf(dateFileName, "/run/media/nvme0n1p1/file/%s_%u_%s.bin", pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/run/media/nvme0n1p1/file/%s_%s.bin", pFileStruct->fileType, pFileStruct->fileOldMakeTime);
			}
			else if(saveFileEn == 0x03)
			{
				//sprintf(dateFileName, "/run/media/nvme0n1p1/file/iepe/%u_%s/%s_%u_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/run/media/nvme0n1p1/file/iepe/%u_%s/%s_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
			}
			else if(saveFileEn == 0x04)
			{
				//sprintf(dateFileName, "/run/media/nvme0n1p1/file/normal/%u_%s/%s_%u_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				sprintf(dateFileName, "/run/media/nvme0n1p1/file/normal/%u_%s/%s_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
			}
			else
			{
				return 0;
			}
			pFileStruct->fp = fopen(dateFileName, "a+");  
			if (pFileStruct->fp == NULL)
			{
				printf("file3 is error. %s\n", pFileStruct->fileType);	
				pFileStruct->fileOpenStatus = 0x00;			
			}
			else
			{
				pFileStruct->fileOpenStatus = 0x01;
				fwrite(buffer, sizeof(unsigned char), sizeof(unsigned char)*num*4, pFileStruct->fp);
				pFileStruct->fileByteSum = pFileStruct->fileByteSum + num*4;
				pFileStruct->fileOpenByte = pFileStruct->fileOpenByte + num*4;
				pFileStruct->fileFlushSum = pFileStruct->fileFlushSum + num*4;
			}
		}   
		
		
		if(strcmp(pFileStruct->fileOldFolderTime, pFileStruct->fileNewFolderTime) != 0) 
		{
			pFileStruct->fileOpenStatus = 0x00;	
			pFileStruct->fileNum++;	
			pFileStruct->fileOpenTime = 0x00;	
			pFileStruct->fileOpenByte = 0x00;	
			pFileStruct->fileFlushSum = 0x00;	
			pFileStruct->fileByteSum = 0x00;
			pthread_mutex_lock(&pFileStruct->fileNameLock);
			strcpy(pFileStruct->fileOldMakeTime, pFileStruct->fileNewMakeTime);
			strcpy(pFileStruct->fileOldFolderTime, pFileStruct->fileNewFolderTime);
			pthread_mutex_unlock(&pFileStruct->fileNameLock);
			fclose(pFileStruct->fp);
		}
		else if(pFileStruct->fileByteSum >= FILE_LIMIT_BYTE)
		{
			pFileStruct->fileOpenStatus = 0x00;	
			pFileStruct->fileNum++;	
			pFileStruct->fileOpenTime = 0x00;	
			pFileStruct->fileOpenByte = 0x00;	
			pFileStruct->fileFlushSum = 0x00;	
			pFileStruct->fileByteSum = 0x00;
			pthread_mutex_lock(&pFileStruct->fileNameLock);
			strcpy(pFileStruct->fileOldMakeTime, pFileStruct->fileNewMakeTime);
			//strcpy(pFileStruct->fileOldFolderTime, pFileStruct->fileNewFolderTime);
			pthread_mutex_unlock(&pFileStruct->fileNameLock);
			fclose(pFileStruct->fp);
		}
		else if(pFileStruct->fileOpenByte >= pFileStruct->fileCloseByteCount)
		{
			pFileStruct->fileOpenStatus = 0x00;	
			pFileStruct->fileOpenTime = 0x00;
			pFileStruct->fileOpenByte = 0x00;			
			pFileStruct->fileFlushSum = 0x00;	
			fclose(pFileStruct->fp);
		}
		else if(pFileStruct->fileFlushSum >= pFileStruct->fileFlushByteCount)
		{
			pFileStruct->fileFlushSum = 0x00;	
			fflush(pFileStruct->fp); 
		}
	}
	return 1;
}
/***********************************************************************************************************
* 函数名         :write_pro_data_txt_0()
* 函数功能		 :向指定通道的数据文本中写入数据，写入前检测待写入的数据文本容量，防止超出上限（10M）
* 输入           : *pro_type：通道类型（例：A422）txt_no：通道号（例：0）*buffer：需要写入的数据 num：写入的字节数 
*                  fp：文件指针 *f_open_close_en：文件开关使能 *text_en_byte：使能间隔计数（单位：字节）
* 输出         	 : 文件指针 fp（）
***********************************************************************************************************
unsigned char write_pro_data_file(FileStruct_p pFileStruct, unsigned char *buffer, unsigned int num, unsigned char saveFileEn)
{
	
	char dateFileName[64];
	char numFileName[64];
	if(saveFileEn >= 0x01)
	{

		if(pFileStruct->fileOpenStatus == 0x01)
		{
			fwrite(buffer, sizeof(unsigned char), sizeof(unsigned char)*num, pFileStruct->fp);
			pFileStruct->fileByteSum = pFileStruct->fileByteSum + num;
			pFileStruct->fileOpenByte = pFileStruct->fileOpenByte + num;
			pFileStruct->fileFlushSum = pFileStruct->fileFlushSum + num;
		}
		else
		{
			
			if(saveFileEn == 0x01)
			{
				pthread_mutex_lock(&pFileStruct->fileNameLock);
				sprintf(dateFileName, "/mnt/file/%u_%s/%s_%u_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				pthread_mutex_unlock(&pFileStruct->fileNameLock);
			}
			else if(saveFileEn == 0x02)
			{
				pthread_mutex_lock(&pFileStruct->fileNameLock);
				sprintf(dateFileName, "/run/media/nvme0n1p1/file/%u_%s/%s_%u_%u_%s.bin",pFileStruct->fileFolderNo, pFileStruct->fileOldFolderTime, pFileStruct->fileType, pFileStruct->fileNo, pFileStruct->fileNum, pFileStruct->fileOldMakeTime);
				pthread_mutex_unlock(&pFileStruct->fileNameLock);
			}
			else
			{
				return 0;
			}
			
			pFileStruct->fp = fopen(dateFileName, "a+");  
			if (pFileStruct->fp == NULL)
			{
				printf("file is error.");	
				pFileStruct->fileOpenStatus = 0x00;			
			}
			else
			{
				pFileStruct->fileOpenStatus = 0x01;
				fwrite(buffer, sizeof(unsigned char), sizeof(unsigned char)*num, pFileStruct->fp);
				pFileStruct->fileByteSum = pFileStruct->fileByteSum + num;
				pFileStruct->fileOpenByte = pFileStruct->fileOpenByte + num;
				pFileStruct->fileFlushSum = pFileStruct->fileFlushSum + num;
			}
		}   
		
		if(pFileStruct->fileByteSum >= FILE_LIMIT_BYTE)
		{
			pFileStruct->fileOpenStatus = 0x00;	
			pFileStruct->fileNum++;	
			pFileStruct->fileOpenTime = 0x00;	
			pFileStruct->fileOpenByte = 0x00;	
			pFileStruct->fileFlushSum = 0x00;	
			pFileStruct->fileByteSum = 0x00;
			strcpy(pFileStruct->fileOldMakeTime, pFileStruct->fileNewMakeTime);
			strcpy(pFileStruct->fileOldFolderTime, pFileStruct->fileNewFolderTime);
			fclose(pFileStruct->fp);
		}
		else if(pFileStruct->fileOpenByte >= pFileStruct->fileCloseByteCount)
		{
			pFileStruct->fileOpenStatus = 0x00;	
			pFileStruct->fileOpenTime = 0x00;
			pFileStruct->fileOpenByte = 0x00;			
			pFileStruct->fileFlushSum = 0x00;	
			fclose(pFileStruct->fp);
		}
		else if(pFileStruct->fileFlushSum >= pFileStruct->fileFlushByteCount)
		{
			pFileStruct->fileFlushSum = 0x00;	
			fflush(pFileStruct->fp); 
		}
	}
	return 1;
}

*/