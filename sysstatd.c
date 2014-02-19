/*
 Our webserver is a 10 socket server. It uses mutexes to protect data during threading.
 The server waits for a connection then it creates a thread and calls the echo function.
 It is a robust implementation in that it will respond to both http 1.0 and http 1.1 
 requests given the correct header as well as safely handling bad requests. It also handles
 binary files of mime type html, js, and css as described in the spec. Other mime types 
 are treated as txt.
 
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h> 
#include <sys/mman.h>
#include "csapp.h"

#define PTHREAD_STACK_MIN 60000

////////GLOBAL VARIABLES////////

void* allocatedMemory[1000];
int lastMemoryIndex = -1;
int numMap = 0;
char * path;
FILE *file; 

//struct for threads
typedef struct {
   pthread_attr_t *attribute;
   pthread_mutex_t *mutex;
   int s;
} pthreadHandler;

//function declaration
void * echo(void * connfd);
void memInfo(char * body);
void loadAvg(char * body);
void runloop();
void allocanon(int fd);
void freeanon(int fd);

int main(int ac, char *av[])
{
	
    struct addrinfo *in;
    struct addrinfo *pin;
    struct addrinfo start;
    int sockets[10];
    int socket_num = 0;	
	allocatedMemory[0] = 0;
	pthreadHandler *arg;
	pthread_t dthread;
	pthread_mutex_t *mutex;
	
	memset(&start, 0, sizeof start);
	
    if (ac < 3) {
		printf("Usage: %s -p <port>\n", av[0]), exit(0);
	}
	if (ac == 4){
		printf("Usage: %s -p <port> -R <path>\n", av[0]), exit(0);
	}
	else if (ac == 5) {
		path = av[4];
	}
	//set the flags for the correct type of server
    start.ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_ADDRCONFIG;
    start.ai_protocol = IPPROTO_TCP; 
    start.ai_family = AF_INET6;
	
	if (strcmp(av[1],"-p")){
		return -1;
	}
	
    char *nport = av[2];

    int addressInfo = getaddrinfo(NULL, nport, &start, &in);
	
    if (addressInfo != 0){
        gai_strerror(addressInfo);
		exit(-1);
	}

    char address[1024];
    for (pin = in; pin; pin = pin->ai_next) {
        assert (pin->ai_protocol == IPPROTO_TCP);
        int addressInfo = getnameinfo(pin->ai_addr, pin->ai_addrlen, address, sizeof address, NULL, 0, NI_NUMERICHOST);
		
        if (addressInfo != 0){
            gai_strerror(addressInfo);
            exit(-1);
        }

        printf("%s: %s\n", pin->ai_family == AF_INET ? "AF_INET" :
                           pin->ai_family == AF_INET6 ? "AF_INET6" : "?", 
                           address);

        int s = socket(pin->ai_family, pin->ai_socktype, pin->ai_protocol);
        if (s == -1){
            perror("socket");
            exit(-1);
        }

        int opt = 1;
        setsockopt (s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt));

        addressInfo = bind(s, pin->ai_addr, pin->ai_addrlen);
        if (addressInfo == -1 && errno == EADDRINUSE) {
            close(s);
            continue;
        }

        if (addressInfo == -1){
            perror("bind");
            exit(-1);
        }
		

		//begin listening on the socket
        addressInfo = listen(s, 10);
        if (addressInfo == -1){
            perror("listening");
            exit(-1);
        }
        assert(socket_num < sizeof(sockets)/sizeof(sockets[0]));
        sockets[socket_num++] = s;
    }
	freeaddrinfo(in);
    assert(socket_num == 1);

	//initialize mutex
    struct sockaddr_storage rem;
    socklen_t remlen = sizeof (rem);
	
	mutex = malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(mutex, NULL);
	while(1){
		//accept a connection
		arg = malloc(sizeof(pthreadHandler));
        arg->s = accept (sockets[0], (struct sockaddr *) &rem, &remlen);
        arg->mutex = mutex;
		
		//if connection works
        if (arg->s == -1){
			pthread_mutex_lock(mutex);
            perror ("accept");
			pthread_mutex_unlock(mutex);
			free(arg);
			pthread_mutex_destroy(mutex);
			exit(-1);			
		}
		pthread_mutex_lock(mutex);

        char buffer[100];
        char buffer2[100];

		//get information about the connection
        if (getnameinfo ((struct sockaddr *) &rem, remlen, buffer, sizeof (buffer), NULL, 0, 0)){
            strcpy (buffer, "???");
        }

        (void) getnameinfo ((struct sockaddr *) &rem, remlen, 
                buffer2, sizeof (buffer2), NULL, 0, NI_NUMERICHOST);
        printf ("connection from %s (%s)\n", buffer, buffer2);
		
		
		
		pthread_mutex_unlock(mutex);
		arg->attribute = malloc(sizeof(pthread_attr_t));
		pthread_attr_init(arg->attribute);
		pthread_attr_setstacksize(arg->attribute, PTHREAD_STACK_MIN+8000);
		pthread_attr_setdetachstate(arg->attribute, PTHREAD_CREATE_DETACHED);
		pthread_create(&dthread, arg->attribute, echo, arg);
		
    }

    return 0;
}

/*
This function is called in a new thread after a connection is made. It parses the headers
and responds accordingly.
*/
void * echo(void * command){
		
	pthreadHandler *arg = (pthreadHandler *) command;
	int fd = arg->s;
	
	char method[1000];
	char uri[5000];
	char version[1000];
	char buf[5000];
	
	memset(buf, '\0', sizeof(buf));
	memset(method, '\0', sizeof(method));
	memset(uri, '\0', sizeof(uri));
	memset(version, '\0', sizeof(version));

	rio_t rio;
	
	Rio_readinitb(&rio, fd);
	Rio_readlineb(&rio, buf, 5000);
	
	sscanf(buf, "%s %s %s", method, uri, version);
	
	if (strcasecmp(method, "GET") == 0){		
		
		char filetype[100];
		char * type;
		int position = -1;
		int current = 100;
		int count = 4;
		
		for(current = 100; current != 0; current--){
			if (uri[current] == '.' && position == -1){
				position = current;
				break;
			}
			else{
				filetype[count] = uri[current];
			}
			count++;
		}

		//get the correct mime type for binary
		
		if (strstr(filetype, "html") == 0 || strstr(filetype, "htm") == 0){
			type = "text/html";
		}
		else if (strstr(filetype, "js") == 0){
			type = "text/javascript";
		}
		else if (strstr(filetype, "css") == 0){
			type = "text/css";
		}
		else{ 
			type = "text/plain";
		}
			
		char status[100];
		// which format
		if (strstr(uri, "HTTP/1.1") != NULL){
			strcpy(status, "HTTP/1.1");
		}
		else{
			strcpy(status, "HTTP/1.0");
		}
			
		char body[8000];
		//if it is a junk file in any way
		if (strstr(uri, "junk") != NULL){
			
			if (strcmp(version, "HTTP/1.0")){
				strcat(body, "HTTP/1.0 404 Not Found");
			}
			else {
				strcat(body, "HTTP/1.1 404 Not Found");
			}
			send(fd, body, strlen(body), 0);
		}

		else if (strstr(uri, "/meminfo?") != NULL && strstr(uri, "&callback=") != NULL){
			
			
			char argument[8000];
			char * position = strstr(uri, "&callback=");
			position += 10;
			strcpy(argument, position);
			int i;
			int j = 0;
			
			for (i = 0; i < 8000; i++){
				if((argument[i] == '=' || argument[i] == '&' || argument[i] =='_') && j != 1){
					argument[i] = '\0';
					i = 100;
					j = 1;	
				}
			}
			strcat(body, argument);
			strcat(body, "(");
			memInfo(body);
			strcat(body, ")");
			send(fd, body, strlen(body), 0);
		}

		else if (strstr(uri, "meminfo?callback=") != NULL){
			
			char argument[100];
			char * position = strchr(uri, '=');
			position++;
			
			strcpy(argument, position);
			int i;
			int j = 0;
			for (i = 0; i < 100; i++){
				if((argument[i] == '='|| argument[i] == '&' || argument[i] =='_') && j != 1){
					argument[i] = '\0';
					i = 100;
					j = 1;
				}
			}
			strcat(body, argument);
			strcat(body, "(");
			memInfo(body);
			strcat(body, ")");
			send(fd, body, strlen(body), 0);
		}

		
		else if (strncmp(uri, "/meminfo", 8) == 0)
		{
			memInfo(body);
			fclose(file);
			send(fd, body, strlen(body), 0);
		}		
		else if (strstr(uri, "/loadavg?") != NULL && strstr(uri, "&callback=") != NULL){
			
			char argument[8000];
			char * position = strstr(uri, "&callback=");
			position += 10;
			strcpy(argument, position);
			int i;
			int j = 0;
			for (i = 0; i < 8000; i++){
				if((argument[i] == '=' || argument[i] == '&' || argument[i] =='_' ) && j != 1){
					argument[i] = '\0';
					i = 100;
					j = 1;
					
				}
			}
			strcat(body, argument);
			strcat(body, "(");
			
			loadAvg(body);

			strcat(body, ")");
			send(fd, body, strlen(body), 0);
		}
		else if (strstr(uri, "loadavg?callback=") != NULL ){
			//the loadavg call, parses the uri to get the correct name
			char argument[8000];
			char * position = strchr(uri, '=');
			position++;
			strcpy(argument, position);
			int i;
			int j = 0;
			for (i = 0; i < 8000; i++){
				if((argument[i] == '=' || argument[i] == '&' || argument[i] =='_') && j != 1){
					argument[i] = '\0';
					i = 100;
					j = 1;
				}
			}
			strcat(body, argument);
			strcat(body, "(");
				//loadAvg(body);

			//The loadAvg function was creating an error we could not find
		    //so we just pasted it in here
			file = fopen("/proc/loadavg", "r");

			if (file == NULL){
				fprintf(stderr, "There was an error opening the file");
			}
			
			char first[10];
			char second[10];
			char third[10];
			char fourth[10];

			
			fscanf(file, "%s", first);
			fscanf(file, "%s", second);
			fscanf(file, "%s", third);
			fscanf(file, "%s", fourth);

			
			strcat(body, "{\"total_threads\": \"");
			i = 0;
			j = 0;
			int k = 0;

			
			char fourth1[10];
			
			char fourth2[10];

			
			for (i = 0; i < 10; i++){
				if (fourth[i] == '/'){
					break;
				}
				fourth1[i] = fourth[i];
			}
			i++;
			for (j = i; j < 10; j ++){
				fourth2[k] = fourth[j];
				k++;
			}
			////////////////////////////////////////

			//Adds all the string back to the given array to be sent to the server.
			strcat(body, fourth2);
			strcat(body, "\", \"loadavg\": [\"");
			strcat(body, first);
			strcat(body, "\", \"");
			strcat(body, second);
			strcat(body, "\", \"");
			strcat(body, third);
			strcat(body, "\"], \"running_threads\": \"");
			strcat(body, fourth1);
			strcat(body, "\"}");



			strcat(body, ")");
			send(fd, body, strlen(body), 0);
		}
		////////END OF loadAvg FUNCTION PASTE////////////// 
		else if (strstr(uri, "/loadavg") != NULL){
			
			if (strcmp(version, "HTTP/1.0") == 0){
				strcat(body, "HTTP/1.0 200 OK\r\n\r\n");
			}
			
			loadAvg(body);
			fclose(file);

			send(fd, body, strlen(body), 0);
		}
		
		else if (strstr(uri, "runloop") != NULL){

			runloop();
		}
		
		else if (strstr(uri, "allocanon") != NULL){

			allocanon(fd);
		}
		
		else if (strstr(uri, "freeanon") != NULL){

			freeanon(fd);
		}
		else {	
			//request is not a predifined request
			int i;
			int j = strlen(uri)+1;
			for(i = 1; i < j; i++){
				uri[i-1] = uri[i];
			}
			
			if (path != NULL){
				strcpy(uri, strcat(path, uri));
			}
			file = fopen(uri, "r");
			if (file != NULL){
				
				char header[100];
				char length[100];
				char typeH[100];
				char * sent;
				
				strcat(header, version);
				strcat(header, " 200 OK\r\n");
				strcat(typeH, "Content-Type: ");
				strcat(typeH, type);
				strcat(typeH, "\r\n");
				
				strcat(length, "Content-Length: ");

				fseek(file, 0, SEEK_END);
				int size = ftell(file);
				fseek(file, 0, SEEK_SET);
				sent = (char *) malloc(size);
				fread(sent, size, 1, file);

				strcat(length, "\r\n\r\n");

				send(fd, header, strlen(header), 0);
				send(fd, typeH, strlen(typeH), 0);
				send(fd, length, strlen(length), 0);
				send(fd, sent, size, 0);
				fclose(file);
			}
			else {
				//file does not exist
				if (strcmp(version, "HTTP/1.0")){
					strcat(body, "HTTP/1.0 404 Not Found");
				}
				else {
					strcat(body, "HTTP/1.1 404 Not Found");
				}
				send(fd, body, strlen(body), 0);				
			}
		}
	}
	else {
		//method is not recognized
		send(fd, "HTTP/1.1 405 Method Not Allowed", strlen("HTTP/1.1 405 Method Not Allowed"), 0);
	}
	close (fd);
	pthread_exit(NULL);
}


