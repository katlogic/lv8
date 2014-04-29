/*
 * Lua <-> V8 bridge
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

#ifndef NDEBUG
#include <v8-debug.h>
#else
#include <v8.h>
#endif
#include <assert.h>
#include <string.h>

#define LV8_CPP
#include "lv8.hpp"

using namespace v8;
/* Optional performance hacks. */
#define LV8_CACHE_PERSISTENT 1 // Set to 0 if this turns out slow.

/* lua_pushuserdata() is needed for Persistent<> caching. */
#if LV8_CACHE_PERSISTENT
#define LV8_IDENTITY "lv8::identity"
#ifndef lua_pushuserdata
#include "pudata/pudata.h"
#endif
#endif

/* Shortcut macros. */
#define STATEUD lua_upvalueindex(1)
#define REFTAB lua_upvalueindex(2)
#define OBJMT lua_upvalueindex(3)
#define CTXMT lua_upvalueindex(4)
#define V8_STATE ((lv8_state*)lua_touserdata(L, STATEUD))
#define ISOLATE Isolate::GetCurrent()
#define PROXY Local<FunctionTemplate>::New(ISOLATE, V8_STATE->proxy)
#define LOCAL(v) Local<Value>::New(ISOLATE, (v))
#define OREF(v) Local<Object>::New(ISOLATE, (v->object))
#define REF(t,v) Local<t>::New(ISOLATE, (v))
#define ESCAPE(v) scope.Escape(LOCAL(v));
#define NEWSTR(arg...) String::NewFromUtf8(ISOLATE, arg)

/*
 * Notes on how GC works:
 *
 * All JS values crossing into Lua, and also proxies for Lua objects
 * crossing into V8, are anchored using Persistent<Object> inside
 * lv8_object userdata.
 *
 * Real JS objects (convert_js2lua), when their last Lua anchor dies,
 * can then be GCed by V8 (or might be re-anchored again).
 *
 * JS proxies of Lua objects (convert_lua2js) are stored in REFTAB,
 * both for fast lookups and as an anchor. The JS proxies are weak
 * and js_weak_callback is called when there are no more references
 * in JS. This kills the REFTAB anchor in turn (and eventually
 * allows Lua to GC).
 *
 * In general, accessing Lua objects from JS is faster than
 * the opposite (V8 is not very well cut for efficient embedding).
 *
 * Circular dependencies are prevented by short-circuiting proxies
 * on both sides (convert_lua2js|js2_lua).
 */

/* Find JS object associated with Lua value. */
static lv8_object *persistent_lookup_lua(lua_State *L, int idx)
{
  lua_pushvalue(L, idx);
  lua_rawget(L, REFTAB);
  void *p = lua_touserdata(L, -1);
  lua_pop(L, 1);
  return (lv8_object*)p;
}

/* Find Lua object associated with JS value. */
static void persistent_lookup_js(lua_State *L, lv8_object *v)
{
  lua_pushlightuserdata(L, (void*)v);
  lua_rawget(L, REFTAB);
}

/* Associate js object with Lua value (two-way). */
static void persistent_add(lua_State *L, int idx, lv8_object *v)
{
  assert(!persistent_lookup_lua(L, idx));
  lua_pushvalue(L, idx);
  lua_pushlightuserdata(L, v);
  lua_rawset(L, REFTAB); // Map reftab[lua] = js.
  lua_pushlightuserdata(L, v);
  lua_pushvalue(L, idx);
  lua_rawset(L, REFTAB); // Map reftab[js] = lua.
}

/* Last reference of Lua object released. */
static int lv8_obj_gc(lua_State *L)
{
  lv8_context *o = (lv8_context*)lua_touserdata(L, 1);
  assert(o);
  lua_getmetatable(L, 1);
  assert(!lua_isnil(L, -1));
  if (lua_rawequal(L, -1, CTXMT)) {
    o->context.Reset(); // Kill context if it is one.
  } else assert(lua_rawequal(L, -1, OBJMT));
#if LV8_CACHE_PERSISTENT
  HandleScope scope(ISOLATE);
  OREF(o)->SetHiddenValue(NEWSTR(LV8_IDENTITY),
      Undefined(ISOLATE));
#endif
  o->object.Reset();
  return 0;
}

