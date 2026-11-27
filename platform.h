#ifndef PLATFORM_H
#define PLATFORM_H

#include "irc.h"

void send_response_to_user(user_t* user, char* res_buffer, int message_length);

#endif