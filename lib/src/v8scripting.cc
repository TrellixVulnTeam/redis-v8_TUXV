/*
 * Copyright (c) 2013, Arseniy Pavlenko <h0x91b@gmail.com>
 * All rights reserved.
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <stdio.h>
#include <v8.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include "v8core_js.h"
#include "v8scripting.h"

using namespace v8;

Persistent<Context> persistent_v8_context;
v8::Isolate* isolate;

const char* ToCString(const v8::String::Utf8Value& value);
void *single_thread_function_for_slow_run_js(void *param);
void *setTimeoutExec(void *param);
v8::Handle<v8::Value> parse_response();
char *js_dir = NULL;
char *js_flags = NULL;
int js_code_id = 0;
pthread_t thread_id_for_single_thread_check;
pthread_t thread_id_for_setTimeoutExec;
int js_timeout = 15;
int js_slow = 250;
char *last_js_run = NULL;

void (*redisLogRawPtr)(int, char*);
redisClient* (*redisCreateClientPtr)(int);
redisCommand* (*lookupCommandByCStringPtr)(char*);
void (*callPtr)(redisClient*,int);
robj* (*createStringObjectPtr)(char*,size_t);
sds (*sdsemptyPtr)();
sds (*sdscatlenPtr)(sds, const void *,size_t);
size_t (*sdslenPtr)(const sds);
void (*listDelNodePtr)(list*,listNode*);
void (*decrRefCountPtr)(robj*);
void (*sdsfreePtr)(sds);
void* (*zmallocPtr)(size_t);
void (*zfreePtr)(void*);
void (*redisLogPtr)(int,const char*,...);
void (*addReplyPtr)(redisClient *, robj *);
sds (*sdsnewPtr)(const char*);
robj* (*createObjectPtr)(int,void*);
void (*addReplyBulkPtr)(redisClient*,robj*);
void (*addReplyErrorPtr)(redisClient*,char*);
robj *(*lookupKeyReadPtr)(redisDb*, robj*);
void (*setKeyPtr)(redisDb*, robj*, robj*);
void (*notifyKeyspaceEventPtr)(int, char *, robj *, int );
int (*checkTypePtr)(redisClient *, robj *, int);
int (*getLongLongFromObjectOrReplyPtr)(redisClient *, robj *, long long *, const char *);
robj *(*createStringObjectFromLongLongPtr)(long long value);
void (*dbOverwritePtr)(redisDb *, robj *, robj *);
void (*dbAddPtr)(redisDb *, robj *, robj *);
void (*signalModifiedKeyPtr)(redisDb *, robj *);

redisClient *client=NULL;

char *redisReply = NULL;
char bufForString[4096] = {0};
char lastError[4096] = {0};
int scriptStart = 0;
int timeoutScriptStart = 0;

unsigned int GetTickCount(void) 
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec*1000 + (tv.tv_usec/1000);
}

// v8::Handle<v8::Value> V8BinaryString(const uint8_t* data, size_t size) { 
// 	uint16_t* out = new uint16_t[size]; 
// 	for (size_t n = 0; n < size; n++) out[n] = data[n]; 
// 	Local<String> s = v8::String::NewFromTwoByte(Isolate::GetCurrent(), out, v8::String::kNormalString, size); 
// 	delete[] out; 
// 	return s; 
// } 

v8::Handle<v8::Value> parse_string(char *replyPtr){
	//printf("parse_line_ok replyPtr[0]='%c' string length:%i\n",replyPtr[0],atoi(replyPtr));
	int strlength = atoi(replyPtr);
	bool special_minus_one = false;
	if(strlength==-1){
		strlength = 0;
		special_minus_one = true;
	}
	int len = strstr(replyPtr,"\r\n")-replyPtr;
	if(special_minus_one) len-=2;
	replyPtr+=len+2;
	if(strlength<4096){
		memcpy(bufForString,replyPtr,strlength);
		replyPtr+=strlength+2;
		bufForString[strlength]='\0';
		redisReply = replyPtr;
		if(special_minus_one) return v8::Null();
		v8::Local<v8::String> ret = v8::String::New(bufForString,strlength);
		return ret;
	}
	char *buff= (char*)zmallocPtr(strlength+1);
	memcpy(buff,replyPtr,strlength);
	replyPtr+=strlength+2;
	buff[strlength]='\0';
	if(strlen(bufForString)!=strlength){
		//binary data
		v8::Local<v8::Array> ret = v8::Array::New(strlength);
		for(int i=0;i<strlength;i++){
			ret->Set(v8::Number::New(i), v8::Number::New((unsigned char)bufForString[i]));
		}
		zfreePtr(buff);
		return ret;
	}
	//printf("line is '%s'\n",buff);
	v8::Local<v8::String> ret = v8::String::New(buff);
	zfreePtr(buff);
	redisReply = replyPtr;
	if(special_minus_one) return v8::Null();
	return ret;
}

v8::Handle<v8::Value> parse_error(char *replyPtr){
	int len = strstr(replyPtr,"\r\n")-replyPtr;
	memset(lastError,0,4096);
	strncpy(lastError,replyPtr,len);
	replyPtr+=len+2;
	redisReply = replyPtr;
	printf("lastError set to '%s'\n",lastError);
	return v8::Boolean::New(false);
}

v8::Handle<v8::Value> parse_bulk(char *replyPtr){
	int arr_length = atoi(replyPtr);
	int len = strstr(replyPtr,"\r\n")-replyPtr;
	replyPtr+=len+2;
	redisReply = replyPtr;
	v8::Local<v8::Array> ret = v8::Array::New(arr_length);
	for(int i=0;i<arr_length;i++){
		ret->Set(v8::Number::New(i), parse_response());
	}
	return ret;
}


v8::Handle<v8::Value> parse_response(){
	char *replyPtr = redisReply;
	long long lvalue = 0;
	//printf("replyPtr[0]='%c' reply='%s'\n",replyPtr[0],replyPtr);
	switch(replyPtr[0]){
		case '+':
			return v8::Boolean::New(true);
		case '-':
			return parse_error(++replyPtr);
		case ':':
			lvalue = atoll(++replyPtr);
			if(lvalue > 9007199254740992 || lvalue < -9007199254740992){
				char buf[20] = {0};
				sprintf(buf,"%lli",lvalue);
				v8::Local<v8::String> v8reply = v8::String::New(buf);
				return v8reply;
			}
			return v8::Integer::New(lvalue);
		case '$':
			return parse_string(++replyPtr);
		case '*':
			return parse_bulk(++replyPtr);
		default:
			printf("cant parse reply %s\n",replyPtr);
	}
	return v8::Undefined();
}

void getLastError(const v8::FunctionCallbackInfo<v8::Value>& args) {
	args.GetReturnValue().Set(v8::String::New(lastError));
}

void raw_get(const v8::FunctionCallbackInfo<v8::Value>& args) {
	redisClient *c = client;
	v8::String::Utf8Value strkey(args[0]);
	robj *key = createStringObjectPtr((char*)*strkey,strkey.length());
	robj *reply = lookupKeyReadPtr(c->db,key);
	decrRefCountPtr(key);
	if(reply == NULL || reply->type != REDIS_STRING){
		//printf("reply is NULL or not string\n");
		args.GetReturnValue().Set(v8::Null());
		return;
	}
	//printf("reply is %s\n",reply->ptr);
	v8::Local<v8::String> v8reply = v8::String::New((const char *)reply->ptr);
	//printf("return to v8\n");
	args.GetReturnValue().Set(v8reply);
}

void raw_set(const v8::FunctionCallbackInfo<v8::Value>& args) {
	redisClient *c = client;
	v8::String::Utf8Value strkey(args[0]);
	v8::String::Utf8Value strval(args[1]);
	robj *key = createStringObjectPtr((char*)*strkey,strkey.length());
	robj *val = createStringObjectPtr((char*)*strval,strval.length());
	setKeyPtr(c->db,key,val);
	notifyKeyspaceEventPtr(REDIS_NOTIFY_STRING,(char*)"set",key,c->db->id);
	decrRefCountPtr(key);
	decrRefCountPtr(val);
	args.GetReturnValue().Set(v8::Boolean::New(true));
}

void raw_incrby(const v8::FunctionCallbackInfo<v8::Value>& args) {
	redisClient *c = client;
	long long value, oldvalue, incr;
	robj *newvalue, *key, *reply;
	v8::String::Utf8Value strkey(args[0]);
	Local<Integer> i = Local<Integer>::Cast(args[1]);
	incr = (long long)(i->IntegerValue());
	key = createStringObjectPtr((char*)*strkey,strkey.length());
	reply = lookupKeyReadPtr(c->db,key);
	
	if (reply != NULL && checkTypePtr(c,reply,REDIS_STRING)){
		memset(lastError,0,4096);
		strcpy(lastError,"-value is not integer");
		printf("lastError set to '%s'\n",lastError);
		decrRefCountPtr(key);
		args.GetReturnValue().Set(v8::Boolean::New(false));
		return;
	}
    if (getLongLongFromObjectOrReplyPtr(c,reply,&value,NULL) != REDIS_OK) {
		memset(lastError,0,4096);
		strcpy(lastError,"-getLongLongFromObjectOrReply failed");
		printf("lastError set to '%s'\n",lastError);
		args.GetReturnValue().Set(v8::Boolean::New(false));
		return;
	}
	
	oldvalue = value;
    if (
		(incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue))
		|| (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))
	) 
	{
		memset(lastError,0,4096);
		strcpy(lastError,"-increment or decrement would overflow");
		printf("lastError set to '%s'\n",lastError);
		decrRefCountPtr(key);
		args.GetReturnValue().Set(v8::Boolean::New(false));
		return;
	}
	value += incr;
	newvalue = createStringObjectFromLongLongPtr(value);
	if (reply){
		dbOverwritePtr(c->db,key,newvalue);
	}
	else{
		dbAddPtr(c->db,key,newvalue);
	}
	signalModifiedKeyPtr(c->db,key);
	notifyKeyspaceEventPtr(REDIS_NOTIFY_STRING,(char*)"incrby",key,c->db->id);

	//printf("reply is %s\n",reply->ptr);
	//v8 max integer is 2^53 (+9007199254740992) (-9007199254740992)
	if( value > 9007199254740992 || value < -9007199254740992){
		char buf[20] = {0};
		sprintf(buf,"%lli",value);
		v8::Local<v8::String> v8reply = v8::String::New(buf);
		decrRefCountPtr(key);
		decrRefCountPtr(newvalue);
		args.GetReturnValue().Set(v8reply);
		return;
	}
	v8::Local<v8::Number> v8reply = v8::Number::New(value);
	decrRefCountPtr(key);
	args.GetReturnValue().Set(v8reply);
}

void run(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Locker locker(isolate);
	v8::Isolate::Scope isolateScope(isolate);
	HandleScope handle_scope(isolate);
	v8::Local<v8::Context> v8_context = v8::Local<v8::Context>::New(isolate, persistent_v8_context);
	int argc = args.Length();
	redisCommand *cmd;
	robj **argv;
	redisClient *c = client;
	sds reply;
	
	argv = (robj**)zmallocPtr(sizeof(robj*)*argc);
	
	for (int i = 0; i < args.Length(); i++) {
		HandleScope handle_scope(isolate);
		v8::String::Utf8Value str(args[i]);
		argv[i] = createStringObjectPtr((char*)*str,str.length());
	}
	
	/* Setup our fake client for command execution */
	c->argv = argv;
	c->argc = argc;
	
	/* Command lookup */
	cmd = lookupCommandByCStringPtr((sds)argv[0]->ptr);
	if(!cmd){
		printf("no cmd '%s'!!!\n",(char*)argv[0]->ptr);
		args.GetReturnValue().Set(v8::Undefined());
		return;
	}
	/* Run the command */
	c->cmd = cmd;
	c->cmd->proc(c); //raw call, without redis stats log
	//callPtr(c,REDIS_CALL_STATS);
	reply = sdsemptyPtr();
	if (c->bufpos) {
		reply = sdscatlenPtr(reply,c->buf,c->bufpos);
		c->bufpos = 0;
	}
	
	while(listLength(c->reply)) {
		robj *o = (robj*)listNodeValue(listFirst(c->reply));
		reply = sdscatlenPtr(reply,o->ptr,strlen((const char*)o->ptr));
		listDelNodePtr(c->reply,listFirst(c->reply));
	}
	
	redisReply = reply;
	v8::Handle<v8::Value> ret_value= parse_response();
	v8::Local<v8::String> v8reply = v8::String::New(reply);
	
	sdsfreePtr(reply);
	c->reply_bytes = 0;
	
	for (int j = 0; j < c->argc; j++)
		decrRefCountPtr(c->argv[j]);
	zfreePtr(c->argv);
	
	args.GetReturnValue().Set(ret_value);
}

