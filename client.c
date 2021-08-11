/*
** client.c
** Jason Fry
** Summer 2021
** Socket initialization taken from Brian “Beej Jorgensen” Hall
**		(https://www.beej.us/guide/bgnet/html/index-wide.html)
**
**
**
** Client program to be run on PC or Laptop
** Takes IP address of esp32 as argument
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <ctype.h>
#include <poll.h>

#include <arpa/inet.h>

#define PORT "3333" // the port client will be connecting to

#define MAXDATASIZE 127 // max number of bytes we can get at once
#define XSTR(s) STR(s)
#define STR(s) #s

#define GET "0"
#define SET "1"

//#define NO_NETWORK
//#define VERBOSE

void directControl(int sock);
void directControlFade(int sock);
int isValidDuty(char *str);
int isValidFade(char *str);
void setDuty(int sock, int duty, int time);
int getDuty(int sock);
void tcp_send(int sock, char *buf);
void tcp_recv(int sock, char *buf);

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char *argv[])
{
	int sockfd, numbytes;
#ifndef NO_NETWORK
	char buf[MAXDATASIZE+1];
	struct addrinfo hints, *servinfo, *p;
	int rv;
	char s[INET6_ADDRSTRLEN];

	if (argc != 2) {
		fprintf(stderr,"usage: client hostname\n");
		exit(1);
	}

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((rv = getaddrinfo(argv[1], PORT, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and connect to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
							 p->ai_protocol)) == -1) {
			perror("client: socket");
			continue;
		}

		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("client: connect");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "client: failed to connect\n");
		return 2;
	}

	inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
			  s, sizeof s);
	printf("client: connecting to %s\n", s);

	freeaddrinfo(servinfo); // all done with this structure
#endif

	char usrBuf[MAXDATASIZE+1];
	int badFlag = 0;
	do {
		system("clear");
		if(badFlag){
			printf("%s is not a valid selection\n\n", usrBuf);
			badFlag = 0;
		}
		printf("Select mode:\n\t(1) - direct control\n\t(2) - fade control\n\t(q) - quit\n> ");
		scanf("%"XSTR(MAXDATASIZE)"s", usrBuf);
		int c;
		while((c=fgetc(stdin)) != '\n' && c != EOF); // eat extra chars
		if(usrBuf[1] != '\0')
			badFlag = 1;
		else {
			switch (usrBuf[0]) {
				case '1': {
					system("clear");
					printf("Please wait\n");
					directControl(sockfd);
					break;
				}
				case '2':{
					system("clear");
					printf("Please wait\n");
					directControlFade(sockfd);
					break;
				}
				/*
				case '3': {
					printf("\nSorry, this feature is not yet available.\nPlease make a different selection.\n");
					break;
				}
				case '4': {
					printf("\nSorry, this feature is not yet available.\nPlease make a different selection.\n");
					break;
				}
				 */
				case 'q': {
					system("clear");
					printf("Goodbye!\n");
					break;
				}
				default: {
					badFlag = 1;
				}
			}
		}
		if(badFlag){
			printf("\n%s is not a valid selection. Please make a different selection\n", usrBuf);
		}
		printf("\n");
	} while(strcmp("q", usrBuf) != 0);

#ifndef NO_NETWORK
	close(sockfd);
#endif

	return 0;
}

// user simpy enters desired duty cycle. no fade.
void directControl(int sock){
	char usrBuf[MAXDATASIZE+1];
	int badFlag = 0;
	do {
		int cur_duty = getDuty(sock);
		system("clear");
		// get current duty cycle
		if(badFlag){
			printf("%s is not a valid entry.\nPlease enter a number between -100 and 100 or q.\n\n", usrBuf);
			badFlag = 0;
		}
		printf("Current duty cycle is %d%%. Enter new duty cycle or q to quit.\n> ", cur_duty);

		// remove any input during delay
		struct pollfd fds = {0, POLLIN, 0};
		poll(&fds, 1, 0);
		while(fds.revents == POLLIN){
			int c;
			while((c=fgetc(stdin)) != '\n' && c != EOF); // eat extra lines
			poll(&fds, 1, 0);
		}

		// get user input
		scanf("%"XSTR(MAXDATASIZE)"s", usrBuf);

		// eat extraneous characters
		int c;
		while((c=fgetc(stdin)) != '\n' && c != EOF);
		system("clear");
		printf("Please wait\n");

		// deal with input
		if (strcmp("q", usrBuf) == 0) {
			setDuty(sock, 0, 0);
			printf("\nQuitting direct control. Train will stop.");
		}
		else{
			// check input is valid number
			if(!isValidDuty(usrBuf)){
				badFlag = 1;
				continue;
			}
			// send input and 0 fade time
			setDuty(sock, strtol(usrBuf, NULL, 10), 0);
		}
	}while(strcmp("q", usrBuf) != 0);
}

