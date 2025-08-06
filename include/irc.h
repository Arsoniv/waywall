
#ifndef IRC_H
#define IRC_H
#include "lua.h"
#include <libircclient.h>
#include <pthread.h>
#include <stdbool.h>

#define MAX_QUEUED_MESSAGES 64

struct message_queue {
    char *messages[MAX_QUEUED_MESSAGES];
    volatile int write_pos;
    volatile int read_pos;
    volatile bool should_stop;
};

struct Irc_client {
    irc_session_t *session;
    pthread_t thread_id;
    bool thread_running;
    int callback;
    int index;
    lua_State *L;
    struct message_queue message_queue;
};

struct Irc_client *irc_client_create(const char *ip, long port, const char *nick, const char *pass,
                                     int callback, lua_State *L);
void manage_new_messages();
void irc_client_send(struct Irc_client *client, const char *message);
void irc_client_destroy(struct Irc_client *client);

#endif
