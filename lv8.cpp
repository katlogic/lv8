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
#include <stdlib.h>

#define LV8_CPP
#include "lv8.hpp"

using namespace v8;

/* lua_pushuserdata(). */
#define LV8_IDENTITY "lv8::identity"
#define LV8_NEED_FINHACK ((LUA_VERSION_NUM<503) || defined(LUA_VERSION_LJX))
#ifndef lua_pushuserdata
#include "pudata/pudata.h"
#else
#define lua_newuserdata_old lua_newuserdata
#define lua_pushuserdata_resurrect(L, idx)
#endif

/* Default V8 engine flags. */
#define LV8_DEFAULT_FLAGS "--harmony"

/* Shortcut accessors. */
#define UV_LIB lua_upvalueindex(1) // ORDER luaopen_lv8.
#define UV_STATE lua_upvalueindex(2)
#define UV_REFTAB lua_upvalueindex(3)
#define UV_OBJMT lua_upvalueindex(4)
#define UV_CTXMT lua_upvalueindex(5)
#define LV8_STATE ((lv8_state*)lua_touserdata(L, UV_STATE))
#define ISOLATE Isolate::GetCurrent()

/* Shortcut constructors. */
#define PROXY Local<FunctionTemplate>::New(ISOLATE, LV8_STATE->proxy)
#define GLOBAL Local<ObjectTemplate>::New(ISOLATE, LV8_STATE->gtpl)
#define LOCAL(v) Local<Value>::New(ISOLATE, (v))
#define OREF(v) Local<Object>::New(ISOLATE, (v->object))
#define REF(t,v) Handle<t>::New(ISOLATE, (v))
#define ESCAPE(v) scope.Escape(LOCAL(v));
#define UTF8(arg...) String::NewFromUtf8(ISOLATE, arg)
#define LITERAL(s) \
  String::NewFromUtf8(ISOLATE, s, String::kInternalizedString, sizeof(s)-1)
#define JS_DEFUN(tab, name, fn, data) \
  tab->Set(LITERAL(name), \
      FunctionTemplate::New(ISOLATE, fn, External::New(ISOLATE, data)))
#define UNWRAP_L \
  lua_State *L = (lua_State*)External::Cast(*info.Data())->Value()


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
 * JS proxies of Lua objects (convert_lua2js) are stored in UV_REFTAB,
 * both for fast lookups and as an anchor. The JS proxies are weak
 * and js_weak_callback is called when there are no more references
 * in JS. This kills the UV_REFTAB anchor in turn (and eventually
 * allows Lua to GC). Note that this anchoring is the dreaded
 * object resurrection by finalizer, a feature of Lua 5.3, for Lua 5.2
 * it had to be hacked in (LV8_NEED_FINHACK). For LuaJIT, use LJX.
 *
 * In general, accessing Lua objects from JS is faster than
 * the opposite (V8 is not very well cut for efficient embedding).
 */

/* Find JS object associated with Lua value. */
static lv8_object *persistent_lookup_lua(lua_State *L, int idx)
{
  lua_pushvalue(L, idx);
  lua_rawget(L, UV_REFTAB);
  void *p = lua_touserdata(L, -1);
  lua_pop(L, 1);
  return (lv8_object*)p;
}

/* Find Lua object associated with JS value. */
static void persistent_lookup_js(lua_State *L, lv8_object *v)
{
  lua_pushlightuserdata(L, (void*)v);
  lua_rawget(L, UV_REFTAB);
}

/* Associate js object with Lua value (two-way). */
static void persistent_add(lua_State *L, int idx, lv8_object *v)
{
  assert(idx > 0);
  assert(!persistent_lookup_lua(L, idx));
  lua_pushvalue(L, idx);
  lua_pushlightuserdata(L, v);
  lua_rawset(L, UV_REFTAB); // Map reftab[lua] = js.
  lua_pushlightuserdata(L, v);
  lua_pushvalue(L, idx);
  lua_rawset(L, UV_REFTAB); // Map reftab[js] = lua.
}

/* Kill persistent mapping, expects lv8_object udata on stack. */
static void persistent_del(lua_State *L, lv8_object *v)
{
  assert(!lua_isnil(L, -1));
  assert(lua_touserdata(L, -1) == v);
  lua_pushnil(L);
  lua_rawset(L, UV_REFTAB); // Clear Lua -> JS.
  lua_pushlightuserdata(L, (void*)v);
  lua_pushnil(L);
  lua_rawset(L, UV_REFTAB); // Clear JS -> Lua.
}

