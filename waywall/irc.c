#include "irc.h"
#include "config/vm.h"
#include "util/log.h"
#include <libircclient/libircclient.h>
#include <lua.h>
#include <stdlib.h>
#include <string.h>
#include <util/alloc.h>

#define MAX_CLIENTS 16

static struct Irc_client *all_clients[MAX_CLIENTS];
static pthread_mutex_t all_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static int client_count = 0;

static irc_callbacks_t callbacks = {0};
static bool callbacks_initialized = false;

void
on_any_numeric(irc_session_t *session, unsigned int event, const char *origin, const char **params,
               unsigned int count) {
    struct Irc_client *client = NULL;

    pthread_mutex_lock(&all_clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (all_clients[i]->session == session) {
            client = all_clients[i];
            break;
        }
    }
    pthread_mutex_unlock(&all_clients_mutex);

    if (!client)
        return;

    char msg_buf[1024];

    int n = snprintf(msg_buf, sizeof(msg_buf), "%u", event);
    if (n < 0)
        n = 0;
    if (n >= (int)sizeof(msg_buf))
        n = sizeof(msg_buf) - 1;

    if (origin) {
        int ret = snprintf(msg_buf + n, sizeof(msg_buf) - n, " from %s", origin);
        if (ret < 0)
            ret = 0;
        if (ret >= (int)(sizeof(msg_buf) - n))
            ret = (int)(sizeof(msg_buf) - n) - 1;
        n += ret;
    }

    for (unsigned int i = 0; i < count; i++) {
        if (n >= (int)(sizeof(msg_buf) - 1))
            break; // no space left
        int ret = snprintf(msg_buf + n, sizeof(msg_buf) - n, " %s", params[i]);
        if (ret < 0)
            ret = 0;
        if (ret >= (int)(sizeof(msg_buf) - n))
            ret = (int)(sizeof(msg_buf) - n) - 1;
        n += ret;
    }

    pthread_mutex_lock(&client->data_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!client->data[i].data) {
            client->data[i].data = strdup(msg_buf);
            client->data[i].size = strlen(msg_buf);
            client->data[i].client_index = client->index;
            break;
        }
    }
    pthread_mutex_unlock(&client->data_mutex);
}

void
on_any_event(irc_session_t *session, const char *event, const char *origin, const char **params,
             unsigned int count) {
    struct Irc_client *client = NULL;

    pthread_mutex_lock(&all_clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (all_clients[i]->session == session) {
            client = all_clients[i];
            break;
        }
    }
    pthread_mutex_unlock(&all_clients_mutex);

    if (!client)
        return;

    char msg_buf[1024];

    int n = snprintf(msg_buf, sizeof(msg_buf), "%s", event);
    if (n < 0)
        n = 0;
    if (n >= (int)sizeof(msg_buf))
        n = sizeof(msg_buf) - 1;

    if (origin) {
        int ret = snprintf(msg_buf + n, sizeof(msg_buf) - n, " from %s", origin);
        if (ret < 0)
            ret = 0;
        if (ret >= (int)(sizeof(msg_buf) - n))
            ret = (int)(sizeof(msg_buf) - n) - 1;
        n += ret;
    }

    for (unsigned int i = 0; i < count; i++) {
        if (n >= (int)(sizeof(msg_buf) - 1))
            break; // no space left
        int ret = snprintf(msg_buf + n, sizeof(msg_buf) - n, " %s", params[i]);
        if (ret < 0)
            ret = 0;
        if (ret >= (int)(sizeof(msg_buf) - n))
            ret = (int)(sizeof(msg_buf) - n) - 1;
        n += ret;
    }

    pthread_mutex_lock(&client->data_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!client->data[i].data) {
            client->data[i].data = strdup(msg_buf);
            client->data[i].size = strlen(msg_buf);
            client->data[i].client_index = client->index;
            break;
        }
    }
    pthread_mutex_unlock(&client->data_mutex);
}

static void *
irc_thread(void *arg) {
    struct Irc_client *client = arg;
    irc_run(client->session);
    return NULL;
}

