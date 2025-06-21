//
// Created by arsoniv on 6/10/25.
//

#include "util/log.h"
#include "http.h"
#include "config/vm.h"
#include <pthread.h>
#include <time.h>
#include <lua.h>
#include <lauxlib.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

struct Http_data_object {
    char *data;
    size_t size;
};

static bool request_in_progress = false;

static bool should_send_http_event = false;

void *vm;

static int request_index = 0;

static struct Http_data_object responses[32];

struct Thread_args {
    char *url;
    long sleep_ms;
};

static size_t write_callback(const void *contents, const size_t member_size, const size_t member_count, void *buffer) {

    struct Http_data_object *mem = buffer;

    const size_t new_data_size = member_count * member_size;
    const size_t current_buffer_size = mem->size;

    char *tmp_ptr = realloc(mem->data, current_buffer_size + new_data_size + 1);
    if (!tmp_ptr) return 0;
    mem->data = tmp_ptr;

    memcpy(mem->data + current_buffer_size, contents, new_data_size);
    mem->size += new_data_size;
    mem->data[mem->size] = '\0';

    return new_data_size;
}

struct timespec get_timespec_from_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    return ts;
}

void *http_get(void *arg_void) {
    struct Thread_args *arg = (struct Thread_args *)arg_void;

    CURL *curl = curl_easy_init();

    struct Http_data_object buffer = { .data = malloc(1), .size = 0 };

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

    responses[request_index] = buffer;

    request_index++;
    if (request_index >= 32) request_index = 0;

    free(arg->url);
    free(arg);

    request_in_progress = false;
    should_send_http_event = true;

    return NULL;
}


int l_http_request(lua_State *L) {

    if (!request_in_progress) {

        vm = config_vm_from(L);

        const char *url = luaL_checkstring(L, 1);
        const long sleep_ms = luaL_checkinteger(L, 2);
        char *url_copy = strdup(url);

        struct Thread_args *args = malloc(sizeof(struct Thread_args));
        args->url = url_copy;
        args->sleep_ms = sleep_ms;

        pthread_t thread;
        pthread_create(&thread, NULL, http_get, args);
        pthread_detach(thread);

        request_in_progress = true;

        lua_pushinteger(L, request_index);

        return 1;
    }

    ww_log(LOG_INFO, "Could not make http request because another request is in progress...");
    return 0;
}

int l_http_retrieve(lua_State *L) {

    const int index = luaL_checkint(L, 1);

    if (index < 32 && index >= 0) {
        lua_pushstring(L, responses[index].data);
        free(responses[index].data);
        responses[index].data = NULL;
        return 1;
    }
    return 0;
}

void manage_completed_requests() {
    if (vm && should_send_http_event) {
        config_vm_signal_event(vm, "http");
        should_send_http_event = false;
        ww_log(LOG_INFO, "sent http event");
    }
}