/* Last reference to Lua object from JS died. Remove refs on Lua side. */
static void
js_weak_callback(const WeakCallbackData<v8::Object, lua_State> &data)
{
  HandleScope scope(ISOLATE);
  Local<Object> o = data.GetValue();
  lv8_object *v = (lv8_object*)o->GetAlignedPointerFromInternalField(0);
  lua_State *L = data.GetParameter();

  persistent_lookup_js(L, (lv8_object*)v); // Lookup Lua object.

#ifndef NDEBUG
  lua_getmetatable(L, -1); // These should always short-circuit long before.
  assert(!(lua_rawequal(L, -1, OBJMT) || lua_rawequal(L, -1, CTXMT)));
  lua_pop(L, 1);
#endif

  lua_pushnil(L);
  lua_rawset(L, REFTAB); // Clear Lua -> JS.
  lua_pushlightuserdata(L, (void*)v);
  lua_pushnil(L);
  lua_rawset(L, REFTAB); // Clear JS -> Lua.

  v->object.Reset(); // And kill the Persistent.
}

/* Construct new js context or sandbox. */
int lv8_new_context(struct lua_State *L)
{
  HandleScope scope(ISOLATE);
  lv8_context *ctx = (lv8_context*)lua_newuserdata(L, sizeof(*ctx));
  memset(ctx, 0, sizeof(*ctx));

  lua_pushvalue(L, CTXMT); // Associate obj mt.
  lua_setmetatable(L, -2);

  if (lua_gettop(L) == 1 || lua_isnil(L, 1)) { // No sandbox.
    Local<Context> c = Context::New(ISOLATE);
    ctx->context.Reset(ISOLATE, c);
    ctx->object.Reset(ISOLATE, c->Global());
  } else { // Sandboxed scope, global object is interceptor.
    Local<Context> c = Context::New(ISOLATE, 0, PROXY->InstanceTemplate());
    ctx->context.Reset(ISOLATE, c);
    persistent_add(L, 1, ctx);
    Local<Object> gl = c->Global();
    gl->SetAlignedPointerInInternalField(0, (void*)ctx);
    ctx->object.Reset(ISOLATE, gl);
    ctx->object.SetWeak(L, js_weak_callback);
  }
  return 1;
}

/* Convert Lua value to JS counterpart. */
static Local<Value> convert_lua2js(lua_State *L, int idx)
{
  EscapableHandleScope scope(ISOLATE);
  switch (lua_type(L, idx)) { // Base types.
    case LUA_TBOOLEAN:
      return ESCAPE(Boolean::New(ISOLATE, (bool)lua_toboolean(L, idx)));
    case LUA_TNIL:
      return ESCAPE(Undefined(ISOLATE));
    case LUA_TNUMBER:
      return ESCAPE(Number::New(ISOLATE, lua_tonumber(L, idx)));
    case LUA_TSTRING: {
      size_t n;
      const char *p = lua_tolstring(L, idx, &n);
      return ESCAPE(NEWSTR(p, String::kInternalizedString, (int)n));
    }
  }
  lv8_object *wrapper = (lv8_object*)lua_touserdata(L, idx);
  if (wrapper && lua_getmetatable(L, idx)) { // Possibly wrapped.
    if (lua_rawequal(L, -1, OBJMT) || lua_rawequal(L, -1, CTXMT)) {
      lua_pop(L, 1);
      goto out; // Unwrap JS object.
    }
    lua_pop(L, 1);
  }
  wrapper = persistent_lookup_lua(L, idx);
  if (!wrapper) { // Mapping does not exist yet.
    wrapper = (lv8_object*)lua_newuserdata(L, sizeof(lv8_object));
    if (!wrapper) return ESCAPE(Undefined(ISOLATE));
    memset(wrapper, 0, sizeof(*wrapper));
    Local<Object> no = PROXY->InstanceTemplate()->NewInstance();
    wrapper->object.Reset(ISOLATE, no); // Anchor to persistent wrapper.
    persistent_add(L, idx, wrapper); // Wrap Lua object at stack index 1.
    no->SetAlignedPointerInInternalField(0, (void*)wrapper);
    wrapper->object.SetWeak(L, js_weak_callback);
  }
out:;
  return scope.Escape(Local<Object>::New(ISOLATE, wrapper->object));
}

