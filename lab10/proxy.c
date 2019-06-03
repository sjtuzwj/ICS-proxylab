/*
 * proxy.c - ICS Web proxy
 * ID:517021910799
 * Name:Zhu Wenjie
 */

#include "csapp.h"
#include <stdarg.h>
#include <sys/select.h>
/*
 * Function prototypes
 */
//change port from int to char
int parse_uri(char *uri, char *target_addr, char *path, char *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, size_t size);
//Work
int doit(int connfd, struct sockaddr_in *sockaddr);
void* thread(void*);
//Subprocedure
void printLog(struct sockaddr_in *sockaddr,char* uri, int size);
ssize_t buildHeader(rio_t* Rio, char*Buf,char*re,int*BodySize);
//Ignore error
ssize_t Rio_writen_w(int fd, void *userbuf, size_t n);
ssize_t Rio_readnb_w(rio_t *rp, void *userbuf, size_t n);
ssize_t Rio_readlineb_w(rio_t *fp, void *userbuf, size_t maxlen);
//Semaphore for IO
sem_t mutex;
//storage info to create a thread
typedef struct{
    int fd;
    unsigned int addr;
}ConnInfo;
/*
 * main - Main routine for the proxy program
 */
int main(int argc, char **argv)
{
    char hostname[MAXLINE], port[MAXLINE];
    struct sockaddr_in clientAddr;
    pthread_t tid;
	//Check Phase
	if (argc != 2){ 
        fprintf(stderr, "usage: %s <portname>\n", argv[0]);
        exit(0);
	}
    //Prepare Phase
	Signal(SIGPIPE, SIG_IGN);
	Sem_init(&mutex, 0, 1);
	int listenfd = Open_listenfd(argv[1]);
	//Connect Phase
    while(1){
		socklen_t clientlen = sizeof(clientAddr);
		ConnInfo* conninfo = (ConnInfo*)Malloc(sizeof(ConnInfo));
		conninfo->fd= Accept(listenfd, (SA *)&clientAddr, &clientlen); 	
		Getnameinfo((SA*)&clientAddr, clientlen, hostname, MAXLINE, port, MAXLINE, NI_NUMERICHOST);
        conninfo->addr=clientAddr.sin_addr.s_addr;
		Pthread_create(&tid, NULL, thread, conninfo);
	}
    exit(0);
}
/*
 * thread- manage resources and pass to doit
 */
void* thread(void *vargp)
{
    ConnInfo conninfo = *(ConnInfo*)vargp;
	Pthread_detach(pthread_self());

	struct sockaddr_in socketAddr;
    socketAddr.sin_family = AF_INET;
    socketAddr.sin_addr.s_addr=conninfo.addr;

	int serverfd;
	free(vargp);
    if((serverfd=doit(conninfo.fd, &socketAddr)))
		close(serverfd);
    close(conninfo.fd);
	return NULL;
}

/*
 * doit - main transaction
 */
int doit(int clientfd, struct sockaddr_in *sockaddr)
{
	int serverfd;
	//WriteBuf
	char clientBuf[MAXLINE], serverBuf[MAXLINE];
	//Request Method
	char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
	//Uri
	char target[MAXLINE], path[MAXLINE], port[MAXLINE];
	rio_t clientRio, serverRio;
	
	int reqBodySize = 0, reqHeaderSize=0;
	int resBodySize = 0, resHeaderSize=0;

	//Init Rio for Client
 	Rio_readinitb(&clientRio, clientfd);

	//Parse Request
	if(!Rio_readlineb_w(&clientRio, clientBuf, MAXLINE))
			return 0;
	sscanf(clientBuf, "%s %s %s", method, uri, version);
    if (parse_uri(uri, target, path, port))
			return 0;

	//Build Request Header
	char request[MAXLINE];
	sprintf(request, "%s /%s %s\r\n", method, path, version);
	if (!buildHeader(&clientRio, clientBuf,request,&reqBodySize))
        return 0;

	//Init Rio for Server 
	if (!(serverfd = open_clientfd(target, port)))
		return 0;
	Rio_readinitb(&serverRio, serverfd);

	//Send Request Header to Server 
	reqHeaderSize= strlen(request);
	if (Rio_writen_w(serverfd, request, reqHeaderSize)!= reqHeaderSize)
		return serverfd;

	//Send Request Body to Server 
	if (reqBodySize){
			if(!Rio_readnb_w(&clientRio, clientBuf, reqBodySize))
			return serverfd;
			if(Rio_writen_w(serverfd, clientBuf, reqBodySize)!=reqBodySize)
			return serverfd;
	}

	//Send Response Header to Client
	ssize_t rc;
	while ((rc = Rio_readlineb_w(&serverRio,serverBuf, MAXLINE))){
		if (!strncasecmp(serverBuf, "Content-Length", 14))
			resBodySize = atoi(serverBuf + 15);
		resHeaderSize += rc;
		if(Rio_writen_w(clientfd, serverBuf, rc)!=rc)
			return serverfd;
		if (!strcmp(serverBuf, "\r\n"))
			break;
	}
	if(!rc)
	return serverfd;

	//Send Response Body to Client 
	for (int i=resBodySize; i > 0; i--)
		if ((!Rio_readnb_w(&serverRio, serverBuf, 1) && i>1 )|| Rio_writen_w(clientfd, serverBuf, 1)!=1)
			return serverfd;
/*
	//Big File Error
	if(!Rio_readnb_w(&serverRio, serverBuf, resBodySize))
			return serverfd;
	if(Rio_writen_w(clientfd, serverBuf, resBodySize)!=resBodySize)
			return serverfd;
*/
	//Print Log
    printLog(sockaddr,uri,resBodySize+resHeaderSize);
	return serverfd;
}

