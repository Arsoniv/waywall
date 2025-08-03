
#ifndef IRC_H
#define IRC_H
#include "lua.h"
#include <libircclient/libircclient.h>
#include <pthread.h>

#define MAX_QUEUED_MESSAGES 16

struct Message_data_object {
    char *data;
    size_t size;
    int client_index;
};

struct Irc_client {
    irc_session_t *session;
    pthread_mutex_t data_mutex;
    int callback;
    struct Message_data_object data[MAX_QUEUED_MESSAGES];
    int index;
    lua_State *L;
};

struct Irc_client *irc_client_create(const char *ip, long port, const char *nick, const char *pass,
                                     int callback, lua_State *L);
void manage_new_messages();
void irc_client_send(struct Irc_client *client, const char *message);
void irc_client_destroy(struct Irc_client *client);

#endif
