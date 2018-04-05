/*
 * C11 <threads.h> emulation library
 *
 * (C) Copyright yohhoy 2012.
 * Distributed under the Boost Software License, Version 1.0.
 *
 * Permission is hereby granted, free of charge, to any person or organization
 * obtaining a copy of the software and accompanying documentation covered by
 * this license (the "Software") to use, reproduce, display, distribute,
 * execute, and transmit the Software, and to prepare [[derivative work]]s of the
 * Software, and to permit third-parties to whom the Software is furnished to
 * do so, all subject to the following:
 *
 * The copyright notices in the Software and this entire statement, including
 * the above license grant, this restriction and the following disclaimer,
 * must be included in all copies of the Software, in whole or in part, and
 * all derivative works of the Software, unless such copies or derivative
 * works are solely in the form of machine-executable object code generated by
 * a source language processor.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 * FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <switch/kernel/svc.h>
#include <switch/kernel/mutex.h>
#include <switch/kernel/thread.h>
#include <switch/kernel/condvar.h>
#undef ALIGN
//#undef PACKED
//#ifndef HAVE_FUNC_ATTRIBUTE_PACKED
//#define HAVE_FUNC_ATTRIBUTE_PACKED 0
//#endif
#define SIG_SETMASK 0

#include <stdlib.h>
#ifndef assert
#include <assert.h>
#endif
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <stdint.h> /* for intptr_t */

/*
Configuration macro:

  EMULATED_THREADS_USE_NATIVE_TIMEDLOCK
    Use pthread_mutex_timedlock() for `mtx_timedlock()'
    Otherwise use mtx_trylock() + *busy loop* emulation.
*/
//#define EMULATED_THREADS_USE_NATIVE_TIMEDLOCK

/*
static inline uintptr_t
ALIGN(uintptr_t value, int32_t alignment)
{
   assert((alignment > 0) && _mesa_is_pow_two(alignment));
   return (((value) + (alignment) - 1) & ~((alignment) - 1));
}
*/
/*---------------------------- macros ----------------------------*/
#define ONCE_FLAG_INIT 0
#ifdef INIT_ONCE_STATIC_INIT
#define TSS_DTOR_ITERATIONS PTHREAD_DESTRUCTOR_ITERATIONS
#else
#define TSS_DTOR_ITERATIONS 1  // assume TSS dtor MAY be called at least once.
#endif

// FIXME: temporary non-standard hack to ease transition
//#define _MTX_INITIALIZER_NP PTHREAD_MUTEX_INITIALIZER

/*---------------------------- types ----------------------------*/
/*
typedef pthread_cond_t  cnd_t;
typedef pthread_t       thrd_t;
typedef pthread_key_t   tss_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_once_t  once_flag;
*/

typedef int		thrd_t;
typedef int		tss_t;
typedef int		once_flag;

typedef struct Cond {
	CondVar var;
	Mutex mtx;
} Cond;
typedef Cond cnd_t;

typedef struct Mtx {
	RMutex rmtx;
	int type;
	int init;
} Mtx;
typedef Mtx mtx_t;

typedef struct NXThread {
	Thread thread;
	thrd_t id;
} NXThread;

extern NXThread thread_list[64];

static inline NXThread *find_thread(thrd_t thid) {
	for(int i=1; i<64; i++) {
		if(thread_list[i].id == thid) {
			return &thread_list[i];
		}
	}
	return NULL;
}

static inline NXThread *get_thread() {
	for(int i=1; i<64; i++) {
		if(thread_list[i].id < 1) {
			thread_list[i].id = i;
			return &thread_list[i];
		}
	}
	return NULL;
}

static inline void free_thread(thrd_t thid) {
	for(int i=1; i<64; i++) {
		if(thread_list[i].id == thid) {
			if(thread_list[i].thread.handle) {
				threadClose(&thread_list[i].thread);
				thread_list[i].id = 0;
			}
			break;
		}
	}
}

#define _MTX_INITIALIZER_NP  {}

/*
Implementation limits:
  - Conditionally emulation for "mutex with timeout"
    (see EMULATED_THREADS_USE_NATIVE_TIMEDLOCK macro)
*/
struct impl_thrd_param {
    thrd_start_t func;
    void *arg;
};

static inline void *
impl_thrd_routine(void *p)
{
    struct impl_thrd_param pack = *((struct impl_thrd_param *)p);
    free(p);
    return (void*)(intptr_t)pack.func(pack.arg);
}


/*--------------- 7.25.2 Initialization functions ---------------*/
// 7.25.2.1
static inline void
call_once(once_flag *flag, void (*func)(void))
{
    //pthread_once(flag, func);
}