/* Context which was made weak by obj_gc is collected. */
static void
js_weak_context(const WeakCallbackData<Context, lua_State> &data)
{
  HandleScope scope(ISOLATE);
  Handle<Object> o = data.GetValue()->Global()->GetPrototype()->ToObject();
  lv8_context *v = (lv8_context*)o->GetAlignedPointerFromInternalField(0);
  v->context.Reset(); // This should trigger object collection below
  v->object.Reset();
  v->jscollected = 1;
}

/* Last reference to Lua object from JS died. Remove refs on Lua side. */
static void
js_weak_object(const WeakCallbackData<v8::Object, lua_State> &data)
{
  HandleScope scope(ISOLATE);
  Handle<Object> o = data.GetValue();
  lv8_object *v;
  lua_State *L = data.GetParameter();
  v = (lv8_object*)o->GetAlignedPointerFromInternalField(0);

  persistent_lookup_js(L, (lv8_object*)v); // Lookup Lua object.
  assert(!lua_isnil(L, -1)); // Must be tracked.
  persistent_del(L, v);

  v->object.Reset(); // And kill the Persistent.
}

/* Restart finalizer on Lua 5.2/5.3. */
static void restart_finalizer(lua_State *L, void *p)
{
#if LV8_NEED_FINHACK
  struct gch_t {
    void *gcnext;
    uint8_t tt;
    uint8_t marked;
  } *gch = (gch_t*)(((uint8_t*)p) - LV8_STATE->finhack);
#define FINALIZED (1<<3)
  assert(gch->marked & FINALIZED);
  gch->marked &= ~FINALIZED;
#endif
}

/* Last reference of Lua object released. */
static int lua_obj_gc(lua_State *L)
{
  HandleScope scope(ISOLATE);
  lv8_context *o = (lv8_context*)lua_touserdata(L, 1);
  assert(o);
  lua_getmetatable(L, 1);
  assert(!lua_isnil(L, -1));
  if (lua_rawequal(L, -1, UV_CTXMT)) {
    if (!o->jscollected && !o->resurrected) { // Not already collected context?
      restart_finalizer(L, (void*)o);
      lua_pushuserdata_resurrect(L, 1);
      lua_pushvalue(L, UV_CTXMT);
      lua_setmetatable(L, 1);
      o->context.SetWeak(L, js_weak_context);
      persistent_add(L, 1, o); // Resurrect until js_weak_context kicks in.
      o->resurrected = 1;
      return 0;
    } // NO-OP. *OREF(o) might be null if it was weak already.
  } else { // Kill the cache.
    assert(lua_rawequal(L, -1, UV_OBJMT));
    OREF(o)->SetHiddenValue(LITERAL(LV8_IDENTITY),
      Undefined(ISOLATE));
  }
  o->object.Reset();
  return 0;
}

/* Convert Lua value to JS counterpart. */
static Handle<Value> convert_lua2js(lua_State *L, int idx)
{
  if (idx < 0) // Convert to absolute index.
    idx += lua_gettop(L) + 1;

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
      return ESCAPE(UTF8(p, String::kNormalString, (int)n));
    }
  }
  lv8_object *wrapper = (lv8_object*)lua_touserdata(L, idx);
  if (wrapper && lua_getmetatable(L, idx)) { // Possibly wrapped.
    if (lua_rawequal(L, -1, UV_OBJMT) || lua_rawequal(L, -1, UV_CTXMT)) {
      lua_pop(L, 1);
      goto unwrap; // Unwrap JS object.
    }
    lua_pop(L, 1);
  }
  wrapper = persistent_lookup_lua(L, idx);
  if (!wrapper) { // Mapping does not exist yet.
    wrapper = (lv8_object*)lua_newuserdata(L, sizeof(lv8_object));
    if (!wrapper) return ESCAPE(Undefined(ISOLATE));
    memset(wrapper, 0, sizeof(*wrapper));
    Handle<Object> no = PROXY->InstanceTemplate()->NewInstance();
    wrapper->object.Reset(ISOLATE, no); // Anchor proxy in JS
    persistent_add(L, idx, wrapper); // Anchor Persistent<> UD in Lua
    no->SetAlignedPointerInInternalField(0, (void*)wrapper);
    wrapper->object.SetWeak(L, js_weak_object);
    lua_pop(L, 1);
  }
