#ifndef HTTP_H
#define HTTP_H

#include <lua.h>

int l_http_request(lua_State *L);

int l_http_retrieve(lua_State *L);

void manage_completed_requests();

#endif