// user simpy enters desired duty cycle and fade time
void directControlFade(int sock){
	char usrBuf[MAXDATASIZE+1];
	int badFlag = 0;
	do {
		int cur_duty = getDuty(sock);
		system("clear");
		// get current duty cycle
		if(badFlag){
			printf("%s is not a valid entry.\nPlease enter a number between -100 and 100 and a positive number or q.\n\n", usrBuf);
			badFlag = 0;
		}
		// get current duty cycle
		printf("Current duty cycle is %d%%.\nEnter new duty cycle and fade time separated by a comma or q to quit.\n> ", cur_duty);

		// remove any input during delay
		struct pollfd fds = {0, POLLIN, 0};
		poll(&fds, 1, 0);
		while(fds.revents == POLLIN){
			int c;
			while((c=fgetc(stdin)) != '\n' && c != EOF); // eat extra lines
			poll(&fds, 1, 0);
		}

		// get user input
		scanf("%"XSTR(MAXDATASIZE)"s", usrBuf);

		// eat extraneous characters
		int c;
		while((c=fgetc(stdin)) != '\n' && c != EOF); // eat extra chars

		system("clear");
		printf("Please wait\n");

		// deal with input
		if (strcmp("q", usrBuf) == 0) {
			setDuty(sock, 0, 0);
			printf("\nQuitting direct control. Train will stop.");
		}
		else{
			// check input is valid number
			char *dutyStr = strtok(usrBuf, ",");
			char *fadeStr = strtok(NULL, ",");
			if(!isValidDuty(dutyStr)){
				badFlag = 1;
				continue;
			}
			if(!isValidFade(fadeStr)){
				badFlag = 1;
				continue;
			}
			// send input and 0 fade time
			setDuty(sock, strtol(dutyStr, NULL, 10), strtol(fadeStr, NULL, 10));
		}
	}while(strcmp("q", usrBuf) != 0);
}

// returns 1 if str represents an int between -100 and 100 (inclusive) returns 0 otherwise
int isValidDuty(char *str){
	if(str == NULL)
		return 0;
	int i = 0;
	if(str[0] == '-' || str[0] == '+')
		i = 1;
	for(; i < strlen(str); i++){
		if(!isdigit(str[i])){
			return 0;
		}
	}
	if(strtol(str, NULL, 10) < -100 || strtol(str, NULL, 10) > 100)
		return 0;
	return 1;
}

int isValidFade(char *str){
	if(str == NULL)
		return 0;
	for(int i = 0; i < strlen(str); i++){
		if(!isdigit(str[i])){
			return 0;
		}
	}
	return 1;
}

// requests current duty from esp32 and returns as int
int cur_duty = 0;
int getDuty(int sock){
	char buf[MAXDATASIZE+1] = "42";
	tcp_send(sock, "0");
	tcp_recv(sock, buf);
#ifdef NO_NETWORK
	return cur_duty;
#endif
	return strtol(buf, NULL, 10);
}

// sends duty and fade time to server
void setDuty(int sock, int duty, int time){
	char buf[MAXDATASIZE+1];
	snprintf(buf, MAXDATASIZE, "1 %d %d", duty, time);
	tcp_send(sock, buf);
#ifdef NO_NETWORK
	cur_duty = duty;
#endif
}

// revives MAXDATASIZE bytes from server into buf and null terminates
void tcp_recv(int sock, char *buf){
#ifndef NO_NETWORK
	int len;
	if ((len = recv(sock, buf, MAXDATASIZE, 0)) <= 0) {
		perror("receive");
		exit(len);
	}
	buf[len] = '\0';
#ifdef VERBOSE
	printf("\nrecived %s in %d bytes\n", buf, len);
#endif
#endif
#ifdef NO_NETWORK
	sleep(1);
#endif
}

// sends message of arbitrary length to server
void tcp_send(int sock, char *buf){
	int len = strlen(buf);
#ifndef NO_NETWORK
	int to_write = len;
	while (to_write > 0) {
		int written = send(sock, buf + (len - to_write), to_write, 0);
		if (written < 0) {
			perror("send");
			exit(written);
		}
		to_write -= written;
	}
#ifdef VERBOSE
	printf("\nsent %s in %d bytes\n", buf, len);
#endif
#endif
}