char *file_get_contents(char *filename)
{
	FILE* f = fopen(filename, "r");
	if(!f) return NULL;
	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	char* content = (char*)zmallocPtr(size+1);
	memset(content,0,size);
	rewind(f);
	fread(content, sizeof(char), size, f);
	content[size] = '\0';
	return content;
}

const char* ToCString(const v8::String::Utf8Value& value) {
	return *value ? *value : "<string conversion failed>";
}

void redis_log(const v8::FunctionCallbackInfo<v8::Value>& args) {
	if(args.Length()>=2){
		Local<Integer> i = Local<Integer>::Cast(args[0]);
		int log_level = (int)(i->Int32Value());
		v8::String::Utf8Value str(args[1]);
		const char* cstr = ToCString(str);
		redisLogRawPtr(log_level, (char*)cstr);
	}
}

void initV8(){
	if(js_flags){
		v8::V8::SetFlagsFromString(
			js_flags,
			strlen(js_flags)
		);
	}
	
	pthread_create(&thread_id_for_single_thread_check, NULL, single_thread_function_for_slow_run_js, (void*)NULL);
	
	isolate = v8::Isolate::GetCurrent();
	v8::Locker locker(isolate);
	v8::Isolate::Scope isolateScope(isolate);
	HandleScope handle_scope(isolate);
	
	v8::Handle<v8::ObjectTemplate> global = v8::ObjectTemplate::New();
	v8::Handle<v8::ObjectTemplate> redis = v8::ObjectTemplate::New();
	redis->Set(v8::String::New("__run"), v8::FunctionTemplate::New(run),ReadOnly);
	redis->Set(v8::String::New("__get"), v8::FunctionTemplate::New(raw_get),ReadOnly);
	redis->Set(v8::String::New("__set"), v8::FunctionTemplate::New(raw_set),ReadOnly);
	redis->Set(v8::String::New("__incrby"), v8::FunctionTemplate::New(raw_incrby),ReadOnly);
	redis->Set(v8::String::New("__log"), v8::FunctionTemplate::New(redis_log),ReadOnly);
	redis->Set(v8::String::New("getLastError"), v8::FunctionTemplate::New(getLastError),ReadOnly);
	global->Set(v8::String::New("redis"), redis);
	
	// Create a new context.
	v8::Handle<v8::Context> v8_context = v8::Context::New(isolate,NULL,global);
	
	persistent_v8_context.Reset(isolate, v8_context);
	
	// Enter the created context for compiling and running
	v8::Context::Scope context_scope(v8_context);
	
	v8::Handle<v8::String> source = v8::String::New((const char*)v8core_js);
	v8::Handle<v8::Script> script = v8::Script::Compile(source);
	v8::Handle<v8::Value> result = script->Run();
}