unwrap:;
  return scope.Escape(Local<Object>::New(ISOLATE, wrapper->object));
}

/* Wrap JS object 'o' to Lua proxy. */
void lv8_wrap_js2lua(lua_State *L, Handle<Object> o)
{
  Handle<String> idstr = LITERAL(LV8_IDENTITY);
  Handle<Value> identity = o->GetHiddenValue(idstr);
  if (!identity.IsEmpty() && !identity->IsUndefined()) {
    lua_pushuserdata(L, External::Cast(*identity)->Value());
    return;
  } // Cached udata exists.

  lv8_object *obj = (lv8_object*)lua_newuserdata(L, sizeof(*obj));
  o->SetHiddenValue(idstr, External::New(ISOLATE, (void*)obj));
  memset(obj, 0, sizeof(*obj));
  obj->object.Reset(ISOLATE, o); // Anchor until lua_obj_gc kills it.
  lua_pushvalue(L, UV_OBJMT); // Associate obj mt.
  lua_setmetatable(L, -2);
}

/* Query if given JS object is a proxy for actual Lua object
 * or sandbox (but not context!). */
bool lv8_is_lua_object(lua_State *L, Handle<Object> o)
{
  return PROXY->HasInstance(o);
}

/* 
 * Given the Lua object (lv8_is_lua_object) return whether:
 *  false - it is regular Lua data
 *  true - the object is JS sandbox object (ie userdata)
 * In any case, the actual Lua value is pushed on stack.
 */
bool lv8_is_js_sandbox(lua_State *L, Handle<Object> o, lv8_context **cp = 0)
{
  assert(lv8_is_lua_object(L, o));
  lv8_context *c = (lv8_context*)o->GetAlignedPointerFromInternalField(0);
  if (cp) *cp = c;
  lua_pushuserdata(L, c);
  assert(!lua_isnil(L, -1));
  if (!lua_getmetatable(L, -1)) { // (LIKELY) Lua Object proxy.  {
    lua_pop(L, 1); // Pop udata.
    persistent_lookup_js(L, c); // Get the Lua object.
    return false; // Return the actual lua object.
  }
  assert(lua_rawequal(L, -1, UV_CTXMT));
  lua_pop(L, 1); // Pop meta.
  return true; // It is a sandbox.
}

/* Check if object o is a context object (ie not sandbox). */
bool lv8_is_js_context(Handle<Object> o)
{
  return o->CreationContext()->Global()->GetPrototype()->ToObject() == o;
}

/* Convert JS value to Lua counterpart. */
static void convert_js2lua(lua_State *L, const Handle<Value> &v)
{
  HandleScope scope(ISOLATE);
  if (v.IsEmpty() || v->IsUndefined() || v->IsNull()) {
    lua_pushnil(L);
  } else if (v->IsBoolean() || v->IsBooleanObject()) {
    lua_pushboolean(L, v->BooleanValue());
  } else if (v->IsNumber() || v->IsNumberObject()) {
    lua_pushnumber(L, v->NumberValue());
  } else if (v->IsString() || v->IsStringObject()) {
    String::Utf8Value str(v);
    lua_pushlstring(L, *str, str.length());
  } else { // Must be some sort of other object.
    assert(v->IsObject());
    Handle<Object> o = v->ToObject();
    lv8_context *c;
    if (lv8_is_lua_object(L, o)) { // (LIKELY) Proxied?
      if (!lv8_is_js_sandbox(L, o, &c))
        return; // Return native Lua object.
    } else { // (UNLIKELY) Not proxied, might be context or JS value.
      if (!lv8_is_js_context(o)) {
        lv8_wrap_js2lua(L, o);
        return; // Return JS object proxy (new or cached).
      } // Otherwise it is a context.
      c = (lv8_context*)o->GetAlignedPointerFromInternalField(0);
      lua_pushuserdata(L, c);
    }
    assert(!c->jscollected); // Must not be past weak_context.
    if (!c->resurrected) return; // Just return the pudata value.
    c->context.ClearWeak(); // Caught mid-gc phase.
    lua_pushvalue(L, -1); // Dup for persistent_del.
    persistent_del(L, c); // Undo resurrection.
    c->resurrected = 0;
    return; // Unwrapped context object.
  }
}

