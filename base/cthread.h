
/*
 * Copyright (C) lcinx
 * lcinx@163.com
 */

#ifndef _H_CROSS_THREAD_H_
#define _H_CROSS_THREAD_H_

#ifdef __cplusplus
extern "C" {
#endif


struct cthread_;
struct cmutex_;
struct cspin_ {
	volatile long lock;
};

typedef struct cthread_ * cthread;
typedef struct cmutex_ * cmutex;
typedef struct cspin_ cspin;



int cthread_create(cthread *tid, void *udata, void (*thread_func)(cthread *));

void *cthread_get_udata(cthread *tid);

/* 0 is error thread id. */
unsigned int cthread_thread_id(cthread *tid);

void cthread_suspend(cthread *tid);

void cthread_resume(cthread *tid);

void cthread_join(cthread *tid);

void cthread_release(cthread *tid);


unsigned int cthread_self_id();

void cthread_self_sleep(unsigned int millisecond);



int cmutex_init(cmutex *mutex);

void cmutex_lock(cmutex *mutex);

void cmutex_unlock(cmutex *mutex);

int cmutex_trylock(cmutex *mutex);

void cmutex_destroy(cmutex *mutex);



int cspin_init(cspin *lock);

void cspin_lock(cspin *lock);

void cspin_unlock(cspin *lock);

int cspin_trylock(cspin *lock);

void cspin_destroy(cspin *lock);


#ifdef __cplusplus
}
#endif
#endif

