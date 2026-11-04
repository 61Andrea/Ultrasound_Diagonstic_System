/*
 * A422.h
 *
 *  Created on: 2022Äê1ÔÂ25ÈÕ
 *      Author: hit
 */
#include <sys/socket.h>//socket
#include <arpa/inet.h>//htons() sockaddr_in

typedef struct 
{
	int fd;
    struct sockaddr_in clientAddr;
}IPData_t;


