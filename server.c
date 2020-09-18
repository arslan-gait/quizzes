#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <pthread.h>

typedef enum { false, true } bool;

#define	QLEN			5
#define	BUFSIZE			4096
#define NUM_GROUPS 		32

int passivesock( char *service, char *protocol, int qlen, int *rport );

typedef struct Player_t {
	char *name;
	int fd;
	int score;
	bool isPresent;
} Player;

typedef struct Question_t {
	char *text;
	char *answer;
	char *winnerName;
	int size;
} Question;

typedef struct Group_t {
	char *topic;
	char *name;
	int maxfds;
	Player **players;
	int size;
	int current;
	int leaderSock;
	fd_set cfd;
	bool hasStarted;
	int questionsNum;
	Question **questions;
} Group;

pthread_mutex_t mainMutex = PTHREAD_MUTEX_INITIALIZER;

int     clientGroups[1024];

int 	numGroups = 0;
fd_set	mainMenu;
int		nfds;
Group 	groups[NUM_GROUPS];

void toString(char *buf, int cc) {
	int p;
	for (p = 0; p < cc; p++) {
		if (buf[p] == '\r' || buf[p] == '\n')
			break;
	}
	buf[p] = '\0';
}

void leave(int fd, bool isForced) {

	int w;
	int sw = 0;
	char buf[50];
	int index = clientGroups[fd];
	puts("Inside leave");

	if (clientGroups[fd] == -1) {
		printf("%d is leader\n", fd);
		sw = 1;
	} else {
		printf("%d is client\n", fd);
		for (w = 0; w < groups[clientGroups[fd]].size; w++) {
			if (groups[index].players[w]->fd == fd) {
				groups[index].players[w]->isPresent = false;
				groups[index].players[w]->fd = 0;
				clientGroups[fd] = 0;
				sw = 2;
				FD_CLR(fd, &groups[index].cfd);
				if ( groups[index].maxfds == fd+1 )
					groups[index].maxfds--;
				groups[index].current -= 1;
				FD_SET(fd, &mainMenu);
				printf("Number of left clients=%d\n", groups[index].current);
				break;
			}
		}
	}
	if (isForced) return;
	switch (sw) {
		case 1:
			strcpy(buf, "BAD|you're not allowed to leave\r\n");
            puts("Not allowed.");
			break;
		case 2:
            puts("Success.");
			strcpy(buf, "OK\r\n");
			break;
		default:
            puts("Not in group.");
			strcpy(buf, "BAD|you're not in any group\r\n");
			break;
	}
	write(fd, buf, strlen(buf));
}

void freeGroup(int index) {
    int i;
    printf("Closing group %s\n", groups[index].name);
    printf("Freeing questions... ");
    for (i = 0; i < groups[index].questionsNum; i++) {
        free(groups[index].questions[i]);
    }
    free(groups[index].questions);
    puts("OK.");
    printf("Freeing players... ");
    for (i = 0; i < groups[index].size; i++) {
        FD_SET(groups[index].players[i]->fd, &mainMenu);
        free(groups[index].players[i]);
    }
    free(groups[index].players);
    puts("OK.");
    fflush(stdout);
    groups[index].leaderSock = 0;
    groups[index].current = 0;
    pthread_mutex_lock(&mainMutex);
    numGroups--;
    groups[index].hasStarted = false;
    groups[index].size = 0;
    groups[index].name = "";
    pthread_mutex_unlock(&mainMutex);
}

int getPlayerIndex(int index, int fdss) {
    int i;
    for (i = 0; i < groups[index].size; i++) {
        if (groups[index].players[i]->fd == fdss) {
            return i;
        }
    }
}

void sendResults(char *buf, int index) {
    char itoa[5];
    int i;

    strcpy(buf, "RESULT|");

    for (i = 0; i < groups[index].size; i++) {
        if (!(groups[index].players[i]->isPresent)) continue;
        strcat(buf, groups[index].players[i]->name);
        strcat(buf, "|");
        sprintf(itoa, "%d", groups[index].players[i]->score);
        strcat(buf, itoa);
        strcat(buf, "|");
    }
    buf[strlen(buf) - 1] = '\0';
    char temp[100];
    strcat(buf, "\r\n");
    sprintf(temp, "ENDGROUP|%s\r\n", groups[index].name);
    strcat(buf, temp);
    printf("RESULT=[%s]\n", buf);
    write(groups[index].leaderSock, buf, strlen(buf));
    for (i = 0; i < groups[index].size; i++) {
        if (groups[index].players[i]->isPresent) {
            write(groups[index].players[i]->fd, buf, strlen(buf));
            leave(i, true);
        }
    }
    write(groups[index].leaderSock, temp, strlen(temp));
}

