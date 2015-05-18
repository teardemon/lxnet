
/*
 * Copyright (C) lcinx
 * lcinx@163.com
 */

#if (!defined(__linux__) && !defined(WIN32))
#include "catomic.h"
#include "cthread.h"
#include "crosslib.h"
#include "net_eventmgr.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include "socket_internal.h"
#include "_netsocket.h"
#include "log.h"
#include "cthread_pool.h"

#ifdef _DEBUG_NETWORK
#define debuglog debug_log
#else
#define debuglog(...) ((void) 0)
#endif

/* max events from kevent function. */
#define THREAD_EVENT_SIZE (4096)
struct kqueuemgr {
	int threadnum;
	struct cthread_pool *threadpool;				/* thread pool. */
	volatile char need_exit;						/* exit flag. */

	volatile long event_num;						/* current event number. */
	int kqueue_fd;									/* kqueue handle. */
	struct kevent ev_array[THREAD_EVENT_SIZE];	/* event array. */
};

static struct kqueuemgr *s_mgr = NULL;

/* add socket to event manager. */
void socket_addto_eventmgr(struct socketer *self) {
	bool res = true;
	struct kevent ev;
	EV_SET(&ev, self->sockfd, EVFILT_READ, EV_ADD, 0, 0, self);
	if (kevent(s_mgr->kqueue_fd, &ev, 1, NULL, 0, NULL) == -1) {
		res = false;
	}

	EV_SET(&ev, self->sockfd, EVFILT_WRITE, EV_ADD, 0, 0, self);
	if (kevent(s_mgr->kqueue_fd, &ev, 1, NULL, 0, NULL) == -1) {
		res = false;
	}

	EV_SET(&ev, self->sockfd, EVFILT_READ, EV_DISABLE, 0, 0, self);
	if (kevent(s_mgr->kqueue_fd, &ev, 1, NULL, 0, NULL) == -1) {
		res = false;
	}

	EV_SET(&ev, self->sockfd, EVFILT_WRITE, EV_DISABLE, 0, 0, self);
	if (kevent(s_mgr->kqueue_fd, &ev, 1, NULL, 0, NULL) == -1) {
		res = false;
	}

	if (!res) {
		socketer_close(self);
	}

	debuglog("add read、write event to eventmgr.");
}

