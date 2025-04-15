/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/* 
Please specify the group members here

# Student #1: Kaelin Goodlett
# Student #2: Nathaniel Baker
# Student #3: 

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct
{
    int client_id;               // Identifies the client thread
    int seq_num;                 // Sequence number for ordering and retransmission
    int ack_flag;                // 0 for data, 1 for ACK
    char payload[MESSAGE_SIZE]; // Payload only used for data frames
} packet_t;

typedef struct {
    int epoll_fd;
    int socket_fd;
    int client_id;
    int seq_num;
    long long tx_cnt;
    long long rx_cnt;
    long long total_rtt;
    long total_messages;
    float request_rate;
    struct sockaddr_in server_add;
} client_thread_data_t;

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];

    struct timeval start, end;
    socklen_t add_len = sizeof(data->server_add);
    int seq_num = 0;
    int timeout = 500; // 500ms timeout

    packet_t pkt;
    packet_t recv_pkt;

    int client_id = (int)(pthread_self() %  100000);    // makeshift client id from unique thread ids

    // Initialize new variables of data
    data->client_id = client_id;
    data->seq_num = 0;
    data->tx_cnt = 0;
    data->rx_cnt = 0;

    // Register the socket in the epoll instance
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event);

    while (data->total_messages < num_requests) 
    {
        // Initialize packet
        pkt.client_id = client_id;
        pkt.seq_num = seq_num;
        pkt.ack_flag = 0;
        memcpy(pkt.payload, "ABCDEFGHIJKMLNOP", MESSAGE_SIZE);
        gettimeofday(&start, NULL);

        // Send packet, increment total messages sent counter
        sendto(data->socket_fd, &pkt, sizeof(packet_t), 0, (struct sockaddr *)&data->server_add, add_len);
        data->tx_cnt++;

        int ack_rec = 0;

        while (!ack_rec)
        {
            int n_events = epoll_wait(data->epoll_fd, events, MAX_EVENTS, timeout);

            // If there's no events, resend the packet
            if (n_events == 0)
            {
                // Resend, increment
                sendto(data->socket_fd, &pkt, sizeof(packet_t), 0, (struct sockaddr *)&data->server_add, add_len);
                data->tx_cnt++;
            }

            for (int i = 0; i < n_events; i++) {
                if (events[i].data.fd == data->socket_fd) 
                {
                    // Receive ack flag from recv packet
                    recvfrom(data->socket_fd, &recv_pkt, sizeof(packet_t), 0, NULL, NULL);

                    // If the ack flag is marked as delievered, the client id matches, and the sequence number all match, packet is good to send
                    if (recv_pkt.ack_flag == 1 && recv_pkt.client_id == client_id && recv_pkt.seq_num == pkt.seq_num)
                    {
                        gettimeofday(&end, NULL);
                        data->rx_cnt++;
                        long long rtt = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
                        data->total_rtt += rtt;
                        data->total_messages++;
                        ack_rec = 1;
                    }
                }
            }

        }
        // Increase sequence number to keep packets with each thread
        seq_num++;
    }
    // Update request rate
    data->request_rate = (float) data->total_messages * 1000000 / (float) data->total_rtt;

    close(data->socket_fd);
    close(data->epoll_fd);
    return NULL;
}

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    // Create client threads
    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].epoll_fd = epoll_create1(0);
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0); // UDP socket with DGRAM
        thread_data[i].server_add = server_addr;
    }

    for (int i = 0; i < num_client_threads; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    // Wait for threads to complete, initalize tracker variables
    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0;
    long long total_tx = 0;
    long long total_rx = 0;

    // Tally up stats
    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
    }

    printf("Total Sent Packets: %lld\n", total_tx);
    printf("Total Received Packets: %lld\n", total_rx);
    printf("Total Lost Packets: %lld\n", total_tx - total_rx);

}

void run_server() {
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0); // UDP socket with _DGRAM
    if (server_fd < 0)
    {
        perror("socket() failed");
        exit(1);
    }
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if(bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind() failed");
        exit(1);
    }


    printf("Server is running on port %d...\n", server_port);

    int epoll_fd = epoll_create1(0);
    struct epoll_event event, events[MAX_EVENTS];

    event.events = EPOLLIN;
    event.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    while (1) {
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n_events; i++) {
            if (events[i].data.fd == server_fd) {
                // Accept a new connection
                packet_t recv_pkt;

                recvfrom(server_fd, &recv_pkt, sizeof(packet_t), 0, (struct sockaddr *)&client_addr, &addrlen);

                // If there has been no ack, the packet is updated and sent back
                if (recv_pkt.ack_flag == 0)
                {

                    packet_t ack_pkt;
                    ack_pkt.client_id = recv_pkt.client_id;
                    ack_pkt.seq_num = recv_pkt.seq_num;
                    ack_pkt.ack_flag = 1;
                    memset(ack_pkt.payload, 0, MESSAGE_SIZE);
                    sendto(server_fd, &ack_pkt, sizeof(packet_t), 0, (struct sockaddr *)&client_addr, addrlen);
                }
            } 
        }
    }

    close(server_fd);
    close(epoll_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);

        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}
