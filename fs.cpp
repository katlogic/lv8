/*
 * Lua <-> V8 bridge, filesystem module.
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

#if LV8_FS_API
using namespace v8;

#define ISOLATE Isolate::GetCurrent()
#define NEWSTR(arg...) String::NewFromUtf8(ISOLATE, arg)

/*
 * ArrayBuffers are rather unfortunate in V8. Try to do our
 * best to not clash with other ABs.
 */
static void
pab_weak_callback(const WeakCallbackData<ArrayBuffer,
    Persistent<ArrayBuffer> > &data)
{
  printf("freed !\n");
  Persistent<ArrayBuffer> *pab = data.GetParameter();
  free(data.GetValue()->ToObject()->GetAlignedPointerFromInternalField(1));
  pab->Reset();
  delete pab;
}
#define LV8_AB_MAGIC (void*)(0xDADE1330)
static void *get_arraybuffer(Handle<ArrayBuffer> ab)
{
  if (!ab->IsExternal()) {
    ArrayBuffer::Contents contents = ab->Externalize();
    void *ptr = contents.Data();
    ab->SetAlignedPointerInInternalField(0, LV8_AB_MAGIC);
    ab->SetAlignedPointerInInternalField(1, ptr);
    Persistent<ArrayBuffer> *pab = new Persistent<ArrayBuffer>();
    pab->Reset(ISOLATE, ab);
    pab->SetWeak(pab, pab_weak_callback);
    pab->MarkIndependent();
    return ptr;
  } else if (ab->GetAlignedPointerFromInternalField(0) != LV8_AB_MAGIC) {
    ISOLATE->ThrowException( // Belongs to somebody else
        Exception::Error(NEWSTR("Incompatible ArrayBuffer")));
    return 0;
  } else {
    return ab->GetAlignedPointerFromInternalField(1);
  }
}

static void *get_buf(Handle<Object> b, int32_t off)
{
  if (b->IsArrayBuffer())
    return get_arraybuffer(b.As<ArrayBuffer>());
  Handle<ArrayBufferView> abv = b.As<ArrayBufferView>();
  return (void*)(((char*)get_arraybuffer(abv->Buffer()))
      + abv->ByteOffset());
}

/* Raw filesystem mini-api. Handholding to be done in JS. */
#define DEF_ERR(_) \
  _(EPERM)_(ENOENT)_(ESRCH)_(EINTR)_(EIO)_(ENXIO)_(E2BIG)_(ENOEXEC) \
  _(EBADF)_(ECHILD)_(EAGAIN)_(ENOMEM)_(EACCES)_(EFAULT)_(ENOTBLK) \
  _(EBUSY)_(EEXIST)_(EXDEV)_(ENODEV)_(ENOTDIR)_(EISDIR)_(EINVAL) \
  _(ENFILE)_(EMFILE)_(ENOTTY)_(ETXTBSY)_(EFBIG)_(ENOSPC)_(ESPIPE) \
  _(EROFS)_(EMLINK)_(EPIPE)_(EDOM)_(ENAMETOOLONG)_(ENOSYS)_(ELOOP) \
  _(ETIMEDOUT)_(ERANGE)_(EOVERFLOW)_(ENOTSUP)_(ENOTEMPTY)_(ENOBUFS) \
  _(EINPROGRESS)_(ECONNRESET)_(ECONNREFUSED)_(ECONNABORTED)_(EALREADY) \
  _(EADDRNOTAVAIL)_(EADDRINUSE)

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
  _(O_DIRECT)

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
  case e: errsym = #e;

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
  self->Set(NEWSTR("errcode"), Int32::New(ISOLATE, err));
  self->Set(NEWSTR("errsym"), NEWSTR(errsym));
  self->Set(NEWSTR("errstr"), NEWSTR(strerror(err)));
  self->Set(NEWSTR("syscall"), NEWSTR(name));
}

#define FS_BEFORE int ret;
#define FS_AFTER
#define FS_RET info.GetReturnValue().Set(Int32::New(i, ret));
#define FS(n, args...) \
  static void fs_##n(const v8::FunctionCallbackInfo<Value> &info) { \
    Isolate *i = ISOLATE; \
    HandleScope scope(i); \
    FS_BEFORE \
    ret = n(args); \
    if (ret < 0) \
      do_errno(info, #n); \
    else { \
      FS_AFTER \
    } \
    FS_RET \
  }