char *wrapcodebuf = NULL;
int wrapcodebuf_len = 4096;
char* run_js_returnbuf = NULL;
int run_js_returnbuf_len = 4096;

struct RUN_JS_RETURN {
	char *json;
	int len;
};

RUN_JS_RETURN run_js_return;

RUN_JS_RETURN *run_js(char *code, bool async_call=false){
	v8::Locker locker(isolate);
	v8::Isolate::Scope isolateScope(isolate);
	HandleScope handle_scope(isolate);
	v8::Local<v8::Context> v8_context = v8::Local<v8::Context>::New(isolate, persistent_v8_context);
	v8::Context::Scope context_scope(v8_context);
	int code_length = strlen(code);
	if(wrapcodebuf==NULL){
		wrapcodebuf_len = code_length+170;
		wrapcodebuf = (char*)zmallocPtr(wrapcodebuf_len);
	}
	if(code_length+170>wrapcodebuf_len){
		zfreePtr(wrapcodebuf);
		wrapcodebuf_len = code_length+170;
		wrapcodebuf = (char*)zmallocPtr(wrapcodebuf_len);
	}
	//memset(wrapcodebuf,0,code_length+170);
	wrapcodebuf[0] = '\0';
	if(!async_call){
		sprintf(wrapcodebuf,"inline_redis_func = function(){%s}; redis.inline_return()",code);
	} else {
		sprintf(wrapcodebuf,"(function(){ setTimeout(function(){%s},1); return '{\"ret\":true,\"cmds\":0}' })();",code);
	}
	
	//printf("%s\n",wrapcodebuf);
	
	v8::Handle<v8::String> source = v8::String::New(wrapcodebuf);
	v8::TryCatch trycatch;
	v8::Handle<v8::Script> script = v8::Script::Compile(source);
	if(script.IsEmpty()){
		Handle<Value> exception = trycatch.Exception();
		String::AsciiValue exception_str(exception);
		printf("V8 Exception: %s\n", *exception_str);
		char *errBuf = (char*)zmallocPtr(exception_str.length()+100);
		memset(errBuf,0,exception_str.length());
		sprintf(errBuf,"-Compile error: \"%s\"",*exception_str);
		printf("errBuf is '%s'\n",errBuf);
		run_js_return.json = errBuf;
		run_js_return.len = exception_str.length();
		return &run_js_return;
	}
	
	v8::Handle<v8::Value> result = script->Run();
	
	if (result.IsEmpty()) {  
		Handle<Value> exception = trycatch.Exception();
		String::AsciiValue exception_str(exception);
		printf("Exception: %s\n", *exception_str);
		char *errBuf = (char*)zmallocPtr(exception_str.length()+100);
		memset(errBuf,0,exception_str.length());
		if(!strcmp(*exception_str,"null")){
			sprintf(errBuf,"-Script runs too long, Exception error: \"%s\"",*exception_str);
		}
		else {
			sprintf(errBuf,"-Exception error: \"%s\"",*exception_str);
		}
		run_js_return.json = errBuf;
		run_js_return.len = exception_str.length();
		return &run_js_return;
	}
	v8::String::Utf8Value ascii(result);
	int size = ascii.length();
	if(run_js_returnbuf==NULL){
		run_js_returnbuf = (char*)zmallocPtr(run_js_returnbuf_len);
		memset(run_js_returnbuf,0,run_js_returnbuf_len);
	}
	if(size>=run_js_returnbuf_len){
		zfreePtr(run_js_returnbuf);
		run_js_returnbuf_len = (((size+1)/1024)+1)*1024;
		run_js_returnbuf = (char*)zmallocPtr(run_js_returnbuf_len);
		memset(run_js_returnbuf,0,run_js_returnbuf_len);
	}
	//char *rez= (char*)zmallocPtr(size);
	//memset(run_js_returnbuf,0,size+1);
	memcpy(run_js_returnbuf,*ascii,size);
	run_js_returnbuf[size] = '\0';
	run_js_return.json = run_js_returnbuf;
	run_js_return.len = size;
	return &run_js_return;
}


