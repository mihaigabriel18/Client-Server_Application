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
#include <unordered_map>
#include <vector>
#include <string>
#include <inttypes.h>
#include <iostream>
#include <string>
#include <map>
#include "helpers.h"

using namespace std;

char* stored_buffer;
int stored_buffer_len;

// map from topics to pair of ids subscribed to those topics
// and SF-paramters
unordered_map<string, vector<pair<string, int>>> topic_subscribers;
// map from id to map from message index to bool (whether 
// the message has been sent through the descriptor or not)
unordered_map<string, unordered_map<int, bool>> messages_received;
// map from message index to actual message
unordered_map<int, char*> message_index;
// map from message index to it's topic
map<int, string> topic_index;
// maps from tcp descriptor to it's id, ip and port
unordered_map<int, string> ids;
unordered_map<int, string> ips;
unordered_map<int, string> ports;
// map from id to descriptor
unordered_map<string, int> sockets;
// maps from message index to udp client ip and port
map<int, string> udp_ips;
map<int, string> udp_ports;

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

pair<char*, char*> parseTopic(char* message) {
    char* topic = (char*) malloc((TOPIC_LEN + 1) * sizeof(char));
    DIE(!topic, "Faulty memory allocation");
    memcpy(topic, message, TOPIC_LEN);
    topic[TOPIC_LEN] = '\0';
    
    char* content = (char*) malloc((CONTENT_LEN + DATATYPE_LEN + 1) * sizeof(char));
    DIE(!topic, "Faulty memory allocation");
    memcpy(content, message + TOPIC_LEN, (CONTENT_LEN + DATATYPE_LEN));
    content[CONTENT_LEN + DATATYPE_LEN] = '\0';

    pair<char*, char*> ret(topic, content);
    return ret;
}

char* parseContent(const char* content) {
    char first_byte = content[0];

    char* ans = (char*) malloc(CONTENT_LEN * sizeof(char));
    DIE(!ans, "Faulty memory allocation");
    if (first_byte == 0) {
        char sign_byte = content[1];
        uint32_t result = ntohl(*((uint32_t*) (content + 2)));

        int64_t signed_result = result;
        signed_result = sign_byte == 0 ? signed_result : 0 - signed_result;
        sprintf(ans, "%ld", signed_result);
    } else if (first_byte == 1) {
        uint16_t result = ntohs(*((uint16_t*) (content + 1)));
        char digit1 = result % 10;
        result /= 10;
        char digit2 = result % 10;
        result /= 10;
        sprintf(ans, "%d.%d%d", result, digit2, digit1);
    } else if (first_byte == 2) {
        char sign_byte = content[1];

        uint32_t result = ntohl(*((uint32_t*) (content + 2)));
        uint8_t power = (*((uint8_t*) (content + 6)));
        uint8_t power_cpy = power;

        string ans_s = "";
        while (power--) {
            char c = ((result % 10) + '0');
            ans_s = c + ans_s;
            result /= 10;
        }
        if (power_cpy != 0)
            ans_s = '.' + ans_s;
        if (result == 0)
            ans_s = '0' + ans_s;
        while (result) {
            char c = ((result % 10) + '0');
            ans_s = c + ans_s;
            result /= 10;
        }

        if (sign_byte)
            ans_s = '-' + ans_s;

        memcpy(ans, ans_s.c_str(), ans_s.length() + 1);
    } else if (first_byte == 3) {
        memcpy(ans, content + 1, CONTENT_LEN);

    } else {
        perror("Not a valid data type\n");
    }

    return ans;
}

string parseDataType(char* content) {
    char first_byte = content[0];

    if (first_byte == 0)
        return "INT";
    else if (first_byte == 1)
        return "SHORT_REAL";
    else if (first_byte == 2)
        return "FLOAT";
    else
        return "STRING";
}

