#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <netinet/tcp.h>
#include <iostream>
#include <string>
#include "helpers.h"

using namespace std;

char* stored_buffer;
int stored_buffer_len;

void send_string(char* str, int sockfd, char* buffer) {
    memset(buffer, 0, BUFLEN);
    sprintf(buffer, "%s", str);

    int sent = 0;
    int remaining = BUFLEN - sent;
    
    while (remaining > 0) {
        int bytes_sent_local = send(sockfd, buffer + sent, remaining, 0);
        DIE(bytes_sent_local < 0, "Local send failed");
        sent += bytes_sent_local;
        remaining -= bytes_sent_local;
    }
}

char* receive_string(int sockfd, char* buffer) {
    int received = 0;
    int remaining = BUFLEN - received;

    while (remaining > 0) {
        memset(buffer, 0, BUFLEN);
        int bytes_received_local = recv(sockfd, buffer, BUFLEN, 0);
        if (bytes_received_local == 0)
            return NULL;
        received += bytes_received_local;
        remaining -= bytes_received_local;

        int old_size = strlen(stored_buffer);
        stored_buffer_len = old_size + bytes_received_local;
        if (stored_buffer_len != 0) {
            char* new_buffer = (char*) realloc(stored_buffer, stored_buffer_len);
            DIE(!new_buffer, "Faulty memory reallocation");
            stored_buffer = new_buffer;
        }

        memcpy(stored_buffer + old_size, buffer, bytes_received_local);
    }

    char* ret_str = (char *) malloc(BUFLEN * sizeof(char));
    DIE(!ret_str, "Faulty memory allocation");
    memcpy(ret_str, stored_buffer, BUFLEN);

    int future_buffer_len = stored_buffer_len - BUFLEN;
    char* future_buffer = (char *) malloc(future_buffer_len * sizeof(char) + 1);
    DIE(!future_buffer, "Faulty memory allocation");
    memcpy(future_buffer, stored_buffer + BUFLEN, future_buffer_len);
    future_buffer[future_buffer_len] = '\0';
    stored_buffer = future_buffer;
    stored_buffer_len -= BUFLEN;

    return ret_str; 
}

void init() {
    // preprocessing
    setvbuf(stdout, NULL, _IONBF, BUFSIZ);
    stored_buffer = (char*) malloc (sizeof(char));
    DIE(!stored_buffer, "Faulty memory allocation");
    stored_buffer[0] = '\0';
    stored_buffer_len = 0;
}

int main(int argc, char* argv[]) {
    init();

    int sockfd, ret, portno;
    struct sockaddr_in serv_addr;
    char buffer[BUFLEN];

    DIE(argc != 4, "Incorrect number of arguments in \"subscriber.cpp\"");

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    DIE(sockfd < 0, "Tcp client socket creation failed");

    int flag = 1;
    ret = setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
    DIE(ret < 0, "Neagle algorithm disabled unsuccessfully");

    portno = atoi(argv[3]);

    memset((char *) &serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
    ret = inet_aton(argv[2], &serv_addr.sin_addr);
    DIE(ret == 0, "Error parsing ip address given as argument");

    ret = connect(sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr));
    DIE(ret < 0, "Connection to server error");

    send_string(argv[1], sockfd, buffer);

    send_string(argv[2], sockfd, buffer);

    send_string(argv[3], sockfd, buffer);

    fd_set read_fds;
    fd_set temp_fds;

    FD_ZERO(&read_fds);
    FD_ZERO(&temp_fds);

    FD_SET(sockfd, &read_fds);
    FD_SET(STDIN_FILENO, &read_fds);

    while (1) {
        temp_fds = read_fds;
        ret = select(sockfd + 1, &temp_fds, NULL, NULL, NULL);
        DIE(ret < 0, "Tcp client select error");

        if (FD_ISSET(STDIN_FILENO, &temp_fds)) {
            // reading from stdin
            memset(buffer, 0, BUFLEN);
            string input;
            getline(std::cin, input);
            memcpy(buffer, input.c_str(), input.length());

            if (!isalpha(buffer[0])) {
                perror("Wrong input command format");
                continue;
            }

            if (strncmp(buffer, "exit", 4) == 0)
                break;
            
            char* buffer_copy = (char*) malloc(BUFLEN * sizeof(char));
            DIE(!buffer_copy, "Faulty memory allocation");
            memcpy(buffer_copy, buffer, BUFLEN);

            send_string(buffer_copy, sockfd, buffer);

            char* token = strtok(buffer_copy, " ");
            if (strcmp(token, "subscribe") == 0)
                printf("Subscribed to topic.\n");
            else if (strcmp(token, "unsubscribe") == 0)
                printf("Unsubscribed from topic.\n");
            else
                perror("Wrong input command format");
        }

        if (FD_ISSET(sockfd, &temp_fds)) {
            // receiving from server
            memset(buffer, 0, BUFLEN);
            char* received = receive_string(sockfd, buffer);
            
            if (strncmp(received, "exit", 4) == 0)
                break;

            printf("%s\n", received);
        }
    }

    close(sockfd);
}