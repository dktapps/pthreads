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
#ifndef HAVE_PTHREADS_STORE
#define HAVE_PTHREADS_STORE

#ifndef HAVE_PTHREADS_H
#	include <src/pthreads.h>
#endif

#ifndef HAVE_PTHREADS_OBJECT_H
#	include <src/object.h>
#endif

#ifndef HAVE_PTHREADS_RESOURCES_H
#	include <src/resources.h>
#endif

#ifndef HAVE_PTHREADS_COPY_H
#	include <src/copy.h>
#endif

#ifndef HAVE_PTHREADS_STORE_H
#	include <src/store.h>
#endif

#include <src/compat.h>

#define PTHREADS_STORAGE_EMPTY {0, 0, 0, 0, NULL}


/* {{{ */
static int pthreads_store_tostring(zval *pzval, char **pstring, size_t *slength);
static int pthreads_store_tozval(zval *pzval, char *pstring, size_t slength);
/* }}} */

/* {{{ */
pthreads_store_t* pthreads_store_alloc() {
	pthreads_store_t *store = (pthreads_store_t*) calloc(1, sizeof(pthreads_store_t));

	if (store) {
		store->modcount = 0;
		zend_hash_init(
			&store->hash, 8, NULL,
			(dtor_func_t) pthreads_store_storage_dtor, 1);
	}

	return store;
} /* }}} */

void pthreads_store_sync_local_properties(pthreads_zend_object_t *threaded) { /* {{{ */
	pthreads_object_t *ts_obj = threaded->ts_obj;
	zend_ulong idx;
	zend_string *name;
	zval *val;
	pthreads_storage *ts_val;
	zend_bool remove;

	//TODO: not sure if this is necessary if no modifications
	rebuild_object_properties(&threaded->std);


	if (threaded->local_props_modcount == ts_obj->store.props->modcount) {
		return;
	}

	ZEND_HASH_FOREACH_KEY_VAL(threaded->std.properties, idx, name, val) {
		if (!name) {
			ts_val = TRY_PTHREADS_STORAGE_PTR_P(zend_hash_index_find(&ts_obj->store.props->hash, idx));
		} else {
			ts_val = TRY_PTHREADS_STORAGE_PTR_P(zend_hash_find(&ts_obj->store.props->hash, name));
		}

		remove = 1;
		if (ts_val && IS_PTHREADS_OBJECT(val)) {
			pthreads_object_t* threadedStorage = ((pthreads_zend_object_t *) ts_val->data)->ts_obj;
			pthreads_object_t *threadedProperty = PTHREADS_FETCH_TS_FROM(Z_OBJ_P(val));

			if (threadedStorage->monitor == threadedProperty->monitor) {
				remove = 0;
			}
		}

		if (remove) {
			if (!name) {
				zend_hash_index_del(threaded->std.properties, idx);
			} else {
				zend_hash_del(threaded->std.properties, name);
			}
		}
	} ZEND_HASH_FOREACH_END();
	threaded->local_props_modcount = ts_obj->store.props->modcount;
} /* }}} */

/* {{{ */
static inline void _pthreads_store_bump_modcount_nolock(pthreads_zend_object_t *threaded) {
	if (threaded->local_props_modcount == threaded->ts_obj->store.props->modcount) {
		/* It's possible we may have caused a modification via a connection whose property table was not in sync. This is OK
		 * for writes, because these cases usually cause destruction of outdated caches anyway, but we have to avoid
		 * incorrectly marking the local table as in-sync.
		 */
		threaded->local_props_modcount++;
	}
	threaded->ts_obj->store.props->modcount++;
} /* }}} */

static inline zend_bool pthreads_store_coerce(zval *key, zval *member) {
	zend_ulong hval;

	switch (Z_TYPE_P(key)) {
		case IS_STRING:
			if (ZEND_HANDLE_NUMERIC(Z_STR_P(key), hval)) {
				ZVAL_LONG(member, hval);
				return 0;
			}
		case IS_LONG:
			ZVAL_ZVAL(member, key, 0, 0);
			return 0;

		default:
			ZVAL_STR(member, zval_get_string(key));
			return 1;
	}
}

/* {{{ */
static inline zend_bool pthreads_store_storage_is_pthreads_object(zval *zstorage) {
	pthreads_storage *storage = TRY_PTHREADS_STORAGE_PTR_P(zstorage);
	return storage && storage->type == STORE_TYPE_PTHREADS;
} /* }}} */

/* {{{ */
static inline zend_bool pthreads_store_is_pthreads_object(zend_object *object, zval *key) {
	pthreads_zend_object_t *threaded = PTHREADS_FETCH_FROM(object);
	zval *zstorage;

	if (Z_TYPE_P(key) == IS_LONG) {
		zstorage = zend_hash_index_find(&threaded->ts_obj->store.props->hash, Z_LVAL_P(key));
	} else zstorage = zend_hash_find(&threaded->ts_obj->store.props->hash, Z_STR_P(key));

	return pthreads_store_storage_is_pthreads_object(zstorage);
} /* }}} */

