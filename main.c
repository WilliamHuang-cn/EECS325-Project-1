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

int main(int argc, char *argv[]) {

    signal(SIGINT, SIGHandler);
    signal(SIGQUIT, SIGHandler);

    // input value holders
    char input_buffer[50];      // Using 50 char as default input length
    char buffer[50];
    char host[50], port[50];
    int ret, sockfd, listen_sockfd;
    char *msg;

    // Input flags
    int accept_req = 0, send_msg = 0;

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
    struct addrinfo hint, hint_local, *res = NULL, *serverInfo = NULL, *p;
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
    if ((ret = getaddrinfo(NULL, argv[1], &hint_local, &serverInfo))) {
        printf("getaddrinfo: %s\n", gai_strerror(ret));
        return 1;       // Quit program with error
    }

    for(p = serverInfo; p != NULL; p = p->ai_next) {
        // Try to create socket for addrinfo. Note that the sockets are non-blocking 
        if ((listen_sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("listener: socket");
            printf("Cannot create socket for host. \n");
            continue;
        }

        // setToNonblocking(listen_sockfd);

        if (bind(listen_sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(listen_sockfd);
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

    // FD_SET(listen_sockfd, &rfds);
    // FD_SET(listen_sockfd, &efds);
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

            terminateSession(input_buffer);
            endSession = 0;
            continue;
        }

        // FD_ZERO(&wfds);
        // FD_ZERO(&rfds);
        // FD_ZERO(&efds);
        FD_SET(listen_sockfd, &rfds);
        FD_SET(listen_sockfd, &efds);
        FD_SET(STDIN_FILENO, &rfds);

        printf("Socket fd value: %d\n", listen_sockfd);

        // int rettemp = FD_ISSET(listen_sockfd, &rfds);
        // printf("%d\n", rettemp);
        // rettemp = FD_ISSET(listen_sockfd, &efds);
        // printf("%d\n", rettemp);

        // Default prompt
        printf("#chat with?: \n");
        // Polling from all avaliable inputs
        retval = select(FD_SETSIZE, &rfds, NULL, NULL, NULL);

        if (retval == -1) {
            perror("select()");
            return 2;
        } else if (retval == 0) {
            printf("Select timed out!\n");
            continue;
        } else if (FD_ISSET(STDIN_FILENO, &rfds)) {               // Case: stdin input
            printf("A key was pressed!\n");
            read(STDIN_FILENO, buffer, 50);
            continue;
            
            fgets(input_buffer, 50 , stdin);        // Read user input. Blocking until user finished with EOF
            // fflush(stdin);
            // Analyze input
            // IPv4:port 
            if (!accept_req && !send_msg && (sscanf(input_buffer, "%[0-9.:] %[0-9]", host, port) == 2)) {     // Check if the format is host+port; not answering request or sending messages

                // TODO
                // Assume for now making new connections

                // Make new conncetions
                if ((ret = getaddrinfo(host, port, &hint, &res))) {
                    printf("Invalid address. Please try again. \n");
                    puts(gai_strerror(ret));
                    continue;
                }

                // Open connection to server
                for(p = res; p != NULL; p = p->ai_next) {
                    if ((sockfd = socket(p->ai_family, p->ai_socktype,p->ai_protocol)) == -1) {
                        perror("talker: socket");
                        continue;
                    }
                    setToNonblocking(sockfd);
                    break;
                }
                // No socket can be established
                if (p == NULL) {
                    fprintf(stderr, "sender: failed to create socket\n");
                    printf("Cannot establish connection to host %s \n", host);
                    continue;
                }

                // Send datagram via established socket
                msg = serializeMsg(TYPE_REQUEST, NULL);
                retval = setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

                sendto(sockfd, msg, strlen(msg), 0, p->ai_addr, p->ai_addrlen);
                

                // We don't expect immediate return. So we leave it in select polling
                // if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
                    reqCount++;
                    printf("Waiting for peer to accept connection... \n");
                    FD_SET(sockfd, &rfds);
                    FD_SET(sockfd, &efds);
                // } else {
                    // connectionSuccess(p); 
                // }


            }
        } else if (FD_ISSET(listen_sockfd, &rfds)) {
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