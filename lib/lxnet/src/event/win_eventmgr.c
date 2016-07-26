
/*
 * Copyright (C) lcinx
 * lcinx@163.com
 */

#include <assert.h>
#include <stdlib.h>
#include <process.h>
#include "socket_internal.h"
#include "_netsocket.h"
#include "cthread.h"
#include "crosslib.h"
#include "log.h"

#ifdef _DEBUG_NETWORK
#define debuglog debug_print_call
#else
#define debuglog(...) ((void) 0)
#endif

/* event type. */
enum e_socket_ioevent {
	e_socket_io_event_read_complete = 0,	/* read operate. */
	e_socket_io_event_write_end,			/* write operate. */
	e_socket_io_thread_shutdown,			/* stop iocp. */
};

struct iocpmgr {
	bool is_init;
	bool is_run;
	int thread_num;
	HANDLE completeport;
};

static struct iocpmgr s_iocp = {false};

/* add socket to event manager. */
void eventmgr_add_socket(struct socketer *self) {
	/* socket object point into iocp. */
	CreateIoCompletionPort((HANDLE)self->sockfd, s_iocp.completeport, (ULONG_PTR)self, 0);
}

/* remove socket from event manager. */
void eventmgr_remove_socket(struct socketer *self) {

}

/* set recv event. */
void eventmgr_setup_socket_recv_event(struct socketer *self) {
	self->recv_event.m_event = e_socket_io_event_read_complete;
	if (!PostQueuedCompletionStatus(s_iocp.completeport, 0, (ULONG_PTR)self, &self->recv_event.m_overlap)) {
		socketer_close(self);

		if (catomic_dec(&self->ref) < 1) {
			log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d, connect:%d, deleted:%d", 
					self, (int)catomic_read(&self->recvlock), (int)catomic_read(&self->sendlock), self->sockfd, 
					(int)catomic_read(&self->ref), cthread_self_id(), self->connected, self->deleted);
		}
	}
}

/* set recv data. */
void eventmgr_setup_socket_recv_data_event(struct socketer *self, char *data, int len) {
	DWORD flags = 0;
	DWORD w_length = len;
	WSABUF buf;
	buf.len = len;
	buf.buf = data;

	assert(self != NULL);
	assert(catomic_read(&self->recvlock) == 1);

	if (catomic_read(&self->recvlock) != 1) {
		log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d, connect:%d, deleted:%d", 
				self, (int)catomic_read(&self->recvlock), (int)catomic_read(&self->sendlock), self->sockfd, 
				(int)catomic_read(&self->ref), cthread_self_id(), self->connected, self->deleted);
	}

	memset(&self->recv_event, 0, sizeof(self->recv_event));
	self->recv_event.m_event = e_socket_io_event_read_complete;
	if (WSARecv(self->sockfd, &buf, 1, &w_length, &flags, &self->recv_event.m_overlap, 0) == SOCKET_ERROR) {
		/* overlapped operation failed to start. */
		if (WSAGetLastError() != WSA_IO_PENDING) {
			debuglog("eventmgr_setup_socket_recv_data_event error!, error:%d\n", WSAGetLastError());
			socketer_close(self);

			if (catomic_dec(&self->ref) < 1) {
				log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d, connect:%d, deleted:%d", 
						self, (int)catomic_read(&self->recvlock), (int)catomic_read(&self->sendlock), self->sockfd, 
						(int)catomic_read(&self->ref), cthread_self_id(), self->connected, self->deleted);
			}
		}
	}
}

/* set send event. */
void eventmgr_setup_socket_send_event(struct socketer *self) {
	self->send_event.m_event = e_socket_io_event_write_end;
	if (!PostQueuedCompletionStatus(s_iocp.completeport, 0, (ULONG_PTR)self, &self->send_event.m_overlap)) {
		socketer_close(self);

		if (catomic_dec(&self->ref) < 1) {
			log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d, connect:%d, deleted:%d", 
					self, (int)catomic_read(&self->recvlock), (int)catomic_read(&self->sendlock), self->sockfd, 
					(int)catomic_read(&self->ref), cthread_self_id(), self->connected, self->deleted);
		}
	}
}

/* set send data. */
void eventmgr_setup_socket_send_data_event(struct socketer *self, char *data, int len) {
	DWORD flags = 0;
	DWORD w_length = len;
	WSABUF buf;
	buf.len = len;
	buf.buf = data;

	assert(self != NULL);
	assert(catomic_read(&self->sendlock) == 1);

	if (catomic_read(&self->sendlock) != 1) {
		log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d, connect:%d, deleted:%d", 
				self, (int)catomic_read(&self->recvlock), (int)catomic_read(&self->sendlock), self->sockfd, 
				(int)catomic_read(&self->ref), cthread_self_id(), self->connected, self->deleted);
	}

	memset(&self->send_event, 0, sizeof(self->send_event));
	self->send_event.m_event = e_socket_io_event_write_end;
	if (WSASend(self->sockfd, &buf, 1, &w_length, flags, &self->send_event.m_overlap, 0) == SOCKET_ERROR) {
		/* overlapped operation failed to start. */
		if (WSAGetLastError() != WSA_IO_PENDING) {
			debuglog("eventmgr_setup_socket_send_data_event error!, error:%d\n", WSAGetLastError());
			socketer_close(self);

			if (catomic_dec(&self->ref) < 1) {
				log_error("%x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, thread_id:%d, connect:%d, deleted:%d", 
						self, (int)catomic_read(&self->recvlock), (int)catomic_read(&self->sendlock), self->sockfd, 
						(int)catomic_read(&self->ref), cthread_self_id(), self->connected, self->deleted);
			}
		}
	}
}

