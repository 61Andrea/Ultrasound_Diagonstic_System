 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/statvfs.h>
#include <sys/time.h>


#define 	FILENAME_LENGTH 256
#define 	BUFFER_SIZE 1024

typedef struct {
    char function[FILENAME_LENGTH];
    unsigned short year;
    unsigned char month;
    unsigned char day;
    unsigned char hour;
    unsigned char minute;
	unsigned char second;
} FileNameInfo_t;


typedef struct {
    unsigned short year;
    unsigned short month;
    unsigned short day;
    unsigned short hour;
    unsigned short minute;
	unsigned short second;
} TimeParams_t;

typedef struct 
{
    TimeParams_t start;
    TimeParams_t end;
} TimeRange_t;

FileNameInfo_t g_tfileList[BUFFER_SIZE];
int g_fileCount;

int parse_filename(const char *fileName, FileNameInfo_t *info) ;
int time_in_range(const FileNameInfo_t *info, const TimeParams_t *start, const TimeParams_t *end) ;
void receive_time(const unsigned char *buffer, TimeParams_t *timeParams);
int compare_file_info(const void *a, const void *b);
int search_and_sort_files(const TimeRange_t *timeRange, FileNameInfo_t *buffer, int *count);

