/*
 * Lua <-> V8 bridge, low-level FFI bindings.
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>
#include <grp.h>

#include "lv8.hpp"
#include "macros.hpp"

#if LV8_BINDING
using namespace v8;

/*
 * ArrayBuffers are rather unfortunate in V8. Try to do our
 * best to not clash with other ABs.
 */
static void
pab_weak_callback(const WeakCallbackData<ArrayBuffer,
    Persistent<ArrayBuffer> > &data)
{
  Persistent<ArrayBuffer> *pab = data.GetParameter();
  free(data.GetValue()->ToObject()->GetAlignedPointerFromInternalField(1));
  pab->Reset();
  delete pab;
}

/* Externalize new ArrayBuffer */
#define LV8_AB_MAGIC (void*)(0xDADE1330)
static void *externalize_ab(Handle<ArrayBuffer> ab)
{
  ArrayBuffer::Contents contents = ab->Externalize();
  void *ptr = contents.Data();
  ab->SetAlignedPointerInInternalField(0, LV8_AB_MAGIC);
  ab->SetAlignedPointerInInternalField(1, ptr);
  Persistent<ArrayBuffer> *pab = new Persistent<ArrayBuffer>();
  pab->Reset(ISOLATE, ab);
  pab->SetWeak(pab, pab_weak_callback);
  pab->MarkIndependent();
  return ptr;
}

/* Translate ArrayBuffer to pointer. */
static inline void *get_arraybuffer(Handle<ArrayBuffer> ab)
{
  if (!ab->IsExternal()) {
    return externalize_ab(ab);
#ifndef NDEBUG
  } else if (ab->GetAlignedPointerFromInternalField(0) != LV8_AB_MAGIC) {
    ISOLATE->ThrowException( // Belongs to somebody else
        Exception::Error(UTF8("Incompatible ArrayBuffer")));
    return 0;
#endif
  } else {
    return ab->GetAlignedPointerFromInternalField(1);
  }
}

/* Translate ArrayBuffer (or ArrayBufferView). */
static inline void *get_buf(Handle<Object> b, int32_t off)
{
  if (b->IsArrayBuffer())
    return get_arraybuffer(b.As<ArrayBuffer>());
  Handle<ArrayBufferView> abv = b.As<ArrayBufferView>();
  return (void*)(((char*)get_arraybuffer(abv->Buffer()))
      + abv->ByteOffset());
}

/* Errno constants. */
#define DEF_ERR(_) \
  _(EPERM)_(ENOENT)_(ESRCH)_(EINTR)_(EIO)_(ENXIO)_(E2BIG)_(ENOEXEC) \
  _(EBADF)_(ECHILD)_(EAGAIN)_(ENOMEM)_(EACCES)_(EFAULT)_(ENOTBLK) \
  _(EBUSY)_(EEXIST)_(EXDEV)_(ENODEV)_(ENOTDIR)_(EISDIR)_(EINVAL) \
  _(ENFILE)_(EMFILE)_(ENOTTY)_(ETXTBSY)_(EFBIG)_(ENOSPC)_(ESPIPE) \
  _(EROFS)_(EMLINK)_(EPIPE)_(EDOM)_(ENAMETOOLONG)_(ENOSYS)_(ELOOP) \
  _(ETIMEDOUT)_(ERANGE)_(EOVERFLOW)_(ENOTSUP)_(ENOTEMPTY)_(ENOBUFS) \
  _(EINPROGRESS)_(ECONNRESET)_(ECONNREFUSED)_(ECONNABORTED)_(EALREADY) \
  _(EADDRNOTAVAIL)_(EADDRINUSE)

/* Other C constants. */
#define DEF_CONST(_) \
  _(SEEK_SET)_(SEEK_CUR)_(SEEK_END) \
  _(F_OK)_(R_OK)_(W_OK)_(X_OK) \
  _(S_IFMT)_(S_IFSOCK)_(S_IFLNK)_(S_IFREG) \
  _(S_IFBLK)_(S_IFDIR)_(S_IFCHR)_(S_IFIFO) \
  _(S_ISUID)_(S_ISGID)_(S_ISVTX)_(S_IRWXU) \
  _(S_IRUSR)_(S_IWUSR)_(S_IXUSR)_(S_IRWXG) \
  _(S_IRGRP)_(S_IWGRP)_(S_IXGRP)_(S_IRWXO) \
  _(S_IROTH)_(S_IWOTH)_(S_IXOTH) \
  _(O_PATH)_(O_RDWR) \
  _(O_ACCMODE)_(O_RDONLY)_(O_WRONLY) \
  _(O_CREAT)_(O_EXCL)_(O_NOCTTY) \
  _(O_TRUNC)_(O_APPEND)_(O_DIRECTORY) \
  _(O_EXCL)_(O_NOFOLLOW)_(O_SYNC) \
  _(O_DIRECT) \
  _(CLOCK_REALTIME)_(CLOCK_MONOTONIC)

