#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>

#define RLP_WELCOME 1
#define ERR_NONICKNAMEGIVEN 431
#define ERR_ERRONEUSNICKNAME 432
#define ERR_NICKNAMEINUSE 433
#define ERR_NICKCOLLISION 436

typedef struct {
    int fd;
    char* nick;
    char* user_name;
    char* real_name;
    char* host_name;
} user_t;

typedef struct {
  int len;
  char* body;
} tcp_buf_t;

char message_buffer[4096];
// Consider how many users we actually want to support
user_t users[1024];

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return flags;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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

    inet_ntop(sas->ss_family, src, client_ip, sizeof(client_ip));
    return client_ip;
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

int process_cap(user_t* user, char* cmd, char* args, char* arg_ptr) {
    if (strcasecmp(cmd, "LS") == 0) {
        // TODO make this work
    }
    return 0;
}

int process_ping(user_t* user, char* message) {
    if (message == NULL) {
        message = "";
    }
    // Consider ping could be result in a large message that could not be written
    char res_buffer[512];
    snprintf(res_buffer, sizeof(res_buffer), ":local PONG local %s", message);
    if (send(user->fd, res_buffer, sizeof(res_buffer), MSG_NOSIGNAL) == -1) {
        fprintf(stderr, "failed to send %zu bytes to fd %d\n", sizeof(res_buffer), user->fd);
    }

    return 0;
}

int is_nick_valid(char* nick) {
    if (
        strncmp(nick, ":", 1) == 0 && 
        strncmp(nick, "#", 1) == 0 &&
        strncmp(nick, "&#", 2) == 0 &&
        strchr(nick, " " != NULL)) {
        return 0;
    }

    return 1;
}

int is_nick_in_use(char* nick) {
    for (int i = 0; i < sizeof(users); i++) {
        if (strcasecmp(users[i].nick, nick) == 0) {
            return 1;
        }
    }

    return 0;
}

int process_nick(user_t* user, char* nick) {
    if (nick == NULL) {
        return ERR_NONICKNAMEGIVEN;
    }

    if (is_nick_valid(nick) != 0) {
        return ERR_ERRONEUSNICKNAME;
    }

    if (is_nick_in_use(nick)) {
        return ERR_NICKNAMEINUSE;
    }

    char *dest = malloc(sizeof(nick) + 1);
    strcpy(dest, nick);
    user->nick = dest;
    return 0;
}

// TODO read the docs on this.
int process_user(user_t* user, char* user_param, char* mode_param, char* real_name) {
    char *un_dest = malloc(sizeof(user_param) + 1);
    strcpy(un_dest, user_param);

    char *rn_dest = malloc(sizeof(real_name) + 1);
    strcpy(rn_dest, real_name);

    user->user_name = un_dest;
    user->real_name = rn_dest;
    return 0;
}

void process_user_data(user_t* user, tcp_buf_t tcp_buffer) {
    printf("Got message %s\n from %d\n", tcp_buffer.body, user->fd);
    char *save_ptr = NULL;
    char *cmd_line = strtok_r(tcp_buffer.body, "\r\n", &save_ptr);

    int is_greeting = 0;
    while (cmd_line != NULL) {
        printf("Processing %s\n", cmd_line);

        char *cmd_save_ptr = NULL;
        char *user_cmd = strtok_r(cmd_line, " ", &cmd_save_ptr);

        if (strcasecmp(user_cmd, "CAP") == 0) {
            char* cap_command = strtok_r(NULL, " ", &cmd_save_ptr);
            is_greeting = process_cap(user, cap_command, user_cmd, cmd_save_ptr);
        } else if(strcasecmp(user_cmd, "NICK") == 0) {
            char* nick = strtok_r(NULL, " ", &cmd_save_ptr);
            process_nick(user, nick);
        } else if (strcasecmp(user_cmd, "USER") == 0) {
            char* user_param = strtok_r(NULL, " ", &cmd_save_ptr);
            char* mode_param = strtok_r(NULL, " ", &cmd_save_ptr);
            char* real_name = strtok_r(NULL, " ", &cmd_save_ptr);
            process_user(user, user_param, mode_param, real_name);
        } else if (strcasecmp(user_cmd, "PING") == 0) {
            char* message = strtok_r(NULL, " ", &cmd_save_ptr);
            process_ping(user, message);
        }

        cmd_line = strtok_r(NULL, "\r\n", &save_ptr);
    }

    if (is_greeting == 1) {
        char res_buffer[512];
        snprintf(
            res_buffer, sizeof(res_buffer),
            ":local 001 %s :Welcome to IRC %s!%s@%s\r\n",
            user->nick, user->nick, user->user_name, user->host_name
        );
        if (send(user->fd, res_buffer, sizeof(res_buffer), MSG_NOSIGNAL) == -1) {
            fprintf(stderr, "failed to send %zu bytes to fd %d\n", sizeof(res_buffer), user->fd);
        }
    }
}

void process_event(struct kevent evt, int server_fd, struct kevent* change_event, int kqueue_fd) {
    // TODO handle events
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
        users[0] = user;
        return;
    }

    if (evt.filter == EVFILT_READ) {
        ssize_t bytes_read = read(target_fd, message_buffer, sizeof(message_buffer));

        if (bytes_read > 0) {
            tcp_buf_t tcp_buffer;
            tcp_buffer.body = message_buffer;
            tcp_buffer.len = bytes_read;

            // TODO get the correct user
            process_user_data(&users[0], tcp_buffer);
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