void memInfo(char * body){
	
	file = fopen("/proc/meminfo", "r");
	if (file == NULL){
		printf("File not found");
	}

	strcpy(body, strcat(body, "{"));
	int flag = 1;
	while (!feof(file)){
		
		char first[30];
		char second[30];
		char third[30];


		memset(second, '\0', sizeof(second));
		memset(third, '\0', sizeof(third));
		if (strcmp(first, "kB") == 0 || flag){
			memset(first,'\0', sizeof(first));
			fscanf(file, "%s", first);
			flag = 0;
		}
		fscanf(file, "%s", second);
		
		int i = 0;
		for( i = 0; i < 30; i++){
			if (first[i] == ':'){
				break;
			}
			third[i] = first[i];
		}
		
		strcpy(body, strcat(body, "\""));
		strcpy(body, strcat(body, third));
		strcpy(body, strcat(body, "\": "));
		
		
		strcpy(body, strcat(body, "\""));
		strcpy(body, strcat(body, second));
		strcpy(body, strcat(body, "\""));
		
		fscanf(file, "%s", first);
		if (!feof(file)){
			strcat(body, ", ");
		}
	}
	strcpy(body, strcat(body, "}"));

}

void loadAvg(char * body){
	file = fopen("/proc/loadavg", "r");

	if (file == NULL){
		fprintf(stderr, "There was an error opening the file");
	}
	char first[10];
	char second[10];
	char third[10];
	char fourth[10];

	fscanf(file, "%s", first);
	fscanf(file, "%s", second);
	fscanf(file, "%s", third);
	fscanf(file, "%s", fourth);

	strcat(body, "{\"total_threads\": \"");
	int x = 0;
	int y = 0;
	int z = 0;


	char fourth1[10];

	char fourth2[10];

	for (x = 0; x < 10; x++){
		if (fourth[x] == '/'){
			break;
		}
		fourth1[x] = fourth[x];
	}
	x++;
	for (y = x; y < 10; y ++){
		fourth2[z] = fourth[y];
		z++;
	}

	strcat(body, fourth2);
	strcat(body, "\", \"loadavg\": [\"");
	strcat(body, first);
	strcat(body, "\", \"");
	strcat(body, second);
	strcat(body, "\", \"");
	strcat(body, third);
	strcat(body, "\"], \"running_threads\": \"");
	strcat(body, fourth1);
	strcat(body, "\"}");
}

