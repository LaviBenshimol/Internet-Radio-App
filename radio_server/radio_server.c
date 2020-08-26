#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
/*	SENDER		*/

#define BUFF_SIZE 1024
#define TIMETOLIVE 15
/*messages we send*/
#define WELCOME 0
#define ANNOUNCE 1
#define PERMITSONG 2
#define INVALIDCOMMAND 3
#define NEWSTATIONS 4

/*messages we can receive*/
#define HELLO 0
#define ASKSONG 1
#define UPSONG 2

/* A linked list node */
typedef struct udpNodes {
	int stationFD;
	int stationNumber;
	uint8_t songNameSize;
	char* songName;
	FILE *fp;
	struct sockaddr_in station;
	struct udpNodes *next;
} udpNode;

typedef struct tcpClients {     
	int clientFD;
	pthread_t th;
	struct sockaddr_in client;
	struct tcpClients *next;
} tcpClient;

void* welcome_func();
void* TCPManager(void* client);
void shutdown_and_goodbye();
void invalid_command(int type, tcpClient *client);
void removeClinet(tcpClient* del);
void releaseUdpNode();
void releaseTCPclients();

udpNode *head = NULL, *temp, *temp2;
tcpClient *TCPhead = NULL, *Ttemp = NULL, *Ttemp2 = NULL;
int welcome_socket, client_socket_fd, noOfClients = 0, portnoTCP, uploadFlag = 0, qFlaf = 1;
uint8_t temp8;
uint16_t numSongs, temp16, temp162, portnoUDP;
uint32_t mCastG, temp32;
struct sockaddr_in cli_addr;
pthread_mutex_t lock;
pthread_t welcome_th;
u_char ttl = TIMETOLIVE;