/*------------- 7.25.3 Condition variable functions -------------*/
// 7.25.3.1
static inline int
cnd_broadcast(cnd_t *cond)
{
    assert(cond != NULL);
    // return (pthread_cond_broadcast(cond) == 0) ? thrd_success : thrd_error;
    return (condvarWakeAll(&cond->var) == 0) ? thrd_success : thrd_error;
}

// 7.25.3.2
static inline void
cnd_destroy(cnd_t *cond)
{
    assert(cond);
    //pthread_cond_destroy(cond);
}

// 7.25.3.3
static inline int
cnd_init(cnd_t *cond)
{
    assert(cond != NULL);
    mutexInit(&cond->mtx);
    condvarInit(&cond->var, &cond->mtx);
    return thrd_success;
   // return (pthread_cond_init(cond, NULL) == 0) ? thrd_success : thrd_error;
}

// 7.25.3.4
static inline int
cnd_signal(cnd_t *cond)
{
    assert(cond != NULL);
    return (condvarWakeOne(&cond->var) == 0) ? thrd_success : thrd_error;
    //return (pthread_cond_signal(cond) == 0) ? thrd_success : thrd_error;
}

// 7.25.3.5
static inline int
cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *abs_time)
{
    int rt;

    assert(mtx != NULL);
    assert(cond != NULL);
    assert(abs_time != NULL);

	u64 t = (u64)abs_time->tv_sec * 1000000LL + (u64)abs_time->tv_nsec / 1000LL;

    rt = condvarWaitTimeout(&cond->var, t); //pthread_cond_timedwait(cond, mtx, &abs_time);
    if (rt == 0xEA01)
        return thrd_busy;
    return (rt == 0) ? thrd_success : thrd_error;
}

// 7.25.3.6
static inline int
cnd_wait(cnd_t *cond, mtx_t *mtx)
{
    assert(mtx != NULL);
    assert(cond != NULL);
    return (condvarWait(&cond->var) == 0) ? thrd_success : thrd_error;
    //return (pthread_cond_wait(cond, mtx) == 0) ? thrd_success : thrd_error;
}


/*-------------------- 7.25.4 Mutex functions --------------------*/
// 7.25.4.1
static inline void
mtx_destroy(mtx_t *mtx)
{
    assert(mtx != NULL);
    //pthread_mutex_destroy(mtx);
}

/*
 * XXX: Workaround when building with -O0 and without pthreads link.
 *
 * In such cases constant folding and dead code elimination won't be
 * available, thus the compiler will always add the pthread_mutexattr*
 * functions into the binary. As we try to link, we'll fail as the
 * symbols are unresolved.
 *
 * Ideally we'll enable the optimisations locally, yet that does not
 * seem to work.
 *
 * So the alternative workaround is to annotate the symbols as weak.
 * Thus the linker will be happy and things don't clash when building
 * with -O1 or greater.
 */
 /*
#if defined(HAVE_FUNC_ATTRIBUTE_WEAK) && !defined(__CYGWIN__)
__attribute__((weak))
int pthread_mutexattr_init(pthread_mutexattr_t *attr);

__attribute__((weak))
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);

__attribute__((weak))
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
#endif
*/

// 7.25.4.2
static inline int
mtx_init(mtx_t *mtx, int type)
{
	assert(mtx != NULL);
	
	//if(mtx->init <= 0) {
	//	mtx_init(mtx, mtx_plain);
	//}
	
	if (type != mtx_plain && type != mtx_timed && type != mtx_try
      && type != (mtx_plain|mtx_recursive)
      && type != (mtx_timed|mtx_recursive)
      && type != (mtx_try|mtx_recursive))
        return thrd_error;
        
    rmutexInit(&mtx->rmtx);
    mtx->type = type;
	return thrd_success;
}

// 7.25.4.3
static inline int
mtx_lock(mtx_t *mtx)
{
    assert(mtx != NULL);
	rmutexLock(&mtx->rmtx);
	return thrd_success;
}

static inline int
mtx_trylock(mtx_t *mtx) {

	assert(mtx != NULL);
	rmutexTryLock(&mtx->rmtx);
	return thrd_success;
}

static inline void
thrd_yield(void);

// 7.25.4.4
static inline int
mtx_timedlock(mtx_t *mtx, const struct timespec *ts)
{
    assert(mtx != NULL);
    assert(ts != NULL);

    time_t expire = time(NULL);
    expire += ts->tv_sec;
    while (mtx_trylock(mtx) != thrd_success) {
        time_t now = time(NULL);
        if (expire < now)
            return thrd_busy;
        // busy loop!
        thrd_yield();
    }
    return thrd_success;
}

