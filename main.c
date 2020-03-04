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
#define TYPE_DENY (4)
#define TYPE_CONFIRM (5)
#define TYPE_H_ACK (6)
#define TYPE_C_ACK (7)

#define REQUESTING (0)              // A session that is requested but not confirmed by peer
#define ESTABLISHED (1)
#define CLOSING (2)
#define WAITING_CONFIRM (3)             // A seesion that is started by peer but not confirmed by host

// Defines a linked list of saved sessions
struct session {
    int sockfd;
    struct addrinfo *address;
    // char *host;
    // char *port;
    struct timeval lastUpdated;
    struct session *nextSession;
    int reqCount;
    int status;             // As defined above
};

// Saved sessions
struct session _activeSessions;
struct session *activeSessions = &_activeSessions;

// Flag controlling main loop
int volatile keepRunning = 1;
int volatile endSession = 0;

// Temp var
char *message = "asdfg";

void SIGHandler(int);
void terminateAllSessions();
char *serializeMsg(int, char*);
int unserializeMsg(char *, int *, char *);
void connectionSuccess(struct addrinfo*);
int setToNonblocking(int);

int createSock(char *, char *, struct addrinfo *, struct addrinfo *, int *);
int createSockAndBind(char *, char *, struct addrinfo *, struct addrinfo *, int *);

