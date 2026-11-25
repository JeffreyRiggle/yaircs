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

#define RLP_WELCOME 1
#define RPL_NOTOPIC 331
#define RPL_TOPIC 332
#define RPL_TOPICWHOTIME 333
#define RPL_NAMEREPLY 353
#define RPL_ENDOFNAMES 366
#define ERR_NONICKNAMEGIVEN 431
#define ERR_ERRONEUSNICKNAME 432
#define ERR_NICKNAMEINUSE 433
#define ERR_NICKCOLLISION 436
#define ERR_NEEDMOREPARAMS 461
#define ERR_ALREADYREGISTERED 462
#define ERR_BADCHANNELKEY 475

// TODO this should not be fixed like this
static char* server_name = "local";

typedef struct {
    int fd;
    char* nick;
    char* user_name;
    char* real_name;
    char* host_name;
    int connection_established;
} user_t;

typedef struct {
    char* name;
    char* topic;
    char* created_by;
    char* created_at;
    char* key;
    int participants[256];
} channel_t;

// I don't really like this name
typedef struct {
  int len;
  char* body;
} tcp_buf_t;

char message_buffer[4096];
// Consider how many users/channels we actually want to support
user_t users[1024];
int next_user_slot = 0;
channel_t channels [1024];
int next_channel_index = 0;

void send_response_to_user(user_t* user, char* res_buffer, int message_length) {
    if (send(user->fd, res_buffer, message_length, MSG_NOSIGNAL) == -1) {
        fprintf(stderr, "failed to send %d bytes to fd %d\n", message_length, user->fd);
    }
}

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

    inet_ntop(sas->ss_family, src, client_ip, INET6_ADDRSTRLEN);
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

void process_cap(user_t* user, char* cmd, char* args, char* arg_ptr) {
    // https://modern.ircdocs.horse/#join-message
    if (strcasecmp(cmd, "LS") == 0) {
    }
}

void process_ping(user_t* user, char* message) {
    if (message == NULL) {
        message = "";
    }
    char res_buffer[512];
    int message_length = snprintf(res_buffer, sizeof(res_buffer), ":%s PONG %s :%s\r\n", server_name, server_name, message);
    send_response_to_user(user, res_buffer, message_length);
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
    size_t user_count = sizeof(users) / sizeof(users[0]);
    for (int i = 0; i < user_count; i++) {
        if (users[i].nick == NULL) {
            continue;
        }

        if (strcasecmp(users[i].nick, nick) == 0) {
            return 1;
        }
    }

    return 0;
}

void process_nick(user_t* user, char* nick) {
    // https://modern.ircdocs.horse/#join-message
    char res_buffer[512];

    if (nick == NULL) {
        int message_length = snprintf(
            res_buffer, sizeof(res_buffer),
            ":%s %03d %s :No nickname given\r\n",
            server_name, ERR_NONICKNAMEGIVEN,
            user->nick ? user->nick : "*"
        );
        send_response_to_user(user, res_buffer, message_length);
        return;
    }

    if (is_nick_valid(nick) != 1) {
        int message_length = snprintf(
            res_buffer, sizeof(res_buffer),
            ":%s %03d %s :Erroneus nickname\r\n",
            server_name, ERR_ERRONEUSNICKNAME,
            user->nick ? user->nick : "*"
        );
        send_response_to_user(user, res_buffer, message_length);
        return;
    }

    if (is_nick_in_use(nick)) {
        int message_length = snprintf(
            res_buffer, sizeof(res_buffer),
            ":%s %03d %s :Nickname is already in use\r\n",
            server_name, ERR_ERRONEUSNICKNAME,
            user->nick ? user->nick : "*"
        );
        send_response_to_user(user, res_buffer, message_length);
        return;
    }

    int message_length = snprintf(
        res_buffer, sizeof(res_buffer),
        ":%s NICK %s\r\n",
        user->nick ? user->nick : "*",
        nick
    );

    char *dest = malloc(sizeof(nick) + 1);
    strcpy(dest, nick);
    user->nick = dest;
    send_response_to_user(user, res_buffer, message_length);
}