void *startQuiz(void *arg) {

	int 	index = (int) arg;
	char 	buf[BUFSIZE];
	int 	i;
	fd_set 	rfds;
	fd_set 	ansfds;
	int 	maxAnsfds = 0;
	FD_ZERO(&rfds);	
	struct timeval timeout;
    printf("Starting quiz %s.\nRemoving players from main.\n", groups[index].name);
    fflush(stdout);
	for (i = 0; i < groups[index].size; i++) {
		FD_CLR( groups[index].players[i]->fd, &mainMenu );
	}

	int 	qNum = -1;
	int 	answered = groups[index].current;
	int fdss;
	bool isFirstQ = true;
	int timer = 10;
	int isTimeout = false;
	for(;;) {
		fflush(stdout);
		if (groups[index].current == 0) {
			break;
		}
		printf("Current players: %d\n", groups[index].current);
		fflush(stdout);
		if (answered >= groups[index].current) {
			if (!isFirstQ) {
				if (isTimeout == true) {    // if timeout happened, remove clients who didn't answer(i.e. fd's in the set)
                    puts("Timeout! Removing clients who didn't answer.");
                    printf("maxfds=%d\n", groups[index].maxfds);
                    fflush(stdout);
					sprintf(buf, "ENDGROUP|%s\r\n", groups[index].name);
					int j;
                    select(groups[index].maxfds, NULL, &groups[index].cfd, NULL, NULL);
                    for (j = 0; j < groups[index].maxfds; j++) {
                        printf("j=%d\n", j);
                        if (FD_ISSET(j, &groups[index].cfd)) {
                            FD_CLR(j, &groups[index].cfd);
                            if ( groups[index].maxfds == j+1 )
                                groups[index].maxfds--;
                            printf("%d removed\n", j);
                            write(j, buf, strlen(buf));
                            leave(j, true);
                        }
                    }
					puts("out of timeout");
				} else puts("not timeout");
				fflush(stdout);
                if (groups[index].current == 0) break;

				FD_ZERO(&groups[index].cfd);
				puts("Sending winner.");

				memcpy((char *)&groups[index].cfd, (char *)&ansfds, sizeof(groups[index].cfd)); // moving online fd's to the set
				groups[index].maxfds = maxAnsfds;
				sprintf(buf, "WIN|%s\r\n", groups[index].questions[qNum]->winnerName);
				write(groups[index].leaderSock, buf, strlen(buf));

				for (i = 0; i < groups[index].size; i++) {		
					if (groups[index].players[i]->isPresent != 0) {
						printf("writing to %d\n", groups[index].players[i]->fd);
						write(groups[index].players[i]->fd, buf, strlen(buf));
					}				
				}	
			} else puts("First question.");
			qNum++;
			isFirstQ = false;
			if (qNum == groups[index].questionsNum) {
				puts("Quiz is finished.");
				break;
			}
			puts("Sending question.");
			sprintf(buf, "QUES|%d|%s", groups[index].questions[qNum]->size, groups[index].questions[qNum]->text);
            for (i = 0; i < groups[index].size; i++) {
                if (groups[index].players[i]->isPresent) {
                    write(groups[index].players[i]->fd, buf, strlen(buf));
                    printf("Written to the client %d\n", groups[index].players[i]->fd);
                }
            }
            write(groups[index].leaderSock, buf, strlen(buf));
            fflush(stdout);
			isTimeout = false;
			answered = 0;		
			maxAnsfds = 0;
			timeout.tv_sec = timer;
			timeout.tv_usec = 0;
			FD_ZERO(&ansfds);		
		}

		memcpy((char *)&rfds, (char *)&groups[index].cfd, sizeof(rfds));

		int sret = select(groups[index].maxfds, &rfds, NULL, NULL, &timeout);
		if (sret < 0) { // error
			fprintf( stderr, "server select: %s\n", strerror(errno) );
			exit(-1);
		} else if (sret == 0) {	// timeout
			answered = groups[index].size;
			isTimeout = true;
			timeout.tv_sec = timer;
			timeout.tv_usec = 0;
			puts("Timeout in thread");
			continue;
		}
		
		for ( fdss = 0; fdss < groups[index].maxfds; fdss++ )
		{
			if (FD_ISSET(fdss, &rfds)) {
				printf("Timer is on %ld sec\n", timeout.tv_sec);
				int cc = read(fdss, buf, BUFSIZE);
				toString(buf, cc);
				printf("from client %d in thread = [%s]\n", fdss, buf);
				answered++;

				if (cc <= 0 || strcmp(buf, "LEAVE") == 0) {
					puts("The client has left the group.");
                    leave(fdss, false);
					printf("Current=%d\n", groups[index].current);
				} else {
					printf("%d removed from cfd\n", fdss);
					fflush(stdout);
					FD_SET(fdss, &ansfds);
					if (fdss + 1 > maxAnsfds)
						maxAnsfds = fdss + 1;
					FD_CLR(fdss, &groups[index].cfd);

					char *token = strtok (buf,"|");
					token = strtok (NULL,"|");
					printf("token=%s\n", token);
                    i = getPlayerIndex(index, fdss);
					if (strcmp(token, groups[index].questions[qNum]->answer) == 0) {
						if (strcmp(groups[index].questions[qNum]->winnerName, "") == 0) { // first
                            groups[index].questions[qNum]->winnerName = groups[index].players[i]->name;
                            groups[index].players[i]->score += 2;
                            printf("Winner of question %d is %s\n", qNum + 1, groups[index].players[i]->name);
						} else {    // right ans
                            groups[index].players[i]->score += 1;
                            fflush(stdout);
                            puts("Right WORKS!");
						}
					} else if (strcmp(token, "NOANS") == 0) {
						puts("NOANS WORKS!");
						continue;
					} else {    //wrong ans
                        groups[index].players[i]->score -= 1;
                        puts("Wrong WORKS!");
					}
				}
			}
		}
	}
	puts("Quiz end.");
	fflush(stdout);
    sendResults(buf, index);
    freeGroup(index);
	puts("Quiz thread exit");
	fflush(stdout);
	pthread_exit(NULL);
}