int newSession(struct session **, int, struct addrinfo *, struct timeval *, int);
int removeSession(struct session **, int, struct session *);
int terminateSession(struct session **, int);
struct session *findSessionBySock(struct session **, int);
struct session *findSessionByHost(struct session **, char *, char *);

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
    char *input, *prompt_host, *prompt_port;
    struct session *prompt_session;

    // Connection flags
    int reqCount = 0;

    // Select vars
    fd_set wfds, rfds, efds;
    FD_ZERO(&wfds);
    FD_ZERO(&rfds);
    FD_ZERO(&efds);
    int retval;
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;

    // Connection vars
    struct addrinfo hint, hint_local, *res = NULL, *serverInfo = NULL;
    char buf[100];
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
        
        // Prompt session termination. Note this is blocking: no new messages or commands allowed until finished
        if (endSession) {
            printf("#terminate session (for help <h>): ");
            fgets(input_buffer, 50, stdin);
            printf("You want to drop: %s", input_buffer);

            // Extract host and port from input
            if (sscanf(input_buffer, "%[0-9.:] %[0-9]", host, port) == 2) {
                // Send terminate message to peer
                struct session *s = findSessionByHost(&activeSessions, host, port);
                terminateSession(&activeSessions, s->sockfd);
                endSession = 0;
                continue;
            } else {
                printf("Invalid input. Please try again.\n");
                continue;
            }

            continue;
        }

        // Initialize fd sets
        FD_ZERO(&wfds);
        FD_ZERO(&rfds);
        FD_ZERO(&efds);
        FD_SET(listen_sockfd, &rfds);
        FD_SET(listen_sockfd, &efds);
        FD_SET(STDIN_FILENO, &rfds);
        for (struct session *p = activeSessions; p!=NULL; p=p->nextSession) {
            FD_SET(p->sockfd, &rfds);
            FD_SET(p->sockfd, &efds);
        }

        // Default prompt
        if (!send_msg && !confirm_req) printf("#chat with?: ");
        if (send_msg) printf("[%s:%s] your message: ", prompt_host, prompt_port);
        if (confirm_req) printf("[%s:%s] accept request? (y/n) ", prompt_host, prompt_port);
        fflush(stdout);

        // Polling from all avaliable inputs
        retval = select(FD_SETSIZE, &rfds, &wfds, &efds, NULL);

        if (retval == -1) {
            perror("select()");
            return 2;
        } else if (retval == 0) {
            printf("Select timed out!\n");
            continue;
        } else if (FD_ISSET(STDIN_FILENO, &rfds)) {               // Case: stdin input            
            fgets(input_buffer, 50 , stdin);        // Read user input. 
            // Analyze input
            // IPv4:port + creating a new connection
            if (!send_msg && !term_session && !confirm_req && (sscanf(input_buffer, "%[0-9.:] %[0-9]", host, port) == 2)) {     // Check if the format is host+port; not answering request or sending messages

                // Try finding existing session by host and port
                prompt_session = findSessionByHost(&activeSessions, host, port);
                if (prompt_session != NULL) {
                    prompt_host = host;
                    prompt_port = port;
                    if (prompt_session->status == ESTABLISHED) {
                        send_msg = 1;
                    } else if (prompt_session->status == WAITING_CONFIRM) {
                        confirm_req = 1;
                    }
                    continue;
                }

                if((ret = createSock(host, port, &hint, res, &sockfd)) != 0) {
                    res = NULL;
                    continue;
                };

                // Send datagram via established socket
                char *msg = serializeMsg(TYPE_REQUEST, NULL);
                if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
                    perror("setsockopt()");
                    continue;
                }
                sendto(sockfd, msg, strlen(msg), 0, res->ai_addr, res->ai_addrlen);
                newSession(&activeSessions, sockfd, res, NULL, REQUESTING);
                res = NULL;
                printf("Waiting for peer to accept connection... \n");
                FD_SET(sockfd, &rfds);
                FD_SET(sockfd, &efds);
                continue;
            }

            // String + Sending message
            if (send_msg && (sscanf(input_buffer, "%s", input) == 1)) {
                char *msg = serializeMsg(TYPE_MSG, input);
                sendto(prompt_session->sockfd, msg, strlen(msg), 0, prompt_session->address->ai_addr, prompt_session->address->ai_addrlen);
                send_msg = 0;
                continue;
            }

            // y/n + Confirming request
            if (confirm_req && (sscanf(input_buffer, "%1[ynYN]", input) == 1)) {
                char *msg;
                if (input == "y" || input == "Y") {
                    // Confirms request
                    prompt_session->status = ESTABLISHED;
                    msg = serializeMsg(TYPE_CONFIRM, NULL);
                } else {
                    // Denies request
                    msg = serializeMsg(TYPE_DENY, NULL);
                    struct session *pre; 
                    removeSession(&activeSessions, prompt_session->sockfd, pre);
                }
                sendto(prompt_session->sockfd, msg, strlen(msg), 0, prompt_session->address->ai_addr, prompt_session->address->ai_addrlen);
                confirm_req = 0;
                continue;
            }

            // Already handled earlier
            // IPv4:port + terminating a session
            // if (term_session && (sscanf(input_buffer, "%[0-9.:] %[0-9]", host, port) == 2)) {
            //     continue;
            // }

            // No match. 
            printf("Invalid input. Please try again. \n");
            continue;

        } else if (FD_ISSET(listen_sockfd, &rfds)) {                // Incoming transmission/request on listening socket
            // printf("Something on the listening socket... \n");
            // continue;

            // Deal with incoming package
            if (recvfrom(listen_sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&inc_addr, &addr_len) == -1){
                perror("recvfrom");
                continue;
            }

            struct sockaddr_in *sin = (struct sockaddr_in *)&inc_addr;
            char *in_host = inet_ntoa(sin->sin_addr);
            char *in_port;
            int r = sprintf(in_port, "%d", ntohs(sin->sin_port));
            printf("#session request from: %s %s\n", in_host, in_port);
            if((ret = createSock(in_host, in_port, &hint, res, &sockfd)) != 0) {
                res = NULL;
                continue;
            }
            newSession(&activeSessions, sockfd, res, NULL, WAITING_CONFIRM);

        } else if (FD_ISSET(listen_sockfd, &efds)) {
            // Handle error on listening socket
            printf("Something wrong with listening socket. \n");
            continue;
        } else {                // Incoming transmission on established sessions
            // Iterate through activeSessions and process 
            struct session *previous = NULL;
            for (struct session *p=activeSessions; p != NULL; p=p->nextSession) {
                if (FD_ISSET(p->sockfd, &rfds)) {
                    // Incoming transmission on socket
                    char msg[50];
                    int type;
                    if (recvfrom(p->sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&inc_addr, &addr_len) == -1){
                        perror("recvfrom");
                        continue;
                    }
                    struct sockaddr_in *sin = (struct sockaddr_in *)&inc_addr;
                    char *in_host = inet_ntoa(sin->sin_addr);
                    char *in_port;
                    char *rep;
                    int r = sprintf(in_port, "%d", ntohs(sin->sin_port));

                    unserializeMsg(buf, &type, msg);
                    switch (type)
                    {
                        case TYPE_HEARTBEAT:
                            /* code */
                            break;
                        case TYPE_CLOSE:
                            rep = serializeMsg(TYPE_C_ACK, NULL);
                            sendto(p->sockfd, msg, strlen(msg), 0, p->address->ai_addr, p->address->ai_addrlen);
                            p->status = CLOSING;
                            break;
                        case TYPE_DENY:
                            printf("#failure: %s %s\n", in_host, in_port);
                            removeSession(&activeSessions, p->sockfd, previous);
                            p = previous;
                            break;
                        case TYPE_CONFIRM:
                            p->status = ESTABLISHED;
                            printf("#success: %s %s\n", in_host, in_port);
                            break;
                        case TYPE_MSG:
                            printf("#[%s %s] sent msg: %s\n", in_host, in_port, msg);
                            break;
                        case TYPE_C_ACK:
                            printf("#terminating session with %s %s\n", in_host, in_port);
                            removeSession(&activeSessions, p->sockfd, previous);
                            p = previous;
                            break;
                        case TYPE_H_ACK:
                            break;
                        default:
                            printf("Unrecognized incoming message.");
                            break;
                    }
                    // if (p->status == REQUESTING && p->reqCount <= 3) {             // Hearing back from a connection request and retry
                    //     // retryConnection();
                    //     continue;
                    // }
                    // if (p->status == REQUESTING && p->reqCount > 3) {             // Hearing back from a connection request and stop retry
                    //     // connectionTimeout();
                    //     continue;
                    // }
                    // if (p->status == ESTABLISHED) {             // Getting a heart beat
                        
                    //     // TODO
                    //     // ACK to heartbeat
                    //     // Response to closing request

                    //     continue;
                    // }
                    // if (p->status == CLOSING) {
                    //     char *msg = serializeMsg(TYPE_CONFIRM, NULL);
                    //     continue;
                    // }
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
    // freeaddrinfo(res);
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
    for (struct session *s = activese) {

    }
}

// Generate message (str) based on message type
char *serializeMsg(int msg_type, char *msg) {
    // uint32_t 0;
    return message;
}

// Unserialize mssage (str) and returns mesage type and origial message
int unserializeMsg(char *input, int *msg_type, char *msg) {
    return 0;
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

int removeSession(struct session **s, int sockfd, struct session *p) {
    return 0;
}

int terminateSession(struct session **s, int sockfd) {
    return 0;
}

struct session *findSessionBySock(struct session **s, int sockfd) {
    return 0;
}
struct session *findSessionByHost(struct session **s, char *host, char *port) {
    return 0;
}