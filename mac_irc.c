#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <time.h>
#include "irc.h"
#include "platform.h"

void send_response_to_user(user_t* user, char* res_buffer, int message_length) {
    if (send(user->fd, res_buffer, message_length, MSG_NOSIGNAL) == -1) {
        fprintf(stderr, "failed to send %d bytes to fd %d\n", message_length, user->fd);
    }
}

char* get_address(void *addr) {
    char* client_ip = malloc(INET6_ADDRSTRLEN);
    struct sockaddr_storage *sas = addr;
    void *src;

    if (sas->ss_family == AF_INET) {
        struct sockaddr_in* sa4 = addr;
        src = &(sa4->sin_addr);
    } else if (sas->ss_family == AF_INET6) {
        struct sockaddr_in6* sa6 = addr;
        src = &(sa6->sin6_addr);
    } else {
        return NULL;
    }

    inet_ntop(sas->ss_family, src, client_ip, INET6_ADDRSTRLEN);
    return client_ip;
}

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return flags;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void process_event(struct kevent evt, int server_fd, struct kevent* change_event, int kqueue_fd) {
    int target_fd = evt.ident;

    if (evt.flags & EV_EOF) {
        printf("Client has disconnected closing fd %d\n", target_fd);
        close(target_fd);
        return;
    }

    if (target_fd == server_fd) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd == -1) {
            fprintf(stderr, "Failed to accept connection!\n");
            return;
        }

        int nonblocking_res = set_nonblocking(client_fd);
        if (nonblocking_res == -1) {
            fprintf(stderr, "Failed to set client connection to nonblocking, terminating connection");
            close(client_fd);
            return;
        }

        EV_SET(change_event, client_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        if (kevent(kqueue_fd, change_event, 1, NULL, 0, NULL) == -1) {
            fprintf(stderr, "Failed to register client to a new socket\n");
            close(client_fd);
        }

        user_t user = {0};
        user.fd = client_fd;
        user.host_name = get_address(&client_addr);
        users[next_user_slot++] = user;
        return;
    }

    if (evt.filter == EVFILT_READ) {
        ssize_t bytes_read = read(target_fd, message_buffer, sizeof(message_buffer));

        if (bytes_read > 0) {
            // NULL terminate reused buffer
            if (bytes_read < sizeof(message_buffer)) {
                message_buffer[bytes_read] = '\0';
            }

            tcp_buf_t tcp_buffer;
            tcp_buffer.body = message_buffer;
            tcp_buffer.len = bytes_read;

            // TODO figure this out its not working
            int client_index = get_client_index(target_fd);

            if (client_index == -1) {
                fprintf(stderr, "Failed to find user for %d\n", target_fd);
                close(target_fd);
                return;
            }

            process_user_data(&users[client_index], tcp_buffer);
            return;
        }

        if (bytes_read == 0) {
            printf("Unable to read data killing connection\n");
            close(target_fd);
            return;
        }

        printf("Failed to read data\n");
    }
}

int connect_server() {
    struct sockaddr_in server_addr;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        fprintf(stderr, "Failed to get socket for TCP server\n");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(6667);

    int bind_res = bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (bind_res < 0) {
        fprintf(stderr, "Failed to bind server on socket %d. Closing server socket!\n", bind_res);
        close(server_fd);
        return -1;
    }

    int listen_res = listen(server_fd, SOMAXCONN);
    if (listen_res < 0) {
        fprintf(stderr, "Failed to listen on TCP server %d. Closing server socket!\n", listen_res);
        close(server_fd);
        return -1;
    }

    int nonblocking_res = set_nonblocking(server_fd);
    if (nonblocking_res == -1) {
        fprintf(stderr, "Failed to set nonblocking mode. Closing server socket!\n");
        return -1;
    }

    return server_fd;
}

int main() {
    int kqueue_fd;
    struct kevent change_event, event_list[32];

    int server_fd = connect_server();

    if (server_fd == -1) {
        fprintf(stderr, "Failed to create server\n");
    }

    kqueue_fd = kqueue();
    if (kqueue_fd == -1) {
        fprintf(stderr, "Failed to create queue. Closing server\n");
        close(server_fd);
        return -1;
    }

    EV_SET(&change_event, server_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(kqueue_fd, &change_event, 1, NULL, 0, NULL) == -1) {
        fprintf(stderr, "Failed to register socket closing server\n");
        close(server_fd);
        return -1;
    }

    while(1) {
        int event_count = kevent(kqueue_fd, NULL, 0, event_list, 32, NULL);
        if (event_count == -1) {
            fprintf(stderr, "Failed to wait on kqueue\n");
            break;
        }

        for (int i = 0; i < event_count; i++) {
            struct kevent target_event = event_list[i];
            process_event(target_event, server_fd, &change_event, kqueue_fd);
        }
    }

    close(server_fd);
    close(kqueue_fd);
    return 0;
}