/* {{{ */
int pthreads_store_delete(zend_object *object, zval *key) {
	int result = FAILURE;
	zval member;
	pthreads_zend_object_t *threaded = PTHREADS_FETCH_FROM(object);
	pthreads_object_t *ts_obj = threaded->ts_obj;
	zend_bool coerced = pthreads_store_coerce(key, &member);

	rebuild_object_properties(&threaded->std);

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		zend_bool was_pthreads_object = pthreads_store_is_pthreads_object(object, &member);
		if (Z_TYPE(member) == IS_LONG) {
			result = zend_hash_index_del(&ts_obj->store.props->hash, Z_LVAL(member));
		} else result = zend_hash_del(&ts_obj->store.props->hash, Z_STR(member));

		if (result == SUCCESS && was_pthreads_object) {
			_pthreads_store_bump_modcount_nolock(threaded);
		}
		//TODO: sync local properties?
		pthreads_monitor_unlock(ts_obj->monitor);
	} else result = FAILURE;

	if (result == SUCCESS) {
		if (Z_TYPE(member) == IS_LONG) {
			zend_hash_index_del(threaded->std.properties, Z_LVAL(member));
		} else zend_hash_del(threaded->std.properties, Z_STR(member));
	}

	if (coerced)
		zval_ptr_dtor(&member);

	return result;
}
/* }}} */

/* {{{ */
zend_bool pthreads_store_isset(zend_object *object, zval *key, int has_set_exists) {
	zend_bool isset = 0;
	zval member;
	pthreads_zend_object_t *threaded = PTHREADS_FETCH_FROM(object);
	pthreads_object_t *ts_obj = threaded->ts_obj;
	zend_bool coerced = pthreads_store_coerce(key, &member);

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		zval *zstorage;

		if (Z_TYPE(member) == IS_LONG) {
			zstorage = zend_hash_index_find(&ts_obj->store.props->hash, Z_LVAL(member));
		} else zstorage = zend_hash_find(&ts_obj->store.props->hash, Z_STR(member));

		if (zstorage) {
			isset = 1;
			if (has_set_exists == ZEND_PROPERTY_NOT_EMPTY) {
				switch (Z_TYPE_P(zstorage)) {
					case IS_NULL:
					case IS_FALSE:
						isset = 0;
						break;
					case IS_LONG:
						if (Z_LVAL_P(zstorage) == 0) {
							isset = 0;
						}
						break;
					case IS_DOUBLE:
						if (Z_DVAL_P(zstorage) == 0.0) {
							isset = 0;
						}
						break;
					case IS_STRING:
						if (Z_STRLEN_P(zstorage) == 0 || Z_STRVAL_P(zstorage)[0] == '0') {
							isset = 0;
						}
						break;
					case IS_PTR: {
							/* serialized array, serialized object, Threaded object, or resource */
							pthreads_storage *serialized = (pthreads_storage *) Z_PTR_P(zstorage);
							if (serialized->type == STORE_TYPE_ARRAY && serialized->exists == 0) {
								isset = 0;
							}
						} break;
					case IS_ARRAY:
						if (zend_hash_num_elements(Z_ARRVAL_P(zstorage)) == 0) {
							isset = 0;
						}
						break;
					default:
						break;
				}
			} else if (has_set_exists == ZEND_PROPERTY_ISSET) {
				if (Z_TYPE_P(zstorage) == IS_NULL) {
					isset = 0;
				}
			} else if (has_set_exists != ZEND_PROPERTY_EXISTS) {
				ZEND_ASSERT(0);
			}
		}
		pthreads_monitor_unlock(ts_obj->monitor);
	}

	if (coerced)
		zval_ptr_dtor(&member);

	return isset;
} /* }}} */

/* {{{ */
int pthreads_store_read(zend_object *object, zval *key, int type, zval *read) {
	int result = FAILURE;
	zval member, *property = NULL;
	pthreads_zend_object_t *threaded = PTHREADS_FETCH_FROM(object);
	pthreads_object_t *ts_obj = threaded->ts_obj;
	zend_bool coerced = pthreads_store_coerce(key, &member);

	rebuild_object_properties(&threaded->std);

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		pthreads_store_sync_local_properties(threaded);

		//lookup threaded objects from local property table cache
		if (Z_TYPE(member) == IS_LONG) {
			property = zend_hash_index_find(threaded->std.properties, Z_LVAL(member));
		} else property = zend_hash_find(threaded->std.properties, Z_STR(member));

		if (property && IS_PTHREADS_OBJECT(property)) {
			pthreads_monitor_unlock(ts_obj->monitor);
			ZVAL_COPY(read, property);
			if (coerced) {
				zval_ptr_dtor(&member);
			}
			return SUCCESS;
		}

		zval *zstorage;

		if (Z_TYPE(member) == IS_LONG) {
			zstorage = zend_hash_index_find(&ts_obj->store.props->hash, Z_LVAL(member));
		} else zstorage = zend_hash_find(&ts_obj->store.props->hash, Z_STR(member));

		if (zstorage) {
			pthreads_storage *serialized = TRY_PTHREADS_STORAGE_PTR_P(zstorage);
			/* strictly only reads are supported */
			if ((serialized == NULL || serialized->type != STORE_TYPE_PTHREADS) && type != BP_VAR_R && type != BP_VAR_IS){
				zend_throw_error(zend_ce_error, "Indirect modification of non-Threaded members of %s is not supported", ZSTR_VAL(object->ce->name));
				result = FAILURE;
			} else {
				pthreads_store_restore_zval(read, zstorage);
				result = SUCCESS;
			}
		}
		pthreads_monitor_unlock(ts_obj->monitor);
	}

	if (result != SUCCESS) {
		ZVAL_NULL(read);
	} else {
		if (IS_PTHREADS_OBJECT(read)) {
			rebuild_object_properties(&threaded->std);
			if (Z_TYPE(member) == IS_LONG) {
				zend_hash_index_update(threaded->std.properties, Z_LVAL(member), read);
			} else zend_hash_update(threaded->std.properties, Z_STR(member), read);
			Z_ADDREF_P(read);
		}
	}

	if (coerced)
		zval_ptr_dtor(&member);

	return SUCCESS;
} /* }}} */