int main(int argc, char *argv[]) {
	socklen_t addrSize;
	int threadVal, one = 1;
	int server_socket_fd,state;
	struct sockaddr_in serv_addr;
	int n, i, byte_no, counter = 0, sum_of_data_sent = 0;
	char *songBuff[BUFF_SIZE];
	// error check command line arguments
	if (argc < 4) {
		fprintf(stderr,"ERROR, arguments are missing\n");
		return 1;
	}
	portnoUDP = atoi(argv[3]);
	mCastG = inet_addr(argv[2]);
	numSongs = argc - 4;
	//opening udp threads for songs received in argv
	for(i = 0; i < numSongs; i++) {
		temp = (udpNode *)malloc(sizeof(udpNode));
		if(!temp) {
			printf("ERROR malloc udpNode\n");
			shutdown_and_goodbye();
			return 1;
		}
		temp -> songNameSize = strlen(argv[4+i]);
		temp -> stationNumber = i;
		temp -> songName = (char*)malloc(sizeof(temp -> songNameSize + 1));
		if(!temp->songName) {
			printf("ERROR malloc udpNode - song name\n");
			free(temp);
			shutdown_and_goodbye();
			return 1;	
		}
		memcpy(temp -> songName, argv[4 + i], strlen(argv[4+i]));
		temp -> songName[temp->songNameSize] = '\0';
		temp -> station.sin_family = AF_INET;
		temp -> station.sin_port = htons(portnoUDP);
		temp32 = inet_addr(argv[2]) + htonl(i);
		temp->station.sin_addr.s_addr = temp32;
		temp->stationFD = socket(AF_INET, SOCK_DGRAM, 0);
		if(temp->stationFD < 0) {
			perror("ERROR in opening UDP socket\n");
			free(temp -> songName);
			shutdown_and_goodbye();
			return 1;
		}
		setsockopt(temp -> stationFD, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
		temp -> fp = fopen((const char*)(temp -> songName), "r");
		if(temp -> fp == NULL) {
			printf("ERROR opening file\n");
			if(close(temp -> stationFD) < 0)
				perror("ERROR closing UDP socket\n");
			free(temp -> songName);
			shutdown_and_goodbye();
			return 1;
		}
		if(i == 0) {
			head = temp;
			temp2 = head;
			head -> next = NULL;
		} else {
			temp2 -> next = temp;
			temp2 = temp2 -> next;
			temp = temp2;
		}
	}
	temp = NULL;
	// setup socket
	welcome_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (welcome_socket < 0) {	
		perror("ERROR opening socket");
		shutdown_and_goodbye();
		return 1;
	}
	// setup server information
	memset((char *) &serv_addr, 0, sizeof(serv_addr));
	portnoTCP = atoi(argv[1]); //tcp port
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(portnoTCP);
	memset(serv_addr.sin_zero, '\0', sizeof(serv_addr.sin_zero));  

	// resolve binding issues in the os
	setsockopt(welcome_socket,SOL_SOCKET,SO_REUSEADDR,(char *) &one,sizeof(int));

	// bind the socket to an address
	if (bind(welcome_socket, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
		perror("ERROR on binding");
		shutdown_and_goodbye();
		return 1;
	}
	
	threadVal = pthread_create(&welcome_th, NULL, welcome_func, NULL);
	if(threadVal != 0 ) {
		printf("ERROR opening welcome thread\n");
		shutdown_and_goodbye();
		return 1;
	}
	while(qFlaf) {
		temp = head;
		while(temp != NULL) {
			byte_no = fread(songBuff, 1, BUFF_SIZE, temp -> fp);
			if(byte_no == 0) {
				fseek(temp -> fp, 0, SEEK_SET);
				byte_no = fread(songBuff, 1, BUFF_SIZE, temp -> fp);
			}
			//printf("byte_no = %d\n", byte_no);
			if(sendto(temp->stationFD, songBuff, sizeof(songBuff), 0, (struct sockaddr*) &temp->station, sizeof(temp->station)) < 0) {
				perror("ERROR in sending data:");
				shutdown_and_goodbye();
				return 1;
			}
			temp = temp->next;
		}
		usleep(62500);
	}
	shutdown_and_goodbye();
	pthread_join(welcome_th, NULL);
	return 1;
}


void* welcome_func() {
	int thVal, retVal, i;
	char input[10], addr[INET_ADDRSTRLEN], buff[3];
	tcpClient *cPtr;
	socklen_t addr_size;
	fd_set fds;
	udpNode *wPtr;
	
	printf("Entering welcome thread\n");
	listen(welcome_socket, 100);  // limit to 1 and try to add 2.
	addr_size = sizeof(cli_addr);
	while(qFlaf) {
		memset(input, '\0', 10);
		printf("Please type q to quit or p to print the stations that we are advertising and the tcp clients\n");
		FD_ZERO(&fds);
		FD_SET(welcome_socket, &fds);
		FD_SET(fileno(stdin), &fds);
		retVal = select(FD_SETSIZE, &fds, NULL, NULL, NULL);
		if(retVal <= 0 && (errno != EINTR)) {
			printf("ERROR in select, HELLO\n");
			shutdown_and_goodbye();
			qFlaf = 0;
			pthread_exit(&welcome_th);
		}
		if(FD_ISSET(welcome_socket, &fds)) {
			noOfClients++;
			Ttemp = (tcpClient*)malloc(sizeof(tcpClient));
			if(Ttemp == NULL) {
				printf("ERROR allocating memory for tcp client\n");
				shutdown_and_goodbye();
				qFlaf = 0;
				pthread_exit(&welcome_th);
			}
			Ttemp -> client.sin_family = AF_INET;
			Ttemp -> client.sin_addr.s_addr = htonl(INADDR_ANY);
			Ttemp -> client.sin_port = htons(portnoTCP);
			Ttemp -> clientFD = accept(welcome_socket, (struct sockaddr *) &(Ttemp -> client), &addr_size);
			thVal = pthread_create(&(Ttemp -> th), NULL, TCPManager, Ttemp);
			if(thVal != 0) {
				printf("ERROR opening thread\n");
				shutdown_and_goodbye();
				qFlaf = 0;
				pthread_exit(&welcome_th);
			}
			Ttemp -> next = NULL;
			if(noOfClients == 1) {
				TCPhead = Ttemp;
				Ttemp2 = Ttemp;
				Ttemp2 -> next = NULL;
			} else {
				Ttemp2 -> next = Ttemp;
				Ttemp2 = Ttemp;
				Ttemp = Ttemp -> next;
			}
		} else if (FD_ISSET(fileno(stdin), &fds)) {
			scanf("%10s", input);
			if(strlen(input) != 1) 
				input[0] = 0; ///so it will go to default
			switch(input[0]) {
				case 'p':
					printf("We have %d stations:\n",  numSongs);
					wPtr = head;
					while(wPtr != NULL) {
						printf("Station %d: %s\n",wPtr -> stationNumber, inet_ntoa(wPtr->station.sin_addr));
						printf("%s\n", wPtr ->  songName);						
						wPtr = wPtr -> next;
					}
					printf("We have %d clients:\n", noOfClients);
					cPtr = TCPhead;
					i = 1;
					while(cPtr != NULL) {
						printf("Client %d with an ip address: %s\n", i, inet_ntoa(cPtr -> client.sin_addr));
						cPtr = cPtr -> next;
						i++;
					}
					break;
				case 'q':
						printf("We are closing the server after user request, we free all the memory and close all the sockets\n");
						shutdown_and_goodbye();
						qFlaf = 0;
					break;
				default:
						printf("Please retry by enter a valid command\n");
			} //switch
		} //else if
	} //while
}

void* TCPManager(void* client) {
	int i=0, helloFlag = 1, retVal, byteCount, num_parts, alreadyHave = 0, tcpqFlag = 1;
	tcpClient *cl = (tcpClient*)client, *tPtr;
	fd_set fds;
	uint8_t commandType;
	uint16_t staNum;
	char buffer[BUFF_SIZE], inbox[BUFF_SIZE + 1];
	udpNode *sPtr, *tempi;
	char *newSongName;
	//struct for Timeout in select
	struct timeval timeout;
	/*if there is a problem with a specific client we are closing him only*/
	while(tcpqFlag) {
		FD_ZERO(&fds);
		FD_SET(cl -> clientFD , &fds);
		if(helloFlag) { 
			//define the time for timeout
			timeout.tv_sec = 0;
			timeout.tv_usec = 300000;
			retVal = select(FD_SETSIZE, &fds, NULL, NULL, &timeout);
		} else {
			retVal = select(FD_SETSIZE, &fds, NULL, NULL, NULL);
		}
		if(retVal <= 0 && (errno != EINTR)) {
			if(helloFlag) {
				printf("Timeout - didn't received HELLO\nRemoving client\n");
				removeClinet(cl);
			}
			break;
		}
		if(FD_ISSET(cl -> clientFD, &fds)) {
			if((byteCount = read(cl -> clientFD, inbox, BUFF_SIZE)) <= 0) {
				printf("client closed the connection\n");
				removeClinet(cl);
				tcpqFlag = 0;
				break;
			}
			commandType = (uint8_t) inbox[0];
			switch(commandType) {
				case HELLO:
					if(!helloFlag) {
						invalid_command(3, cl);
						tcpqFlag = 0;
						break;
					}
					if(byteCount != 3) {
						invalid_command(6, cl);
						tcpqFlag = 0;
						break;
					}
					if(inbox[1] || inbox[2]) {
						invalid_command(6, cl);
						tcpqFlag = 0;
						break;
					}
					helloFlag = 0;
					buffer[0] = WELCOME;
					temp162 = htons(numSongs);
					temp16 = htons(portnoUDP);
					temp32 = htonl(mCastG);
					memcpy(buffer + 1, &temp162, 2);
					memcpy(buffer + 3, &temp32, 4);
					memcpy(buffer + 7, &temp16, 2);
					if(send(cl -> clientFD, &buffer, 9, 0) != 9) {
						printf("wrong amount of byte sent\n");
						removeClinet(cl);
						tcpqFlag = 0;
						break;
					}
					break;
				case ASKSONG:
					if(helloFlag) {
						invalid_command(2, cl);
						tcpqFlag = 0;
						break;
					}
					if(byteCount != 3) {
						invalid_command(6, cl);
						tcpqFlag = 0;
						break;
					}
					staNum = 0;
					staNum = inbox[1]*10;
					staNum += inbox[2];
					if(staNum >= numSongs) {
						invalid_command(2, cl);
						tcpqFlag = 0;
						break;
					}
					sPtr = head;
					while(sPtr != NULL) {
						if(sPtr -> stationNumber == staNum)
							break;
						sPtr = sPtr -> next;
					}
					memset(buffer, '\0', BUFF_SIZE);
					buffer[0] = ANNOUNCE;
					buffer[1] = sPtr -> songNameSize;
					memcpy(buffer + 2, sPtr -> songName, sPtr -> songNameSize);
					if((byteCount = send(cl -> clientFD, &buffer, 2 + (sPtr -> songNameSize), 0)) != 2 + (sPtr -> songNameSize)) {
						printf("announce: wrong amount of byte sent\n");
						removeClinet(cl);
						tcpqFlag = 0;
					}
					break;
				case UPSONG:
					if(helloFlag) {
						invalid_command(2, cl);
						tcpqFlag = 0;
						break;
					}
					if(byteCount < 5) {
						invalid_command(6, cl);
						tcpqFlag = 0;
						break;
					}
					temp32 = htonl(*(((uint32_t*) (&(inbox)[1]))));
					if(temp32 < 2000 || temp32 > 10000000) {
						invalid_command(4, cl);
						tcpqFlag = 0;
						break;
					}
					temp8 = inbox[5];
					newSongName = (char*)malloc(temp8 + 1);
					memcpy(newSongName, (inbox + 6), temp8 + 1);
					newSongName[temp8] = '\0';
					tempi = head;
					while(tempi != NULL) {
						if(strcmp(tempi -> songName, newSongName) == 0) {
							alreadyHave = 1;
							printf("This song has a station\n");
							break;
						}
						tempi = tempi -> next;
					}
					if(uploadFlag || alreadyHave) { /*decline request for UpSong - someone else is uploading*/
						buffer[0] = PERMITSONG;
						buffer[1] = 0;
						free(newSongName);
						if((byteCount = send(cl -> clientFD, &buffer, 2, 0)) != 2) {
							printf("Wrong amount of byte sent\n");
							removeClinet(cl);
							tcpqFlag = 0;
							break;
						}
						alreadyHave = 0;
					} else {
						uploadFlag = 1;
						/*sending approval*/
						buffer[0] = PERMITSONG;
						buffer[1] = 1;
						if((byteCount = send(cl -> clientFD, &buffer, 2, 0)) != 2) {
							printf("Wrong amount of byte sent\n");
							removeClinet(cl);
							tcpqFlag = 0;
							break;
						}
						/*arranging the new song node*/
						sPtr = (udpNode*)malloc(sizeof(udpNode));
						sPtr -> songNameSize = temp8;
						sPtr -> songName = newSongName;
						if((sPtr -> fp = fopen(sPtr -> songName, "w")) == NULL) {
							printf("ERROR opening file\n");
							removeClinet(cl);
							tcpqFlag = 0;
							break;
						}
						num_parts = 0;
						/*receiving song*/
						while(num_parts < temp32) {
							timeout.tv_sec = 0;
							timeout.tv_usec = 3000000;
							FD_ZERO(&fds);
							FD_SET(cl -> clientFD , &fds);
							printf("waiting %d..\n", ++i);
							retVal = select(FD_SETSIZE, &fds, NULL, NULL, &timeout);
							if(retVal <= 0 && (errno != EINTR)) {
								uploadFlag = 0;
								invalid_command(7,cl);
								fclose(sPtr -> fp);
								free(sPtr -> songName);
								free(sPtr);
								break;//breaks the while
							}
							/*need to check if need to add timeout here!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
							if((byteCount = read(cl -> clientFD, inbox, BUFF_SIZE)) <= 0) {
								printf("ERROR receiving song\n");
								fclose(sPtr -> fp);
								free(sPtr -> songName);
								free(sPtr);
								removeClinet(cl);
								tcpqFlag = 0;
								break;//breaks the while
							}
							num_parts += byteCount;
							fwrite(inbox, 1, byteCount, sPtr -> fp);
						}
						if(num_parts < temp32)
							break;//to break the switch in case of an error
						printf("FINISHED RECEIVING\n");
						/*error - in file we need to sent with. closing server*/
						if((fclose(sPtr -> fp)) != 0) {
							printf("ERROR closing file\n");
							free(sPtr -> songName);
							free(sPtr);
							qFlaf = 0;
							tcpqFlag = 0;
							break;
						}
						/*error - in file we need to sent with. closing server*/
						if((sPtr -> fp = fopen(sPtr -> songName, "r")) == NULL) {
							printf("ERROR opening file\n");
							free(sPtr -> songName);
							free(sPtr);
							qFlaf = 0;
							tcpqFlag = 0;
							break;
						}						
						printf("Opened file for reading\n");
						numSongs++;
						sPtr -> stationNumber = numSongs - 1;
						sPtr -> next = NULL;
						sPtr -> station.sin_family = AF_INET;
						sPtr -> station.sin_port = htons(portnoUDP);
						sPtr -> station.sin_addr.s_addr = mCastG + htonl(numSongs - 1);
						sPtr -> stationFD = socket(AF_INET, SOCK_DGRAM, 0);
						printf("opened socket\n");
						/*error - opening UDP socket. closing server*/
						if(sPtr -> stationFD < 0) {
							perror("ERROR in opening UDP socket\n");
							fclose(sPtr -> fp);
							free(sPtr -> songName);
							free(sPtr);
							qFlaf = 0;
							tcpqFlag = 0;
							break;
						}
						setsockopt(sPtr -> stationFD, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
						/*adding the song to the list*/
						tempi = head;
						while(tempi->next != NULL)
							tempi = tempi -> next;
						tempi -> next = sPtr;
						printf("new song struct added\n");
						tPtr = TCPhead;
						while(tPtr != NULL) {						
							memset(buffer, '\0', BUFF_SIZE);
							buffer[0] = NEWSTATIONS;
							temp16 = htons(numSongs);
							memcpy(buffer + 1, &temp16, 2);
							if((byteCount = send(tPtr -> clientFD, &buffer, 3, 0)) != 3) {
								printf("Wrong amount of byte sent (newStation)\n");
								shutdown_and_goodbye();
								tcpqFlag = 0;
								qFlaf = 0;
							}
							tPtr = tPtr -> next;
						}
						printf("NEW STATION SENT\n");
						uploadFlag = 0;
					}
					break;
				default:
					invalid_command(5, cl);
			}//switch
		}//if fd_isset
	}//while 1
}

/* invalid command list:
	1. station no. does not exist.
	2. another message received before hello.
	3. more than one hello sent.
	4. file size is incompatible.
	5. wrong command type.
	6. wrong message format.
	7. uploading song stopped in the middle
*/

void invalid_command(int type, tcpClient *client) {
	char *str, buffer[BUFF_SIZE];
	int byteCount;
	uint8_t len;
	memset(buffer, '\0', BUFF_SIZE);
	buffer[0] = INVALIDCOMMAND;
	switch(type) {
			case 1:
				printf("Station number given does not exist\n");
				str = "Station number given does not exist";
				len = strlen(str);
				buffer[1] = len;
				break;
			case 2:
				printf("Hello did not sent yet from user and another message was received\n");
				str = "Hello did not sent yet from user and another message was received";
				len = strlen(str);
				buffer[1] = len;
				break;
			case 3:
				printf("More than one HELLO sent\n");
				str = "More than one HELLO sent";
				len = strlen(str);
				buffer[1] = len;
				break;
			case 4:
				printf("File size is incompatible\n");
				str = "File size is incompatible";
				len = strlen(str);
				buffer[1] = len;
				break;
			case 5:
				printf("Unknown command received\n");
				str = "Unknown command received";
				len = strlen(str);
				buffer[1] = len;
				break;
			case 6:
				printf("Wrong message format\n");
				str = "Wrong message format";
				len = strlen(str);
				buffer[1] = len;
				break;
			case 7:
				printf("Uploading song did not succeed\n");
				str = "Uploading song did not succeed";
				len = strlen(str);
				buffer[1] = len;
				break;
	}//switch
	memcpy(buffer + 2, str, len);
	if((byteCount = send(client -> clientFD, &buffer, 2 + len, 0)) < 0)
		printf("ERROR sending invalid message\n");
	removeClinet(client);
}

void removeClinet(tcpClient* del) {
	pthread_mutex_lock(&lock);
	Ttemp = TCPhead;
	if(del == TCPhead) {
		if(del -> next == NULL) {//if there is only one client
			if(close(del -> clientFD) < 0)
				perror("ERROR closing socket\n");		
			free(del);
			noOfClients = 0;
			TCPhead = NULL;
		} else {
			Ttemp2 = TCPhead -> next;
			if(close(del -> clientFD) < 0)
				perror("ERROR closing socket\n");
			free(del);
			noOfClients = 0;
			TCPhead = Ttemp2;
		}
	} else {//if del == head
		while(Ttemp != del) {
			Ttemp2 = Ttemp; //holds the prev
			Ttemp = Ttemp -> next;
		}
		Ttemp2 -> next = Ttemp -> next; //prev points to the next problem with NULL ?
		noOfClients--;
		if(close(del -> clientFD) < 0)
				perror("ERROR closing TCP socket\n");
		free(Ttemp);
	}//else
	pthread_mutex_unlock(&lock);
}

/*free allocation*/
void shutdown_and_goodbye() {
	tcpClient *ptr;			
	ptr = TCPhead;
	if(TCPhead != NULL) {
		while(ptr != NULL) {
			pthread_cancel(ptr -> th);
			ptr = ptr -> next;
		}
		releaseTCPclients();
		TCPhead = NULL;
	}
	if(head != NULL) {
		releaseUdpNode();
		head = NULL;
	}
}

void releaseUdpNode() {
	udpNode *uPtr = head, *uPtr2;
	if(uPtr -> next == NULL) { //only one in the list
		fclose(uPtr -> fp);
		if(close(uPtr -> stationFD) < 0)
			perror("ERROR closing TCP socket\n");
		free(uPtr -> songName);
		free(uPtr);
	} else {
		uPtr2 = uPtr -> next;
		while(uPtr2 != NULL) {
			fclose(uPtr -> fp);
			if(close(uPtr -> stationFD) < 0)
				perror("ERROR closing TCP socket\n");
			free(uPtr -> songName);
			free(uPtr);
			uPtr = uPtr2;
			uPtr2 = uPtr2 -> next;
		}
		fclose(uPtr -> fp);
		if(close(uPtr -> stationFD) < 0)
			perror("ERROR closing TCP socket\n");
		free(uPtr -> songName); //release the last one
		free(uPtr);
	}
	printf("Released songs list allocations and closed files\n");
}

void releaseTCPclients() {
	tcpClient *aPtr = TCPhead, *aPtr2;
	if(aPtr -> next = NULL) {
		if(close(aPtr -> clientFD) < 0)
			perror("ERROR closing TCP socket\n");
		free(aPtr);
	} else {
		aPtr2 = aPtr -> next;
		while(aPtr2 != NULL) {
			if(close(aPtr -> clientFD) < 0)
				perror("ERROR closing TCP socket\n");
			free(aPtr);
			aPtr = aPtr2;
			aPtr2 = aPtr2 -> next;
		}
		free(aPtr);
	}
	printf("Released client list\n");
}