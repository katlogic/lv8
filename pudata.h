/*
 * lua_pushuserdata() polyfill (slow).
 * (C) Copyright 2014, Karel Tuma <kat@lua.cz>, All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef lua_pushuserdata
#define PUDATA_RIDX "__pudata_field"

/* Resolve actual userdata. */
static void lua_pushuserdata_portable(lua_State *L, void *p)
{
  luaL_getmetatable(L, PUDATA_RIDX);
  lua_pushlightuserdata(L, p);
  lua_rawget(L, -2);
  lua_replace(L, -2);
}

/* For this to work; we have to wrap lua_newuserdata. */
static void *lua_newuserdata_wrapper(lua_State *L, size_t sz)
{
  void *p = lua_newuserdata(L, sz);
  luaL_getmetatable(L, PUDATA_RIDX);
  if (lua_isnil(L, -1)) { /* Create if missing. */
    lua_pop(L, 1);
    luaL_newmetatable(L, PUDATA_RIDX);
    lua_pushvalue(L, -1);
    lua_setmetatable(L, -2); /* Point to self. */
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode"); /* Weak values (udata). */
  }
  lua_pushlightuserdata(L, p); /* Key. */
  lua_pushvalue(L, -3); /* Value. */
  lua_rawset(L, -3); /* Associate. */
  lua_pop(L, 1); /* Pop metatable. */
  return p;
}

#define lua_pushuserdata(L, p) lua_pushuserdata_portable(L, p)
#define lua_newuserdata(L, sz) lua_newuserdata_wrapper(L, sz)
#endif

