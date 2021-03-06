/*
	Copyright (C) 2009-2011 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "types.h"
#include "task.h"
#include <stdio.h>

#ifdef _WINDOWS
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#ifdef _MSC_VER
class Task::Impl {
public:
	Impl();
	~Impl();

	bool spinlock;

	void start(bool spinlock);
	void shutdown();

	//execute some work
	void execute(const TWork &work, void* param);

	//wait for the work to complete
	void* finish();

	static DWORD __stdcall s_taskProc(void *ptr);
	void taskProc();
	void init();

	//the work function that shall be executed
	TWork work;
	void* param;

	HANDLE incomingWork, workDone, hThread;
	volatile bool bIncomingWork, bWorkDone, bKill;
	bool bStarted;
};

static void* killTask(void* task)
{
	((Task::Impl*)task)->bKill = true;
	return 0;
}

Task::Impl::~Impl()
{
	shutdown();
}

Task::Impl::Impl()
	: work(NULL)
	, bIncomingWork(false)
	, bWorkDone(true)
	, bKill(false)
	, bStarted(false)
	, incomingWork(INVALID_HANDLE_VALUE)
	, workDone(INVALID_HANDLE_VALUE)
	, hThread(INVALID_HANDLE_VALUE)
{
}

DWORD __stdcall Task::Impl::s_taskProc(void *ptr)
{
	//just past the buck to the instance method
	((Task::Impl*)ptr)->taskProc();
	return 0;
}

void Task::Impl::taskProc()
{
	for(;;) {
		if(bKill) break;
		
		//wait for a chunk of work
		if(spinlock) 
		{
			while(!bIncomingWork) Sleep(0); 
		}
		else 
			WaitForSingleObject(incomingWork,INFINITE); 
		
		bIncomingWork = false; 
		//execute the work
		ResetEvent(workDone);
		param = work(param);
		ResetEvent(incomingWork);
		//signal completion
		if(!spinlock) SetEvent(workDone); 
		bWorkDone = true;
	}
}

void Task::Impl::start(bool spinlock)
{
	bIncomingWork = false;
	bWorkDone = true;
	bKill = false;
	bStarted = true;
	this->spinlock = spinlock;
	incomingWork = CreateEvent(NULL,FALSE,FALSE,NULL);
	workDone = CreateEvent(NULL,FALSE,FALSE,NULL);
	hThread = CreateThread(NULL,0,Task::Impl::s_taskProc,(void*)this, 0, NULL);
}
void Task::Impl::shutdown()
{
	if(!bStarted) return;
	bStarted = false;

	execute(killTask,this);
	finish();

	CloseHandle(incomingWork);
	CloseHandle(workDone);
	CloseHandle(hThread);

	incomingWork = INVALID_HANDLE_VALUE;
	workDone = INVALID_HANDLE_VALUE;
	hThread = INVALID_HANDLE_VALUE;
}

void Task::Impl::execute(const TWork &work, void* param) 
{
	//setup the work
	this->work = work;
	this->param = param;
	bWorkDone = false;
	//signal it to start
	if(!spinlock) SetEvent(incomingWork); 
	bIncomingWork = true;
}

void* Task::Impl::finish()
{
	//just wait for the work to be done
	if(spinlock) 
		while(!bWorkDone) 
			Sleep(0);
	else WaitForSingleObject(workDone,INFINITE); 
	return param;
}

#else

class Task::Impl {
private:
	pthread_t _thread;
	bool _isThreadRunning;

public:
	Impl();
	~Impl();

	void start(bool spinlock);
	void execute(const TWork &work, void *param);
	void* finish();
	void shutdown();

	pthread_mutex_t mutex;
	pthread_cond_t condWork;
	TWork work;
	void *param;
	void *ret;
	bool exitThread;
	
	volatile bool spinlock, bIncomingWork, bWorkDone;
};

static void* taskProc(void *arg)
{
	Task::Impl *ctx = (Task::Impl *)arg;
	//if(ctx->spinlock)
	do {
		
		//wait for a chunk of work
		if(ctx->spinlock) 
		{
			while(!ctx->bIncomingWork) usleep(0);
			ctx->bIncomingWork = false;
			if (ctx->work != NULL) {
				ctx->ret = ctx->work(ctx->param);
				ctx->bWorkDone = true;
			} else {
				ctx->ret = NULL;
			}
			
			ctx->work = NULL;
		}
		else
		{
			pthread_mutex_lock(&ctx->mutex);

			while (ctx->work == NULL && !ctx->exitThread) {
				pthread_cond_wait(&ctx->condWork, &ctx->mutex);
			}

			if (ctx->work != NULL) {
				ctx->ret = ctx->work(ctx->param);
			} else {
				ctx->ret = NULL;
			}

			ctx->work = NULL;
			pthread_cond_signal(&ctx->condWork);

			pthread_mutex_unlock(&ctx->mutex);
		}

	} while(!ctx->exitThread);

	return NULL;
}

Task::Impl::Impl()
{
	_isThreadRunning = false;
	work = NULL;
	param = NULL;
	ret = NULL;
	exitThread = false;

	pthread_mutex_init(&mutex, NULL);
	pthread_cond_init(&condWork, NULL);
}

Task::Impl::~Impl()
{
	shutdown();
	pthread_mutex_destroy(&mutex);
	pthread_cond_destroy(&condWork);
}

void Task::Impl::start(bool spinlock)
{
	pthread_mutex_lock(&this->mutex);
	
	if (this->_isThreadRunning) {
		pthread_mutex_unlock(&this->mutex);
		return;
	}

	this->work = NULL;
	this->param = NULL;
	this->ret = NULL;
	this->exitThread = false;
	this->spinlock = spinlock;
	pthread_create(&this->_thread, NULL, &taskProc, this);
	this->_isThreadRunning = true;

	pthread_mutex_unlock(&this->mutex);
}

void Task::Impl::execute(const TWork &work, void *param)
{
	if(!spinlock) {
		pthread_mutex_lock(&this->mutex);

		if (work == NULL || !this->_isThreadRunning) {
			pthread_mutex_unlock(&this->mutex);
			return;
		}
		this->work = work;
		this->param = param;
		this->bIncomingWork = true;
		pthread_cond_signal(&this->condWork);

		pthread_mutex_unlock(&this->mutex);
	}
	else {
		this->work = work;
		this->param = param;
		this->bWorkDone = false;
		this->bIncomingWork = true;
	}
}

void* Task::Impl::finish()
{
	void *returnValue = NULL;

	if(!spinlock) {
		pthread_mutex_lock(&this->mutex);

		if (!this->_isThreadRunning) {
			pthread_mutex_unlock(&this->mutex);
			return returnValue;
		}

		while (this->work != NULL) {
			pthread_cond_wait(&this->condWork, &this->mutex);
		}

		returnValue = this->ret;

		pthread_mutex_unlock(&this->mutex);
	}
	else {
		while(!bWorkDone)
			sched_yield();
		returnValue = this->ret;
	}

	return returnValue;
}

void Task::Impl::shutdown()
{
	pthread_mutex_lock(&this->mutex);

	if (!this->_isThreadRunning) {
		pthread_mutex_unlock(&this->mutex);
		return;
	}

	this->work = NULL;
	this->exitThread = true;
	pthread_cond_signal(&this->condWork);

	pthread_mutex_unlock(&this->mutex);

	pthread_join(this->_thread, NULL);

	pthread_mutex_lock(&this->mutex);
	this->_isThreadRunning = false;
	pthread_mutex_unlock(&this->mutex);
}
#endif

void Task::start(bool spinlock) { impl->start(spinlock); }
void Task::shutdown() { impl->shutdown(); }
Task::Task() : impl(new Task::Impl()) {}
Task::~Task() { delete impl; }
void Task::execute(const TWork &work, void* param) { impl->execute(work,param); }
void* Task::finish() { return impl->finish(); }