/*
*UTILITY
*/
void printLog(struct sockaddr_in *sockaddr,char* uri, int size){
	char logstring[MAXLINE];
	format_log_entry(logstring, sockaddr, uri, size);
	//Thread Safe
	P(&mutex);
	printf("%s\n", logstring);
	V(&mutex);
}



ssize_t buildHeader(rio_t* Rio, char*Buf,char*re,int*BodySize){
	ssize_t rc;
	while ((rc = Rio_readlineb_w(Rio,Buf, MAXLINE))){
		if (!strncasecmp(Buf, "Content-Length", 14))
			*BodySize = atoi(Buf + 15);
		strcat(re,Buf);
		if (!strcmp(Buf, "\r\n"))
			break;
	}
	return rc;
}

/*
 * parse_uri - URI parser
 *
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, char *port){
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
        hostname[0] = '\0';
        return -1;
    }

    /* Extract the host name */
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    if (hostend == NULL)
        return -1;
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';

    //adapt to return char*
    int p;
    /* Extract the port number */
    p = 80; /* default */
    if (*hostend == ':')
        p = atoi(hostend + 1);
    sprintf(port,"%d",p);

    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
        pathname[0] = '\0';
    }
    else {
        pathbegin++;
        strcpy(pathname, pathbegin);
    }

    return 0;
}

/*
 * format_log_entry - Create a formatted log entry in logstring.
 *
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), the number of bytes
 * from the server (size).
 */
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr,char *uri, size_t size){
    time_t now;
    char time_str[MAXLINE];
    unsigned long host;
    unsigned char a, b, c, d;

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    /*
     * Convert the IP address in network byte order to dotted decimal
     * form. Note that we could have used inet_ntoa, but chose not to
     * because inet_ntoa is a Class 3 thread unsafe function that
     * returns a pointer to a static variable (Ch 12, CS:APP).
     */
    host = ntohl(sockaddr->sin_addr.s_addr);
    a = host >> 24;
    b = (host >> 16) & 0xff;
    c = (host >> 8) & 0xff;
    d = host & 0xff;

    /* Return the formatted log entry string */
    sprintf(logstring, "%s: %d.%d.%d.%d %s %zu", time_str, a, b, c, d, uri, size);
}

ssize_t Rio_writen_w(int fd, void *userbuf, size_t n){
	ssize_t rc;
	if ((rc = rio_writen(fd, userbuf, n)) != n){
		fprintf(stderr, "%s: %s\n", "Rio writen error", strerror(errno));
		return 0;
	}
	return rc;
}

ssize_t Rio_readnb_w(rio_t *fp, void *userbuf, size_t n){
    ssize_t rc;
    if ((rc = rio_readnb(fp, userbuf, n)) != n){
        fprintf(stderr, "%s: %s\n", "Rio readnb error", strerror(errno));
        return 0;
    }
    return rc;
}

ssize_t Rio_readlineb_w(rio_t *fp, void *userbuf, size_t maxlen){
    ssize_t rc;
    if ((rc = rio_readlineb(fp, userbuf, maxlen)) < 0){
        fprintf(stderr, "%s: %s\n", "Rio readlineb error", strerror(errno));
        return 0;
    }
    return rc;
}