void *createGroup(void *arg) {
	int timer = 60;
	int bufs = 12;
	char buf[12];	
	int fd = (int) arg;
    int index = clientGroups[fd];
    clientGroups[fd] = -1;          // -1 for admin
	int i;
	int cc;
	int quizSize;
	
	int Lnum = groups[index].size;
	groups[index].size = 0;



	FD_ZERO(&(groups[index].cfd));
	puts("Reading quiz.");

	strcpy(buf, "SENDQUIZ\r\n");
	write(fd, buf, strlen(buf));

	fd_set temp;
	FD_ZERO(&temp);
	FD_SET(fd, &temp);

	struct timeval timeout;
	timeout.tv_sec = timer;
	timeout.tv_usec = 0;

	int sret = select(fd + 1, &temp, NULL, NULL, &timeout);
	if (sret < 0) {
		perror("select");
		pthread_exit(NULL);
	} else if (sret == 0) {
		groups[index].name = "";
		clientGroups[fd] = 0;
		puts("SENDQUIZ timeout.");
		strcpy(buf, "BAD|timeout\r\n");
		write(fd, buf, strlen(buf));
		close(fd);
		pthread_exit( NULL );
	}

	if (( cc = read( fd, buf, bufs)) <=0 ) { // 20 bytes
		perror("read from leader");
        groups[index].name = "";
        clientGroups[fd] = 0;
		close(fd);
		pthread_exit( NULL );
	}

	groups[index].players = (Player **) malloc(sizeof(Player *) * groups[index].size);
	for (i = 0; i < Lnum; i++) {
		groups[index].players[i] = (Player *) malloc (sizeof(Player));
		groups[index].players[i]->fd = 0;
		groups[index].players[i]->score = 0;
	}


	toString(buf, cc);

	char token[20];
	memcpy(token, buf, 5);
	token[5] = '\0';

	printf("From leader=%s\n", token);
	if (strcmp(token, "QUIZ|") != 0) {
		puts("NOT QUIZ");
		strcpy(buf, "BAD|not quiz\r\n");
		write(fd, buf, strlen(buf));
        groups[index].name = "";
        clientGroups[fd] = 0;
		close(fd);
		pthread_exit( NULL );
	}
	i=5;
	int l = 0;
	while (buf[i]!='|') {
		i++;
		l++;
	}
	memcpy(token, &buf[5], l);
	token[l] = '\0';
	quizSize = atoi(token);
	printf("quizSize=%d\n", quizSize);
	int ss;
	(quizSize > BUFSIZE) ? (ss = BUFSIZE) : (ss = quizSize);
	printf("ss=%d\n", ss);
	char *quiz = malloc (sizeof(char)*ss);
	
	while (buf[i] != '|') {
		i++;
	}
	i++;

	memcpy(quiz, &buf[i], bufs - i);
	quiz[bufs - i] = '\0';
	if (strlen(quiz) < quizSize) {	
		puts("yes\n");
		
		int t = quizSize - strlen(quiz);
		//printf("t=%d\n", t);
		fflush(stdout);
		int j;
		char temp[2];

		for (j = 0; j < t; j++) {
			cc = read( fd, temp, 1);
			temp[1] = '\0';
			strcat(quiz, temp);	
		}		
	}

	int newLines = 0;
	
	for (i = 0; i < quizSize; i++) {
		if (quiz[i] == '\n' && quiz[i+1] == '\n') {
			newLines++;
		}
	}

	int questionsNum = newLines / 2;
	groups[index].questionsNum = questionsNum;
	groups[index].questions = (Question **) malloc(sizeof(Question *) * questionsNum);


	if (groups[index].questions == NULL) {
		perror("malloc");
		pthread_exit(NULL);
	}

	char questionTemp[BUFSIZE];
	char ansTemp[BUFSIZE];
	int start, bytes = 0;
	int questionIdx = 0;
	
	start = 0;
	for (i = 0; i < quizSize; i++) {	
		bytes++;
		if (quiz[i] == '\n' && quiz[i+1] == '\n') {	// i = end of str

			groups[index].questions[questionIdx] = malloc(sizeof(Question));
			if (groups[index].questions[questionIdx] == NULL) {
				perror("malloc");
				pthread_exit(NULL);
			}
			memcpy(questionTemp, &quiz[start], bytes + 1);
			questionTemp[bytes + 1] = '\0';
			groups[index].questions[questionIdx]->text = strdup(questionTemp);
			groups[index].questions[questionIdx]->size = strlen(questionTemp);
			groups[index].questions[questionIdx]->winnerName = "";
			int j = i + 2;
			int ansFrom = j;
			bytes = 0;
			while (quiz[j] != '\n') {
				bytes++;
				j++;
			}
			memcpy(ansTemp, &quiz[ansFrom], bytes);
			ansTemp[bytes] = '\0';
			i = j + 1;
			start = i + 1;
			bytes=0;
			//printf("ansT=[%s]\n", ansTemp);
			groups[index].questions[questionIdx]->answer = strdup(ansTemp);
			questionIdx++;
		}
	}

	printf("Number of questions: %d\n", questionsNum);
	strcpy(buf, "OK\r\n");
	write(fd, buf, strlen(buf));
	free(quiz);
    FD_SET( fd, &mainMenu );
    pthread_mutex_lock(&mainMutex);
	numGroups++;
	groups[index].size = Lnum;
    pthread_mutex_unlock(&mainMutex);
	puts("Leader thread exit");
	pthread_exit( NULL );
}

