#include "irc.h"
#include "lauxlib.h"
#include "util/log.h"
#include <libircclient.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/alloc.h>

#define MAX_CLIENTS 8

static struct Irc_client *all_clients[MAX_CLIENTS];
static int client_count = 0;
static irc_callbacks_t callbacks = {0};
static bool callbacks_initialized = false;

static void
queue_init(struct message_queue *queue) {
    memset(queue->messages, 0, sizeof(queue->messages));
    queue->write_pos = 0;
    queue->read_pos = 0;
    queue->should_stop = false;
}

static bool
queue_push(struct message_queue *queue, const char *message) {
    if (!queue || !message)
        return false;

    int next_write = (queue->write_pos + 1) % MAX_QUEUED_MESSAGES;

    if (next_write == queue->read_pos) {
        int next_read = (queue->read_pos + 1) % MAX_QUEUED_MESSAGES;
        if (queue->messages[queue->read_pos]) {
            free(queue->messages[queue->read_pos]);
            queue->messages[queue->read_pos] = NULL;
        }
        queue->read_pos = next_read;
    }

    queue->messages[queue->write_pos] = strdup(message);
    if (!queue->messages[queue->write_pos]) {
        return false;
    }

    queue->write_pos = next_write;
    return true;
}

static char *
queue_pop(struct message_queue *queue) {
    if (!queue || queue->read_pos == queue->write_pos) {
        return NULL;
    }

    char *message = queue->messages[queue->read_pos];
    queue->messages[queue->read_pos] = NULL;
    queue->read_pos = (queue->read_pos + 1) % MAX_QUEUED_MESSAGES;

    return message;
}

static void
queue_cleanup(struct message_queue *queue) {
    if (!queue)
        return;

    for (int i = 0; i < MAX_QUEUED_MESSAGES; i++) {
        if (queue->messages[i]) {
            free(queue->messages[i]);
            queue->messages[i] = NULL;
        }
    }
    queue->read_pos = queue->write_pos = 0;
}

// Find client by session - must be called from IRC thread
static struct Irc_client *
find_client_by_session(irc_session_t *session) {
    // This is called from IRC callbacks which run in the IRC thread
    // We can safely read the all_clients array since it's only modified
    // from the main thread and clients are never freed while threads are running
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (all_clients[i] && all_clients[i]->session == session) {
            return all_clients[i];
        }
    }
    return NULL;
}

static void
format_irc_message(char *buf, size_t buf_size, const char *prefix, const char *origin,
                   const char **params, unsigned int count) {
    int n = snprintf(buf, buf_size, "%s", prefix ? prefix : "");
    if (n < 0 || n >= (int)buf_size) {
        n = buf_size - 1;
        buf[n] = '\0';
        return;
    }

    if (origin && n < (int)buf_size - 1) {
        int ret = snprintf(buf + n, buf_size - n, " from %s", origin);
        if (ret > 0 && ret < (int)(buf_size - n)) {
            n += ret;
        }
    }

    for (unsigned int i = 0; i < count && n < (int)buf_size - 1; i++) {
        if (params[i]) {
            int ret = snprintf(buf + n, buf_size - n, " %s", params[i]);
            if (ret > 0 && ret < (int)(buf_size - n)) {
                n += ret;
            }
        }
    }

    buf[buf_size - 1] = '\0';
}

void
on_any_numeric(irc_session_t *session, unsigned int event, const char *origin, const char **params,
               unsigned int count) {
    struct Irc_client *client = find_client_by_session(session);
    if (!client)
        return;

    char msg_buf[1024];
    char event_str[32];
    snprintf(event_str, sizeof(event_str), "%u", event);

    format_irc_message(msg_buf, sizeof(msg_buf), event_str, origin, params, count);
    queue_push(&client->message_queue, msg_buf);
}

void
on_any_event(irc_session_t *session, const char *event, const char *origin, const char **params,
             unsigned int count) {
    struct Irc_client *client = find_client_by_session(session);
    if (!client || !event)
        return;

    char msg_buf[1024];
    format_irc_message(msg_buf, sizeof(msg_buf), event, origin, params, count);
    queue_push(&client->message_queue, msg_buf);
}

static void *
irc_thread(void *arg) {
    struct Irc_client *client = (struct Irc_client *)arg;
    if (!client || !client->session) {
        ww_log(LOG_ERROR, "Invalid client in IRC thread");
        return NULL;
    }

    ww_log(LOG_INFO, "IRC thread starting for client %d", client->index);

    int ret = irc_run(client->session);
    if (ret != 0) {
        ww_log(LOG_WARN, "irc_run() exited with error: %s",
               irc_strerror(irc_errno(client->session)));
    } else {
        ww_log(LOG_INFO, "irc_run() exited normally");
    }

    // Signal disconnection
    queue_push(&client->message_queue, "DISCONNECTED");
    client->message_queue.should_stop = true;

    ww_log(LOG_INFO, "IRC thread ending for client %d", client->index);
    return NULL;
}

