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

int main(int argc, char *argv[]) {

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

    memset(&hint, 0, sizeof hint);
    hint.ai_family = PF_UNSPEC;     // Either IPv4 of IPv6
    hint.ai_socktype = SOCK_DGRAM;     // request for UDP 
    hint.ai_flags = AI_NUMERICHOST; // allows only numerical address input

    memset(&hint_local, 0, sizeof hint_local);
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

        // int reuse = 1;
        // int err = setsockopt(listen_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        // if (0 != err) {
            // err = bind(listen_sockfd, res->ai_addr, res->ai_addrlen);
            // perror("setsockopt()");
            // return 2;
        // }
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

    freeaddrinfo(serverInfo);
    printf("Listening on port: %s\n", argv[1]);

        FD_SET(listen_sockfd, &rfds);
        FD_SET(listen_sockfd, &efds);

        printf("Socket fd value: %d\n", listen_sockfd);

        // Polling from all avaliable inputs
        retval = select(FD_SETSIZE, &rfds, NULL, NULL, NULL);

        if (retval == -1) {
            perror("select()");
            return 2;
        } else if (retval == 0) {
            printf("Select timed out!\n");
            return 2;
        } else if (FD_ISSET(listen_sockfd, &rfds)) {
            printf("Something on the listening socket... \n");
            // continue;

            // Deal with bad package
            if (recvfrom(listen_sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&inc_addr, &addr_len) == -1){
                perror("recvfrom");
                return 2;
            }
            struct sockaddr_in *sin = (struct sockaddr_in *)&inc_addr;
            unsigned char *ip = (unsigned char *)&sin->sin_addr.s_addr;
            printf("#session request from: %s\n", ip);          // Assuming IPv4 address
        }

    close(listen_sockfd);
    freeaddrinfo(res);
    return 0;
}