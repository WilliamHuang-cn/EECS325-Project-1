#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>

#define TYPE_REQUEST (0)
#define TYPE_HEARTBEAT (1)
#define TYPE_MSG (2)
#define TYPE_CLOSE (3)

#define REQUESTING (0)              // A session that is requested but not confirmed by peer
#define ESTABLISHED (1)
#define CLOSING (2)
#define WAITING_CONFIRM (3)             // A seesion that is started by peer but not confirmed by host

// Defines a linked list of saved sessions
struct session {
    int sockfd;
    struct addrinfo *address;
    struct timeval lastUpdated;
    struct session *nextSession;
    int reqCount;
    int status;             // As defined above
} *activeSessions;


// Flag controlling main loop
int volatile keepRunning = 1;
int volatile endSession = 0;

// Temp var
char *message = "asdfg";

void SIGHandler(int);
void terminateAllSessions();
void terminateSession(char []);
char *serializeMsg(int, char*);
void connectionSuccess(struct addrinfo*);
int setToNonblocking(int);

int createSock(char *, char *, struct addrinfo *, struct addrinfo *, int *);
int createSockAndBind(char *, char *, struct addrinfo *, struct addrinfo *, int *);

int newSession(struct session **, int, struct addrinfo *, struct timeval *, int);
int removeSession(struct session **, int);
struct session *findSessionBySock(int);
struct session *findSessionByHost(char *, char *);

int main(int argc, char *argv[]) {

    signal(SIGINT, SIGHandler);
    signal(SIGQUIT, SIGHandler);

    // input value holders
    char input_buffer[50];      // Using 50 char as default input length
    char buffer[50];
    char host[50], port[50];
    int ret, sockfd, listen_sockfd;

    // Input flags
    int send_msg = 0, confirm_req = 0, term_session = 0;
    char *input;

    // Connection flags
    int reqCount = 0;

    // Saved sessions
    // ????

    // Select vars
    fd_set wfds, rfds, efds;
    FD_ZERO(&wfds);
    FD_ZERO(&rfds);
    FD_ZERO(&efds);
    // FD_SET(STDIN_FILENO, &rfds);
    int retval;
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;

    // Connection vars
    struct addrinfo hint, hint_local, *res = NULL, *serverInfo = NULL;
    char buf[50];
    struct sockaddr_storage inc_addr;
    socklen_t addr_len;

    memset(&hint, '\0', sizeof hint);
    hint.ai_family = PF_UNSPEC;     // Either IPv4 of IPv6
    hint.ai_socktype = SOCK_DGRAM;     // request for UDP 
    hint.ai_flags = AI_NUMERICHOST; // allows only numerical address input

    memset(&hint_local, '\0', sizeof hint);
    hint_local.ai_family = PF_UNSPEC;  // Either IPv4 of IPv6
    hint_local.ai_socktype = SOCK_DGRAM;     // request for UDP 
    hint_local.ai_flags = AI_PASSIVE;        // Use local ip address

    // Server init with input argv[1] (listening port)
    createSockAndBind(NULL, argv[1], &hint_local, serverInfo, &listen_sockfd);

    freeaddrinfo(serverInfo);
    printf("Listening on port: %s\n", argv[1]);

    // Main polling loop
    while (keepRunning) {
        
        // Promt session termination
        if (endSession) {
            printf("#terminate session (for help <h>): ");
            fgets(input_buffer, 50, stdin);
            printf("You want to drop: %s", input_buffer);

            // TODO
            // findSessionByHost();
            // removeSession();

            terminateSession(input_buffer);
            endSession = 0;
            continue;
        }

        // Initialize fd sets
        FD_ZERO(&wfds);
        FD_ZERO(&rfds);
        FD_ZERO(&efds);
        FD_SET(listen_sockfd, &rfds);
        FD_SET(listen_sockfd, &efds);
        FD_SET(STDIN_FILENO, &rfds);

        // Default prompt
        printf("#chat with?: ");
        fflush(stdout);
        // Polling from all avaliable inputs
        retval = select(FD_SETSIZE, &rfds, NULL, NULL, NULL);

        if (retval == -1) {
            perror("select()");
            return 2;
        } else if (retval == 0) {
            printf("Select timed out!\n");
            continue;
        } else if (FD_ISSET(STDIN_FILENO, &rfds)) {               // Case: stdin input
            // read(STDIN_FILENO, buffer, 50);
            
            fgets(input_buffer, 50 , stdin);        // Read user input. 
            // Analyze input
            // IPv4:port + creating a new connection
            if (!send_msg && !term_session && !confirm_req && (sscanf(input_buffer, "%[0-9.:] %[0-9]", host, port) == 2)) {     // Check if the format is host+port; not answering request or sending messages

                // TODO
                // Assume for now making new connections
                // findSessionByHost(host, port);

                if((ret = createSock(host, port, &hint, res, &sockfd)) != 0) continue;

                // Send datagram via established socket
                char *msg;
                msg = serializeMsg(TYPE_REQUEST, NULL);
                if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
                    perror("setsockopt():");
                    continue;
                }
                sendto(sockfd, msg, strlen(msg), 0, res->ai_addr, res->ai_addrlen);
                newSession(&activeSessions, sockfd, res, NULL, REQUESTING);
                printf("Waiting for peer to accept connection... \n");
                FD_SET(sockfd, &rfds);
                FD_SET(sockfd, &efds);
                continue;
            }

            // String + Sending message
            if (send_msg && (sscanf(input_buffer, "%s", input) == 1)) {
                continue;
            }

            // y/n + Confirming request
            if (confirm_req && (sscanf(input_buffer, "%1[ynYN]", input) == 1)) {
                continue;
            }

            // IPv4:port + terminating a session
            if (term_session && (sscanf(input_buffer, "%[0-9.:] %[0-9]", host, port) == 2)) {
                continue;
            }

            // No match. 
            printf("Bad input. Please try again");
            continue;

        } else if (FD_ISSET(listen_sockfd, &rfds)) {                // Incoming transmission/request on listening socket
            printf("Something on the listening socket... \n");
            // continue;

            // Deal with bad package
            if (recvfrom(listen_sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&inc_addr, &addr_len) == -1){
                perror("recvfrom");
                continue;
            }
            struct sockaddr_in *sin = (struct sockaddr_in *)&inc_addr;
            unsigned char *ip = (unsigned char *)&sin->sin_addr.s_addr;
            printf("#session request from: %s\n", ip);          // Assuming IPv4 address
        } else {                // Incoming transmission on established sessions
            // Iterate through activeSessions and process 
            for (struct session *p=activeSessions; p != NULL; p=p->nextSession) {
                if (FD_ISSET(p->sockfd, &rfds)) {
                    // Incoming transmission on socket
                    if (p->status == REQUESTING && p->reqCount <= 3) {             // Hearing back from a connection request and retry
                        // retryConnection();
                        continue;
                    }
                    if (p->status == REQUESTING && p->reqCount > 3) {             // Hearing back from a connection request and stop retry
                        // connectionTimeout();
                        continue;
                    }
                    if (p->status == ESTABLISHED) {             // Getting a heart beat
                        
                        // TODO
                        // ACK to heartbeat
                        
                        continue;
                    }
                }

                if (FD_ISSET(p->sockfd, &efds)) {
                    // Error on transmission on socket
                    continue;
                }
            }
        }
    }

    terminateAllSessions();
    close(listen_sockfd);
    freeaddrinfo(res);
    return 0;
}