static void checkstate(lua_State *L);

/* Copy attributes of o to dst. */
bool lv8_shallow_copy(lua_State *L, Handle<Object> dst, Handle<Object> o)
{
  HandleScope scope(ISOLATE);
  if (o.IsEmpty() || o->IsUndefined() || !o->IsObject())
    return false;
  if (lv8_is_lua_object(L, o)) { // Sandbox or proxy for Lua object
    if (!lv8_is_js_sandbox(L, o)) { // It is a Lua native.
      bool res = lv8_shallow_copy_from_lua(L, dst, -1); // Copy it
      lua_pop(L, 1);
      return res;
    }
    lua_pop(L, 1); // Pop the sandbox object on stack.
  }
  Handle<Array> a = o->GetPropertyNames();
  uint32_t n = a->Length();
  for (uint32_t i = 0; i < n; i++) {
    Handle<Value> propname = a->Get(i);
    a->CreationContext()->Enter();
    Handle<Value> val = o->Get(propname);
    a->CreationContext()->Exit();
    dst->Set(propname, val);
  }
}

/* Check if given object at idx is wrapped (context, js object etc). */
lv8_context *lv8_unwrap(lua_State *L, int idx)
{
  if (lua_getmetatable(L, idx)) {
    if (lua_rawequal(L, -1, UV_CTXMT) || lua_rawequal(L, -1, UV_OBJMT)) {
      lua_pop(L, 1);
      return (lv8_context*)lua_touserdata(L, idx);
    }
    lua_pop(L, 1);
  }
  return 0;
}

/* Copy fields of lua table at idx to dst. */
bool lv8_shallow_copy_from_lua(lua_State *L, Handle<Object> dst, int idx)
{
  if (!lua_istable(L, idx)) {
    if (lv8_object *o = lv8_unwrap(L, idx)) {
      lv8_shallow_copy(L, dst, OREF(o));
      return true; // Succesfuly unwrapped.
    }
    return false;
  }
  lua_pushnil(L);
  while (lua_next(L, idx)) { // Populate from table.
    dst->Set(convert_lua2js(L, -2), convert_lua2js(L, -1));
    lua_pop(L, 1); // Pop value, keep key for next.
  }
  return true;
}

/* Context factory common to Lua and JS callers. */
lv8_context *lv8_context_factory(struct lua_State *L)
{
  checkstate(L);
  HandleScope scope(ISOLATE);
  lv8_context *ctx = (lv8_context*)lua_newuserdata(L, sizeof(*ctx));
  memset(ctx, 0, sizeof(*ctx));
  lua_pushvalue(L, UV_CTXMT); // Associate obj mt.
  lua_setmetatable(L, -2);

  Local<Context> c = Context::New(ISOLATE, 0, GLOBAL);
  Handle<Object> glproxy = c->Global();
  Handle<Object> gl = glproxy->GetPrototype()->ToObject();
  ctx->context.Reset(ISOLATE, c);
  gl->SetAlignedPointerInInternalField(0, (void*)ctx);
  ctx->object.Reset(ISOLATE, gl);
  ctx->object.SetWeak(L, js_weak_object);

  return ctx;
}

/* Construct new JS context. */
int lv8_create_context(struct lua_State *L)
{
  HandleScope scope(ISOLATE);
  lv8_object *ctx = lv8_context_factory(L);
  if (lua_gettop(L) > 1)
    lv8_shallow_copy_from_lua(L, OREF(ctx), 1);
  return 1;
}

/* Wrapper for lv8 table __call. */
static int lua_create_context_call(struct lua_State *L) {
  lua_remove(L, 1); // Remove lib table.
  return lv8_create_context(L);
}