/* Convert JS value to Lua counterpart. */
static void convert_js2lua(lua_State *L, const Local<Value> &v)
{
  HandleScope scope(ISOLATE);
  if (v->IsBoolean()) {
    lua_pushboolean(L, v->BooleanValue());
  } else if (v->IsNumber()) {
    lua_pushnumber(L, v->NumberValue());
  } else if (v->IsUndefined() || v->IsNull()) {
    lua_pushnil(L);
  } else if (v->IsString()) {
    String::Utf8Value str(v);
    lua_pushlstring(L, *str, str.length());
  } else { // Must be some sort of object.
    Local<Object> o = v->ToObject();
    if (PROXY->HasInstance(v)) { // Possibly wrapped Lua.
      persistent_lookup_js(L, (lv8_object*)
          o->GetAlignedPointerFromInternalField(0));
      assert(!lua_isnil(L, -1));
      return; // Unwrapped Lua object.
    }
#if LV8_CACHE_PERSISTENT
    /* 
     * Ok, this is pretty bad. Apparently it is not possible to easily find out
     * if a JS object already has a persistent reference (short of iterating our
     * REFTAB). Which means we create a duplicate persistent ref every time we
     * import a js object.
     *
     * To alleviate this, we inject a hidden property into JS objects which
     * directly link to relevant userdata. This is done only if using
     * extended Lua dialect which allows lua_pushuserdata. Double-wrapping
     * overhead to make it work in standard would kill benefits of this hack
     * in the first place.
     */
    Local<String> idstr = NEWSTR(LV8_IDENTITY);
    Local<Value> identity = o->GetHiddenValue(idstr);
    if (!identity.IsEmpty()) {
      lua_pushuserdata(L, External::Cast(*identity)->Value());
      return; // Already wrapped JS object.
    }
    lv8_object *obj = (lv8_object*)lua_newuserdata(L, sizeof(*obj));
    o->SetHiddenValue(idstr, External::New(ISOLATE, (void*)obj));
#else
    lv8_object *obj = (lv8_object*)lua_newuserdata(L, sizeof(*obj));
#endif
    memset(obj, 0, sizeof(*obj));
    obj->object.Reset(ISOLATE, o); // Anchor until lv8_obj_gc kills it.
    lua_pushvalue(L, OBJMT); // Associate obj mt.
    lua_setmetatable(L, -2);
  }
}

/* Common context header. */
#define CB_LUA_COMMON \
  HandleScope scope(ISOLATE); \
  lv8_object *p = (lv8_object*)lua_touserdata(L, 1); \
  Local<Object> o = OREF(p); \
  Local<Context> ctx = o->CreationContext(); \
  ctx->Enter();

/* Call from Lua to JS. */
static int lv8_obj_lua2js_call(lua_State *L)
{
  int caught = 0;
  {
    CB_LUA_COMMON; // Enter context.
    int argc = lua_gettop(L)-1;
    Local<Value> argv[argc];

    for (int i = 0; i < argc; i++) // Convert argv.
      argv[i] = convert_lua2js(L, i+2);

    TryCatch exc; // CAVEAT: Must be destroyed before longjmp.
    Local<Value> res = o->CallAsFunction(o, argc, argv);
    if (exc.HasCaught()) {
      Local<Object> eo = exc.Exception()->ToObject();
      luaL_traceback(L, L, *String::Utf8Value(eo->Get(NEWSTR("stack"))), 1);
      eo->Set(NEWSTR("traceback"), convert_lua2js(L, -1));
      lua_pop(L, 1);
      convert_js2lua(L, eo);
      caught = 1;
    } else convert_js2lua(L, res); // Otherwise convert result.
    ctx->Exit(); // Leave context.
  }
  if (caught) {
    lua_error(L); // Error object already on stack.
  }
  return 1;
}

/* Get JS object property. */
static int lv8_obj_index(lua_State *L)
{
  CB_LUA_COMMON;
  convert_js2lua(L, o->Get(convert_lua2js(L, 2)));
  ctx->Exit();
  return 1;
}

