#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "irc.h"
#include "platform.h"

int fd = 1;

void send_response_to_user(user_t* user, char* res_buffer, int message_length) {
    printf("Sending to %s: %s", user->nick, res_buffer);
}

void add_user() {
    printf("Adding user %d\n", fd);
    users[next_user_slot].fd = fd++;
    users[next_user_slot++].host_name = "127.0.0.1";
}

void disconnect_user() {
    int fd;
    printf("User fd:\n");
    if (scanf("%d", &fd) != 1) {
        printf("Invalid fd");
        return;
    }

    int ind = get_client_index(fd);
    users[ind].fd = 0;
}

int main() {
    char buffer[512];

    while (1) {
        printf("Provide command:");
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            continue;
        }

        char *save_ptr = NULL;
        char* cmd = strtok_r(buffer, " ", &save_ptr);

        if (strcasecmp(cmd, "exit") == 0) {
            break;
        }

        if (strcasecmp(cmd, "connect\n") == 0) {
            add_user();
        } else if (strcasecmp(cmd, "disconnect\n") == 0) {
            disconnect_user();
        } else if (strcasecmp(cmd, "send") == 0) {
            int fd = atoi(strtok_r(NULL, " ", &save_ptr));
            char* command = strtok_r(NULL, "", &save_ptr);

            tcp_buf_t req = {0};
            req.body = command;
            req.len = sizeof(command);

            int client_index = get_client_index(fd);
            process_user_data(&users[client_index], client_index, req);
        }
    }
}