/* Common sandbox factory for JS and Lua. */
lv8_context *lv8_sandbox_factory(lua_State *L)
{
    checkstate(L);
    HandleScope scope(ISOLATE);
    lv8_context *ctx = (lv8_context*)lua_newuserdata(L, sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));
    lua_pushvalue(L, UV_CTXMT); // Associate obj mt.
    lua_setmetatable(L, -2);

    Handle<Context> c = Context::New(ISOLATE, 0, PROXY->InstanceTemplate());
    ctx->context.Reset(ISOLATE, c);
    Handle<Object> glproxy = c->Global();
    Handle<Object> gl = c->Global()->GetPrototype()->ToObject();
    gl->SetAlignedPointerInInternalField(0, (void*)ctx);
    ctx->object.Reset(ISOLATE, gl); // Intercept.
    ctx->object.SetWeak(L, js_weak_object);
}

/* Construct new JS sandbox. */
int lv8_create_sandbox(struct lua_State *L)
{
  luaL_checkany(L, 1);
  lv8_sandbox_factory(L);
  return 1;
}

/* Common context header. */
#define CB_LUA_COMMON \
  HandleScope scope(ISOLATE); \
  lv8_object *p = (lv8_object*)lua_touserdata(L, 1); \
  Handle<Object> o = OREF(p); \
  Handle<Context> ctx = o->CreationContext(); \
  ctx->Enter();

/* Call from Lua to JS. */
static int lua_obj_lua2js_call(lua_State *L)
{
  int caught = 0;
  {
    CB_LUA_COMMON; // Enter context.
    if (lua_gettop(L) == 1)
      lua_pushnil(L);
    int argc = lua_gettop(L)-2;
    Handle<Value> receiver;
    if (lua_isnil(L, 2))
      receiver = ctx->Global();
    else
      receiver = convert_lua2js(L, 2);
    Handle<Value> argv[argc];

    for (int i = 0; i < argc; i++) // Convert argv.
      argv[i] = convert_lua2js(L, i+3);

    TryCatch exc; // CAVEAT: Must be destroyed before longjmp.
    Handle<Value> res = o->CallAsFunction(receiver, argc, argv);
    if (exc.HasCaught()) {
      Handle<Object> eo = exc.Exception()->ToObject();
      luaL_traceback(L, L, *String::Utf8Value(eo->Get(LITERAL("stack"))), 1);
      eo->Set(LITERAL("traceback"), convert_lua2js(L, -1));
      lua_pop(L, 1);
      convert_js2lua(L, eo);
      caught = 1;
    } else convert_js2lua(L, res); // Otherwise convert result.
    ctx->Exit(); // Leave context.
  }
  if (caught)
    lua_error(L); // Error object already on stack.
  return 1;
}

/* Construct object as 'new arg1(arg2...)' */
int lv8_create_instance(lua_State *L)
{
  if (lua_gettop(L) < 1 ||
      !lua_getmetatable(L, 1) ||
      !lua_rawequal(L, -1, UV_OBJMT))
    luaL_argerror(L, 1, "JS prototype");

  int caught = 0;

  HandleScope scope(ISOLATE);
  {
    CB_LUA_COMMON; // Enter context.
    int argc = lua_gettop(L)-1;
    Handle<Value> argv[argc];

    for (int i = 0; i < argc; i++) // Convert argv.
      argv[i] = convert_lua2js(L, i+2);

    TryCatch exc; // CAVEAT: Must be destroyed before longjmp.
    Handle<Value> res = o->CallAsConstructor(argc, argv);
    if (exc.HasCaught()) {
      Handle<Object> eo = exc.Exception()->ToObject();
      luaL_traceback(L, L, *String::Utf8Value(eo->Get(LITERAL("stack"))), 1);
      eo->Set(LITERAL("traceback"), convert_lua2js(L, -1));
      lua_pop(L, 1);
      convert_js2lua(L, eo);
      caught = 1;
    } else convert_js2lua(L, res); // Otherwise convert result.
    ctx->Exit(); // Leave context.
  }
  if (caught)
    lua_error(L); // Error object already on stack.
  return 1;
}

/* Get JS object property. */
static int lua_obj_index(lua_State *L)
{
  CB_LUA_COMMON;
  convert_js2lua(L, o->Get(convert_lua2js(L, 2)));
  ctx->Exit();
  return 1;
}

/* Set JS object property. */
static int lua_obj_newindex(lua_State *L)
{
  CB_LUA_COMMON;
  o->Set(convert_lua2js(L, 2), convert_lua2js(L, 3));
  ctx->Exit();
  return 0;
}