// 7.25.4.5
/*
static inline int
mtx_trylock(mtx_t *mtx)
{
    assert(mtx != NULL);
	return mtx_lock(mtx);
    //return (pthread_mutex_trylock(mtx) == 0) ? thrd_success : thrd_busy;
}
*/

// 7.25.4.6
static inline int
mtx_unlock(mtx_t *mtx)
{
    assert(mtx != NULL);
    //if(mtx->type & mtx_recursive) {
		rmutexUnlock(&mtx->rmtx);
	//} else {
	//	mutexUnlock(&mtx->mtx);
	//}
	return thrd_success;
    //return (pthread_mutex_unlock(mtx) == 0) ? thrd_success : thrd_error;
}


/*------------------- 7.25.5 Thread functions -------------------*/
// 7.25.5.1
static inline int
thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
	printf("thrd_create\n");
    struct impl_thrd_param *pack;
    assert(thr != NULL);
    
    NXThread *nx_thread = get_thread();
    if(nx_thread == NULL) {
		return thrd_error;
	}
	*thr = nx_thread->id;
    
    pack = (struct impl_thrd_param *)malloc(sizeof(struct impl_thrd_param));
    if (!pack) return thrd_nomem;
    pack->func = func;
    pack->arg = arg;

    if(threadCreate(&nx_thread->thread, (ThreadFunc) impl_thrd_routine, pack, 0x5000, 0x2C, -2) != 0) {
    //if (pthread_create(thr, NULL, impl_thrd_routine, pack) != 0) {
        free(pack);
        return thrd_error;
    }
    
    if(threadStart(&nx_thread->thread) != 0) {
		free(pack);
        return thrd_error;
	}
    
    return thrd_success;
}

// 7.25.5.2
static inline thrd_t
thrd_current(void)
{
	printf("thrd_current\n");
	return 0;
    //return pthread_self();
}

// 7.25.5.3
static inline int
thrd_detach(thrd_t thr)
{
	printf("thrd_detach\n");
	return thrd_success;
    //return (pthread_detach(thr) == 0) ? thrd_success : thrd_error;
}

// 7.25.5.4
static inline int
thrd_equal(thrd_t thr0, thrd_t thr1)
{
	printf("thrd_equal\n");
	return thr0 == thr1;
    //return pthread_equal(thr0, thr1);
}

// 7.25.5.5
static inline void
thrd_exit(int res)
{
	printf("thrd_exit\n");
	svcExitThread();
    //pthread_exit((void*)(intptr_t)res);
}

// 7.25.5.6
static inline int
thrd_join(thrd_t thr, int *res)
{
	printf("thrd_join\n");
	
	NXThread *nx_thread = find_thread(thr);
    if(nx_thread == NULL) {
		return thrd_error;
	}
	
	threadWaitForExit(&nx_thread->thread);
	*res = 0;
	/*
    void *code;
    if (pthread_join(thr, &code) != 0)
        return thrd_error;
    if (res)
        *res = (int)(intptr_t)code;
    */
    return thrd_success;
}

// 7.25.5.7
static inline void
thrd_sleep(const struct timespec *time_point, struct timespec *remaining)
{
	printf("thrd_sleep\n");
	
	assert(time_point != NULL);
	
    u64 t = (u64)time_point->tv_sec * 1000000LL + (u64)time_point->tv_nsec / 1000LL;
    svcSleepThread(t);
    //nanosleep(&req, NULL);

}

// 7.25.5.8
static inline void
thrd_yield(void)
{
    //sched_yield();
    svcSleepThread(1000 * 1000);
}


/*----------- 7.25.6 Thread-specific storage functions -----------*/
// 7.25.6.1
static inline int
tss_create(tss_t *key, tss_dtor_t dtor)
{
    //assert(key != NULL);
    //return (pthread_key_create(key, dtor) == 0) ? thrd_success : thrd_error;
    return thrd_error;
}

// 7.25.6.2
static inline void
tss_delete(tss_t key)
{
    //pthread_key_delete(key);
}

// 7.25.6.3
static inline void *
tss_get(tss_t key)
{
	return NULL;
    //return pthread_getspecific(key);
}

// 7.25.6.4
static inline int
tss_set(tss_t key, void *val)
{
	return thrd_error;
    //return (pthread_setspecific(key, val) == 0) ? thrd_success : thrd_error;
}


/*-------------------- 7.25.7 Time functions --------------------*/
// 7.25.6.1
static inline int
timespec_get(struct timespec *ts, int base)
{
    if (!ts) return 0;
    if (base == TIME_UTC) {
		ts->tv_sec = time(NULL);
		ts->tv_nsec = 0;
        return base;
    }
    return 0;
}