void sendOpenGroups(int fd) {
	char temp[BUFSIZE];
	char buf[BUFSIZE];
	int groupCount = 0, k;
	strcpy(buf, "OPENGROUPS");

    pthread_mutex_lock(&mainMutex);
	if (numGroups == 0) {
        pthread_mutex_unlock(&mainMutex);
		strcat(buf, "\r\n");
		write(fd, buf, strlen(buf));
		return;
	}
    pthread_mutex_unlock(&mainMutex);
    strcat(buf, "|");
	for (k = 0; k < NUM_GROUPS; k++) {
        pthread_mutex_lock(&mainMutex);
        if (!groups[k].hasStarted && groups[k].size > 0) {
            groupCount++;
            sprintf(temp, "%s|%s|%d|%d|", groups[k].topic, groups[k].name, groups[k].size, groups[k].current);
            strcat(buf, temp);
        }
        pthread_mutex_unlock(&mainMutex);
	}

	buf[strlen(buf) - 1] = '\0';
	strcat(buf, "\r\n");
	printf("Sending open groups to %d\n", fd);
	write(fd, buf, strlen(buf));
}

int groupExists(char *name) { // if found, returns index of the group, else returns -1
	int k;
	for (k = 0; k < NUM_GROUPS; k++) {
        pthread_mutex_lock(&mainMutex);
		if (strcmp(name, groups[k].name) == 0) {
            pthread_mutex_unlock(&mainMutex);
			printf("Group exists, index=%d\n", k);
            fflush(stdout);
			return k;
		}
        pthread_mutex_unlock(&mainMutex);
	}
	puts("Group doesn't exist.");
    fflush(stdout);
	return -1;
}