RUN_JS_RETURN *call_js(redisClient *c){
	v8::Locker locker(isolate);
	v8::Isolate::Scope isolateScope(isolate);
	HandleScope handle_scope(isolate);
	v8::Local<v8::Context> v8_context = v8::Local<v8::Context>::New(isolate, persistent_v8_context);
	v8::Context::Scope context_scope(v8_context);
	
	Handle<v8::Object> global = v8_context->Global();
	Handle<v8::Value> value = global->Get(String::New("jscall_wrapper_function"));
	Handle<v8::Function> jscall_wrapper_function = v8::Handle<v8::Function>::Cast(value);
	
	int argc = c->argc-1;
	
	Handle<Value> *args = (Handle<Value>*)zmallocPtr(argc*sizeof(Handle<Value>));
	for (int i = 1; i <= argc; i++) { 
		args[i-1] = v8::String::New((const char*)c->argv[i]->ptr); 
	}
	
	v8::TryCatch trycatch;
	v8::Handle<v8::Value> result = jscall_wrapper_function->Call(global, argc, args);
	zfreePtr(args);
	if (result.IsEmpty()) {  
		Handle<Value> exception = trycatch.Exception();
		String::AsciiValue exception_str(exception);
		printf("Exception: %s\n", *exception_str);
		int length = exception_str.length()+100;
		char *errBuf = (char*)zmallocPtr(length);
		memset(errBuf,0,length);
		if(!strcmp(*exception_str,"null")){
			sprintf(errBuf,"-Script runs too long, Exception error: \"%s\"",*exception_str);
		}
		else {
			sprintf(errBuf,"-Exception error: \"%s\"",*exception_str);
		}
		run_js_return.json = errBuf;
		run_js_return.len = exception_str.length();
		return &run_js_return;
	}
	
	v8::String::Utf8Value ascii(result);
	int size = ascii.length();
	if(run_js_returnbuf==NULL){
		run_js_returnbuf = (char*)zmallocPtr(run_js_returnbuf_len);
		memset(run_js_returnbuf,0,run_js_returnbuf_len);
	}
	if(size>=run_js_returnbuf_len){
		zfreePtr(run_js_returnbuf);
		run_js_returnbuf_len = (((size+1)/1024)+1)*1024;
		run_js_returnbuf = (char*)zmallocPtr(run_js_returnbuf_len);
		memset(run_js_returnbuf,0,run_js_returnbuf_len);
	}
	memcpy(run_js_returnbuf,*ascii,size);
	run_js_returnbuf[size] = '\0';
	run_js_return.json = run_js_returnbuf;
	run_js_return.len = size;
	return &run_js_return;
}