/* {{{ */
int pthreads_store_write(zend_object *object, zval *key, zval *write, zend_bool coerce_array_to_threaded) {
	int result = FAILURE;
	zval vol, member, zstorage;
	pthreads_zend_object_t *threaded =
		PTHREADS_FETCH_FROM(object);
	pthreads_object_t *ts_obj = threaded->ts_obj;
	zend_bool coerced = 0;

	if (Z_TYPE_P(write) == IS_ARRAY && coerce_array_to_threaded == PTHREADS_STORE_COERCE_ARRAY) {
		/* coerce arrays into threaded objects */
		object_init_ex(
			&vol, pthreads_threaded_array_entry);
		pthreads_store_merge(Z_OBJ(vol), write, 1, PTHREADS_STORE_COERCE_ARRAY);
		/* this will be addref'd when caching the object */
		Z_SET_REFCOUNT(vol, 0);
		write = &vol;
	}

	if (Z_TYPE_P(write) == IS_OBJECT) {
		/* when we copy out in another context, we want properties table
			to be initialized */
		rebuild_object_properties(Z_OBJ_P(write));
	}

	if (pthreads_store_save_zval(&zstorage, write) != SUCCESS) {
		zend_throw_error(zend_ce_error, "Unsupported data type");
		return FAILURE;
	}

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		if (!key) {
			zend_ulong next = zend_hash_next_free_element(&ts_obj->store.props->hash);
#if PHP_VERSION_ID >= 80000
			if (next == ZEND_LONG_MIN) next = 0;
#endif
			ZVAL_LONG(&member, next);
		} else {
			coerced = pthreads_store_coerce(key, &member);
		}

		zend_bool was_pthreads_object = pthreads_store_is_pthreads_object(object, &member);
		if (Z_TYPE(member) == IS_LONG) {
			if (zend_hash_index_update(&ts_obj->store.props->hash, Z_LVAL(member), &zstorage))
				result = SUCCESS;
		} else {
			/* anything provided by this context might not live long enough to be used by another context,
			 * so we have to hard copy, even if the string is interned. */
			zend_string *orig_key = Z_STR(member);
			zend_string *keyed = zend_string_init(ZSTR_VAL(orig_key), ZSTR_LEN(orig_key), 1);

			if (zend_hash_update(&ts_obj->store.props->hash, keyed, &zstorage)) {
				result = SUCCESS;
			}
			zend_string_release(keyed);
		}
		if (result == SUCCESS && was_pthreads_object) {
			_pthreads_store_bump_modcount_nolock(threaded);
		}
		//this isn't necessary for any specific property write, but since we don't have any other way to clean up local
		//cached Threaded references that are dead, we have to take the opportunity
		pthreads_store_sync_local_properties(threaded);

		pthreads_monitor_unlock(ts_obj->monitor);
	}

	if (result != SUCCESS) {
		pthreads_store_storage_dtor(&zstorage);
	} else {
		if (IS_PTHREADS_OBJECT(write) || IS_PTHREADS_CLOSURE_OBJECT(write)) {
			/*
				This could be a volatile object, but, we don't want to break
				normal refcounting, we'll read the reference only at volatile objects
			*/
			rebuild_object_properties(&threaded->std);

			if (Z_TYPE(member) == IS_LONG) {
				zend_hash_index_update(threaded->std.properties, Z_LVAL(member), write);
			} else {
				zend_string *keyed = zend_string_dup(Z_STR(member), 0);
				if (zend_hash_update(
					threaded->std.properties, keyed, write)) {
					result = SUCCESS;
				}
				zend_string_release(keyed);
			}
			Z_ADDREF_P(write);
		}
	}

	if (coerced)
		zval_ptr_dtor(&member);

	return result;
} /* }}} */

/* {{{ */
int pthreads_store_count(zend_object *object, zend_long *count) {
	pthreads_object_t* ts_obj = PTHREADS_FETCH_TS_FROM(object);

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		(*count) = zend_hash_num_elements(&ts_obj->store.props->hash);
		pthreads_monitor_unlock(ts_obj->monitor);
	} else (*count) = 0L;

	return SUCCESS;
} /* }}} */

/* {{{ */
int pthreads_store_shift(zend_object *object, zval *member) {
	pthreads_zend_object_t* threaded = PTHREADS_FETCH_FROM(object);
	pthreads_object_t *ts_obj = threaded->ts_obj;

	rebuild_object_properties(&threaded->std);

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		zval key;
		HashPosition position;
		zval *zstorage;

		zend_hash_internal_pointer_reset_ex(&ts_obj->store.props->hash, &position);
		if ((zstorage = zend_hash_get_current_data_ex(&ts_obj->store.props->hash, &position))) {
			zend_hash_get_current_key_zval_ex(&ts_obj->store.props->hash, &key, &position);
			zend_bool was_pthreads_object;

			pthreads_store_restore_zval_ex(member, zstorage, &was_pthreads_object);
			if (Z_TYPE(key) == IS_LONG) {
				zend_hash_index_del(&ts_obj->store.props->hash, Z_LVAL(key));
				zend_hash_index_del(threaded->std.properties, Z_LVAL(key));
			} else {
				zend_hash_del(&ts_obj->store.props->hash, Z_STR(key));
				zend_hash_del(threaded->std.properties, Z_STR(key));
			}

			if (was_pthreads_object) {
				_pthreads_store_bump_modcount_nolock(threaded);
			}
			//TODO: maybe we should be syncing local properties here?
		} else ZVAL_NULL(member);
		pthreads_monitor_unlock(ts_obj->monitor);

		return SUCCESS;
	}

	return FAILURE;
} /* }}} */