/* Print constructor name of js objects. */
static int lua_obj_tostring(lua_State *L)
{
  CB_LUA_COMMON;
  lua_getmetatable(L, 1);
  assert(!lua_isnil(L, -1));
  if (lua_rawequal(L, -1, UV_CTXMT)) {
    if (PROXY->HasInstance(o)) { // Sandbox.
      persistent_lookup_js(L, p);
      assert(!lua_isnil(L,-1));
      lua_pushfstring(L,"js<*sandbox>: %p", o->GetAlignedPointerFromInternalField(0));
    } else {
      lua_pushfstring(L,"js<*context>: %p", o->GetAlignedPointerFromInternalField(0));
    }
  } else {
    if (o->IsNativeError()) {
      Handle<Object> tb = o->Get(LITERAL("traceback"))->ToObject();
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
    Handle<Array> a = Handle<Array>::Cast(o);
    if (idx < a->Length()) {
      lua_pushinteger(L, idx);
      convert_js2lua(L, a->Get(idx));
      nret = 2;
    }
  }
  ctx->Exit();
  return nret;
}

/* Enumerate indexed properties of array instance. */
static int lua_obj_ipairs(lua_State *L)
{
  int err = 0;
  {
    CB_LUA_COMMON;
    ctx->Exit();
    if (!o->IsArray()) {
      err = 1;
    } else {
      Handle<Array> a = Handle<Array>::Cast(o);
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
static int lua_obj_pairs(lua_State *L)
{
  CB_LUA_COMMON;
  Handle<Array> a = o->GetPropertyNames();
  uint32_t n = a->Length();
  lua_pushcfunction(L, js_object_pairs_aux);
  lua_createtable(L, n, 2); // Table t.
  for (uint32_t i = 0; i < n; i++) {
    Handle<Value> propname = a->Get(i);
    if (propname.IsEmpty()||
        propname->IsUndefined() ||
        propname->IsNull())
      continue;
    convert_js2lua(L, propname); // Key.
    convert_js2lua(L, o->Get(propname)); // Val.
    lua_rawset(L, -3); // Set t[key] = val.
  }
  lua_pushnil(L); // Start index for next().
  ctx->Exit();
  return 3;
}

/* Get length of JS object. */
static int lua_obj_len(lua_State *L)
{
  CB_LUA_COMMON;
  convert_js2lua(L, o->Get(LITERAL("length")));
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
  lua_gettable(L, -2);
  return 1;
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
  if (lua_pcall(L, narg, nret, 0) == LUA_OK) {
    return false;
  }
  ISOLATE->ThrowException(Exception::Error(UTF8(lua_tostring(L, -1))));
  lua_pop(L, 1); // Pop error message.
  return true;
}

/* Get holder[idx]. */
static void lv8_getidx_cb(uint32_t idx,
    const PropertyCallbackInfo<Value> &info)
{
  UNWRAP_L;
  gettab(L);
  convert_js2lua(L, info.Holder());
  lua_pushnumber(L, idx);
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
  if (exception(L, 2, 1))
    return;
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
  if (exception(L, 3, 0))
    info.GetReturnValue().Set(Undefined(ISOLATE));
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
  if (exception(L, 3, 0))
    info.GetReturnValue().Set(Boolean::New(ISOLATE,false));
  info.GetReturnValue().Set(Boolean::New(ISOLATE, true));
}

/* Enumerate both array and hash. */
static void lv8_enumprop_cb(const PropertyCallbackInfo<Array> &info)
{
  HandleScope scope(ISOLATE);
  UNWRAP_L;
  convert_js2lua(L, info.Holder()); // Load table
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    ISOLATE->ThrowException(LITERAL("Only lua tables can be enumerated"));
  }
  uint32_t i, n = 0; // Slower: lua_rawlen(L -1);
  Handle<Array> a = Array::New(ISOLATE, n);
  i = 0;
  lua_pushnil(L); // First key.
  for (i = 0; lua_next(L, -2); i++) {
    a->Set(i, convert_lua2js(L, -2)); // Set key.
    lua_pop(L, 1); // Pop value.
  }
  lua_pop(L, 1); // Pop table.
  info.GetReturnValue().Set(a);
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
  Handle<Array> array = Array::New(ISOLATE, nres);
  for (uint32_t i = 0; i < nres; i++)
    array->Set(i, convert_lua2js(L, top + i + 1));
  info.GetReturnValue().Set(array);
  lua_pop(L, nres);
}

/* Eval a string (with explicit context or default one). */
static void js_vm_eval(const v8::FunctionCallbackInfo<Value> &info) {
  UNWRAP_L;
}

/* Construct a JS context. */
static void js_vm_context(const v8::FunctionCallbackInfo<Value> &info) {
  UNWRAP_L;
//  info.GetReturnValue().Set(convert_lua2js(L, lua_gettop(L)));
  lua_pop(L, 1);
}

static void js_vm_sandbox(const v8::FunctionCallbackInfo<Value> &info) {
  UNWRAP_L;
//  info.GetReturnValue().Set(conver_lua2js(L, lua_gettop(L)));
  lua_pop(L, 1);
}


/* Configure V8 flags. */
static int lua_v8_flags(lua_State *L)
{
  int top = lua_gettop(L);
  size_t n;
  for (int i = 1; i <= top; i++)
    if (const char *s = lua_tolstring(L, i, &n))
      V8::SetFlagsFromString(s, (int)n);
  lua_settop(L, 1);
  return 1; // Allow method chaining.
}

/* Attempt to force GC cycle (still unreliable). */
static int lua_force_gc(lua_State *L)
{
  {
    /* 
     * V8 keeps one instace stale for fast reuse.
     * Allocate dummy one to force flush.
     */
    Isolate *i = Isolate::GetCurrent();
    HandleScope h(i);
    Handle<Context> generic = Context::New(i);
    Handle<Context> context = Context::New(i, 0, GLOBAL);
    Handle<Context> sandbox = Context::New(i, 0, PROXY->InstanceTemplate());
  }
  while (!v8::V8::IdleNotification());
}

/* ArrayBuffer allocator. */
class ab_allocator : public ArrayBuffer::Allocator {
  public:
  virtual void* Allocate(size_t length) {
    void *data = calloc(length, 1);
    return data;
  }
  virtual void* AllocateUninitialized(size_t length) {
    void *data = malloc(length);
    return data;
  }
  virtual void Free(void* data, size_t length) {
    free(data);
  }
};

/* JavaScript raw vm.* API subtable. */
Handle<ObjectTemplate> static lv8_vm_init(lua_State *L)
{
  EscapableHandleScope scope(ISOLATE);
  Local<ObjectTemplate> vm = ObjectTemplate::New();
  JS_DEFUN(vm, "eval", js_vm_eval, L); // Execute.
  JS_DEFUN(vm, "context", js_vm_eval, L); // Create context.
  JS_DEFUN(vm, "sandbox", js_vm_eval, L); // Create sandbox.
  return scope.Escape(vm);
}

/* Initialize global state. Must be in context. */
static void checkstate(lua_State *L)
{
  lv8_state *state = LV8_STATE;
  if (!state->initialized) {
    V8::SetArrayBufferAllocator(new ab_allocator());

    state->initialized = 1;
    HandleScope scope(ISOLATE);

    Handle<ObjectTemplate> gtpl = ObjectTemplate::New();
    gtpl->SetInternalFieldCount(1); // Normal context template.
    state->gtpl.Reset(ISOLATE, gtpl);

    Handle<FunctionTemplate> proxy =
      FunctionTemplate::New(ISOLATE); // Sandbox proxy template.
    state->proxy.Reset(ISOLATE, proxy);

    Handle<ObjectTemplate> tpl = proxy->InstanceTemplate();
    tpl->SetInternalFieldCount(1); // Points to wrapped lua object.
    tpl->SetNamedPropertyHandler( // Named properties.
        lv8_getprop_cb, lv8_setprop_cb, 0,
        lv8_delprop_cb, lv8_enumprop_cb,
        External::New(ISOLATE, L));
    tpl->SetIndexedPropertyHandler( // Indexed properties.
        lv8_getidx_cb, lv8_setidx_cb, 0,
        lv8_delidx_cb, 0,
        External::New(ISOLATE, L));
    tpl->SetCallAsFunctionHandler(lv8_js2lua_call, External::New(ISOLATE, L));

    Handle<Context> tmp = Context::New(ISOLATE);
    tmp->Enter();
#if LV8_FS_API
    lv8_wrap_js2lua(L, lv8_fs_init()->NewInstance());
    lua_setfield(L, -2, "fs");
#endif
    lv8_wrap_js2lua(L, lv8_vm_init(L)->NewInstance());
    lua_setfield(L, -2, "vm");
    tmp->Exit();
  }
}

/* Metatable for UV_OBJMT/UV_CTXMT JS proxy. */
static const struct luaL_Reg lv8_object_mt[] = {
  { "__index",    lua_obj_index },      // Load array slot or property.
  { "__newindex", lua_obj_newindex },   // Store array slot or property.
  { "__call",     lua_obj_lua2js_call}, // Call a function.
  { "__gc",       lua_obj_gc },         // Proxy is about to be destroyed.
  { "__tostring", lua_obj_tostring },   // JS object to readable string.
  { "__pairs",    lua_obj_pairs },      // Generic iterator (incl own props).
  { "__ipairs",   lua_obj_ipairs },     // Array iterator.
  { "__len",      lua_obj_len },        // Equals 'obj.length'.
  { 0, 0 }
};

/* Library. */
static const struct luaL_Reg lv8_lib[] = {
  { "flags",    lua_v8_flags},          // Set V8 flags.
  { "gc",       lua_force_gc},        // Force gc.
  { "new",      lv8_create_instance },// Call 'new' in JS to construct instance.
  { "sandbox",  lv8_create_sandbox }, // Create sandbox.
  { "context",  lv8_create_context }, // Create JS context.
  { "__call",   lua_create_context_call }, // Ditto.
  { 0, 0 }
};

/* Discard remaining v8 state after Lua side is closed. */
int luaclose_lv8(lua_State *L)
{
  lv8_state *state = (lv8_state*)lua_touserdata(L, 1);
  state->proxy.Reset(); // Should have no refs.
  state->gtpl.Reset(); // Should have no refs.
  return 0;
}

#if LV8_NEED_FINHACK
/* Trap size of userdata. */
static lua_Alloc olda;
static ptrdiff_t finhack = 0;
static void *fakealloc(void *ud, void *ptr, size_t o, size_t n)
{
  if (n == 0)
    return olda(ud, ptr, o, n);
  finhack = n;
  void *r = olda(ud, ptr, o, n);
  return r;
}
#endif

/* Initialize fs and vm subtables. */
/* Lua and V8 states. */
int luaopen_lv8(lua_State *L)
{
  V8::SetFlagsFromString(LV8_DEFAULT_FLAGS, sizeof(LV8_DEFAULT_FLAGS)-1);
#if LV8_NEED_FINHACK
  void *ud;
  olda = lua_getallocf(L, &ud);
  lua_setallocf(L, fakealloc, ud);
  lua_newuserdata_old(L, 0);
  assert(finhack);
  lua_setallocf(L, olda, ud);
#endif
  lua_settop(L, 0);

  /* UV #1: Library. */
  lua_newtable(L);
  lua_pushvalue(L, -1);
  lua_setmetatable(L, -2);

  /* UV #2: Globally shared state which holds our proxy template. */
  lv8_state *state = (lv8_state*) lua_newuserdata(L, sizeof(lv8_state));
  memset(state, 0, sizeof(*state));
#if LV8_NEED_FINHACK
  state->finhack = finhack;
#endif
  lua_newtable(L); // State cleanup mt.
  lua_pushcfunction(L, luaclose_lv8);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, 2); // Pops mt.

  /* UV #3-#5: REFTAB, OBJMT, CTXMT. */
  for (int i = 3; i <= 5; i++)
    lua_newtable(L);

  /* UV #5: Configure CTXMT. */
  for (int i = 1; i <= 5; i++) // Dup UVs.
    lua_pushvalue(L, i);
  luaL_setfuncs(L, lv8_object_mt, 5);

  /* UV #4: Configure OBJMT. */
  lua_pushvalue(L, 4);
  for (int i = 1; i <= 5; i++) // Dup UVs.
    lua_pushvalue(L, i);
  luaL_setfuncs(L, lv8_object_mt, 5);
  lua_pop(L, 1); // Pop #4.

  lua_pushvalue(L, 1);
  lua_insert(L, 1);
  assert(lua_gettop(L) == 6);

  /* Library methods. Consume #1..#5 */
  luaL_setfuncs(L, lv8_lib, 5);
  return 1;
}

