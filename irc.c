#include "irc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include "platform.h"

#define RLP_WELCOME 1
#define RPL_NOTOPIC 331
#define RPL_TOPIC 332
#define RPL_TOPICWHOTIME 333
#define RPL_NAMEREPLY 353
#define RPL_ENDOFNAMES 366
#define ERR_NOSUCHCHANNEL 403
#define ERR_NONICKNAMEGIVEN 431
#define ERR_ERRONEUSNICKNAME 432
#define ERR_NICKNAMEINUSE 433
#define ERR_NICKCOLLISION 436
#define ERR_NOTONCHANNEL 442
#define ERR_NEEDMOREPARAMS 461
#define ERR_ALREADYREGISTERED 462
#define ERR_BADCHANNELKEY 475

char* server_name = "local";
char message_buffer[4096];
// Consider how many users/channels we actually want to support
user_t users[1024];
int next_user_slot = 0;
channel_t channels [1024];
int next_channel_index = 0;

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

int add_channel_topic(user_t* user, channel_t* channel, char* buffer, int buffer_size) {
    if (channel->topic != NULL) {
        return snprintf(
            buffer, buffer_size,
            ":%s %03d %s %s :%s\r\n",
            server_name, RPL_TOPIC, 
            user->nick ? user->nick : "*",
            channel->name, channel->topic
        );
    }

    return snprintf(
        buffer, buffer_size,
        ":%s %03d %s %s :No topic is set\r\n",
        server_name, RPL_NOTOPIC,
        user->nick ? user->nick : "*",
        channel->name
    );
}

void process_join(user_t* user, char* channel_name, char* channel_key) {
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
    
    for (int i = 0; i < sizeof(channel->participants) / sizeof(channel->participants[0]); i++) {
        if (channel->participants[i] == NULL) {
            channel->participants[i] = user;
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
    message_length += add_channel_topic(user, channel, res_buffer + message_length, sizeof(res_buffer));

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
        if (channel->participants[i] == NULL) {
            break;
        }

        message_length += snprintf(
            res_buffer + message_length, sizeof(res_buffer),
            " %s",
            channel->participants[i]->nick 
        );
    }
    message_length += snprintf(
        res_buffer + message_length, sizeof(res_buffer),
        "\r\n:%s %03d %s %s :End of /Names list\r\n",
        server_name, RPL_ENDOFNAMES, user->nick, channel_name
    );

    send_response_to_user(user, res_buffer, message_length);
}

void process_private_message(user_t* user, char* target, char* message) {
    // https://modern.ircdocs.horse/#privmsg-message
    // TODO validation
    // For now lets just assume we can only support channels
    channel_t* channel = find_channel(target);

    char res_buffer[512];
    for (int i = 0; i < sizeof(channel->participants) / sizeof(channel->participants[0]); i++) {
        user_t* participant = channel->participants[i];
        // Don't send to self
        if (participant == NULL || participant == user) {
            continue;
        }
        
        int message_length = snprintf(
            res_buffer, sizeof(res_buffer),
            ":%s!%s@%s PRIVMSG %s %s\r\n",
            user->nick,
            user->user_name,
            user->host_name,
            target,
            message
        );

        send_response_to_user(participant, res_buffer, message_length);
    }
}

void send_no_such_channel(user_t* user, char* channel_name) {
    char res_buffer[512];
    int message_length = snprintf(
            res_buffer, sizeof(res_buffer),
            ":%s %03d %s %s :No such channel\r\n",
            server_name, ERR_NOSUCHCHANNEL,
            user->nick ? user->nick : "*",
            channel_name
        );

    send_response_to_user(user, res_buffer, message_length);
}

void send_not_in_channel(user_t* user, char* channel_name) {
    char res_buffer[512];
    int message_length = snprintf(
        res_buffer, sizeof(res_buffer),
        ":%s %03d %s %s :You're not on that channel\r\n",
        server_name, ERR_NOTONCHANNEL,
        user->nick ? user->nick : "*",
        channel_name
    );

    send_response_to_user(user, res_buffer, message_length);
}

void process_part(user_t* user, char* channel_name, char* reason) {
    // https://modern.ircdocs.horse/#part-message
    channel_t* channel = find_channel(channel_name);

    if (channel == NULL) {
        send_no_such_channel(user, channel_name);
        return;
    }

    int channel_index = -1;
    for (int i = 0; i < sizeof(channel->participants) / sizeof(channel->participants[0]); i++) {
        if (channel->participants[i] == user) {
            channel_index = i;
            break;
        }
    }

    if (channel_index == -1) {
        send_not_in_channel(user, channel_name);
        return;
    }

    channel->participants[channel_index] = NULL;

    if (reason == NULL) {
        return;
    }

    char res_buffer[512];
    for (int i = 0; i < sizeof(channel->participants) / sizeof(channel->participants[0]); i++) {
        user_t* participant = channel->participants[i];
        // Don't send to self
        if (participant == NULL || participant == user) {
            continue;
        }

        int message_length = snprintf(
            res_buffer, sizeof(res_buffer),
            ":%s!%s@%s PRIVMSG %s %s\r\n",
            user->nick,
            user->user_name,
            user->host_name,
            channel_name,
            reason
        );

        send_response_to_user(participant, res_buffer, message_length);
    }
}

void process_topic(user_t* user, char* channel_name, char* topic) {
    // https://modern.ircdocs.horse/#topic-message
    char res_buffer[512]; 
    channel_t* channel = find_channel(channel_name);

    if (channel == NULL) {
        send_no_such_channel(user, channel_name);
        return;
    }

    int channel_index = -1;
    for (int i = 0; i < sizeof(channel->participants) / sizeof(channel->participants[0]); i++) {
        if (channel->participants[i] == user) {
            channel_index = i;
            break;
        }
    }

    if (channel_index == -1) {
        send_not_in_channel(user, channel_name);
        return;
    }

    if (topic == NULL) {
        int message_length = add_channel_topic(user, channel, res_buffer, sizeof(res_buffer));
        send_response_to_user(user, res_buffer, message_length);
        return;
    }

    char *dest = malloc(sizeof(topic) + 1);
    strcpy(dest, topic);
    channel->topic = dest;

    for (int i = 0; i < sizeof(channel->participants) / sizeof(channel->participants[0]); i++) {
        user_t* participant = channel->participants[i];
        if (participant == NULL) {
            continue;
        }
        
        int message_length = add_channel_topic(participant, channel, res_buffer, sizeof(res_buffer));
        send_response_to_user(participant, res_buffer, message_length);
    }
}

void process_user_data(user_t* user, tcp_buf_t tcp_buffer) {
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
            process_join(user, channel, channel_key);
        } else if (strcasecmp(user_cmd, "PRIVMSG") == 0) {
            char* target = strtok_r(NULL, " ", &cmd_save_ptr);
            char* message = strtok_r(NULL, "", &cmd_save_ptr);
            process_private_message(user, target, message);
        } else if (strcasecmp(user_cmd, "PART") == 0) {
            char* target = strtok_r(NULL, " ", &cmd_save_ptr);
            char* message = strtok_r(NULL, "", &cmd_save_ptr);
            process_part(user, target, message);
        } else if (strcasecmp(user_cmd, "TOPIC") == 0) {
            char* channel = strtok_r(NULL, " ", &cmd_save_ptr);
            char* topic = strtok_r(NULL, "", &cmd_save_ptr);
            process_topic(user, channel, topic + 1);
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