void SIGHandler(int sig) {
    if (sig == SIGINT) {
        // Terminate main loop
        keepRunning = 0;
    } else if (sig == SIGQUIT) {
        // prompt user for specific termination
        endSession = 1;
    }
}

// Drops all connections. One-sided
void terminateAllSessions() {
    printf("terminating all sessions... \n");
}


void terminateSession(char str[]) {
    printf("terminate session %s", str);
}

// Generate message (str) based on message type
char *serializeMsg(int msg_type, char *msg) {
    // uint32_t 0;
    return message;
}

// Save and monitor established connections
void connectionSuccess(struct addrinfo *info) {
    // TODO 
}

int setToNonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Creates a socket. 
int createSock(char *host, char *port, struct addrinfo *hint, struct addrinfo *res, int *sockfd) {

    int ret;
    struct addrinfo *p;
    if ((ret = getaddrinfo(host, port, hint, &res))) {
        printf("Invalid address. Please try again. \n");
        puts(gai_strerror(ret));
        return 1;
    }

    // Open connection to server
    for(p = res; p != NULL; p = p->ai_next) {
        if ((*sockfd = socket(p->ai_family, p->ai_socktype,p->ai_protocol)) == -1) {
            perror("talker: socket");
            return 1;
        }
        break;
    }
    // No socket can be established
    if (p == NULL) {
        fprintf(stderr, "sender: failed to create socket\n");
        printf("Cannot establish connection to host %s \n", host);
        return 1;
    }

    res = p;
    return 0;
}

// Creates a socket and binds to port. 
int createSockAndBind(char *host, char *port, struct addrinfo *hint, struct addrinfo *res, int *sockfd) {
    
    int ret;
    struct addrinfo *p;
    if ((ret = getaddrinfo(host, port, hint, &res))) {
        printf("getaddrinfo: %s\n", gai_strerror(ret));
        return 1;       // Quit program with error
    }

    for(p = res; p != NULL; p = p->ai_next) {
        // Try to create socket for addrinfo. Note that the sockets are non-blocking 
        if ((*sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("listener: socket");
            printf("Cannot create socket for host. \n");
            continue;
        }

        if (bind(*sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(*sockfd);
            perror("listener: bind");
            printf("Cannot bind socket to port. \n");
            continue;
        }
        break;
    }

    if (p == NULL) {
        fprintf(stderr, "listener: failed to bind socket\n");
        return 1;
    }

    res = p;
    return 0;
}

int newSession(struct session **s, int sockfd, struct addrinfo *addr, struct timeval *time, int status) {
    return 0;
}

int removeSession(struct session **s, int sockfd) {
    return 0;
}
struct session *findSessionBySock(int sockfd) {
    return 0;
}
struct session *findSessionByHost(char *host, char *port) {
    return 0;
}