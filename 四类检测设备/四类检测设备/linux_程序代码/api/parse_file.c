
#include "parse_file.h"


#define PAESE_DBG_PRINT

extern unsigned char g_saveFileEn;



int parse_filename(const char *fileName, FileNameInfo_t *info) {
    int result = sscanf(fileName, "%[^_]_%4hu_%2hhu_%2hhu_%2hhu_%2hhu_%2hhu.bin",
                  info->function, 
                  &info->year, &info->month, &info->day,
                  &info->hour, &info->minute, &info->second) == 7;
#ifdef PAESE_DBG_PRINT		  
	printf("Parsing fileName: %s, Result: %d\n", fileName, result);
#endif
	return result;
}


int time_in_range(const FileNameInfo_t *info, const TimeParams_t *start, const TimeParams_t *end) {
    struct tm startTime = {0}, endTime = {0}, fileTime = {0};
	
    startTime.tm_year = start->year - 1900;
    startTime.tm_mon = start->month - 1;
    startTime.tm_mday = start->day;
    startTime.tm_hour = start->hour;
    startTime.tm_min = start->minute;
    startTime.tm_sec = start->second;

	endTime.tm_year = end->year - 1900;
    endTime.tm_mon = end->month - 1;
    endTime.tm_mday = end->day;
    endTime.tm_hour = end->hour;
    endTime.tm_min = end->minute;
	endTime.tm_sec = end->second;
	
    fileTime.tm_year = info->year - 1900;
    fileTime.tm_mon = info->month - 1;
    fileTime.tm_mday = info->day;
    fileTime.tm_hour = info->hour;
    fileTime.tm_min = info->minute;
	fileTime.tm_sec = info->second;
	
    time_t tFileTime = mktime(&fileTime);
    time_t tStartTime = mktime(&startTime);
    time_t tEndTime = mktime(&endTime);
#ifdef PAESE_DBG_PRINT	
	printf("Checking time range for file: %04hu-%02hhu-%02hhu %02hhu:%02hhu:%02hhu\n",
           info->year, info->month, info->day, info->hour, info->minute, info->second);
#endif		   
    return difftime(tFileTime, tStartTime) >= 0 && difftime(tEndTime, tFileTime) >= 0;
}



int compare_file_info(const void *a, const void *b) {
    FileNameInfo_t *info1 = (FileNameInfo_t *)a;
    FileNameInfo_t *info2 = (FileNameInfo_t *)b;

    // 按年份排序
    if (info1->year != info2->year) {
        return info1->year - info2->year;
    }

    // 按月份排序
    if (info1->month != info2->month) {
        return info1->month - info2->month;
    }

    // 按日期排序
    if (info1->day != info2->day) {
        return info1->day - info2->day;
    }

    // 按小时排序
    if (info1->hour != info2->hour) {
        return info1->hour - info2->hour;
    }

    // 按分钟排序
    if (info1->minute != info2->minute) {
        return info1->minute - info2->minute;
    }

	if (info1->second != info2->second) {
        return info1->second - info2->second;
    }
	
    return 0;
}



int search_and_sort_files(const TimeRange_t *timeRange, FileNameInfo_t *buffer, int *count) {
    struct dirent *dp;
	DIR *dir;
	
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
        return -1;
    }

    while ((dp = readdir(dir)) != NULL) {
        if (dp->d_type == DT_REG) {
            const char *dot = strrchr(dp->d_name, '.');
            if (dot && strcmp(dot, ".bin") == 0) {
                FileNameInfo_t info;
                if (parse_filename(dp->d_name, &info)) {
                    if (time_in_range(&info, &timeRange->start, &timeRange->end)) {
                        if (*count < BUFFER_SIZE) {
                            buffer[*count] = info;
                            (*count)++;
                        } else {
                            fprintf(stderr, "缓冲区已满，无法存储更多文件信息\n");
                            closedir(dir);
                            return -1;
                        }
                    }
                }
            }
        }
    }

    closedir(dir);
#ifdef PAESE_DBG_PRINT
	printf("Found %d matching files\n", *count);
#endif
    qsort(buffer, *count, sizeof(FileNameInfo_t), compare_file_info);

    return 0;
}


void receive_time(const unsigned char *buffer, TimeParams_t *timeParams) 
{
	memcpy(&timeParams->year,   buffer,  2);
	memcpy(&timeParams->month,  buffer + 2,  2);
	memcpy(&timeParams->day,    buffer + 4,  2);
	memcpy(&timeParams->hour,   buffer + 6,  2);
	memcpy(&timeParams->minute, buffer + 8, 2);
	memcpy(&timeParams->second, buffer + 10, 2);
#ifdef PAESE_DBG_PRINT	
	printf("Parsed time params: %04hu-%02hu-%02hu %02hu:%02hu:%02hu\n",
	   timeParams->year, timeParams->month, timeParams->day,
	   timeParams->hour, timeParams->minute, timeParams->second);
#endif
}