void load_user_script(char *file){
	v8::Locker locker(isolate);
	v8::Isolate::Scope isolateScope(isolate);
	HandleScope handle_scope(isolate);
	v8::Local<v8::Context> v8_context = v8::Local<v8::Context>::New(isolate, persistent_v8_context);

	v8::Context::Scope context_scope(v8_context);
	char* core = file_get_contents(file);
	v8::Handle<v8::String> source = v8::String::New(core);
	v8::TryCatch trycatch;
	v8::Handle<v8::Script> script = v8::Script::Compile(source);
	if(script.IsEmpty()){
		Handle<Value> exception = trycatch.Exception();
		String::AsciiValue exception_str(exception);
		printf("V8 Exception: %s\n", *exception_str);
		char *errBuf = (char*)zmallocPtr(4096); //TODO: calc size
		memset(errBuf,0,4096);
		sprintf(errBuf,"-Compile error: \"%s\"",*exception_str);
		printf("errBuf is '%s'\n",errBuf);
		return;
	}
	v8::Handle<v8::Value> result = script->Run();
	if (result.IsEmpty()) {  
		Handle<Value> exception = trycatch.Exception();
		String::AsciiValue exception_str(exception);
		printf("Exception: %s\n", *exception_str);
		char *errBuf = (char*)zmallocPtr(4096); //TODO: calc size
		memset(errBuf,0,4096);
		sprintf(errBuf,"-Exception error: \"%s\"",*exception_str);
		return;
	}
	zfreePtr(core);
}

