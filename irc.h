#ifndef IRC_H
#define IRC_H

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

// TODO this should not be fixed like this
extern char* server_name;

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
    user_t* participants[256];
} channel_t;

// I don't really like this name
typedef struct {
  int len;
  char* body;
} tcp_buf_t;

extern char message_buffer[4096];
// Consider how many users/channels we actually want to support realistically 65535 is our max
extern user_t users[1024];
extern int next_user_slot;
extern channel_t channels [1024];
extern int next_channel_index;

int process_user_data(user_t* user, tcp_buf_t tcp_buffer);
int get_client_index(int fd);
#endif