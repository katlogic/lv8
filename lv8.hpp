//
// Lua <-> V8 bridge
// (C) Copyright 2014, Karel Tuma <kat@lua.cz>, All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

extern "C" {
#ifndef LV8_CPP
#define LV8_EXTERN extern
#endif
#include "lv8.h"
}

enum {
  LV8_OBJ_LUA,
  LV8_OBJ_JS,
  LV8_OBJ_CTX,
  LV8_OBJ_SB
};

struct lv8_object {
  int type;
  v8::Persistent<v8::Object> object;
};

struct lv8_state {
  int initialized;
  v8::Persistent<v8::FunctionTemplate> proxy;
  v8::Persistent<v8::ObjectTemplate> gtpl;
  ptrdiff_t finhack;
};

struct lv8_context : lv8_object {
  v8::Persistent<v8::Context> context;
  unsigned jscollected;
  unsigned resurrected;
};

/* Public C++ API */
v8::Local<v8::ObjectTemplate> lv8_fs_init();
void lv8_wrap_js2lua(lua_State *L, v8::Handle<v8::Object> o);
lv8_context *lv8_unwrap_lua(lua_State *L, int idx, int *type = 0);
lv8_context *lv8_unwrap_js(lua_State *L, v8::Handle<v8::Object> o, bool context = false);
bool lv8_shallow_copy(lua_State *L, v8::Handle<v8::Object> dst, v8::Handle<v8::Object> o);
bool lv8_shallow_copy_from_lua(lua_State *L, v8::Handle<v8::Object> dst, int idx);
bool lv8_is_js_sandbox(lua_State *L, v8::Handle<v8::Object> o, lv8_context **cp);
bool lv8_is_js_context(v8::Handle<v8::Object> o);