/* Set JS object property. */
static int lv8_obj_newindex(lua_State *L)
{
  CB_LUA_COMMON;
  o->Set(convert_lua2js(L, 2), convert_lua2js(L, 3));
  ctx->Exit();
  return 0;
}

/* Print constructor name of js objects. */
static int lv8_obj_tostring(lua_State *L)
{
  CB_LUA_COMMON;
  lua_getmetatable(L, 1);
  assert(!lua_isnil(L, -1));
  if (lua_rawequal(L, -1, CTXMT)) {
    if (PROXY->HasInstance(o)) { // Sandbox.
      persistent_lookup_js(L, p);
      assert(!lua_isnil(L,-1));
      lua_pushfstring(L,"js<*sandbox>: %p", *o);
    } else {
      lua_pushfstring(L,"js<*context>: %p", *o);
    }
  }else {
    if (o->IsNativeError()) {
      Local<Object> tb = o->Get(NEWSTR("traceback"))->ToObject();
      if (!tb.IsEmpty()) {
        lua_pushfstring(L,*String::Utf8Value(tb));
        ctx->Exit();
        return 1;
      }
    }
    lua_pushfstring(L, "js<%s>: %p",
          *String::Utf8Value(o->GetConstructorName()), *o);
  }
  ctx->Exit();
  return 1;
}

/* Array() ipairs iterator. */
static int js_array_ipairs_aux(lua_State *L)
{
  CB_LUA_COMMON;
  int nret = 0;
  uint32_t idx = 0;
  if (o->IsArray()) {
    if (!lua_isnil(L, 2))
      idx = lua_tonumber(L, 2)+1;
    Local<Array> a = Local<Array>::Cast(o);
    if (idx < a->Length()) {
      lua_pushinteger(L, idx);
      convert_js2lua(L, Local<Array>::Cast(o)->CloneElementAt(idx));
      nret = 3;
    }
  }
  ctx->Exit();
  return nret;
}

/* Enumerate indexed properties of array instance. */
static int lv8_obj_ipairs(lua_State *L)
{
  int err = 0;
  {
    CB_LUA_COMMON;
    ctx->Exit();
    if (!o->IsArray()) {
      err = 1;
    } else {
      Array *a = Array::Cast(*o);
      lua_pushcfunction(L, js_array_ipairs_aux);
      lua_pushvalue(L, 1);
      lua_pushnil(L);
      return 3;
    }
  }
  if (err)
    luaL_error(L, "Only JS Array() can be used with ipairs()");
}

/* Just call next(). */
static int js_object_pairs_aux(lua_State *L)
{
  lua_settop(L, 2);
  if (lua_next(L, 1))
    return 2;
  return 0;
}

/* Enumerate everything in given object. */
static int lv8_obj_pairs(lua_State *L)
{
  CB_LUA_COMMON;
  Local<Array> a = o->GetOwnPropertyNames();
  uint32_t n = a->Length();
  lua_pushcfunction(L, js_object_pairs_aux);
  lua_newtable(L); // Table t.
  for (uint32_t i = 0; i < n; i++) {
    Local<Value> propname = Local<Array>::Cast(o)->CloneElementAt(i);
    convert_js2lua(L, propname); // Key.
    convert_js2lua(L, o->Get(propname)); // Val.
    lua_rawset(L, -3); // Set t[key] = val.
  }
  lua_pushnil(L); // Start index for next().
  ctx->Exit();
  return 3;
}

/* Get length of JS object. */
static int lv8_obj_len(lua_State *L)
{
  CB_LUA_COMMON;
  convert_js2lua(L, o->Get(NEWSTR("length")));
  ctx->Exit();
  return 1;
}

/* Protected settable. */
static int settab_aux(lua_State *L)
{
  lua_settable(L, -3);
  return 0;
}

/* Prepare protected t[k] = v call. */
static void settab(lua_State *L)
{
  lua_pushcfunction(L, settab_aux);
}

/* Protected gettable. */
static int gettab_aux(lua_State *L)
{
  lua_gettable(L, -3);
  return 0;
}

/* Prepare protected v = t[k] call. */
static void gettab(lua_State *L)
{
  lua_pushcfunction(L, gettab_aux);
}