/* Common template functions - return integer return value. */
FS(rename, ASTR(0), ASTR(1))
FS(ftruncate, AINT(0), ALONG(1))
FS(truncate, ASTR(0), ALONG(1))
FS(chown, ASTR(0), AINT(1), AINT(2))
FS(fchown, AINT(0), AINT(1), AINT(2))
FS(lchown, ASTR(0), AINT(1), AINT(2))
FS(chmod, ASTR(0), AINT(1))
FS(fchmod, AINT(0), AINT(1))
FS(link, ASTR(0), ASTR(1))
FS(symlink, ASTR(0), ASTR(1))
FS(unlink, ASTR(0))
FS(rmdir, ASTR(0))
FS(mkdir, ASTR(0), AINT(1))
FS(close, AINT(0))
FS(open, ASTR(0), AINT(1), AINT(2))
FS(utimes, ASTR(0), (const struct timeval*)ABUF(1))
FS(futimes, AINT(0), (const struct timeval*)ABUF(1))
FS(fsync, AINT(0))
#undef FS_BEFORE
#define FS_BEFORE ssize_t ret;
FS(write, AINT(0), ABUFP(1,3), (size_t)AINT(2))
FS(read, AINT(0), ABUFP(1,3), (size_t)AINT(2))
FS(pwrite, AINT(0), ABUFP(1,4), (size_t)AINT(2), ALONG(3))
FS(pread, AINT(0), ABUFP(1,4), (size_t)AINT(2), ALONG(3))
#undef FS_BEFORE
#undef FS_RET
#define FS_BEFORE off_t ret;
#define FS_RET info.GetReturnValue().Set(Number::New(i, ret));
FS(lseek, AINT(0), ALONG(1), AINT(2))
#undef FS_RET
#define FS_RET info.GetReturnValue().Set(Int32::New(i, ret));

/* Readlink and stat stores result in this (and return integer). */
#undef FS_BEFORE
#undef FS_AFTER
#define FS_BEFORE char buf[PATH_MAX+1]; ssize_t ret; \
    Local<Object> self = info.This()->ToObject();
#define FS_AFTER self->Set(NEWSTR("readlink_buf"), \
    NEWSTR(buf,String::kNormalString,(int)ret));
FS(readlink, ASTR(0), buf, PATH_MAX)
#undef FS_BEFORE
#undef FS_AFTER
#define ST_FIELDS(N) \
    N(dev)  N(ino)  N(mode) N(nlink)  N(uid) \
    N(gid)  N(rdev) N(size) N(blksize)N(blocks) \
    N(atime)N(mtime)N(ctime)
#define ASSIGN_STAT(n) \
  stat_buf->Set(Int32::New(i, counter++), Number::New(i, ((double)st.st_##n)));
#define FS_BEFORE struct stat st; int ret; uint32_t counter = 0; \
    Local<Object> self = info.This()->ToObject();
#define FS_AFTER Local<Array> stat_buf = Array::New(i, 14); \
  ST_FIELDS(ASSIGN_STAT) \
  self->Set(NEWSTR("stat_buf"), stat_buf);
FS(stat, ASTR(0), &st)
FS(lstat, ASTR(0), &st)
FS(fstat, AINT(0), &st)

/* Return string of computed realpath (or undefined on error). */
static void fs_realpath(const v8::FunctionCallbackInfo<Value> &info) {
  Isolate *i = ISOLATE;
  HandleScope scope(i);
  char buf[PATH_MAX+1];
  char *ret = realpath(ASTR(0), buf);
  if (!ret) {
    do_errno(info, "realpath");
    info.GetReturnValue().Set(Undefined(ISOLATE));
  } else {
    info.GetReturnValue().Set(NEWSTR(buf));
  }
}

/* Return array of directory entries, '.' and '.. excluded. */
static void fs_readdir(const v8::FunctionCallbackInfo<Value> &info) {
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
      ents->Set(idx++, NEWSTR(dep->d_name));
    }
    closedir(d);
    info.GetReturnValue().Set(ents);
  }
}

#define FS_IMPLEMENTS(_) \
  _(fsync)_(open)_(close)_(mkdir)_(rmdir)_(unlink)_(symlink)_(link) \
  _(fchmod)_(chmod)_(lchown)_(fchown)_(chown)_(truncate) \
  _(ftruncate)_(rename)_(readlink)_(stat)_(lstat)_(fstat) \
  _(realpath)_(readdir)_(read)_(write)_(lseek)_(pread)_(pwrite) \
  _(utimes)_(futimes)

/* Initialize and return global fs.* object template. */
Local<ObjectTemplate> lv8_fs_init()
{
  EscapableHandleScope scope(ISOLATE);
  Local<ObjectTemplate> fs = ObjectTemplate::New();
#define FS_EXPORT(n) fs->Set(NEWSTR(#n), FunctionTemplate::New(ISOLATE, fs_##n));
  FS_IMPLEMENTS(FS_EXPORT) // Define FS api.
#define FS_CONST(n) fs->Set(NEWSTR(#n), Int32::New(ISOLATE, n));
  DEF_ERR(FS_CONST)
  DEF_CONST(FS_CONST)
  return scope.Escape(fs);
}
#endif