#define ASTR(n) *String::Utf8Value(info[n]->ToString())
#define AINT(n) info[n]->ToInt32()->Value()
#define ALONG(n) info[n]->ToNumber()->Value()
#define ABUF(n) get_buf(info[n]->ToObject(), 0)
#define ABUFP(n, off) get_buf(info[n]->ToObject(), AINT(off))

static const char *mini_strerrno(int err)
{
  static char errbuf[4096];
  const char *errsym = errbuf;
#define SELECT_ERRSYM(e) \
  case e: errsym = #e; break;

  switch (err) {
    DEF_ERR(SELECT_ERRSYM)
    default:
      sprintf(errbuf, "E%d", err);
  }
  return errsym;
}

static void do_errno(const v8::FunctionCallbackInfo<Value> &info,
    const char *name)
{
  int err = errno;
  HandleScope scope(ISOLATE);
  Local<Object> self = info.This()->ToObject();
  const char *errsym = mini_strerrno(err);
  self->Set(UTF8("errcode"), Int32::New(ISOLATE, err));
  self->Set(UTF8("errsym"), UTF8(errsym));
  self->Set(UTF8("errstr"), UTF8(strerror(err)));
  self->Set(UTF8("syscall"), UTF8(name));
}

#define B_BEFORE(n,args...) int ret = n(args); if (ret < 0)
#define B_AFTER
#define B_RET info.GetReturnValue().Set(Int32::New(i, ret));
#define BIND(n, args...) \
  static void binding_##n(const v8::FunctionCallbackInfo<Value> &info) { \
    Isolate *i = ISOLATE; \
    HandleScope scope(i); \
    B_BEFORE(n,args) \
      do_errno(info, #n); \
    else { \
      B_AFTER \
    } \
    B_RET \
  }

/* Common template functions - return value as-is. */
BIND(chdir, ASTR(0));
BIND(getgid, );
BIND(getuid, );
BIND(setgid, AINT(0));
BIND(setuid, AINT(0));
BIND(umask, AINT(0));
BIND(getgroups, AINT(0), (gid_t*)ABUF(1));
BIND(setgroups, AINT(0), (const gid_t*)ABUF(1));
BIND(initgroups, ASTR(0), AINT(1));
BIND(kill, AINT(0), AINT(1));
BIND(clock_gettime, AINT(0), (struct timespec*)ABUF(1));
BIND(rename, ASTR(0), ASTR(1))
BIND(ftruncate, AINT(0), ALONG(1))
BIND(truncate, ASTR(0), ALONG(1))
BIND(chown, ASTR(0), AINT(1), AINT(2))
BIND(fchown, AINT(0), AINT(1), AINT(2))
BIND(lchown, ASTR(0), AINT(1), AINT(2))
BIND(chmod, ASTR(0), AINT(1))
BIND(fchmod, AINT(0), AINT(1))
BIND(link, ASTR(0), ASTR(1))
BIND(symlink, ASTR(0), ASTR(1))
BIND(unlink, ASTR(0))
BIND(rmdir, ASTR(0))
BIND(mkdir, ASTR(0), AINT(1))
BIND(close, AINT(0))
BIND(open, ASTR(0), AINT(1), AINT(2))
BIND(utimes, ASTR(0), (const struct timeval*)ABUF(1))
BIND(futimes, AINT(0), (const struct timeval*)ABUF(1))
BIND(fsync, AINT(0))
#undef B_BEFORE
#define B_BEFORE(n,args...) ssize_t ret = n(args); if (ret < 0)
BIND(write, AINT(0), ABUFP(1,3), (size_t)AINT(2))
BIND(read, AINT(0), ABUFP(1,3), (size_t)AINT(2))
BIND(pwrite, AINT(0), ABUFP(1,4), (size_t)AINT(2), ALONG(3))
BIND(pread, AINT(0), ABUFP(1,4), (size_t)AINT(2), ALONG(3))
#undef B_BEFORE
#undef B_RET
#define B_BEFORE(n,args...) off_t ret = n(args); if (ret < 0)
#define B_RET info.GetReturnValue().Set(Number::New(i, ret));
BIND(lseek, AINT(0), ALONG(1), AINT(2))
#undef B_RET
#define B_RET info.GetReturnValue().Set(Int32::New(i, ret));