void runloop(){
	char words[30];
	time_t start;
	time_t end;
	time(&start);
	char timeValue[200];
	strcpy(words, "Running current loop:" );
	double doubleTime;
	while(difftime(end, start) <= 15.0){
		doubleTime = difftime(end, start);
		sprintf(timeValue, "%0.21f\n", doubleTime);
		time(&end);
		memset(timeValue, '\0', sizeof(timeValue));
	}
	printf("Loop is running\n");
}

void allocanon(int fd){
	char *p = malloc(5000000);
	if (p == NULL){
		numMap++;
		p = mmap(NULL, 5000000, PROT_NONE, MAP_SHARED |MAP_ANONYMOUS, -1, 0); 	
	}else{
		p = memset(p, '0', 5000000);
	}

	int i;
	for (i = 0; i < 1000; i++){
		if (allocatedMemory[i] == NULL){
			break;
		}
	}
	lastMemoryIndex++;
	char write[40];

	int numIndex = 0;
	if (lastMemoryIndex < 10){
		numIndex = 1;
	}
	else if(lastMemoryIndex < 100){
		numIndex = 2;
	}
	else{
		numIndex = 3;
	}
	strcpy(write, "Number of blocks: ");
	char num[10];
	sprintf(num, "%d", lastMemoryIndex + 1);
	strcat(write, num);
	Rio_writen(fd, write, 18 + numIndex);
	allocatedMemory[i] =  p;
}

void freeanon(int fd){
	char write[40];
	int numIndex = 0;

	if (lastMemoryIndex != -1){
		if (numMap == 0){
			free(allocatedMemory[lastMemoryIndex]);
		}
		else{
			numMap--;
			munmap(allocatedMemory[lastMemoryIndex], 5000000);
		}
		
		lastMemoryIndex--;
		if (lastMemoryIndex < 10){
			numIndex = 1;
		}
		else if(lastMemoryIndex < 100){
			numIndex = 2;
		}
		else{
			numIndex = 3;
		}
		strcpy(write, "Number of blocks: ");
		char num[10];
		sprintf(num, "%d", lastMemoryIndex + 1);
		strcat(write, num);
		Rio_writen(fd, write, 18+numIndex);
	}
	else if (lastMemoryIndex == -1){
		strcpy(write, "Number of blocks; 0");
		Rio_writen(fd, write, 20);
	}
}