void process_user(user_t* user, char* user_name, char* mode_param, char* real_name) {
    if (real_name == NULL) {
        char res_buffer[512];
        int message_length = snprintf(
            res_buffer, sizeof(res_buffer),
            ":%s %03d %s USER :Not enough parameters\r\n",
            server_name, ERR_NEEDMOREPARAMS,
            user->nick ? user->nick : "*"
        );
        send_response_to_user(user, res_buffer, message_length);
        return;
    }

    if (user->user_name != NULL) {
        char res_buffer[512];
        int message_length = snprintf(
            res_buffer, sizeof(res_buffer),
            ":%s %03d %s :You may not reregister\r\n",
            server_name, ERR_ALREADYREGISTERED,
            user->nick ? user->nick : "*"
        );
        send_response_to_user(user, res_buffer, message_length);
        return;
    }

    char *un_dest = malloc(sizeof(user_name) + 1);
    strcpy(un_dest, user_name);

    char *rn_dest = malloc(sizeof(real_name) + 1);
    strcpy(rn_dest, real_name);

    user->user_name = un_dest;
    user->real_name = rn_dest;
}

channel_t* find_channel(char* name) {
    for (int i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        if (channels[i].name != NULL && strcasecmp(channels[i].name, name) == 0) {
            return &channels[i];
        }
    }

    return NULL;
}

channel_t* get_or_create_channel(char* channel_name, char* channel_key) {
    channel_t* channel = find_channel(channel_name);

    if (channel != NULL) {
        return channel;
    }

    channel_t* new_channel = &channels[next_channel_index++];

    char* cn_dest = malloc(sizeof(channel_name) + 1);
    strcpy(cn_dest, channel_name);
    
    new_channel->name = cn_dest;
    new_channel->participants[0] = -1;
    
    if (channel_key != NULL) {
        char* k_dest = malloc(sizeof(channel_key) + 1);
        strcpy(k_dest, channel_key);
        new_channel->key = k_dest;
    }

    return new_channel;
}

char* get_current_timestamp_utc() {
    time_t now = time(NULL);
    struct tm utc_time;
    char* buffer = malloc(21);

    // TODO handle failure case
    if (gmtime_r(&now, &utc_time) != NULL) {
        strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc_time);
    }

    return buffer;
}

int get_client_index(int fd) {
    size_t user_count = sizeof(users) / sizeof(users[0]);
    for (int i = 0; i < user_count; i++) {
        if (users[i].fd == fd) {
            return i;
        }
    }

    return -1;
}

void process_join(user_t* user, int user_index, char* channel_name, char* channel_key) {
    // https://modern.ircdocs.horse/#join-message
    // TODO validation

    char res_buffer[512]; 
    channel_t* channel = get_or_create_channel(channel_name, channel_key);

    if (channel->key != NULL && (channel_key == NULL || strcasecmp(channel->key, channel_key) != 0)) {
        int message_length = snprintf(
            res_buffer, sizeof(res_buffer),
            ":%s %03d %s %s :Cannot join channel (+k)\r\n",
            server_name, ERR_BADCHANNELKEY,
            user->nick ? user->nick : "*",
            channel_name
        );
        send_response_to_user(user, res_buffer, message_length);
        return;
    }
    
    for (int i = 0; i < sizeof(channel->participants) / sizeof(user->fd); i++) {
        if (channel->participants[i] == -1) {
            channel->participants[i] = user_index;
            channel->participants[i + 1] = -1;
            break;
        }
    }

    // Joined channel
    int message_length = snprintf(
        res_buffer, sizeof(res_buffer),
        ":%s JOIN %s\r\n",
        user->nick ? user->nick : "*",
        channel_name
    );

    // Topic
    if (channel->topic != NULL) {
        message_length += snprintf(
            res_buffer + message_length, sizeof(res_buffer),
            ":%s %03d %s :%s\r\n",
            server_name, RPL_TOPIC, channel_name, channel->topic
        );
    } else {
        message_length += snprintf(
            res_buffer + message_length, sizeof(res_buffer),
            ":%s %03d %s :No topic is set\r\n",
            server_name, RPL_NOTOPIC, channel_name
        );
    }

    // Who created the channel
    if (channel->created_by != NULL) {
        message_length += snprintf(
            res_buffer + message_length, sizeof(res_buffer),
            ":%s %03d %s %s %s\r\n",
            server_name, RPL_TOPICWHOTIME, channel_name,
            channel->created_by, channel->created_at
        );
    } else {
        channel->created_by = user->nick;
        channel->created_at = get_current_timestamp_utc();
    }
    
    // List of users in the channel
    message_length += snprintf(
        res_buffer + message_length, sizeof(res_buffer),
        ":%s %03d %s = %s :",
        server_name, RPL_NAMEREPLY, user->nick, channel_name 
    );
    for (int i = 0; i < sizeof(channel->participants) / sizeof(channel->participants[0]); i++) {
        // Finished reading participants.
        if (channel->participants[i] == -1) {
            break;
        }

        message_length += snprintf(
            res_buffer + message_length, sizeof(res_buffer),
            " %s",
            users[channel->participants[i]].nick 
        );
    }
    message_length += snprintf(
        res_buffer + message_length, sizeof(res_buffer),
        "\r\n:%s %03d %s %s :End of /Names list\r\n",
        server_name, RPL_ENDOFNAMES, user->nick, channel_name
    );

    send_response_to_user(user, res_buffer, message_length);
}