/* {{{ */
int pthreads_store_chunk(zend_object *object, zend_long size, zend_bool preserve, zval *chunk) {
	pthreads_zend_object_t* threaded = PTHREADS_FETCH_FROM(object);
	pthreads_object_t *ts_obj = threaded->ts_obj;

	rebuild_object_properties(&threaded->std);

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		HashPosition position;
		zval *zstorage;

		array_init(chunk);
		zend_hash_internal_pointer_reset_ex(&ts_obj->store.props->hash, &position);
		zend_bool removed_pthreads_object = 0;
		while((zend_hash_num_elements(Z_ARRVAL_P(chunk)) < size) &&
			(zstorage = zend_hash_get_current_data_ex(&ts_obj->store.props->hash, &position))) {
			zval key, zv;

			zend_hash_get_current_key_zval_ex(&ts_obj->store.props->hash, &key, &position);

			zend_bool was_pthreads_object;
			pthreads_store_restore_zval_ex(&zv, zstorage, &was_pthreads_object);
			if (!removed_pthreads_object) {
				removed_pthreads_object = was_pthreads_object;
			}
			if (Z_TYPE(key) == IS_LONG) {
				zend_hash_index_update(
					Z_ARRVAL_P(chunk), Z_LVAL(key), &zv);
				zend_hash_index_del(&ts_obj->store.props->hash, Z_LVAL(key));
				zend_hash_index_del(threaded->std.properties, Z_LVAL(key));
			} else {
				zend_hash_update(
					Z_ARRVAL_P(chunk), Z_STR(key), &zv);
				zend_hash_del(&ts_obj->store.props->hash, Z_STR(key));
				zend_hash_del(threaded->std.properties, Z_STR(key));
			}

			zend_hash_internal_pointer_reset_ex(&ts_obj->store.props->hash, &position);
		}
		if (removed_pthreads_object) {
			_pthreads_store_bump_modcount_nolock(threaded);
		}
		//TODO: sync local properties?
		pthreads_monitor_unlock(ts_obj->monitor);

		return SUCCESS;
	}

	return FAILURE;
} /* }}} */

/* {{{ */
int pthreads_store_pop(zend_object *object, zval *member) {
	pthreads_zend_object_t* threaded = PTHREADS_FETCH_FROM(object);
	pthreads_object_t *ts_obj = threaded->ts_obj;

	rebuild_object_properties(&threaded->std);

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		zval key;
		HashPosition position;
		zval *zstorage;

		zend_hash_internal_pointer_end_ex(&ts_obj->store.props->hash, &position);
		if ((zstorage = zend_hash_get_current_data_ex(&ts_obj->store.props->hash, &position))) {
			zend_hash_get_current_key_zval_ex(&ts_obj->store.props->hash, &key, &position);

			zend_bool was_pthreads_object;
			pthreads_store_restore_zval_ex(member, zstorage, &was_pthreads_object);

			if (Z_TYPE(key) == IS_LONG) {
				zend_hash_index_del(
					&ts_obj->store.props->hash, Z_LVAL(key));
				zend_hash_index_del(
					threaded->std.properties, Z_LVAL(key));
			} else {
				zend_hash_del(
					&ts_obj->store.props->hash, Z_STR(key));
				zend_hash_del(
					threaded->std.properties, Z_STR(key));
			}
			if (was_pthreads_object) {
				_pthreads_store_bump_modcount_nolock(threaded);
			}
			//TODO: sync local properties?
		} else ZVAL_NULL(member);

		pthreads_monitor_unlock(ts_obj->monitor);

		return SUCCESS;
	}

   return FAILURE;
} /* }}} */

/* {{{ */
void pthreads_store_tohash(zend_object *object, HashTable *hash) {
	pthreads_zend_object_t *threaded = PTHREADS_FETCH_FROM(object);
	pthreads_object_t *ts_obj = threaded->ts_obj;

	rebuild_object_properties(&threaded->std);

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		zend_string *name = NULL;
		zend_ulong idx;
		zval *zstorage;

		pthreads_store_sync_local_properties(threaded);

		ZEND_HASH_FOREACH_KEY_VAL(&ts_obj->store.props->hash, idx, name, zstorage) {
			zval pzval;
			zend_string *rename;


			/* don't overwrite pthreads objects already in the ht (we might be writing std.properties, which will already contain cached Threaded references)
			 * since we sync_local_properties above, if there's a Threaded object in std.properties, it's definitely going to be the same object, so we just
			 * leave it alone and don't overwrite it. */
			if (pthreads_store_storage_is_pthreads_object(zstorage)) {
				if (!name) {
					if (zend_hash_index_exists(hash, idx))
						continue;
				} else {
					if (zend_hash_exists(hash, name))
						continue;
				}
			}

			pthreads_store_restore_zval(&pzval, zstorage);

			if (!name) {
				if (!zend_hash_index_update(hash, idx, &pzval)) {
					zval_ptr_dtor(&pzval);
				}
			} else {
				rename = zend_string_init(name->val, name->len, 0);
				if (!zend_hash_update(hash, rename, &pzval))
					zval_ptr_dtor(&pzval);
				zend_string_release(rename);
			}
		} ZEND_HASH_FOREACH_END();

		pthreads_monitor_unlock(ts_obj->monitor);
	}
} /* }}} */

/* {{{ */
void pthreads_store_free(pthreads_store_t *store){
	zend_hash_destroy(&store->hash);
	free(store);
} /* }}} */