/* 
 * Perform protected call.
 * On error, translate Lua exception to JS exception.
 */
static bool exception(lua_State *L, int narg, int nret)
{
  if (lua_pcall(L, narg, nret, 0) == LUA_OK)
    return false;
  ISOLATE->ThrowException(convert_lua2js(L, -1));
  lua_pop(L, 1);
  return true;
}

/* Load L callback argument. */
#define UNWRAP_L \
  lua_State *L = (lua_State*)External::Cast(*info.Data())->Value()

/* Get holder[idx]. */
static void lv8_getidx_cb(uint32_t idx,
    const PropertyCallbackInfo<Value> &info)
{
  UNWRAP_L;
  settab(L);
  lua_pushnumber(L, idx);
  convert_js2lua(L, info.Holder());
  if (exception(L, 2, 1)) {
    info.GetReturnValue().Set(Undefined(ISOLATE));
    return;
  }
  info.GetReturnValue().Set(convert_lua2js(L, -1));
  lua_pop(L, 1);
}

/* Set holder[idx] = val. */
static void lv8_setidx_cb(uint32_t idx, Local<Value> val,
    const PropertyCallbackInfo<Value> &info)
{
  UNWRAP_L;
  settab(L);
  convert_js2lua(L, info.Holder());
  lua_pushnumber(L, idx);
  convert_js2lua(L, val);
  if (exception(L, 3, 0)) {
    info.GetReturnValue().Set(Undefined(ISOLATE));
    return;
  }
  lua_pop(L, 1);
}

/* Set holder[idx] = nil. */
static void lv8_delidx_cb(uint32_t idx,
    const PropertyCallbackInfo<Boolean> &info)
{
  UNWRAP_L;
  settab(L);
  convert_js2lua(L, info.Holder());
  lua_pushnumber(L, idx);
  lua_pushnil(L);
  if (exception(L, 3, 0)) {
    info.GetReturnValue().Set(Boolean::New(ISOLATE, false));
    return;
  }
  info.GetReturnValue().Set(Boolean::New(ISOLATE, true));
}

/* Get holder.prop. */
static void lv8_getprop_cb(Local<String> prop,
    const PropertyCallbackInfo<Value> &info)
{
  UNWRAP_L;
  gettab(L);
  convert_js2lua(L, info.Holder());
  convert_js2lua(L, prop);
  if (exception(L, 2, 1)) {
    info.GetReturnValue().Set(Undefined(ISOLATE));
    return;
  }
  info.GetReturnValue().Set(convert_lua2js(L, -1));
  lua_pop(L, -1);
}

/* Set holder.prop = val. */
static void lv8_setprop_cb(Local<String> prop, Local<Value> val,
    const PropertyCallbackInfo<Value> &info)
{
  UNWRAP_L;
  settab(L);
  convert_js2lua(L, info.Holder());
  convert_js2lua(L, prop);
  convert_js2lua(L, val);
  if (exception(L, 3, 0)) {
    info.GetReturnValue().Set(Undefined(ISOLATE));
    return;
  }
}

/* Set holder.prop = nil. */
static void lv8_delprop_cb(Local<String> prop,
    const PropertyCallbackInfo<Boolean> &info)
{
  UNWRAP_L;
  settab(L);
  convert_js2lua(L, info.Holder());
  convert_js2lua(L, prop);
  lua_pushnil(L);
  if (exception(L, 3, 0)) {
    info.GetReturnValue().Set(Boolean::New(ISOLATE,false));
    return;
  }
  info.GetReturnValue().Set(Boolean::New(ISOLATE, true));
}

static void lv8_enumprop_cb(const PropertyCallbackInfo<Array> &info)
{
}

static void lv8_enumidx_cb(const PropertyCallbackInfo<Array> &info)
{
}