void process_private_message(user_t* user, int user_index, char* target, char* message) {
    // https://modern.ircdocs.horse/#privmsg-message
    // TODO validation
    // For now lets just assume we can only support channels
    channel_t* channel = find_channel(target);

    char res_buffer[512];
    for (int i = 0; i < sizeof(channel->participants) / sizeof(channel->participants[0]); i++) {
        int participant_index = channel->participants[i];
        // Don't send to self
        if (participant_index == user_index) {
            continue;
        }
        
        // Finished reading participants.
        if (participant_index == -1) {
            break;
        }
        
        user_t participant = users[participant_index];
        int message_length = snprintf(
            res_buffer, sizeof(res_buffer),
            ":%s!%s@%s PRIVMSG %s %s\r\n",
            user->nick,
            user->user_name,
            user->host_name,
            target,
            message
        );

        send_response_to_user(&participant, res_buffer, message_length);
    }
}

void process_user_data(user_t* user, int user_index, tcp_buf_t tcp_buffer) {
    printf("Message from fd %d:\r\n %s\r\n", user->fd, tcp_buffer.body);
    char *save_ptr = NULL;
    char *cmd_line = strtok_r(tcp_buffer.body, "\r\n", &save_ptr);

    while (cmd_line != NULL) {
        char *cmd_save_ptr = NULL;
        char *user_cmd = strtok_r(cmd_line, " ", &cmd_save_ptr);

        if (strcasecmp(user_cmd, "CAP") == 0) {
            char* cap_command = strtok_r(NULL, " ", &cmd_save_ptr);
            process_cap(user, cap_command, user_cmd, cmd_save_ptr);
        } else if (strcasecmp(user_cmd, "NICK") == 0) {
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
        } else if (strcasecmp(user_cmd, "JOIN") == 0) {
            // TODO support multi channel joins
            char* channel = strtok_r(NULL, " ", &cmd_save_ptr);
            char* channel_key = strtok_r(NULL, " ", &cmd_save_ptr);
            process_join(user, user_index, channel, channel_key);
        } else if (strcasecmp(user_cmd, "PRIVMSG") == 0) {
            char* target = strtok_r(NULL, " ", &cmd_save_ptr);
            char* message = strtok_r(NULL, "", &cmd_save_ptr);
            process_private_message(user, user_index, target, message);
        }

        cmd_line = strtok_r(NULL, "\r\n", &save_ptr);
    }

    if (user->connection_established != 1 && user->nick != NULL && user->user_name != NULL) {
        user->connection_established = 1;
        char res_buffer[512];
        int message_length = snprintf(
            res_buffer, sizeof(res_buffer),
            ":%s %03d %s :Welcome to IRC %s!%s@%s\r\n",
            server_name, RLP_WELCOME, user->nick,
            user->nick, user->user_name, user->host_name
        );
        send_response_to_user(user, res_buffer, message_length);
    }
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

            int client_index = get_client_index(target_fd);

            if (client_index == -1) {
                fprintf(stderr, "Failed to find user for %d\n", target_fd);
                close(target_fd);
                return;
            }

            process_user_data(&users[client_index], client_index, tcp_buffer);
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