/* {{{ */
static pthreads_storage* pthreads_store_create(zval *unstore){
	pthreads_storage *storage = NULL;

	if (Z_TYPE_P(unstore) == IS_INDIRECT)
		return pthreads_store_create(Z_INDIRECT_P(unstore));
	if (Z_TYPE_P(unstore) == IS_REFERENCE)
		return pthreads_store_create(&Z_REF_P(unstore)->val);

	storage = (pthreads_storage*) calloc(1, sizeof(pthreads_storage));


	switch(Z_TYPE_P(unstore)){
		case IS_RESOURCE: {
			pthreads_resource resource = malloc(sizeof(*resource));
			storage->type = STORE_TYPE_RESOURCE;
			if (resource) {
				resource->original = Z_RES_P(unstore);
				resource->ls = TSRMLS_CACHE;

				storage->data = resource;
				Z_ADDREF_P(unstore);
			}
		} break;

		case IS_OBJECT:
			if (instanceof_function(Z_OBJCE_P(unstore), zend_ce_closure)) {
				const zend_function *def =
					zend_get_closure_method_def(PTHREADS_COMPAT_OBJECT_FROM_ZVAL(unstore));
				storage->type = STORE_TYPE_CLOSURE;
				storage->data =
					(zend_function*) malloc(sizeof(zend_op_array));
				memcpy(storage->data, def, sizeof(zend_op_array));
				break;
			}

			if (instanceof_function(Z_OBJCE_P(unstore), pthreads_threaded_base_entry)) {
				pthreads_zend_object_t *threaded = PTHREADS_FETCH_FROM(Z_OBJ_P(unstore));
				if (threaded->original_zobj != NULL) {
					threaded = threaded->original_zobj;
				}
				storage->type = STORE_TYPE_PTHREADS;
				storage->data = threaded;
				break;
			}

			/* non-threaded, non-closure objects cannot be copied */
			free(storage);
			storage = NULL;
			break;

		case IS_ARRAY: if (pthreads_store_tostring(unstore, (char**) &storage->data, &storage->length)==SUCCESS) {
			storage->exists = zend_hash_num_elements(Z_ARRVAL_P(unstore));
			storage->type = STORE_TYPE_ARRAY;
		} break;
		default:
			ZEND_ASSERT(0);

	}
	return storage;
}
/* }}} */

/* {{{ */
zend_result pthreads_store_save_zval(zval *zstorage, zval *write) {
	zend_result result = FAILURE;
	switch (Z_TYPE_P(write)) {
		case IS_NULL:
		case IS_FALSE:
		case IS_TRUE:
		case IS_LONG:
		case IS_DOUBLE:
			ZVAL_COPY(zstorage, write);
			result = SUCCESS;
			break;
		case IS_STRING:
			if (GC_FLAGS(Z_STR_P(write)) & IS_STR_PERMANENT) { //interned by OPcache, or provided by builtin class
				ZVAL_STR(zstorage, Z_STR_P(write));
			} else {
				ZVAL_STR(zstorage, zend_string_init(Z_STRVAL_P(write), Z_STRLEN_P(write), 1));
			}
			result = SUCCESS;
			break;
		default: {
			pthreads_storage *storage = pthreads_store_create(write);
			if (storage != NULL) {
				ZVAL_PTR(zstorage, pthreads_store_create(write));
				result = SUCCESS;
			} else {
				result = FAILURE;
			}
		} break;
	}
	return result;
} /* }}} */

/* {{{ */
static int pthreads_store_convert(pthreads_storage *storage, zval *pzval){
	int result = SUCCESS;

	switch(storage->type) {
		case STORE_TYPE_RESOURCE: {
			pthreads_resource stored = (pthreads_resource) storage->data;

			if (stored->ls != TSRMLS_CACHE) {
				zval *search = NULL;
				zend_resource *resource, *found = NULL;

				ZEND_HASH_FOREACH_VAL(&EG(regular_list), search) {
					resource = Z_RES_P(search);
					if (resource->ptr == stored->original->ptr) {
						found = resource;
						break;
					}
				} ZEND_HASH_FOREACH_END();

				if (!found) {
					ZVAL_RES(pzval, stored->original);
					if (zend_hash_next_index_insert(&EG(regular_list), pzval)) {
						pthreads_resources_keep(stored);
					} else ZVAL_NULL(pzval);
					Z_ADDREF_P(pzval);
				} else ZVAL_COPY(pzval, search);
			} else {
				ZVAL_RES(pzval, stored->original);
				Z_ADDREF_P(pzval);
			}
		} break;

		case STORE_TYPE_CLOSURE: {
			char *name;
			size_t name_len;
			zend_string *zname;
			zend_function *closure = pthreads_copy_function((zend_function*) storage->data);

			zend_create_closure(pzval, closure, zend_get_executed_scope(), closure->common.scope, NULL);

			name_len = spprintf(&name, 0, "Closure@%p", zend_get_closure_method_def(PTHREADS_COMPAT_OBJECT_FROM_ZVAL(pzval)));
			zname = zend_string_init(name, name_len, 0);

			if (!zend_hash_update_ptr(EG(function_table), zname, closure)) {
				result = FAILURE;
				zval_dtor(pzval);
			} else result = SUCCESS;
			efree(name);
			zend_string_release(zname);
		} break;

		case STORE_TYPE_PTHREADS: {
			pthreads_zend_object_t* threaded = storage->data;

			if (!pthreads_globals_object_connect(threaded, NULL, pzval)) {
				zend_throw_exception_ex(
					spl_ce_RuntimeException, 0,
					"pthreads detected an attempt to connect to an object which has already been destroyed");
				result = FAILURE;
			}
		} break;

		case STORE_TYPE_ARRAY:
			result = pthreads_store_tozval(pzval, (char*) storage->data, storage->length);
		break;

		default: ZEND_ASSERT(0);
	}

	if (result == FAILURE) {
		ZVAL_NULL(pzval);
	}

	return result;
}
/* }}} */