/* Stat stores result in this (and return integer). */
#undef B_BEFORE
#undef B_AFTER
#define ST_FIELDS(N) \
    N(dev)  N(ino)  N(mode) N(nlink)  N(uid) \
    N(gid)  N(rdev) N(size) N(blksize)N(blocks) \
    N(atime)N(mtime)N(ctime)
#define ASSIGN_STAT(n) \
  stat_buf->Set(Int32::New(i, counter++), Number::New(i, ((double)st.st_##n)));
#define B_BEFORE(n,args...) struct stat st; uint32_t counter = 0; \
    Local<Object> self = info.This()->ToObject(); int ret = \
    n(args); if (ret < 0)
#define B_AFTER Local<Array> stat_buf = Array::New(i, 14); \
  ST_FIELDS(ASSIGN_STAT) \
  self->Set(UTF8("stat_buf"), stat_buf);
BIND(stat, ASTR(0), &st)
BIND(lstat, ASTR(0), &st)
BIND(fstat, AINT(0), &st)

/* Functions which need buffer and return it or NULL. */
#undef B_BEFORE
#undef B_AFTER
#undef B_RET
#define B_BEFORE(n,args...) char buf[PATH_MAX+1]; char *ret = n(args); if (!ret)
#define B_AFTER info.GetReturnValue().Set(Undefined(ISOLATE));
#define B_RET info.GetReturnValue().Set(UTF8(buf));
BIND(realpath, ASTR(0), buf);
BIND(getcwd, buf, PATH_MAX);

/* Functions which return return ssize_t (but we pass string to js). */
#undef B_BEFORE
#define B_BEFORE(n,args...) char buf[PATH_MAX+1]; ssize_t ret = n(args); if (ret < 0)
BIND(readlink, ASTR(0), buf, PATH_MAX)

/* Functions with void return. */
#undef B_BEFORE
#undef B_AFTER
#undef B_RET
#define B_BEFORE(n,args...) n(args); if (0)
#define B_RET info.GetReturnValue().Set(Undefined(ISOLATE));
#define B_AFTER info.GetReturnValue().Set(Undefined(ISOLATE));
BIND(abort, );

/*
 * Return array of directory entries, '.' and '..' excluded.
 * This is implemented high-level because calling readdir for each
 * entry would kill performance on JS->C++ FFI overhead.
 */
static void binding_readdir(const v8::FunctionCallbackInfo<Value> &info) {
  Isolate *i = ISOLATE;
  HandleScope scope(i);
  DIR *d = opendir(ASTR(0));
  if (!d) {
    do_errno(info, "readdir");
    info.GetReturnValue().Set(Undefined(ISOLATE)); // Undefined on error.
  } else {
    Local<Array> ents = Array::New(i);
    union {
      struct dirent de;
      char pad[sizeof(de) + PATH_MAX+1];
    } de;
    struct dirent *dep = &de.de;
    uint32_t idx = 0;
    while (!readdir_r(d, &de.de, &dep) && dep) {
      if (!strcmp(dep->d_name, ".") ||
          !strcmp(dep->d_name, ".."))
        continue;
      ents->Set(idx++, UTF8(dep->d_name));
    }
    closedir(d);
    info.GetReturnValue().Set(ents);
  }
}

#define B_IMPLEMENTS(_) \
  _(fsync)_(open)_(close)_(mkdir)_(rmdir)_(unlink)_(symlink)_(link) \
  _(fchmod)_(chmod)_(lchown)_(fchown)_(chown)_(truncate) \
  _(ftruncate)_(rename)_(readlink)_(stat)_(lstat)_(fstat) \
  _(realpath)_(readdir)_(read)_(write)_(lseek)_(pread)_(pwrite) \
  _(utimes)_(futimes)_(kill)_(clock_gettime)_(initgroups)_(setgroups) \
  _(getgroups)_(getgid)_(setgid)_(setuid)_(getuid)_(umask) \
  _(getcwd)_(chdir)_(abort)

#define JS_DEFUN(tab, name, fn, data) \
  tab->Set(LITERAL(name), \
      FunctionTemplate::New(ISOLATE, fn, External::New(ISOLATE, data)))

/* Eval a string (with explicit context or default one). */
static void js_vm_eval(const v8::FunctionCallbackInfo<Value> &info) {
  HandleScope scope(ISOLATE);
  UNWRAP_L;
  Handle<Value> ctx = info[0];
  Handle<Value> source = info[1];
  Handle<Value> file = info[2];
  Handle<Value> dryrun = info[3];

  Handle<Context> c;

  lv8_context *p = lv8_unwrap_js(L, ctx->ToObject(), true);
  assert(p && (p->type == LV8_OBJ_CTX || p->type == LV8_OBJ_SB));

  c = CREF(p);
  c->Enter();

  TryCatch tc;
  Handle<Script> script = Script::New(source->ToString(), file);
  Handle<Value> ex = tc.Exception();
  if (!ex.IsEmpty()) { // Caught syntax error, don't execute.
    Handle<Object> exo = ex->ToObject();
    Handle<Message> msg = tc.Message();
    if (!msg.IsEmpty()) { // V8 won't tell by default.
      exo->Set(UTF8("sourceLine"), msg->GetSourceLine());
      exo->Set(UTF8("scriptResourceName"), msg->GetScriptResourceName());
      exo->Set(UTF8("lineNumber"), Int32::New(ISOLATE, msg->GetLineNumber()));
      exo->Set(UTF8("startPosition"), Int32::New(ISOLATE, msg->GetStartPosition()));
      exo->Set(UTF8("endPosition"), Int32::New(ISOLATE, msg->GetEndPosition()));
      exo->Set(UTF8("startColumn"), Int32::New(ISOLATE, msg->GetStartColumn()));
      exo->Set(UTF8("endColumn"), Int32::New(ISOLATE, msg->GetStartColumn()));
    }
    tc.ReThrow();
  } else if (!(!dryrun.IsEmpty() && dryrun->IsTrue())) {
    info.GetReturnValue().Set(script->Run());
  }

  c->Exit();
}

/* Construct a JS context. */
static void js_vm_context(const v8::FunctionCallbackInfo<Value> &info) {
  UNWRAP_L;
  lv8_context *c = lv8_context_factory(L);
  if (!info[0].IsEmpty() && info[0]->IsObject())
    lv8_shallow_copy(L, OREF(c), info[0]->ToObject());
  info.GetReturnValue().Set(OREF(c));
  lua_pop(L, 1);
}

/* Construct a JS sandbox. */
static void js_vm_sandbox(const v8::FunctionCallbackInfo<Value> &info) {
  UNWRAP_L;
  Handle<Object> o = info[0]->ToObject();
  if (lv8_object *p = lv8_unwrap_js(L, o)) {
    lv8_push(L, p);
    lv8_context *c = lv8_sandbox_factory(L, -1);
    info.GetReturnValue().Set(OREF(c));
    lua_pop(L, 2); // Kept alive by js2lua or finalizer resurrection.
  }
}

/* Initialize low-level bindings. */
Handle<ObjectTemplate> lv8_binding_init(lua_State *L)
{
  EscapableHandleScope scope(ISOLATE);
  Local<ObjectTemplate> b = ObjectTemplate::New();
#define B_EXPORT(n) b->Set(UTF8(#n), FunctionTemplate::New(ISOLATE, binding_##n));
  B_IMPLEMENTS(B_EXPORT) // Define FS api.
#define B_CONST(n) b->Set(UTF8(#n), Int32::New(ISOLATE, n));
  DEF_ERR(B_CONST)
  DEF_CONST(B_CONST)

  JS_DEFUN(b, "eval", js_vm_eval, L); // Execute.
  JS_DEFUN(b, "context", js_vm_context, L); // Create context.
  JS_DEFUN(b, "sandbox", js_vm_sandbox, L); // Create sandbox.

  b->Set(LITERAL("v8_version"), UTF8(V8::GetVersion()));

  extern char **environ;
  Local<ObjectTemplate> env = ObjectTemplate::New(); // Export environ.
  for (int i = 0; environ[i]; i++) {
    char *sep = strchr(environ[i], '=');
    env->Set(UTF8(environ[i], String::kNormalString, sep - environ[i]),
        UTF8(sep + 1));
  }
  b->Set(LITERAL("env"), env);

  b->Set(LITERAL("pid"), Int32::New(ISOLATE, getpid()));

  b->Set(LITERAL("arch"), LITERAL(
#if defined(__amd64__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
  "x64"
#elif defined(__arm__) || defined(_ARM) || defined(_M_ARM)
  "arm"
#elif defined(__i386) || defined(_M_IX86) || defined(_X86_) || defined(__INTEL__)
  "ia32"
#elif defined(__mips) || defined(__MIPS__)
  "mips"
#else
#error "Unable to detect CPU architecture"
#endif
  ));

  b->Set(LITERAL("platform"), LITERAL(
#if defined(V8_OS_LINUX) || defined(V8_OS_ANDROID)
  "linux"
#elif defined(V8_OS_BSD)
  "bsd"
#elif defined(V8_OS_WIN)
  "win32"
#else
#error "Failed to detect platform."
#endif
  ));
  return scope.Escape(b);
}
#endif