/* iocp work thread function. */
static void _iocp_thread_run(void *data) {
	struct iocpmgr *mgr = (struct iocpmgr *)data;
	HANDLE cp = mgr->completeport;	/* complete port handle. */
	DWORD len = 0;					/* len variable is real transfers byte size. */
	ULONG_PTR s = (ULONG_PTR)NULL;	/* call = CreateIoCompletionPort((HANDLE), self->sockfd, s_iocp.completeport, (ULONG_PTR)self, 0); transfers self pointer. */
	struct overlappedstruct *ov = NULL;
	LPOVERLAPPED ol_ptr = NULL;		/* ol_ptr variable is io handle the overlap result, this is actually a very important parameter, because it is used for each I/O data operation .*/
	BOOL res;

	/*
	 * 10000 --- wait time. ms.
	 * INFINITE --- wait forever. 
	 */
	while (mgr->is_run) {
		ol_ptr = NULL;
		s = 0;
		ov = NULL;
		len = 0;
		res = GetQueuedCompletionStatus(cp, &len, &s, &ol_ptr, INFINITE /* 10000 */);
		debuglog("res:%d, ol_ptr:%x, s:%x\n", res, ol_ptr, s);
		if ((ol_ptr) && (s)) {
			struct socketer *sser = (struct socketer *)s;
			ov = CONTAINING_RECORD(ol_ptr, struct overlappedstruct, m_overlap);
			switch (ov->m_event) {

			/* recv. */
			case e_socket_io_event_read_complete: {
					if (catomic_read(&sser->recvlock) != 1) {
						log_error("res:%d, %x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, len:%d, thread_id:%d, error:%d, connect:%d, deleted:%d", 
								res, sser, (int)catomic_read(&sser->recvlock), (int)catomic_read(&sser->sendlock), sser->sockfd, 
								(int)catomic_read(&sser->ref), (int)len, cthread_self_id(), WSAGetLastError(), sser->connected, sser->deleted);
					}


					debuglog("read handle complete! line:%d thread_id:%d\n", __LINE__, cthread_self_id());
					socketer_on_recv(sser, (int)len);
				}
				break;

			/* send. */
			case e_socket_io_event_write_end: {
					if (catomic_read(&sser->sendlock) != 1) {
						log_error("res:%d, %x socket recvlock:%d, sendlock:%d, fd:%d, ref:%d, len:%d, thread_id:%d, error:%d, connect:%d, deleted:%d", 
								res, sser, (int)catomic_read(&sser->recvlock), (int)catomic_read(&sser->sendlock), sser->sockfd, 
								(int)catomic_read(&sser->ref), (int)len, cthread_self_id(), WSAGetLastError(), sser->connected, sser->deleted);
					}

					debuglog("send handle complete! line:%d thread_id:%d\n", __LINE__, cthread_self_id());
					socketer_on_send(sser, (int)len);
				}
				break;
			case e_socket_io_thread_shutdown: {
					Sleep(100);
					free((void *)s);
					free(ov);
					return;
				}
				break;
			default: {
					log_error("unknow type!.");
				}
			}
		}
	}
}

/*
 * initialize event manager. 
 * socketer_num --- socket total number. must greater than 1.
 * thread_num --- thread number, if less than 0, then start by the number of cpu threads 
 */
bool eventmgr_init(int socketer_num, int thread_num) {
	if (s_iocp.is_init)
		return false;

	if (socketer_num < 1)
		return false;

	if (thread_num <= 0) {
		thread_num = get_cpu_num();
	}

	s_iocp.thread_num = thread_num;

	/* create complete port, fourthly parameter is 0. */
	s_iocp.completeport = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)0, 0);
	if (!s_iocp.completeport)
		return false;

	s_iocp.is_init = true;
	s_iocp.is_run = true;

	/* create iocp work thread. */
	for (; thread_num > 0; --thread_num) {
		_beginthread(_iocp_thread_run, 0, &s_iocp);
	}

	/* initialize windows socket dll*/
	{
		WSADATA ws;
		WSAStartup(MAKEWORD(2,2), &ws);
	}
	return true;
}

/*
 * release event manager.
 */
void eventmgr_release() {
	int i;
	if (!s_iocp.is_init)
		return;

	for (i = 0; i < s_iocp.thread_num; ++i) {
		struct overlappedstruct *cs;
		struct socketer *sock;
		cs = (struct overlappedstruct *)malloc(sizeof(struct overlappedstruct));
		sock = (struct socketer *)malloc(sizeof(struct socketer));
		cs->m_event = e_socket_io_thread_shutdown;

		/* let iocp work thread exit. */
		PostQueuedCompletionStatus(s_iocp.completeport, 0, (ULONG_PTR)sock, &cs->m_overlap);
	}

	s_iocp.is_run = false;
	s_iocp.is_init = false;

	Sleep(500);
	CloseHandle(s_iocp.completeport);
	WSACleanup();
}