void load_user_scripts_from_folder(char *folder){
	DIR *dp;
	struct dirent *dirp;
	unsigned char isFolder =0x4;
	int len = 0;
	if((dp  = opendir(folder)) != NULL) {
		while ((dirp = readdir(dp)) != NULL) {
			//files.push_back(string(dirp->d_name));
			if(strcmp(".", dirp->d_name) && strcmp("..", dirp->d_name)){
				len = strlen (dirp->d_name);
				if(dirp->d_type == isFolder){
					char subfolder[1024] = {0};
					sprintf(subfolder,"%s%s/",folder,dirp->d_name);
					load_user_scripts_from_folder(subfolder);
				}
				else if(strcmp (".js", &(dirp->d_name[len - 3])) == 0){
					char file[1024] = {0};
					sprintf(file,"%s%s",folder,dirp->d_name);
					redisLogRawPtr(REDIS_NOTICE,file);
					load_user_script(file);
				}
			}
		}
		closedir(dp);
	} else {
		redisLogRawPtr(REDIS_NOTICE, (char*)"js-dir from config - not found");
	}
}

struct ThreadJSClientAndCode {
	redisClient *c;
	char *code;
};

void *setTimeoutExec(void *param)
{
	while(1){
		usleep(50000); //50ms
		v8::Locker locker(isolate);
		v8::Isolate::Scope isolateScope(isolate);
		HandleScope handle_scope(isolate);
		v8::Local<v8::Context> v8_context = v8::Local<v8::Context>::New(isolate, persistent_v8_context);
		Locker v8Locker(isolate);
		v8::Context::Scope context_scope(v8_context);
		v8::Handle<v8::String> source = v8::String::New("redis._runtimeouts()");
		v8::TryCatch trycatch;
		v8::Handle<v8::Script> script = v8::Script::Compile(source);
		if(script.IsEmpty()){
			Handle<Value> exception = trycatch.Exception();
			String::AsciiValue exception_str(exception);
			printf("V8 Exception: %s\n", *exception_str);
			char *errBuf = (char*)zmallocPtr(4096); //TODO: calc size
			memset(errBuf,0,4096);
			sprintf(errBuf,"-Compile error: \"%s\"",*exception_str);
			printf("errBuf is '%s'\n",errBuf);
			continue;
		}
		timeoutScriptStart = GetTickCount();
		v8::Handle<v8::Value> result = script->Run();
		timeoutScriptStart = 0;
		if (result.IsEmpty()) {  
			Handle<Value> exception = trycatch.Exception();
			String::AsciiValue exception_str(exception);
			printf("Exception: %s\n", *exception_str);
			char *errBuf = (char*)zmallocPtr(4096); //TODO: calc size
			memset(errBuf,0,4096);
			sprintf(errBuf,"-Exception error: \"%s\"",*exception_str);
			continue;
		}
	}
	return 0;
}

void *single_thread_function_for_slow_run_js(void *param)
{
	bool slow_report = false;
	while(1){
		usleep(100000); //100ms
		unsigned int dt = GetTickCount() - scriptStart;
		if(scriptStart != 0 && last_js_run!=NULL && dt > js_slow){
			if(!slow_report){
				slow_report = true;
				printf("run_js running more than %ims, log function\n",js_slow);
				redisLogRawPtr(REDIS_NOTICE, (char*)"JS slow function:");
				redisLogRawPtr(REDIS_NOTICE, (char*)last_js_run);
			}
		}
		else
			slow_report = false;
		
		if(scriptStart != 0 && last_js_run!=NULL && dt > js_timeout*1000){
			printf("run_js running more than %i sec, kill it\n",js_timeout);
			redisLogRawPtr(REDIS_NOTICE, (char*)"JS to slow function, kill it:");
			redisLogRawPtr(REDIS_NOTICE, (char*)last_js_run);
			v8::V8::TerminateExecution();
			scriptStart = 0;
		}
		
		unsigned int dtt = GetTickCount() - timeoutScriptStart;
		if(timeoutScriptStart != 0 && dtt > js_timeout*1000){
			printf("some of timeout/interval runned for %i sec, kill it\n",js_timeout);
			redisLogRawPtr(REDIS_NOTICE, (char*)"some of timeouts/intervals works to long, kill last one.");
			v8::V8::TerminateExecution();
			run_js((char*)"clearInterval(redis._last_interval_id)");
			timeoutScriptStart = 0;
		}
	}
	return 0;
}

