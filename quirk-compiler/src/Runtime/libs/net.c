#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <gc.h>

#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #define getcwd _getcwd

    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <errno.h>
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

#ifndef QUIRK_TYPES_H
#include "../types.h"
#endif



int Net_socket() {
    return socket(AF_INET, SOCK_STREAM, 0);
}

int Net_bind(int sockfd, String* host, int port) {
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;

    char* safe_host = make_safe_cstr(host);

    serv_addr.sin_addr.s_addr = (safe_host && strlen(safe_host) > 0)
                                ? inet_addr(safe_host)
                                : INADDR_ANY;
    serv_addr.sin_port = htons(port);

    // Allow rebinding while a previous socket on this port is in
    // TIME_WAIT — the standard server-restart case. Without this every
    // test run would have to wait ~60s before the port frees.
    int one = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    return bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
}

int Net_listen(int sockfd, int backlog) {
    return listen(sockfd, backlog);
}

int Net_accept(int sockfd) {
    struct sockaddr_in cli_addr;
    socklen_t clilen = sizeof(cli_addr);
    return accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
}

int Net_connect(int sockfd, String* host, int port) {
    if (!host || !host->buffer) return -1;
    
    char* safe_host = make_safe_cstr(host);
    if (!safe_host) return -1;

    struct sockaddr_in serv_addr;
    struct hostent *server = gethostbyname(safe_host); 
    
    if (server == NULL) return -1;

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port);

    return connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
}
int Net_send(int sockfd, String* data) {
    if (!data || !data->buffer) return 0;
    return send(sockfd, data->buffer, data->length, 0);
}

String* Net_recv(int sockfd, int size) {
    char* buffer = GC_malloc(size + 1);
    int n = recv(sockfd, buffer, size, 0);
    if (n < 0) return make_String("");
    buffer[n] = '\0';
    return make_String(buffer);
}

void Net_close(int sockfd) {
    #ifdef _WIN32
        closesocket(sockfd);
    #else
        close(sockfd);
    #endif
}