/* {{{ */
void pthreads_store_restore_zval_ex(zval *unstore, zval *zstorage, zend_bool *was_pthreads_storage) {
	*was_pthreads_storage = 0;
	switch (Z_TYPE_P(zstorage)) {
		case IS_NULL:
		case IS_FALSE:
		case IS_TRUE:
		case IS_LONG:
		case IS_DOUBLE:
			/* simple values are stored directly */
			ZVAL_COPY(unstore, zstorage);
			break;
		case IS_STRING:
			if (GC_FLAGS(Z_STR_P(zstorage)) & IS_STR_PERMANENT) {
				/* string from OPcache or internal class */
				ZVAL_STR(unstore, Z_STR_P(zstorage));
			} else {
				/* we can't use this directly (it would cause refcount races) */
				ZVAL_STR(unstore, zend_string_init(Z_STRVAL_P(zstorage), Z_STRLEN_P(zstorage), 0));
			}
			break;
		case IS_PTR:
			{
				/* threaded object, serialized object, resource, serialized array */
				pthreads_storage *storage = (pthreads_storage *) Z_PTR_P(zstorage);
				*was_pthreads_storage = storage->type == STORE_TYPE_PTHREADS;
				pthreads_store_convert(storage, unstore);
			}
			break;
		default:
			ZEND_ASSERT(0);
	}
} /* }}} */

/* {{{ */
void pthreads_store_restore_zval(zval *unstore, zval *zstorage) {
	zend_bool dummy;
	pthreads_store_restore_zval_ex(unstore, zstorage, &dummy);
} /* }}} */

/* {{{ */
static void pthreads_store_hard_copy_storage(zval *new_zstorage, zval *zstorage) {
	if (Z_TYPE_P(zstorage) == IS_PTR) {
		pthreads_storage *storage = (pthreads_storage *) Z_PTR_P(zstorage);
		pthreads_storage *copy = malloc(sizeof(pthreads_storage));

		memcpy(copy, storage, sizeof(pthreads_storage));

		switch (copy->type) {
			case STORE_TYPE_ARRAY: if (storage->length) {
				copy->data = (char*) malloc(copy->length+1);
				if (!copy->data) {
					break;
				}
				memcpy(copy->data, (const void*) storage->data, copy->length);
				((char *)copy->data)[copy->length] = 0;
			} break;
			default: break;
		}
		ZVAL_PTR(new_zstorage, copy);
	} else if (Z_TYPE_P(zstorage) == IS_STRING) {
		ZVAL_STR(new_zstorage, zend_string_init(Z_STRVAL_P(zstorage), Z_STRLEN_P(zstorage), 1));
	} else {
		ZEND_ASSERT(!Z_REFCOUNTED_P(zstorage));
		ZVAL_COPY(new_zstorage, zstorage);
	}
} /* }}} */

static HashTable *pthreads_store_copy_hash(HashTable *source);

static int pthreads_store_copy_zval(zval *dest, zval *source) {
	if (Z_TYPE_P(source) == IS_INDIRECT)
		return pthreads_store_copy_zval(dest, Z_INDIRECT_P(source));
	if (Z_TYPE_P(source) == IS_REFERENCE)
		return pthreads_store_copy_zval(dest, &Z_REF_P(source)->val);

	int result = FAILURE;
	switch (Z_TYPE_P(source)) {
		case IS_NULL:
		case IS_TRUE:
		case IS_FALSE:
		case IS_LONG:
		case IS_DOUBLE:
			ZVAL_DUP(dest, source);
			result = SUCCESS;
		break;

		case IS_STRING:
			ZVAL_STR(dest, zend_string_new(Z_STR_P(source)));
			result = SUCCESS;
		break;

		case IS_ARRAY:
			ZVAL_ARR(dest, pthreads_store_copy_hash(Z_ARRVAL_P(source)));
			result = SUCCESS;
		break;

		case IS_OBJECT:
			if (instanceof_function(Z_OBJCE_P(source), pthreads_threaded_base_entry)) {
				pthreads_globals_object_connect(PTHREADS_FETCH_FROM(Z_OBJ_P(source)), NULL, dest);
				result = SUCCESS;
			} else if (instanceof_function(Z_OBJCE_P(source), zend_ce_closure)) {
				const zend_function *def =
					zend_get_closure_method_def(PTHREADS_COMPAT_OBJECT_FROM_ZVAL(source));

				char *name;
				size_t name_len;
				zend_string *zname;
				zend_function *closure = pthreads_copy_function(def);

				//TODO: executed_scope() doesn't seem appropriate here, especially not during initial bootup ...
				zend_create_closure(dest, closure, zend_get_executed_scope(), closure->common.scope, NULL);

				name_len = spprintf(&name, 0, "Closure@%p", zend_get_closure_method_def(PTHREADS_COMPAT_OBJECT_FROM_ZVAL(dest)));
				zname = zend_string_init(name, name_len, 0);

				if (!zend_hash_update_ptr(EG(function_table), zname, closure)) {
					result = FAILURE;
					zval_dtor(dest);
				} else result = SUCCESS;
				efree(name);
				zend_string_release(zname);

				result = SUCCESS;
			}
		break;

		case IS_CONSTANT_AST:
			//TODO: this doesn't copy the internal AST structures properly, only adds references to them >.<
			ZVAL_AST(dest, zend_ast_copy(GC_AST(Z_AST_P(source))));
			result = SUCCESS;
		break;
		default:
			result = FAILURE;
	}
	return result;
}

