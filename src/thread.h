/*
  +----------------------------------------------------------------------+
  | pthreads                                                             |
  +----------------------------------------------------------------------+
  | Copyright (c) Joe Watkins 2012 - 2015                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Joe Watkins <krakjoe@php.net>                                |
  +----------------------------------------------------------------------+
 */
#ifndef HAVE_PTHREADS_THREAD_H
#define HAVE_PTHREADS_THREAD_H

#ifndef HAVE_PTHREADS_H
#	include <src/pthreads.h>
#endif

#ifndef HAVE_PTHREADS_RESOURCES_H
#	include <src/resources.h>
#endif

#ifndef HAVE_PTHREADS_SOCKET_H
#	include <src/socket.h>
#endif

typedef struct _pthreads_ident_t {
	zend_ulong id;
	void*** ls;
} pthreads_ident_t;

/* {{{ */
typedef struct _pthreads_object_t {
	zend_ulong refcount;
	pthread_t thread;
	unsigned int scope;
	zend_ulong options;
	pthreads_monitor_t *monitor;
	union {
		pthreads_store_t *props;
		pthreads_socket_t *sock;
	} store;
	zval user_exception_handler;
	pthreads_stack_t *stack;
	pthreads_ident_t creator;
	pthreads_ident_t local;
} pthreads_object_t; /* }}} */

/* {{{ */
struct _pthreads_zend_object_t;
typedef struct _pthreads_zend_object_t pthreads_zend_object_t;

struct _pthreads_zend_object_t {
	pthreads_object_t *ts_obj;
	pthreads_ident_t owner;
	pthreads_zend_object_t *original_zobj; //NULL if this is the original object
	zend_long local_props_modcount;
	zend_object std;
}; /* }}} */

/* {{{ */
typedef struct _pthreads_routine_arg_t {
	pthreads_zend_object_t *thread;
	pthreads_monitor_t *ready;
} pthreads_routine_arg_t; /* }}} */

/* {{{ object iterator structure */
typedef struct _pthreads_iterator_t {
	zend_object_iterator zit;
	zval object;
	HashPosition position;
} pthreads_iterator_t; /* }}} */

static inline pthreads_zend_object_t* _pthreads_fetch_object(zend_object *object) {
	return (pthreads_zend_object_t*) ((char*)object - XtOffsetOf(pthreads_zend_object_t, std));
}

/* {{{ fetches a PTHREAD from a specific object in the current context */
#define PTHREADS_FETCH_FROM(object) _pthreads_fetch_object(object) /* }}} */

/* {{{ fetches the current PTHREAD from $this */
#define PTHREADS_FETCH PTHREADS_FETCH_FROM(Z_OBJ(EX(This))) /* }}} */

/* {{{ fetches the internal thread-safe object from the object */
#define PTHREADS_FETCH_TS_FROM(object) PTHREADS_FETCH_FROM(object)->ts_obj /* }}} */

/* {{{ fetches the internal thread-safe object from $this */
#define PTHREADS_FETCH_TS PTHREADS_FETCH_TS_FROM(Z_OBJ(EX(This))) /* }}} */

/* {{{ option constants */
#define PTHREADS_INHERIT_NONE      0x00000000
#define PTHREADS_INHERIT_INI       0x00000001
#define PTHREADS_INHERIT_CONSTANTS 0x00000010
#define PTHREADS_INHERIT_FUNCTIONS 0x00000100
#define PTHREADS_INHERIT_CLASSES   0x00001000
#define PTHREADS_INHERIT_INCLUDES  0x00010000
#define PTHREADS_INHERIT_COMMENTS  0x00100000
#define PTHREADS_INHERIT_ALL       0x00111111
#define PTHREADS_ALLOW_HEADERS	   0x10000000 /* }}} */

/* {{{ scope constants */
#define PTHREADS_SCOPE_UNKNOWN     (0)
#define PTHREADS_SCOPE_THREADED    (1<<1)
#define PTHREADS_SCOPE_THREAD      (1<<2)
#define PTHREADS_SCOPE_WORKER      (1<<3)
#define PTHREADS_SCOPE_SOCKET	   (1<<4) /* }}} */

/* {{{ scope macros */
#define PTHREADS_IS_KNOWN_ENTRY(t)      ((t)->ts_obj->scope)
#define PTHREADS_IS_SOCKET(t)       	((t)->ts_obj->scope & PTHREADS_SCOPE_SOCKET)
#define PTHREADS_IS_THREAD(t)           ((t)->ts_obj->scope & PTHREADS_SCOPE_THREAD)
#define PTHREADS_IS_WORKER(t)           ((t)->ts_obj->scope & PTHREADS_SCOPE_WORKER)
#define PTHREADS_IS_THREADED(t)         ((t)->ts_obj->scope & PTHREADS_SCOPE_THREADED) /* }}} */

/* {{{ pthread_self wrapper */
static inline zend_ulong pthreads_self() {
#ifdef _WIN32
	return (zend_ulong) GetCurrentThreadId();
#else
	return (zend_ulong) pthread_self();
#endif
} /* }}} */

/* {{{ tell if the calling thread created referenced PTHREAD */
#define PTHREADS_IN_CREATOR(t)	((t)->ts_obj->creator.ls == TSRMLS_CACHE) /* }}} */

/* {{{ tell if the calling thread owns this pthreads zend object */
#define PTHREADS_THREAD_OWNS(t) ((t)->owner.ls == TSRMLS_CACHE) /* }}} */

/* {{{ tell if the referenced thread is the threading context */
#define PTHREADS_IN_THREAD(t)	((t)->ts_obj->local.ls == TSRMLS_CACHE) /* }}} */

#endif /* HAVE_PTHREADS_THREAD_H */

