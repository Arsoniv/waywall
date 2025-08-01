
#include "http.h"
#include "config/vm.h"
#include "util/log.h"
#include <curl/curl.h>
#include <lauxlib.h>
#include <lua.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_RESPONSES 32

struct Http_data_object {
    char *data;
    size_t size;
    bool should_send_event;
};

void *vm_http;

static int request_index = 0;
static struct Http_data_object responses[MAX_RESPONSES];

static pthread_mutex_t responses_mutex = PTHREAD_MUTEX_INITIALIZER;

struct Thread_args {
    char *url;
    long sleep_ms;
    int request_index;
};

static size_t
write_callback(const void *contents, const size_t member_size, const size_t member_count,
               void *buffer) {

    struct Http_data_object *mem = buffer;

    const size_t new_data_size = member_count * member_size;
    const size_t current_buffer_size = mem->size;

    char *tmp_ptr = realloc(mem->data, current_buffer_size + new_data_size + 1);
    if (!tmp_ptr)
        return 0;
    mem->data = tmp_ptr;

    memcpy(mem->data + current_buffer_size, contents, new_data_size);
    mem->size += new_data_size;
    mem->data[mem->size] = '\0';

    return new_data_size;
}

struct timespec
get_timespec_from_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    return ts;
}

void *
http_get(void *arg_void) {
    struct Thread_args *arg = (struct Thread_args *)arg_void;

    CURL *curl = curl_easy_init();

    struct Http_data_object buffer = {.data = malloc(1), .size = 0, .should_send_event = false};

    curl_easy_setopt(curl, CURLOPT_URL, arg->url);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);

    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    if (arg->sleep_ms > 0) {
        const struct timespec ts = get_timespec_from_ms(arg->sleep_ms);

        nanosleep(&ts, NULL);
    }

    const CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        ww_log(LOG_INFO, "http request completed successfully.");
    } else {
        ww_log(LOG_ERROR, "http request failed.");
    }

    curl_easy_cleanup(curl);

    buffer.should_send_event = true;

    pthread_mutex_lock(&responses_mutex);
    responses[arg->request_index] = buffer;
    pthread_mutex_unlock(&responses_mutex);

    free(arg->url);
    free(arg);

    return NULL;
}

int
l_http_request(lua_State *L) {
    pthread_mutex_lock(&responses_mutex);
    const bool can_use = responses[request_index].data == NULL;
    pthread_mutex_unlock(&responses_mutex);

    if (can_use) {
        vm_http = config_vm_from(L);

        const char *url = luaL_checkstring(L, 1);
        const long sleep_ms = luaL_checkinteger(L, 2);
        char *url_copy = strdup(url);

        struct Thread_args *args = malloc(sizeof(struct Thread_args));
        args->url = url_copy;
        args->sleep_ms = sleep_ms;
        args->request_index = request_index;

        request_index++;
        if (request_index >= MAX_RESPONSES)
            request_index = 0;

        pthread_t thread;
        pthread_create(&thread, NULL, http_get, args);
        pthread_detach(thread);

        lua_pushinteger(L, args->request_index);

        return 1;
    }
    return 0;
}

void
manage_completed_requests() {
    if (vm_http) {
        for (int i = 0; i < MAX_RESPONSES; i++) {
            if (responses[i].should_send_event) {
                config_vm_signal_event_string_int(vm_http, "http", responses[i].data, i);
                responses[i].should_send_event = false;
            }
        }
    }
}