/* remove socket from event manager. */
void socket_removefrom_eventmgr(struct socketer *self) {
	struct kevent ev;
	EV_SET(&ev, self->sockfd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	kevent(s_mgr->kqueue_fd, &ev, 1, NULL, 0, NULL);
	EV_SET(&ev, self->sockfd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
	kevent(s_mgr->kqueue_fd, &ev, 1, NULL, 0, NULL);
	
	debuglog("remove all event from eventmgr.");
}

/* set recv event. */
void socket_setup_recvevent(struct socketer *self) {
	struct kevent ev;

	assert(self->recvlock == 1);

	if (self->recvlock != 1) {
		log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d", self, (int)self->recvlock, (int)self->sendlock, self->sockfd, (int)self->ref, cthread_self_id());
	}

	
	EV_SET(&ev, self->sockfd, EVFILT_READ, EV_ENABLE, 0, 0, self);
	if (kevent(s_mgr->kqueue_fd, &ev, 1, NULL, 0, NULL) == -1) {
		/*log_error("kqueue, setup recv event to kqueue set on fd %d error!, errno:%d", self->sockfd, NET_GetLastError());*/
		socketer_close(self);
		if (catomic_dec(&self->ref) < 1) {
			log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d, connect:%d, deleted:%d", self, (int)self->recvlock, (int)self->sendlock, self->sockfd, (int)self->ref, cthread_self_id(), self->connected, self->deleted);
		}
	}
	debuglog("setup recv event to eventmgr.");
}

/* remove recv event. */
void socket_remove_recvevent(struct socketer *self) {
	struct kevent ev;

	assert(self->recvlock == 1);

	if (self->recvlock != 1) {
		log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d", self, (int)self->recvlock, (int)self->sendlock, self->sockfd, (int)self->ref, cthread_self_id());
	}

	EV_SET(&ev, self->sockfd, EVFILT_READ, EV_DISABLE, 0, 0, self);
	if (kevent(s_mgr->kqueue_fd, &ev, 1, NULL, 0, NULL) == -1) {
		/*log_error("kqueue, remove recv event from kqueue set on fd %d error!, errno:%d", self->sockfd, NET_GetLastError());*/
		socketer_close(self);
	}
	debuglog("remove recv event from eventmgr.");
}

/* set send event. */
void socket_setup_sendevent(struct socketer *self) {
	struct kevent ev;

	assert(self->sendlock == 1);
	if (self->sendlock != 1) {
		log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d", self, (int)self->recvlock, (int)self->sendlock, self->sockfd, (int)self->ref, cthread_self_id());
	}

	EV_SET(&ev, self->sockfd, EVFILT_WRITE, EV_ENABLE, 0, 0, self);
	if (kevent(s_mgr->kqueue_fd, &ev, 1, NULL, 0, NULL) == -1) {
		/*log_error("kqueue, setup send event to kqueue set on fd %d error!, errno:%d", self->sockfd, NET_GetLastError());*/
		socketer_close(self);
		if (catomic_dec(&self->ref) < 1) {
			log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d, connect:%d, deleted:%d", self, (int)self->recvlock, (int)self->sendlock, self->sockfd, (int)self->ref, cthread_self_id(), self->connected, self->deleted);
		}
	}
	debuglog("setup send event to eventmgr.");
}

/* remove send event. */
void socket_remove_sendevent(struct socketer *self) {
	struct kevent ev;

	assert(self->sendlock == 1);
	if (self->sendlock != 1) {
		log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d", self, (int)self->recvlock, (int)self->sendlock, self->sockfd, (int)self->ref, cthread_self_id());
	}

	EV_SET(&ev, self->sockfd, EVFILT_WRITE, EV_DISABLE, 0, 0, self);
	if (kevent(s_mgr->kqueue_fd, &ev, 1, NULL, 0, NULL) == -1) {
		/*log_error("kqueue, remove send event from kqueue set on fd %d error!, errno:%d", self->sockfd, NET_GetLastError());*/
		socketer_close(self);
	}
	debuglog("remove send event from eventmgr.");
}

static struct kevent *pop_event(struct kqueuemgr *self) {
	int index = catomic_dec(&self->event_num);
	if (index < 0)
		return NULL;
	else
		return &self->ev_array[index];
}

/* execute the task callback function. */
static int task_func(void *argv) {
	struct kqueuemgr *mgr = (struct kqueuemgr *)argv;
	struct kevent *ev;
	struct socketer *sock;
	for (;;) {
		if (mgr->need_exit)
			return -1;
		ev = pop_event(mgr);
		if (!ev)
			return 0;
		assert(ev->udata != NULL);
		sock = (struct socketer *)ev->udata;

		/* error event. */
		if (ev->flags & EV_ERROR) {
			socketer_close(sock);
			continue;
		}

		/* can read event. */
		if (ev->filter == EVFILT_READ) {
			if (catomic_compare_set(&sock->recvlock, 0, 1)) {
				catomic_inc(&sock->ref);
			}
			socketer_on_recv(sock, 0);
		}

		/* can write event. */
		if (ev->filter == EVFILT_WRITE) {
			if (catomic_compare_set(&sock->sendlock, 0, 1)) {
				catomic_inc(&sock->ref);
			}
			socketer_on_send(sock, 0);
		}
	}
}

#define EVERY_THREAD_PROCESS_EVENT_NUM 8
/* get need resume thread number. */
static int leader_func(void *argv) {
	struct kqueuemgr *mgr = (struct kqueuemgr *)argv;

	/* wait event. */
	if (mgr->need_exit) {
		return -1;
	} else {
		struct timespec timeout;
		timeout.tv_sec = 0;
		timeout.tv_nsec = 50 * 1000000;
		int num = kevent(mgr->kqueue_fd, NULL, 0, mgr->ev_array, THREAD_EVENT_SIZE, &timeout);
		if (num > 0) {
			catomic_set(&mgr->event_num, num);
			num = (num + (int)(EVERY_THREAD_PROCESS_EVENT_NUM) - 1) / (int)(EVERY_THREAD_PROCESS_EVENT_NUM);
		} else if (num < 0) {
			if (num == -1 && NET_GetLastError() == EINTR)
				return 0;
			log_error("kevent return value < 0, error, return value:%d, errno:%d", num, NET_GetLastError());
		}
		return num;
	}
}


/* 
 * initialize event manager. 
 * socketnum --- socket total number. must greater than 1.
 * threadnum --- thread number, if less than 0, then start by the number of cpu threads 
 * */
bool eventmgr_init(int socketnum, int threadnum) {
	if (s_mgr)
		return false;
	if (socketnum < 1)
		return false;
	if (threadnum <= 0)
		threadnum = get_cpu_num();
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		return false;
	s_mgr = (struct kqueuemgr *)malloc(sizeof(struct kqueuemgr));
	if (!s_mgr)
		return false;

	/* initialize. */
	s_mgr->event_num = 0;
	s_mgr->kqueue_fd = kqueue();
	if (s_mgr->kqueue_fd == -1) {
		free(s_mgr);
		s_mgr = NULL;
		return false;
	}

	s_mgr->threadnum = threadnum;
	s_mgr->need_exit = false;

	/*  first building kqueue module, and then create thread pool. */
	s_mgr->threadpool = cthread_pool_create(threadnum, s_mgr, leader_func, task_func);
	if (!s_mgr->threadpool) {
		close(s_mgr->kqueue_fd);
		free(s_mgr);
		s_mgr = NULL;
		return false;
	}
	return true;
}

/* 
 * release event manager.
 * */
void eventmgr_release() {
	if (!s_mgr)
		return;

	/* set exit flag. */
	s_mgr->need_exit = true;

	/* release thread pool. */
	cthread_pool_release(s_mgr->threadpool);

	/* close kqueue some. */
	close(s_mgr->kqueue_fd);
	free(s_mgr);
	s_mgr = NULL;
}

#endif


