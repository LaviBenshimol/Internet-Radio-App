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
#include <sys/stat.h>

#define BUFF_SIZE 1024
/*message we send*/
#define HELLO 0
#define ASKSONG 1
#define UPSONG 2
/*message we receive*/
#define WELCOME 0
#define ANNOUNCE 1
#define PERMITSONG 2
#define INVALID 3
#define NEWSTATION 4
 
void* listenRadio();
void shutdown_and_goodbye(); /*closing socket and kill the thread*/

uint8_t commandType, songNameSize;
uint16_t reserved, numStations = 0, server_radio_port, stnm;
uint32_t songSize, multicastGroup, temp32;
//struct for temp data - for displaying data
struct in_addr temp;
//UDP Socket and streaming initialing
int udp_sock, client_socket_fd, changeStation = 0, staNum = 0, qFlag = 1, retE;
socklen_t addr_size;
pthread_t radio_th;
struct sockaddr_in radio_addr;
FILE *fp;

int main(int argc, char *argv[]) {
	//declaring variables
	int port_no, num_parts, byte_no, retVal, threadVal, i, permit, bytesRead;
	/*ask - askSong sent and not yet answered, permit - permit sent and not yet answered, newFlag - upload finished 
	but newStation not yet received*/
	int askFlag = 0, permitFlag = 0, newFlag = 0; 
	char buffer[BUFF_SIZE + 1], inbox[BUFF_SIZE + 1], input[10], *songAnnounced, *replyString, recName[201];
	buffer[BUFF_SIZE] = '\0';
	char buff[3];
	uint8_t replyStringSize;
	fd_set fds;
	FILE *fp;
	struct sockaddr_in server_addr; 
	struct stat st;


	//struct for Timeout in select
	struct timeval timeout;
	
	// Read command line arguments, need to get server name(ip) and server port (number)
	if (argc < 3) {
		fprintf(stderr,"usage %s hostname port\n", argv[0]);
		return 1;
	}
	// Convert the port argv to the appropriate data type
	port_no = atoi(argv[2]);

	// setup the TCP socket 
	if((client_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("ERROR openning socket");
		return 1;
	}
	// clear our the struct server_addr 
	memset((char *) &server_addr, 0, sizeof(server_addr));

	// set up the TCP socket 1
	server_addr.sin_family = AF_INET;					//IPv4
	server_addr.sin_port = htons(port_no);				//host to network byte order
	server_addr.sin_addr.s_addr = inet_addr(argv[1]); 	//turn char to int as needed in the struct
	memset(server_addr.sin_zero, '\0', sizeof(server_addr.sin_zero));  
	
	// trying to connect to the server (TCP socket)
	if (connect(client_socket_fd,(struct sockaddr *) &server_addr,sizeof(server_addr)) < 0) { 
		printf("no connection\n");
		perror("ERROR connecting");
		return 1;
	}
//	printf("trying reading\n");
	/*check if you get welcome before sending hello*/////////////////////////////////////////NOT WORKING
/*	if((byte_no = read(client_socket_fd, inbox, BUFF_SIZE)) > 0) {
		printf("got a server from server before sending Hello\n");
		if(inbox[0] == 0) {
			printf("Received Welcome before hello\nGoodbye\n");
			close(client_socket_fd);
			return 1;
		}
	}*/
	printf("Sending Hello\n");
	//send hello
	byte_no = 0;
	reserved = 0;
	buff[0] = HELLO;
	buff[1] = htons(reserved);
	byte_no = send(client_socket_fd,&buff,sizeof(buff),0);
	if(byte_no != 3 || byte_no < 0) {
		printf("ERROR sending Hello\nClosing socket and communication\n");
		if(close(client_socket_fd) < 0) {
			perror("ERROR closing TCP socket\n");
		}
		return 1;
	}
	//wait for response using select
	timeout.tv_sec = 0;
	timeout.tv_usec = 300000;
	
	/*waiting for Welcome*/
	FD_ZERO(&fds); 
	FD_SET(client_socket_fd, &fds);
	retVal = select(FD_SETSIZE, &fds, NULL, NULL, &timeout);
	if(retVal <= 0 && (errno != EINTR)) {
		printf("Timeout - didn't received WELCOME\nClosing the communication\n");
		if(close(client_socket_fd) < 0)
			perror("ERROR closing TCP socket\n");
		return 1;
	}
	if((byte_no = read(client_socket_fd, inbox, BUFF_SIZE)) <= 0) {
		perror("Can not read from socket\nClosing socket and communication\n");
		if(close(client_socket_fd) < 0)
			perror("ERROR closing TCP socket\n");
		return 1;
	}
	if(commandType = (uint8_t) inbox[0] != 0) {
		printf("Message type is not Welcome\nClosing socket and communication\n");
		if(close(client_socket_fd) < 0)
			perror("ERROR closing TCP socket\n");
		return 1;
	}
	if(byte_no != 9) {
		printf("Wrong message format\nClosing socket and communication\n");
		if(close(client_socket_fd) < 0)
			perror("ERROR closing TCP socket\n");
		return 1;
	}
	/*handle welcome*/
	numStations = inbox[1]*10 + inbox[2];
	multicastGroup = htonl(*(((uint32_t*) (&(inbox)[3]))));
	server_radio_port = htons(*(((uint16_t*) (&(inbox)[7]))));
	temp.s_addr = multicastGroup;
	// routine with SERVER
	printf("Welcome to Lavi and Inbal's radio fun program!\n");
	printf("The number of stations is: %u \n multicastGroup is: %s \n radioPOrt is: %u\n",numStations,inet_ntoa(temp),server_radio_port);

	radio_addr.sin_family = AF_INET;
	radio_addr.sin_port = htons(server_radio_port);
	radio_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	/*UDP thread - for listening*/
	threadVal = pthread_create(&radio_th, NULL, listenRadio, NULL);
	if(threadVal != 0) {
		printf("ERROR opening thread for radioClosing socket and communication\n");
		if(close(client_socket_fd) < 0)
			perror("ERROR closing TCP socket\n");
		return 1;
	}

	while(qFlag) {
		FD_ZERO(&fds);
		FD_SET(fileno(stdin), &fds);
		FD_SET(client_socket_fd, &fds);
		//wait for response using select
		if(newFlag) {
			timeout.tv_sec = 2;
			timeout.tv_usec = 0;
		} else {		
			timeout.tv_sec = 0;
			timeout.tv_usec = 300000;
		}
		memset(buff, '\0', sizeof(buff));
		printf("Please enter a station number, 's' to send a song or 'q' to quit.\n");
		if(newFlag || permitFlag || askFlag)
			retVal = select(FD_SETSIZE, &fds, NULL, NULL, &timeout);
		else
			retVal = select(FD_SETSIZE, &fds, NULL, NULL, NULL);
		if(retVal <= 0 && (errno != EINTR)) {
			if(askFlag) {
				printf("Timeout - didn't received ANNOUNCE\n");
				shutdown_and_goodbye(); //closing thread & sockets
			}
			if(permitFlag) {
				printf("Timeout - didn't received PermitSong\n");
				shutdown_and_goodbye(); //closing thread & sockets
			}
			if(newFlag) {
				printf("Timeout - didn't received NewStation after finishing uploading song\n");
				shutdown_and_goodbye(); //closing thread & sockets
			}
			return 1;
		}
		if(FD_ISSET(fileno(stdin), &fds)) {
			scanf("%10s",input);
			/*calculating the station number we got from the user*/
			if(input[0] >= '0' && input[0] <= '9') {
				i = 0; staNum = 0;
				while(input[i] != '\0') {
					staNum *= 10;
					staNum += input[i] - 48;			
					i++;
				} // while reading input
				if(staNum < numStations) {
					changeStation = 1;
					buff[0] = ASKSONG;
					stnm = htons(staNum);
					memcpy((buff+1),&stnm,2);
					if(send(client_socket_fd, &buff, 3, 0) != 3) {
						printf("ERROR sending AskSong\n");
						shutdown_and_goodbye();
						return 1;
					}
					askFlag = 1;
				} else {
					printf("Station %d does not exist\n", staNum);
				}
			} else {
				if(strlen(input) != 1)
					input[0] = 0;//so it will enter default 
				switch (input[0]) {
					case 's':
						memset(buffer, '\0', BUFF_SIZE);
						memset(recName, '\0', 200);
						buffer[0] = 2;
						printf("Please enter a valid song name to transmit (in the form 'song.mp3'), if you want quit enter anything else\n");
						printf("Please enter less than 200 characters and press enter\n");
						scanf("%s", recName);
						if((songNameSize = strlen(recName)) >= 200) {
							printf("The name you entered is longer than 200 characters\n");           //// check
						} else {
							if((fp = fopen(recName, "r")) == NULL) {
								printf("The song: %s file doesn't exists or cannot be opened\n", recName);
								break;
							} 
							stat(recName, &st);
							temp32 = st.st_size;
							if(temp32 < 2000 || temp32 > 10000000) {
								printf("File size is incompatible\n"); //// check
								break;
							} else {
								songSize = htonl(temp32);
								memcpy(buffer + 1, &songSize, 4);
								memcpy(buffer + 6, recName, songNameSize);
								buffer[5] = songNameSize;
								songSize = st.st_size;
								permitFlag = 1;
								if((byte_no = send(client_socket_fd, &buffer, songNameSize + 6, 0)) != songNameSize + 6) {
									printf("ERROR sending AskSong\n");
									shutdown_and_goodbye();
								}
								printf("SSSEEENNND\n");
							}
						}
						break;
					case 'q':
						qFlag = 0;
						break;
					default :
						printf("Please retry by enter a valid station number or 'q' to quit\n");
				}//switch
			}//else
			memset(input, '\0', sizeof(input));
		} // if input - stdin
		else if(FD_ISSET(client_socket_fd, &fds)) {
			printf("Got a message from server\n");
			if((byte_no = read(client_socket_fd, inbox, BUFF_SIZE)) <= 0) {
				printf("The server closed the connection!\n");
				shutdown_and_goodbye();
				return 1;
			}
			commandType = (uint8_t) inbox[0];
			switch(commandType) {
				case WELCOME:
						/*we already dealt with welcome = received more than one*/
						printf("More than one WELCOME received\n");
						shutdown_and_goodbye();
						return 1;
					break;
				case ANNOUNCE:
					if(permitFlag) {
						printf("Got announce instead of permit\n");
						shutdown_and_goodbye();
						return 1;
					}
					if(!askFlag) { //got announce without sending AskDong
						printf("Got announce before AskSong\n");
						shutdown_and_goodbye();
						return 1;
					}
					if(byte_no <= 2) {
						printf("Wrong message format\n");
						shutdown_and_goodbye();
						return 1;
					}
					askFlag = 0;
					songNameSize = inbox[1];
					songAnnounced = (char*)malloc(songNameSize + 1);
					if(!songAnnounced) {
						printf("malloc failed\n");
					}
					memcpy(songAnnounced,(inbox+2),songNameSize + 1);
					songAnnounced[songNameSize]='\0';
					printf("announce!!!!!!!!\nThe name of the song is:\n %s\n", songAnnounced);
					free(songAnnounced);
					break;
				case PERMITSONG:
					if(askFlag) {//the server answered permit in response to AskSong
						printf("Got permit instead of Announce\n");
						shutdown_and_goodbye();
						return 1;
					}
					if(!permitFlag) { 
						printf("Got permit before sending UpSong\n");
						shutdown_and_goodbye();
						return 1;
					}
					if(byte_no != 2) {
						printf("Wrong message format\n");
						shutdown_and_goodbye();
						return 1;
					}
					permitFlag = 0;
					if(inbox[1] == 1) {
						printf("permission granted, song size = %d\n", (int)songSize);
						byte_no = 0;
						while(byte_no < songSize) {
							memset(buffer, '\0', BUFF_SIZE);
							if((bytesRead = fread(buffer, 1, BUFF_SIZE, fp)) < 0) {
								printf("ERROR reading from file\n");
								shutdown_and_goodbye();
								return 1;
							}
							if((num_parts = send(client_socket_fd, &buffer, bytesRead, 0)) < 0) {
								printf("ERROR in sending file\n");
								shutdown_and_goodbye();
								return 1;
							} else {
								byte_no += num_parts;
							}
							printf("\ruploading %.1lf%%", (double)100*(byte_no)/songSize);
							usleep(8000);
						}
						printf("\nFinished sending song\n");
						newFlag = 1;
					} else {
						printf("The server declined the request\n");
					}
					break;
				case INVALID:
					if(byte_no <= 2) {
						printf("Wrong message format\n");
						shutdown_and_goodbye();
						return 1;
					}
					replyStringSize = inbox[1];
					replyString = (char*)malloc(replyStringSize + 1);
					if(!replyString) {
						printf("malloc failed\n");
						shutdown_and_goodbye();
						return 1;
					}
					memcpy(replyString,(inbox + 2),replyStringSize + 1);
					replyString[replyStringSize] = '\0';
					printf("%s\n",replyString);
					free(replyString);
					shutdown_and_goodbye();
					return 1;
					break;
				case NEWSTATION:
					if(byte_no != 3) {
						printf("Wrong message format\n");
						shutdown_and_goodbye();
						return 1;
					}
					if(newFlag)
						newFlag = 0;
					numStations = 0;
					numStations = inbox[1]*10 + inbox[2];
					printf("There is a NEW STATION!!!!\nNow we have %u stations\n", numStations);
					break;
				default: 
					printf("Recieved unknown message type from server, closing the communication\n");
					if(pthread_cancel(radio_th))
						printf("ERROR canceling thread\n");
					qFlag = 0;
			}
			memset(inbox, '\0', sizeof(inbox));
		}//if msg from server
	}//while qFlag
	printf("We are closing the communication!\n");
	pthread_join(radio_th, NULL);
	// clean up the file descriptors
	if(close(client_socket_fd) < 0)
		perror("ERROR closing socket\n");
	if(close(udp_sock) < 0 )
		perror("ERROR closing UDP socket\n");
	printf("Bye Bye!\n");
	return 0;
}//main

void shutdown_and_goodbye() {
	if(pthread_cancel(radio_th))
		printf("ERROR canceling thread\n");
	if(close(client_socket_fd) < 0)
		perror("ERROR closing TCP socket");
	if(close(udp_sock) < 0 )
		perror("ERROR closing UDP socket");
	printf("closed communication, bye\n");
}

void* listenRadio() {
	printf("listen radio thread\n");
	int song_byte_counter;
	char song_data[BUFF_SIZE];
	struct ip_mreq mreq;
	addr_size = sizeof(radio_addr);
	udp_sock = socket(AF_INET,SOCK_DGRAM,0);
	if(udp_sock < 0) {
		printf("error in opening udp socket\n");
		qFlag = 0;
		pthread_exit(&retE);   //check ?
	}
	if(bind(udp_sock,(struct sockaddr*)&radio_addr,sizeof(radio_addr)) != 0) {
		perror("UDP bind failed\n");
		qFlag = 0;
		pthread_exit(&retE);
	}
	mreq.imr_multiaddr.s_addr = temp.s_addr;
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);

	if(setsockopt(udp_sock,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq)) != 0) {
		perror("ERROR on setsocketopt\n");
		qFlag = 0;
		pthread_exit(&retE);
	}
	fp = popen("play -t mp3 -> /dev/null 2>&1", "w");
	// check if fp is ok
	if(fp == NULL)
		perror("ERROR opening file");
	//start playing
	while (qFlag) {
		if(changeStation) {
			changeStation = 0;
			//change station 
			if(setsockopt(udp_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
				printf("couldnt drop memebership\n");
				qFlag = 0;
				pthread_exit(&retE);
			}
			mreq.imr_multiaddr.s_addr = temp.s_addr + htonl(staNum);
			if(setsockopt(udp_sock,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq)) != 0) {
				printf("ERROR on setsocketoptions\n");
				qFlag = 0;
				pthread_exit(&retE);
			}
		}
		song_byte_counter = recvfrom(udp_sock, song_data, BUFF_SIZE, 0, (struct sockaddr*)&radio_addr, &addr_size);
		if(song_byte_counter > 0)
			fwrite (song_data , sizeof(char), song_byte_counter, fp);
		else {
			printf("ERROR reading from UDP socket\n");
			qFlag = 0;
			pthread_exit(&retE);
		}
	}
	pthread_exit(&retE);
}