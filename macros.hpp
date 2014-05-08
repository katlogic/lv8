/*
 * Lua <-> V8 bridge, common macros.
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

#define ISOLATE Isolate::GetCurrent()
#define LOCAL(v) Local<Value>::New(ISOLATE, (v))
#define REF(t,v) Handle<t>::New(ISOLATE, (v))
#define ESCAPE(v) scope.Escape(LOCAL(v));
#define UTF8(arg...) String::NewFromUtf8(ISOLATE, arg)
#define LITERAL(s) \
  String::NewFromUtf8(ISOLATE, s, String::kInternalizedString, sizeof(s)-1)
#define THROW(s) \
  ISOLATE->ThrowException(Exception::Error(UTF8(s)));
#define UNWRAP_L \
  lua_State *L = (lua_State*)External::Cast(*info.Data())->Value()
#define OREF(v) Local<Object>::New(ISOLATE, (v->object))
#define CREF(v) Local<Context>::New(ISOLATE, (v->context))