static HashTable *pthreads_store_copy_hash(HashTable *source) {
	Bucket *p;
	zval newzval;

	//TODO: what about IS_ARRAY_IMMUTABLE?
	HashTable *ht = (HashTable*) pemalloc(sizeof(HashTable), GC_FLAGS(source) & IS_ARRAY_PERSISTENT);
	zend_hash_init(ht, source->nNumUsed, NULL, source->pDestructor, GC_FLAGS(source) & IS_ARRAY_PERSISTENT);

	ZEND_HASH_FOREACH_BUCKET(source, p){
		if(pthreads_store_copy_zval(&newzval, &p->val) == FAILURE){
			continue;
		}

		if (p->key) {
			zend_hash_update(ht, zend_string_new(p->key), &newzval);
		} else {
			zend_hash_index_update(ht, p->h, &newzval);
		}
	} ZEND_HASH_FOREACH_END();

	return ht;
}

/* {{{ */
int pthreads_store_separate(zval * pzval, zval *separated) {
	if(pthreads_store_copy_zval(separated, pzval) != SUCCESS){
		ZVAL_NULL(separated);
		return FAILURE;
	}
	return SUCCESS;
} /* }}} */

/* {{{ */
static int pthreads_store_tostring(zval *pzval, char **pstring, size_t *slength) {
	int result = FAILURE;
	if (pzval && (Z_TYPE_P(pzval) != IS_NULL)) {
		smart_str smart;
		memset(&smart, 0, sizeof(smart_str));

		php_serialize_data_t vars;

		PHP_VAR_SERIALIZE_INIT(vars);
		php_var_serialize(&smart, pzval, &vars);
		PHP_VAR_SERIALIZE_DESTROY(vars);

		if (EG(exception)) {
			smart_str_free(&smart);

			*pstring = NULL;
			*slength = 0;
			return FAILURE;
		}

		if (smart.s) {
			*slength = smart.s->len;
			if (*slength) {
				*pstring = malloc(*slength+1);
				if (*pstring) {
					memcpy(
						(char*) *pstring, (const void*) smart.s->val, smart.s->len
					);
					(*pstring)[*slength] = 0;
					result = SUCCESS;
				}
			} else *pstring = NULL;
		}

		smart_str_free(&smart);
	} else {
		*slength = 0;
		*pstring = NULL;
	}
	return result;
} /* }}} */

/* {{{ */
static int pthreads_store_tozval(zval *pzval, char *pstring, size_t slength) {
	int result = SUCCESS;

	if (pstring) {
		const unsigned char* pointer = (const unsigned char*) pstring;

		if (pointer) {
			php_unserialize_data_t vars;

			PHP_VAR_UNSERIALIZE_INIT(vars);
			if (!php_var_unserialize(pzval, &pointer, pointer+slength, &vars)) {
				result = FAILURE;
			} else if (Z_REFCOUNTED_P(pzval)) {
				gc_check_possible_root(Z_COUNTED_P(pzval));
			}
			PHP_VAR_UNSERIALIZE_DESTROY(vars);
		} else result = FAILURE;
	} else result = FAILURE;

	return result;
} /* }}} */