/* Calls from JS to Lua. 'v8::' Because of namespace clash. */
static void lv8_js2lua_call(const v8::FunctionCallbackInfo<Value> &info)
{
  HandleScope scope(ISOLATE); // To kill locals below.
  UNWRAP_L;
  Handle<Object> self = info.Holder();
  int argc = info.Length();
  int top = lua_gettop(L);

  persistent_lookup_js(L,
      (lv8_object*)self->GetAlignedPointerFromInternalField(0));
  assert(!lua_isnil(L, -1)); // Look up the actual Lua object.

  for (int i = 0; i < argc; i++) // Convert input args.
    convert_js2lua(L, info[i]);

  if (exception(L, argc, LUA_MULTRET)) { // Perform call.
    info.GetReturnValue().Set(Boolean::New(ISOLATE, false));
    return; // Propagate exception.
  }


  int nres = lua_gettop(L) - top; // Convert output results.
  if (!nres) { // No results.
    info.GetReturnValue().Set(Undefined(ISOLATE));
  } if (nres == 1) { // Single result.
    info.GetReturnValue().Set(convert_lua2js(L, -1));
  } else { // Multiple results will go into array.
    Local<Array> array = Array::New(ISOLATE, nres);
    for (uint32_t i = 0; i < nres; i++)
      array->Set(i, convert_lua2js(L, top + i + 1));
    info.GetReturnValue().Set(array);
  }
  lua_pop(L, nres);
}

static const struct luaL_Reg lv8_object_mt[] = {
  { "__index",    lv8_obj_index },
  { "__newindex", lv8_obj_newindex },
  { "__call",     lv8_obj_lua2js_call},
  { "__gc",       lv8_obj_gc },
  { "__tostring", lv8_obj_tostring },
  { "__pairs",    lv8_obj_pairs },
  { "__ipairs",   lv8_obj_ipairs },
  { "__len",      lv8_obj_len },
  { 0, 0 }
};

/* Discard remaining v8 state after Lua side is closed. */
int luaclose_lv8(lua_State *L)
{
  lv8_state *state = (lv8_state*)lua_touserdata(L, 1);
  state->proxy.Reset(); // Should have no refs.
  return 0;
}

/* Lua and V8 states. */
int luaopen_lv8(lua_State *L)
{
  HandleScope scope(ISOLATE); // To kill locals below.

  while (lua_gettop(L) > 1) { // eg. require("lv8", "--harmony --use_strict")
    size_t n;
    if (const char *s = lua_tolstring(L, 1, &n))
      V8::SetFlagsFromString(s, (int)n);
    lua_pop(L, 1);
  }

  lua_settop(L, 0);

  /* Globally shared state which holds our proxy template. */
  lv8_state *state = (lv8_state*) lua_newuserdata(L, sizeof(lv8_state));
  memset(state, 0, sizeof(*state));
  Local<FunctionTemplate> proxy =
    FunctionTemplate::New(ISOLATE);
  state->proxy.Reset(ISOLATE, proxy);
  Handle<ObjectTemplate> tpl = proxy->InstanceTemplate();
  tpl->SetInternalFieldCount(1); // Points to wrapped lua object.
  tpl->SetNamedPropertyHandler( // Named properties.
      lv8_getprop_cb, lv8_setprop_cb, 0,
      lv8_delprop_cb, lv8_enumprop_cb,
      External::New(ISOLATE, L));
  tpl->SetIndexedPropertyHandler( // Indexed properties.
      lv8_getidx_cb, lv8_setidx_cb, 0,
      lv8_delidx_cb, lv8_enumidx_cb,
      External::New(ISOLATE, L));
  tpl->SetCallAsFunctionHandler(lv8_js2lua_call, External::New(ISOLATE, L));
  lua_newtable(L); // State cleanup mt.
  lua_pushcfunction(L, luaclose_lv8);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);

  /* Set up UV #2-#4. */
  for (int i = 0; i < 3; i++)
    lua_newtable(L);

  /* Set funcs for UV #4 */
  for (int i = 1; i <= 4; i++)
    lua_pushvalue(L, i);
  luaL_setfuncs(L, lv8_object_mt, 4);

  /* Set funcs for UV #3 */
  lua_pushvalue(L, 3);
  for (int i = 1; i <= 4; i++)
    lua_pushvalue(L, i);
  luaL_setfuncs(L, lv8_object_mt, 4);
  lua_pop(L, 1); // Pop #3.

  /* Function to construct new V8 context. Consume #1..#4 */
  lua_pushcclosure(L, lv8_new_context, 4);
  return 1;
}