void initSockets(fd_set* read_fds,fd_set* tmp_fds, int* sockfd_tcp,int* sockfd_udp,
    struct sockaddr_in* serv_addr_udp, int* fdmax, char* port) {

    struct sockaddr_in serv_addr_tcp;
    int ret, portno;
    FD_ZERO(read_fds);
    FD_ZERO(tmp_fds);

    *sockfd_tcp = socket(AF_INET, SOCK_STREAM, 0);
    DIE(*sockfd_tcp < 0, "Error creating tcp socket on server");
    int flag = 1;
    ret = setsockopt(*sockfd_tcp, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
    DIE(ret < 0, "Neagle algorithm disabled unsuccessfully");

    *sockfd_udp = socket(AF_INET, SOCK_DGRAM, 0);
    DIE(*sockfd_udp < 0, "Error creating udp socket on server");


    portno = atoi(port);
    DIE(portno == 0, "Atoi error");


    // seteaza campurile structurii
    memset((char *) serv_addr_udp, 0, sizeof(*serv_addr_udp));
    serv_addr_udp->sin_family = AF_INET;
    serv_addr_udp->sin_addr.s_addr = INADDR_ANY;
    serv_addr_udp->sin_port = htons(portno);

    memset((char *) &serv_addr_tcp, 0, sizeof(serv_addr_tcp));
    serv_addr_tcp.sin_family = AF_INET;
    serv_addr_tcp.sin_addr.s_addr = INADDR_ANY;
    serv_addr_tcp.sin_port = htons(portno);


    // bind structs to sockets
    ret = bind(*sockfd_udp, (struct sockaddr*) serv_addr_udp, sizeof(struct sockaddr));
    DIE(ret < 0, "Unsuccessful udp bind");

    ret = bind(*sockfd_tcp, (struct sockaddr*) &serv_addr_tcp, sizeof(struct sockaddr));
    DIE(ret < 0, "Unsuccessful tcp bind");

    ret = listen(*sockfd_tcp, MAX_CLIENTS);
    DIE(ret < 0, "Listening error");


    // add the new file descriptor (socket from which we accept connections)to 
    FD_SET(*sockfd_udp, read_fds);
    *fdmax = MAX(*fdmax, *sockfd_udp);

    FD_SET(*sockfd_tcp, read_fds);
    *fdmax = MAX(*fdmax, *sockfd_tcp);

    FD_SET(STDIN_FILENO, read_fds); // no need for max here, since STDIN descriptor is 0
}

int read_from_stdin(char* buffer, int fdmax, int sockfd_tcp, int sockfd_udp) {
    memset(buffer, 0, BUFLEN);
    string input;
    getline(std::cin, input);
    memcpy(buffer, input.c_str(), input.length());

    if (strncmp(buffer, "exit", 4) == 0) {
        for (int k = 0; k <= fdmax; k++)
            if (ids[k] != "") {
                char* to_send = (char*) malloc(5 * sizeof(char));
                DIE(!to_send, "Faulty memory allocation");
                memcpy(to_send, "exit", 5);
                send_string(to_send, k, buffer);
                close(k);
            }
        close(sockfd_tcp);
        close(sockfd_udp);
        return 1;
    }
    else
        perror("Invalid server command");
    
    return 0;
}

int read_from_tcp_socket(int sockfd_tcp, char* buffer, fd_set* read_fds,
                         int* fdmax) {
    // new connection request just arrived
    socklen_t clilen = sizeof(struct sockaddr_in);
    struct sockaddr_in tcp_cli_addr;
    int new_sock_tcp_client = accept(sockfd_tcp, 
        (struct sockaddr*) &tcp_cli_addr, &clilen);
    DIE(new_sock_tcp_client < 0, "Accepting new connection");

    // receive message with the id of the client
    char* id = receive_string(new_sock_tcp_client, buffer);
    char* ip = receive_string(new_sock_tcp_client, buffer);
    char* port = receive_string(new_sock_tcp_client, buffer);

    if (sockets[id] > 0) {
        printf("Client %s already connected.\n", id);
        char* to_send = (char*) malloc(5 * sizeof(char));
        DIE(!to_send, "Faulty memory allocation");
        memcpy(to_send, "exit", 5);
        send_string(to_send, new_sock_tcp_client, buffer);
        close(new_sock_tcp_client);
        return 1; // sign to continue the initial function loop
    }

    // add new socket returned by accept() into fds
    FD_SET(new_sock_tcp_client, read_fds);
    *fdmax = MAX(*fdmax, new_sock_tcp_client);                    
                    
    // add to maps
    ids[new_sock_tcp_client] = id;
    ips[new_sock_tcp_client] = ip;
    ports[new_sock_tcp_client] = port;
    sockets[id] = new_sock_tcp_client;
    
    // print welcome message
    printf("New client %s connected from %s:%s.\n", id, ip, port);

    // send messages the client has missed
    string id_s(id);
    for (auto entry : topic_subscribers) {
        for (auto client : entry.second) {
            // if we find the current client to be subscribed
            // to that topic with SF = 1, then we to the next
            if (client.first == id_s && client.second == 1) {
                string topic = entry.first;
                for (auto message : messages_received[id_s]) {
                    int mes_index = message.first;
                    if ((strcmp(topic_index[mes_index].c_str(), topic.c_str()) == 0) &&
                            message.second == false) {
                                        
                        send_string(message_index[mes_index], new_sock_tcp_client, buffer);
                        messages_received[id_s][mes_index] = true;
                    }
                }
                break; // we cannot find more, useless search
            }
        }
    }
    return 0;
}

void read_from_udp_socket(char* buffer, struct sockaddr_in* serv_addr_udp,
                int sockfd_udp, int* current_index) {
    // received message from udp clients, store it and forward
    // it to subscribers
    memset(buffer, 0, BUFLEN);
    unsigned int size = sizeof(*serv_addr_udp);
    int ret = recvfrom(sockfd_udp, buffer, BUFLEN, 0,
                    (struct sockaddr*) serv_addr_udp, &size);
    DIE(ret == 0, "Reading from udp client error");

    // add message to the maps
    pair<char*, char*> m = parseTopic(buffer);
    char* to_send = parseContent(m.second);
    string data_type = parseDataType(m.second);

    // build message
    string to_send_s(to_send);

    string topic(m.first);
    topic_index[*current_index] = topic;
    char* udp_ip = inet_ntoa(serv_addr_udp->sin_addr);
    char* udp_port = (char*) malloc(10 * sizeof(char));
    DIE(!udp_port, "Faulty memory allocation");

    sprintf(udp_port, "%hu", serv_addr_udp->sin_port);
    udp_ips[*current_index] = udp_ip;
    udp_ports[*current_index] = udp_port;
    string built_message = udp_ips[*current_index] + ":" + udp_ports[*current_index] +
            " - " + topic_index[*current_index] + " - " + parseDataType(m.second) +
            " - " + to_send_s;
    const char* __payload = built_message.c_str();
    char* payload = (char*) malloc(strlen(__payload) + 1);
    DIE(!__payload, "Faulty memory allocation");
    memcpy(payload, __payload, strlen(__payload) + 1);

    message_index[*current_index] = payload;
    // set the message not sent for every client
    for (auto entry : sockets) {
        messages_received[entry.first][*current_index] = false;
    }
    
    // set it as sent for ones listening to this topic and send it
    for (auto entry : topic_subscribers[m.first]) {
        int sockfd = sockets[entry.first];
        if (sockfd > 0) {
            send_string(payload, sockfd, buffer);
            messages_received[ids[sockfd]][*current_index] = true;
        }
    }
    (*current_index)++;
}

int  read_from_tcp_clients(char* buffer, fd_set* read_fds, int cli_socket) {
    // we got information on a tcp client socket
    memset(buffer, 0, BUFLEN);
    char* received_m = receive_string(cli_socket, buffer);
    if (received_m == NULL) {
        printf("Client %s disconnected.\n", ids[cli_socket].c_str());
        sockets[ids[cli_socket]] = -1; // the socket for that id is -1 (once created but now unassigned)
        ids[cli_socket] = "";
        ips[cli_socket] = "";
        ports[cli_socket] = "";
        close(cli_socket);
        FD_CLR(cli_socket, read_fds);
        return 1; // sign to continue to the caller
    }

    // analyze whether it is a subscribe or unsubscribe
    char* token = strtok(buffer, " ");
    if (strcmp(token, "subscribe") == 0) {
        char* topic = strtok(NULL, " ");
        char* sf = strtok(NULL, " ");

        int sf_value = atoi(sf);
        if (sf_value != 0 && sf_value != 1) {
            perror("Invalid sf parameter");
            return 1;
        }
        pair<string, int> subscriber(ids[cli_socket], sf_value);
        topic_subscribers[topic].push_back(subscriber);

        // if sf == 1, forward all packets on that topic that
        // have not yet been received by this socket
        if (sf_value == 0)
            return 1; // sign to continue to the caller
                        
        for (auto entry : messages_received[ids[cli_socket]]) {
            int mes_index = entry.first;
            if (strcmp(topic_index[mes_index].c_str(), topic) == 0 &&
                    entry.second == false) {

                send_string(message_index[mes_index], cli_socket, buffer);
                messages_received[ids[cli_socket]][mes_index] = true;
            }
        }
    } else if (strcmp(token, "unsubscribe") == 0) {
        char* topic = strtok(NULL, " ");

        unsigned int k;
        for (k = 0; k < topic_subscribers[topic].size(); k++)
            if (sockets[topic_subscribers[topic][k].first] == cli_socket)
                break;
        topic_subscribers[topic].erase(
                        topic_subscribers[topic].begin() + k);             
    } else {
        perror("Not a valid command from TCP user");
    }
    return 0;
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

    int sockfd_tcp, sockfd_udp;
    char buffer[BUFLEN];
    struct sockaddr_in serv_addr_udp;
    int ret;
    int current_index = 0;

    DIE(argc != 2, "Incorrect number of arguments to \"server.cpp\"");

    fd_set read_fds; // multimea de citire folosita in select()
    fd_set tmp_fds;  // multime folosita temporara 
    int fdmax = 0;       // valoare maxima fd din multimea read_fds
    
    initSockets(&read_fds, &tmp_fds, &sockfd_tcp, &sockfd_udp,
                &serv_addr_udp, &fdmax, argv[1]);

    while(1) {
        tmp_fds = read_fds;
        ret = select(fdmax + 1, &tmp_fds, NULL, NULL, NULL);
        DIE(ret < 0, "Server select error");

        for (int i = 0; i <= fdmax; i++) {
            if (FD_ISSET(i, &tmp_fds)) {
                if (i == STDIN_FILENO) { // reading from stdin
                    ret = read_from_stdin(buffer, fdmax, sockfd_tcp, sockfd_udp);
                    if (ret)
                        return 0; // received exit command
                } else if (i == sockfd_tcp) {
                    ret = read_from_tcp_socket(sockfd_tcp, buffer,
                                               &read_fds, &fdmax);
                    if (ret)
                        continue;
                } else if (i == sockfd_udp) {
                    read_from_udp_socket(buffer, &serv_addr_udp,
                                         sockfd_udp, &current_index);
                } else if (ids[i] != "") {
                    ret = read_from_tcp_clients(buffer, &read_fds, i);
                    if (ret)
                        continue;
                }
            }
        }
    }
}