extern "C"
{
	void v8_exec(redisClient *c,char* code){
		//printf("v8_exec %s\n",code);
		scriptStart = GetTickCount();
		last_js_run = code;
		RUN_JS_RETURN * ret = run_js(code,false);
		last_js_run = NULL;
		scriptStart = 0;
		if(ret->json && ret->json[0]=='-'){
			printf("run_js return error %s\n",ret->json);
			addReplyErrorPtr(c,ret->json);
			if(ret->json!=run_js_returnbuf) zfreePtr(ret->json);
			return;
		}
		robj *obj = createStringObjectPtr(ret->json,ret->len);
		addReplyBulkPtr(c,obj);
		decrRefCountPtr(obj);
	}
	
	void v8_exec_async(redisClient *c,char* code){
		//printf("v8_exec %s\n",code);
		scriptStart = GetTickCount();
		last_js_run = code;
		RUN_JS_RETURN * ret = run_js(code,true);
		last_js_run = NULL;
		scriptStart = 0;
		if(ret->json && ret->json[0]=='-'){
			printf("run_js return error %s\n",ret->json);
			addReplyErrorPtr(c,ret->json);
			if(ret->json!=run_js_returnbuf) zfreePtr(ret->json);
			return;
		}
		robj *obj = createStringObjectPtr(ret->json,ret->len);
		addReplyBulkPtr(c,obj);
		decrRefCountPtr(obj);
	}
	
	void v8_exec_call(redisClient *c){
		if(c->argc<2){
			addReplyErrorPtr(c,(char*)"-Wrong number of arguments, must be at least 2");
			return;
		}
		//printf("v8_exec_call args %i\n",c->argc);
		scriptStart = GetTickCount();
		last_js_run = (char*)c->argv[1]->ptr;
		RUN_JS_RETURN * ret = call_js(c);
		last_js_run = NULL;
		scriptStart = 0;
		if(ret->json && ret->json[0]=='-'){
			printf("call_js return error %s\n",ret->json);
			addReplyErrorPtr(c,ret->json);
			if(ret->json!=run_js_returnbuf) zfreePtr(ret->json);
			return;
		}
		robj *obj = createStringObjectPtr(ret->json,ret->len);
		addReplyBulkPtr(c,obj);
		decrRefCountPtr(obj);
	}
	
	void v8_reload(redisClient *c){
		v8::V8::TerminateExecution();
		persistent_v8_context.Dispose();
		pthread_cancel(thread_id_for_single_thread_check);
		initV8();
		redisLogRawPtr(REDIS_NOTICE, (char*)"V8 core loaded");
		load_user_scripts_from_folder(js_dir);
		redisLogRawPtr(REDIS_NOTICE, (char*)"V8 user script loaded");
		addReplyPtr(c,createObjectPtr(REDIS_STRING,sdsnewPtr("+V8 Reload complete\r\n")));
		pthread_create(&thread_id_for_setTimeoutExec, NULL, setTimeoutExec, (void*)NULL);
	}
	
	void v8setup()
	{
		redisLogRawPtr(REDIS_NOTICE, (char*)"Making redisClient\n");
		client = redisCreateClientPtr(-1);
		client->flags |= REDIS_LUA_CLIENT;
		
		initV8();
		
		if(js_dir==NULL){
			js_dir = (char*)zmallocPtr(1024);
			strcpy(js_dir,"./js/");
		}

		redisLogRawPtr(REDIS_NOTICE, (char*)"V8 core loaded");
		load_user_scripts_from_folder(js_dir);
		redisLogRawPtr(REDIS_NOTICE, (char*)"V8 user script loaded");
		
		pthread_create(&thread_id_for_setTimeoutExec, NULL, setTimeoutExec, (void*)NULL);
	}
	
	void passPointerToRedisLogRaw(void (*functionPtr)(int, char*)){
		printf("passPointerToRedisLogRaw\n");
		redisLogRawPtr = functionPtr;
	}
	
	void passPointerToCreateClient(redisClient* (*functionPtr)(int)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerToCreateClient");
		redisCreateClientPtr = functionPtr;
	}
	
	void passPointerTolookupCommandByCString(redisCommand* (*functionPtr)(char*)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTolookupCommand");
		lookupCommandByCStringPtr = functionPtr;
	}
	
	void passPointerTocall(void (*functionPtr)(redisClient*,int)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTocall");
		callPtr = functionPtr;
	}
	
	void passPointerTocreateStringObject(robj* (*functionPtr)(char*,size_t)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTocreateStringObject");
		createStringObjectPtr = functionPtr;
	}
	
	void passPointerTosdsempty(sds (*functionPtr)()){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTosdsempty");
		sdsemptyPtr = functionPtr;
	}
	
	void passPointerTosdscatlen(sds (*functionPtr)(sds, const void *,size_t)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTosdscatlen");
		sdscatlenPtr = functionPtr;
	}
	
	void passPointerTosdslen(size_t (*functionPtr)(const sds)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTosdslen");
		sdslenPtr = functionPtr;
	}
	
	void passPointerTolistDelNode(void (*functionPtr)(list*,listNode*)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTolistDelNode");
		listDelNodePtr = functionPtr;
	}
	
	void passPointerTodecrRefCount(void (*functionPtr)(robj*)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTodecrRefCount");
		decrRefCountPtr = functionPtr;
	}
	
	void passPointerTosdsfree(void (*functionPtr)(sds)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTosdsfree");
		sdsfreePtr = functionPtr;
	}
	
	void passPointerTozmalloc(void* (*functionPtr)(size_t)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTozmalloc");
		zmallocPtr = functionPtr;
	}
	
	void passPointerTozfree(void (*functionPtr)(void*)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTozfree");
		zfreePtr = functionPtr;
	}
	
	void passPointerToredisLog(void (*functionPtr)(int,const char*,...)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerToredisLog");
		redisLogPtr = functionPtr;
	}
	
	void passPointerToaddReply(void (*functionPtr)(redisClient *, robj *)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerToaddReply");
		addReplyPtr = functionPtr;
	}
	
	void passPointerTosdsnew(sds (*functionPtr)(const char*)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTosdsnew");
		sdsnewPtr = functionPtr;
	}
	
	void passPointerTocreateObject(robj* (*functionPtr)(int,void*)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTocreateObject");
		createObjectPtr = functionPtr;
	}
	
	void passPointerToaddReplyBulk(void (*functionPtr)(redisClient*,robj*)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerToaddReplyBulkLen");
		addReplyBulkPtr = functionPtr;
	}
	
	void passPointerToaddReplyError(void (*functionPtr)(redisClient*,char*)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerToaddReplyError");
		addReplyErrorPtr = functionPtr;
	}
	
	void passPointerTolookupKeyRead(robj *(*functionPtr)(redisDb*, robj *)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTolookupKeyRead");
		lookupKeyReadPtr = functionPtr;
	}
	
	void passPointerTosetKey(void (*functionPtr)(redisDb*, robj*, robj*)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTosetKey");
		setKeyPtr = functionPtr;
	}
	
	void passPointerTodbOverwrite(void (*functionPtr)(redisDb *, robj *, robj *)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTodbOverwrite");
		dbOverwritePtr = functionPtr;
	}
	
	void passPointerTodbAdd(void (*functionPtr)(redisDb *, robj *, robj *)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTodbAdd");
		dbAddPtr = functionPtr;
	}
	
	void passPointerTonotifyKeyspaceEvent(void (*functionPtr)(int, char *, robj *, int )){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTonotifyKeyspaceEvent");
		notifyKeyspaceEventPtr = functionPtr;
	}
	
	void passPointerTocheckType(int (*functionPtr)(redisClient *, robj *, int)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTocheckType");
		checkTypePtr = functionPtr;
	}
	
	void passPointerTogetLongLongFromObjectOrReply(int (*functionPtr)(redisClient *, robj *, long long *, const char *)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTogetLongLongFromObjectOrReply");
		getLongLongFromObjectOrReplyPtr = functionPtr;
	}
	
	void passPointerTocreateStringObjectFromLongLong(robj *(*functionPtr)(long long value)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTocreateStringObjectFromLongLong");
		createStringObjectFromLongLongPtr = functionPtr;
	}
	
	void passPointerTosignalModifiedKey(void (*functionPtr)(redisDb *, robj *)){
		redisLogRawPtr(REDIS_DEBUG, (char*)"passPointerTosignalModifiedKey");
		signalModifiedKeyPtr = functionPtr;
	}
	
	void config_js_dir(char *_js_dir){
		printf("config_js_dir %s\n",_js_dir);
		if(js_dir) free(js_dir);
		js_dir = (char*)malloc(1024);
		strcpy(js_dir,_js_dir);
	}
	
	void config_js_flags(char *_js_flags){
		printf("config_js_flags %s\n",_js_flags);
		if(js_flags) free(js_flags);
		js_flags = (char*)malloc(1024);
		strcpy(js_flags,_js_flags);
	}
	
	void config_js_timeout(int timeout){
		printf("config_js_timeout %i\n",timeout);
		js_timeout = timeout;
	}
	
	void config_js_slow(int slow){
		printf("config_js_slow %i\n",slow);
		js_slow = slow;
	}
	
	char *config_get_js_dir(){
		return js_dir;
	}
	
	char *config_get_js_flags(){
		return js_flags;
	}
	
	int config_get_js_timeout(){
		return js_timeout;
	}
	
	int config_get_js_slow(){
		return js_slow;
	}
}