struct Irc_client *
irc_client_create(const char *ip, long port, const char *nick, const char *pass, int callback,
                  lua_State *L) {
    if (!ip || !nick || !L) {
        ww_log(LOG_ERROR, "Invalid parameters for IRC client creation");
        return NULL;
    }

    if (client_count >= MAX_CLIENTS) {
        ww_log(LOG_ERROR, "Too many IRC clients (max %d)", MAX_CLIENTS);
        return NULL;
    }

    // Initialize callbacks once
    if (!callbacks_initialized) {
        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.event_numeric = on_any_numeric;
        callbacks.event_unknown = on_any_event;
        callbacks.event_privmsg = on_any_event;
        callbacks.event_connect = on_any_event;
        callbacks.event_join = on_any_event;
        callbacks.event_part = on_any_event;
        callbacks.event_quit = on_any_event;
        callbacks_initialized = true;
    }

    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (all_clients[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        ww_log(LOG_ERROR, "No free slots for IRC clients");
        return NULL;
    }

    irc_session_t *session = irc_create_session(&callbacks);
    if (!session) {
        ww_log(LOG_ERROR, "Failed to create IRC session");
        return NULL;
    }

    struct Irc_client *client = zalloc(1, sizeof(struct Irc_client));

    client->session = session;
    client->callback = callback;
    client->index = slot;
    client->L = L;
    client->thread_running = false;
    queue_init(&client->message_queue);

    all_clients[slot] = client;
    client_count++;

    if (irc_connect(session, ip, port, pass, nick, nick, nick) != 0) {
        ww_log(LOG_ERROR, "IRC connection failed: %s", irc_strerror(irc_errno(session)));

        all_clients[slot] = NULL;
        client_count--;
        queue_cleanup(&client->message_queue);
        irc_destroy_session(session);
        free(client);
        return NULL;
    }

    if (pthread_create(&client->thread_id, NULL, irc_thread, client) != 0) {
        ww_log(LOG_ERROR, "Failed to create IRC thread");

        irc_disconnect(session);
        all_clients[slot] = NULL;
        client_count--;
        queue_cleanup(&client->message_queue);
        irc_destroy_session(session);
        free(client);
        return NULL;
    }

    client->thread_running = true;
    ww_log(LOG_INFO, "IRC client created successfully (slot %d)", slot);
    return client;
}

void
irc_client_send(struct Irc_client *client, const char *message) {
    if (!client || !client->session || !message) {
        ww_log(LOG_WARN, "Invalid parameters for IRC send");
        return;
    }

    if (!client->thread_running) {
        ww_log(LOG_WARN, "Cannot send to disconnected IRC client");
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

    ww_log(LOG_INFO, "Destroying IRC client %d", client->index);

    if (client->thread_running && client->session) {
        client->message_queue.should_stop = true;
        irc_disconnect(client->session);

        if (pthread_join(client->thread_id, NULL) != 0) {
            ww_log(LOG_WARN, "Failed to join IRC thread");
        }
        client->thread_running = false;
    }

    if (client->L && client->callback != LUA_NOREF) {
        luaL_unref(client->L, LUA_REGISTRYINDEX, client->callback);
        client->callback = LUA_NOREF;
    }

    if (client->session) {
        irc_destroy_session(client->session);
        client->session = NULL;
    }

    queue_cleanup(&client->message_queue);

    if (client->index >= 0 && client->index < MAX_CLIENTS) {
        all_clients[client->index] = NULL;
        client_count--;
    }

    free(client);
    ww_log(LOG_INFO, "IRC client destroyed");
}

void
manage_new_messages(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct Irc_client *client = all_clients[i];
        if (!client)
            continue;

        char *message;
        while ((message = queue_pop(&client->message_queue)) != NULL) {

            if (client->L && client->callback != LUA_NOREF) {
                lua_rawgeti(client->L, LUA_REGISTRYINDEX, client->callback);
                if (lua_isfunction(client->L, -1)) {
                    lua_pushstring(client->L, message);
                    if (lua_pcall(client->L, 1, 0, 0) != LUA_OK) {
                        const char *error = lua_tostring(client->L, -1);
                        ww_log(LOG_ERROR, "IRC callback error: %s", error ? error : "unknown");
                        lua_pop(client->L, 1);
                    }
                } else {
                    lua_pop(client->L, 1);
                    ww_log(LOG_ERROR, "IRC callback is not a function");
                }
            }

            free(message);
        }
    }
}