/* {{{ */
int pthreads_store_merge(zend_object *destination, zval *from, zend_bool overwrite, zend_bool coerce_array_to_threaded) {
	if (Z_TYPE_P(from) != IS_ARRAY &&
		Z_TYPE_P(from) != IS_OBJECT) {
		return FAILURE;
	}

	switch (Z_TYPE_P(from)) {
		case IS_OBJECT: {
			if (IS_PTHREADS_THREADED_ARRAY(Z_OBJCE_P(from))) {
				pthreads_object_t* threaded[2] = {PTHREADS_FETCH_TS_FROM(destination), PTHREADS_FETCH_TS_FROM(Z_OBJ_P(from))};

				if (pthreads_monitor_lock(threaded[0]->monitor)) {
					if (pthreads_monitor_lock(threaded[1]->monitor)) {
						HashPosition position;
						zval *storage;
						HashTable *tables[2] = {&threaded[0]->store.props->hash, &threaded[1]->store.props->hash};
						zval key;
						zend_bool overwrote_pthreads_object = 0;

						for (zend_hash_internal_pointer_reset_ex(tables[1], &position);
							 (storage = zend_hash_get_current_data_ex(tables[1], &position));
							 zend_hash_move_forward_ex(tables[1], &position)) {
							zend_hash_get_current_key_zval_ex(tables[1], &key, &position);
							if (Z_TYPE(key) == IS_STRING)
								zend_string_delref(Z_STR(key));

							if (!overwrite) {
								if (Z_TYPE(key) == IS_LONG) {
									if (zend_hash_index_exists(tables[0], Z_LVAL(key)))
										continue;
								} else {
									if (zend_hash_exists(tables[0], Z_STR(key)))
										continue;
								}
							}

							if (!overwrote_pthreads_object) {
								overwrote_pthreads_object = pthreads_store_is_pthreads_object(destination, &key);
							}

							zval new_zstorage;
							pthreads_store_hard_copy_storage(&new_zstorage, storage);
							if (Z_TYPE(key) == IS_LONG) {
								zend_hash_index_update(tables[0], Z_LVAL(key), &new_zstorage);
							} else {
								/* anything provided by this context might not live long enough to be used by another context,
								 * so we have to hard copy, even if the string is interned. */
								zend_string *orig_key = Z_STR(key);
								zend_string *keyed = zend_string_init(ZSTR_VAL(orig_key), ZSTR_LEN(orig_key), 1);

								zend_hash_update(tables[0], keyed, &new_zstorage);
								zend_string_release(keyed);
							}
						}
						if (overwrote_pthreads_object) {
							_pthreads_store_bump_modcount_nolock(PTHREADS_FETCH_FROM(destination));
						}
						//TODO: sync local properties?

						pthreads_monitor_unlock(threaded[1]->monitor);
					}

					pthreads_monitor_unlock(threaded[0]->monitor);

					return SUCCESS;

				} else return FAILURE;
			}
		}

		/* fall through on purpose to handle normal objects and arrays */

		default: {
			pthreads_object_t* ts_obj = PTHREADS_FETCH_TS_FROM(destination);

			if (pthreads_monitor_lock(ts_obj->monitor)) {
				HashPosition position;
				zval *pzval;
				int32_t index = 0;
				HashTable *table = (Z_TYPE_P(from) == IS_ARRAY) ? Z_ARRVAL_P(from) : Z_OBJPROP_P(from);

				for (zend_hash_internal_pointer_reset_ex(table, &position);
					(pzval = zend_hash_get_current_data_ex(table, &position));
					zend_hash_move_forward_ex(table, &position)) {
					zval key;

					zend_hash_get_current_key_zval_ex(table, &key, &position);
					if (Z_TYPE(key) == IS_STRING)
						zend_string_delref(Z_STR(key));

					switch (Z_TYPE(key)) {
						case IS_LONG:
							if (!overwrite && zend_hash_index_exists(&ts_obj->store.props->hash, Z_LVAL(key))) {
								goto next;
							}
							pthreads_store_write(destination, &key, pzval, coerce_array_to_threaded);
						break;

						case IS_STRING:
							if (!overwrite && zend_hash_exists(&ts_obj->store.props->hash, Z_STR(key))) {
								goto next;
							}
							pthreads_store_write(destination, &key, pzval, coerce_array_to_threaded);
						break;
					}

next:
					index++;
				}

				pthreads_monitor_unlock(ts_obj->monitor);
			}
		} break;
	}

	return SUCCESS;
} /* }}} */


/* {{{ Will free store element */
void pthreads_store_storage_dtor (zval *zstorage){
	if (!zstorage) return;

	if (Z_TYPE_P(zstorage) == IS_PTR) {
		pthreads_storage *storage = (pthreads_storage *) Z_PTR_P(zstorage);
		switch (storage->type) {
			case STORE_TYPE_CLOSURE:
			case STORE_TYPE_ARRAY:
			case STORE_TYPE_RESOURCE:
				if (storage->data) {
					free(storage->data);
				}
			break;
			default: break;
		}
		free(storage);
	} else if (Z_TYPE_P(zstorage) == IS_STRING) {
		zend_string *str = Z_STR_P(zstorage);
		zend_string_release_ex(str, 1);
	} else {
		/* no action necessary */
	}
} /* }}} */

/* {{{ iteration helpers */
void pthreads_store_reset(zend_object *object, HashPosition *position) {
	pthreads_object_t *ts_obj = PTHREADS_FETCH_TS_FROM(object);

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		zend_hash_internal_pointer_reset_ex(&ts_obj->store.props->hash, position);
		if (zend_hash_has_more_elements_ex(&ts_obj->store.props->hash, position) == FAILURE) { //empty
			*position = HT_INVALID_IDX;
		}
		pthreads_monitor_unlock(ts_obj->monitor);
	}
}

void pthreads_store_key(zend_object *object, zval *key, HashPosition *position) {
	pthreads_object_t *ts_obj = PTHREADS_FETCH_TS_FROM(object);
	zend_string *str_key;
	zend_ulong num_key;

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		switch (zend_hash_get_current_key_ex(&ts_obj->store.props->hash, &str_key, &num_key, position)) {
			case HASH_KEY_NON_EXISTENT:
				ZVAL_NULL(key);
			break;
			case HASH_KEY_IS_LONG:
				ZVAL_LONG(key, num_key);
			break;
			case HASH_KEY_IS_STRING:
				ZVAL_STR(key, zend_string_dup(str_key, 0));
			break;
		}
		pthreads_monitor_unlock(ts_obj->monitor);
	}
}

void pthreads_store_data(zend_object *object, zval *value, HashPosition *position) {
	pthreads_object_t *ts_obj = PTHREADS_FETCH_TS_FROM(object);

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		zval *storage = zend_hash_get_current_data_ex(&ts_obj->store.props->hash, position);

		if (storage) {
			pthreads_store_restore_zval(value, storage);
		} else ZVAL_UNDEF(value);

		pthreads_monitor_unlock(ts_obj->monitor);
	}
}

void pthreads_store_forward(zend_object *object, HashPosition *position) {
	pthreads_object_t *ts_obj = PTHREADS_FETCH_TS_FROM(object);

	if (pthreads_monitor_lock(ts_obj->monitor)) {
		zend_hash_move_forward_ex(
			&ts_obj->store.props->hash, position);
		if (zend_hash_has_more_elements_ex(&ts_obj->store.props->hash, position) == FAILURE) {
			*position = HT_INVALID_IDX;
		}
		pthreads_monitor_unlock(ts_obj->monitor);
	}
} /* }}} */

#endif