void cancelGroup(char *name, int fdss) {
    int index, sw = 0, fd;
    char buf[100];
    puts("Executing CANCELGROUP");
    index = groupExists(name);
    if (index != -1) {
//            if (groups[index].leaderSock != fdss) {
//                pthread_mutex_unlock(&mainMutex);
//                puts("Not group's leader");
//                sw = 3;
//            } else
            pthread_mutex_lock(&mainMutex);
            if (!(groups[index].hasStarted)) {
                sprintf(buf, "ENDGROUP|%s\r\n", groups[index].name);
                if (groups[index].current > 0) {        // sending ENDGROUP for all members if group is not empty
                    select(groups[index].maxfds, NULL, &groups[index].cfd, NULL, NULL);
                    for (fd = 0; fd < groups[index].maxfds; fd++) {
                        if (FD_ISSET(fd, &groups[index].cfd)) {
                            FD_CLR(fd, &groups[index].cfd);
                            FD_SET(fd, &mainMenu);
                            printf("%d removed\n", fd);
                            write(fd, buf, strlen(buf));
                            leave(fd, true);
                        }
                    }
                }
                pthread_mutex_unlock(&mainMutex);
                freeGroup(index);
                sw = 1;
                puts("Success.");
            } else {
                puts("The group has started");
                sw = 2;
            }
    }
    switch(sw) {
        case 0:
            strcpy(buf, "BAD|no such group\r\n");
            puts("Group not found.");
            break;
        case 1:
            strcpy(buf, "OK\r\n");
            break;
        case 3:
            strcpy(buf, "BAD|you're not the leader of this group\r\n");
            break;
        default:
            strcpy(buf, "BAD|the quiz has started\r\n");
            break;
    }
    write(fdss, buf, strlen(buf));
}

int getFreeGroupSlot() {
    int i;
    for (i=0; i<NUM_GROUPS; i++) {
        pthread_mutex_lock(&mainMutex);
        if (strcmp(groups[i].name, "") == 0) {
            pthread_mutex_unlock(&mainMutex);
            printf("New groups's index is %d\n", i);
            return i;
        }
        pthread_mutex_unlock(&mainMutex);
    }
    return -1;
}

int getFreeClientSlot(int groupIndex) {
	int i;
	for (i=0; i<groups[groupIndex].size; i++) {
        printf("players[%d].fd=%d", i, groups[groupIndex].players[i]->fd);
		if (groups[groupIndex].players[i]->fd == 0) {
			printf("Client's index is %d\n", i);
			return i;
		}
	}
}

void disconnectClient(int fd) {
    int i;
    for (i = 0; i < NUM_GROUPS; i++) {
        if (groups[i].leaderSock == fd) {
            printf( "Leader of group %s has gone.\n", groups[i].name);
            clientGroups[fd] = 0;
            groups[i].leaderSock = 0;
            break;
        }
    }
    if (i == NUM_GROUPS)
        printf( "The client %d has gone.\n", fd );
    FD_CLR( fd, &mainMenu );
    if ( nfds == fd+1 )
        nfds--;
    close(fd);
}