struct Irc_client *
irc_client_create(const char *ip, long port, const char *nick, const char *pass, int callback,
                  lua_State *L) {
    pthread_mutex_lock(&all_clients_mutex);

    if (client_count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&all_clients_mutex);
        ww_log(LOG_ERROR, "Too many IRC clients");
        return NULL;
    }

    if (!callbacks_initialized) {
        callbacks.event_numeric = on_any_numeric;
        callbacks.event_unknown = on_any_event;
        callbacks.event_privmsg = on_any_event;
        callbacks.event_connect = on_any_event;
        callbacks_initialized = true;
    }

    irc_session_t *session = irc_create_session(&callbacks);
    if (!session) {
        pthread_mutex_unlock(&all_clients_mutex);
        ww_log(LOG_ERROR, "Failed to create IRC session");
        return NULL;
    }

    struct Irc_client *client = zalloc(1, sizeof(struct Irc_client));

    client->session = session;
    client->callback = callback;
    pthread_mutex_init(&client->data_mutex, NULL);
    client->index = client_count;
    client->L = L;
    all_clients[client_count++] = client;

    pthread_mutex_unlock(&all_clients_mutex);

    if (irc_connect(session, ip, port, pass, nick, nick, nick)) {
        ww_log(LOG_ERROR, "IRC connection failed: %s", irc_strerror(irc_errno(session)));
        irc_destroy_session(session);
        pthread_mutex_destroy(&client->data_mutex);

        pthread_mutex_lock(&all_clients_mutex);
        for (int i = 0; i < client_count; i++) {
            if (all_clients[i] == client) {
                for (int j = i; j < client_count - 1; j++) {
                    all_clients[j] = all_clients[j + 1];
                    if (all_clients[j])
                        all_clients[j]->index = j;
                }
                client_count--;
                break;
            }
        }
        pthread_mutex_unlock(&all_clients_mutex);

        free(client);
        return NULL;
    }

    pthread_t thread;
    pthread_create(&thread, NULL, irc_thread, client);
    pthread_detach(thread);

    return client;
}

void
irc_client_send(struct Irc_client *client, const char *message) {
    if (!client || !client->session || !message) {
        return;
    }

    int ret = irc_send_raw(client->session, message);
    if (ret != 0) {
        ww_log(LOG_WARN, "Failed to send IRC message: %s",
               irc_strerror(irc_errno(client->session)));
    }
}

void
irc_client_destroy(struct Irc_client *client) {
    if (!client)
        return;

    ww_log(LOG_INFO, "Destroying IRC client.");

    irc_disconnect(client->session);
    irc_destroy_session(client->session);

    pthread_mutex_lock(&client->data_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        free(client->data[i].data);
        client->data[i].data = NULL;
        client->data[i].size = 0;
    }
    pthread_mutex_unlock(&client->data_mutex);

    pthread_mutex_destroy(&client->data_mutex);

    pthread_mutex_lock(&all_clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (all_clients[i] == client) {
            for (int j = i; j < client_count - 1; j++) {
                all_clients[j] = all_clients[j + 1];
                if (all_clients[j])
                    all_clients[j]->index = j;
            }
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&all_clients_mutex);

    free(client);
}

void
manage_new_messages() {
    pthread_mutex_lock(&all_clients_mutex);
    int local_client_count = client_count;
    struct Irc_client *local_clients[MAX_CLIENTS];
    for (int i = 0; i < local_client_count; i++) {
        local_clients[i] = all_clients[i];
    }
    pthread_mutex_unlock(&all_clients_mutex);

    for (int i = 0; i < local_client_count; i++) {
        struct Irc_client *client = local_clients[i];

        pthread_mutex_lock(&client->data_mutex);

        for (int j = 0; j < MAX_CLIENTS; j++) {
            struct Message_data_object *msg = &client->data[j];
            if (msg->data) {
                lua_rawgeti(client->L, LUA_REGISTRYINDEX, client->callback);
                lua_pushstring(client->L, msg->data);
                if (lua_pcall(client->L, 1, 0, 0) != LUA_OK) {
                    ww_log(LOG_ERROR, "IRC callback error: %s", lua_tostring(client->L, -1));
                    lua_pop(client->L, 1);
                }

                free(msg->data);
                msg->data = NULL;
                msg->size = 0;
            }
        }

        pthread_mutex_unlock(&client->data_mutex);
    }
}