int main( int argc, char *argv[] )
{
	char		buf[BUFSIZE], *service, *prt = "7891",
				*token, *args[10];
	struct 		sockaddr_in	fsin;
	struct 		timeval		timeout;
	int			msock, i, ssock, fd, rport, cc, sret;
	socklen_t	alen;
	fd_set		rfds;
	pthread_t 	thread;
	
	switch (argc) {
		case	1:
			service = prt;
			break;
		case	2:
			service = argv[1];
			break;
		default:
			fprintf( stderr, "usage: server [port]\n" );
			exit(-1);
	}
	msock = passivesock( service, "tcp", QLEN, &rport );

	nfds = msock+1;
	FD_ZERO(&mainMenu);
	FD_SET( msock, &mainMenu );

    for (i = 0; i < NUM_GROUPS; i++)
        groups[i].name = "";

	for (;;)
	{
		memcpy((char *)&rfds, (char *)&mainMenu, sizeof(rfds));
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		sret = select(nfds, &rfds, NULL, NULL, &timeout);
		if (sret < 0) {
			perror( "server select");
			exit(-1);
		} else if (sret == 0)
			continue;

		if (FD_ISSET( msock, &rfds)) {
			alen = sizeof(fsin);
			ssock = accept(msock, (struct sockaddr *) &fsin, &alen);
			if (ssock < 0) {
				fprintf(stderr, "accept: %s\n", strerror(errno));
				exit(-1);
			}
			puts("A client's connected.");
			sendOpenGroups(ssock);
			FD_SET(ssock, &mainMenu);
			if (ssock + 1 > nfds)
				nfds = ssock + 1;
		}

		for ( fd = 0; fd < nfds; fd++ )	{
			if (fd != msock && FD_ISSET(fd, &rfds)) {
				if ( (cc = read( fd, buf, BUFSIZE )) <= 0 )	{
                    disconnectClient(fd);
				}
				else {
					toString(buf, cc);
					printf( "Message from client: %s\n", buf );
					int l = 0;
					token = strtok (buf,"|");
					while (token != NULL) {
						args[l] = token;
						token = strtok (NULL, "|");
						l++;
					}
					if (strcmp(args[0], "LEAVE") == 0) {
						leave(fd, false);
					} else
					if (strcmp(args[0], "CANCELGROUP") == 0) {
                        cancelGroup(args[1], fd);
					} else
					if (strcmp(args[0], "GETOPENGROUPS") == 0) {
						sendOpenGroups(fd);	
					} else 
					if (strcmp(args[0], "GROUP") == 0) {
						if (groupExists(args[2]) == -1) {	// if group doesn't exist
							FD_CLR( fd, &mainMenu );
                            int index = getFreeGroupSlot();
                            printf("Group index: %d\n", index);
                            if (index == -1) {
                                puts("No free slots.");
                                strcpy(buf, "BAD|no free slots.");
                                continue;
                            }
                            clientGroups[fd] = index;
							groups[index].topic = strdup(args[1]);
							groups[index].name = strdup(args[2]);
							groups[index].size = atoi(args[3]);
							groups[index].leaderSock = fd;
							if (pthread_create(&thread, NULL, createGroup, (void *) fd) != 0 )	{
								perror( "pthread_create");
								exit( EXIT_FAILURE );
							}
						} else {
							strcpy(buf, "NAME\r\n");
							write(fd, buf, strlen(buf));
						}						
					} else 
					if (strcmp(args[0], "JOIN") == 0) {
						int groupIndex = groupExists(args[1]);
						if (groupIndex == -1) {
							strcpy(buf, "NOGROUP\r\n");							
							write(fd, buf, strlen(buf));							
						} else if (groups[groupIndex].hasStarted) {
							strcpy(buf, "FULL\r\n");
							write(fd, buf, strlen(buf));
						} else {
							puts("join");	
							int playerIdx = getFreeClientSlot(groupIndex);
							printf("Client idx = %d\n", playerIdx);

							groups[groupIndex].players[playerIdx]->name = strdup(args[2]);
							groups[groupIndex].players[playerIdx]->fd = fd;
							groups[groupIndex].players[playerIdx]->isPresent = true;
							FD_SET(fd, &groups[groupIndex].cfd);
							if ( fd+1 > groups[groupIndex].maxfds )
								groups[groupIndex].maxfds = fd+1;

							printf("groupIndex in join=%d\n", groupIndex);
							clientGroups[fd] = groupIndex;
							strcpy(buf, "OK\r\n");
							write(fd, buf, strlen(buf));
							groups[groupIndex].current += 1;

							if (groups[groupIndex].current == groups[groupIndex].size) {
                                groups[groupIndex].hasStarted = true;
								if ( pthread_create( &thread, NULL, startQuiz, (void *)  groupIndex) != 0 )	{
									perror( "pthread_create: ");
									exit( EXIT_FAILURE );
								}	
							} else {
								strcpy(buf, "WAIT\r\n");
								write(fd, buf, strlen(buf));
							}							
						}						
					}
				}
			}
		}
	}
}


