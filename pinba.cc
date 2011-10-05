/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2008 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.02 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available at through the world-wide-web at                           |
  | http://www.php.net/license/2_02.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Antony Dovgal <tony@daylessday.org>                         |
  |          Florian Forster <ff at octo.it>  (IPv6 support)             |
  +----------------------------------------------------------------------+

  $Id: pinba.cc,v 1.1.2.9 2009/04/28 10:46:55 tony Exp $ 
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

extern "C" {
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>

#include "php.h"
#include "php_ini.h"
#include "SAPI.h"
#include "ext/standard/info.h"
#include "ext/standard/php_array.h"
}

#include "php_pinba.h"

#include <string>
using namespace std;
#include "pinba-pb.h"

ZEND_DECLARE_MODULE_GLOBALS(pinba)

#ifdef COMPILE_DL_PINBA
BEGIN_EXTERN_C()
ZEND_GET_MODULE(pinba)
END_EXTERN_C()
#endif

static int le_pinba_timer;
static int pinba_socket = -1;

#if ZEND_MODULE_API_NO > 20020429
#define ONUPDATELONGFUNC OnUpdateLong
#else
#define ONUPDATELONGFUNC OnUpdateInt
#endif

#define PINBA_FLUSH_ONLY_STOPPED_TIMERS (1<<0)

typedef struct _pinba_timer_tag { /* {{{ */
	char *name;
	int name_len;
	int name_id;
	char *value; /* we cast all types to string */
	int value_len;
	int value_id;
} pinba_timer_tag_t;
/* }}} */

typedef struct _pinba_timer { /* {{{ */
	int rsrc_id;
	unsigned int started:1;
	unsigned int hit_count;
	pinba_timer_tag_t **tags;
	int tags_num;
	struct {
		int tv_sec;
		int tv_usec;
	} start;
	struct {
		int tv_sec;
		int tv_usec;
	} value;
	zval *data;
	struct timeval tmp_ru_utime;
	struct timeval tmp_ru_stime;
	struct timeval ru_utime;
	struct timeval ru_stime;
	int deleted:1;
} pinba_timer_t;
/* }}} */

#define PHP_ZVAL_TO_TIMER(zval, timer) \
	            ZEND_FETCH_RESOURCE(timer, pinba_timer_t *, &zval, -1, "pinba timer", le_pinba_timer)

#define timeval_cvt(a, b) do { (a)->tv_sec = (b)->tv_sec; (a)->tv_usec = (b)->tv_usec; } while (0);
#define timeval_to_float(t) (float)(t).tv_sec + (float)(t).tv_usec / 1000000.0

/* {{{ internal funcs */

static inline int php_pinba_timer_stop(pinba_timer_t *t) /* {{{ */
{
	struct timeval now;
	struct rusage u, tmp;
	
	if (!t->started) {
		return FAILURE;
	}
	
	gettimeofday(&now, 0);
	timersub(&now, &t->start, &t->value);
	
	if (getrusage(RUSAGE_SELF, &u) == 0) {
		timersub(&u.ru_utime, &t->tmp_ru_utime, &tmp.ru_utime);
		timersub(&u.ru_stime, &t->tmp_ru_stime, &tmp.ru_stime);
		timeradd(&t->ru_utime, &tmp.ru_utime, &t->ru_utime);
		timeradd(&t->ru_stime, &tmp.ru_stime, &t->ru_stime);
	}
	
	t->started = 0;
	return SUCCESS;
}
/* }}} */

static inline void php_pinba_timer_tags_dtor(pinba_timer_tag_t **tags, int tags_num) /* {{{ */
{
	int i;
	pinba_timer_tag_t *tag;

	for (i = 0; i < tags_num; i++) {
		tag = *(tags + i);
		if (tag) {
			if (tag->name) {
				efree(tag->name);
			}
			if (tag->value) {
				efree(tag->value);
			}
			efree(tag);
		}
	}
}
/* }}} */

static void php_pinba_timer_dtor(pinba_timer_t *t) /* {{{ */
{
	php_pinba_timer_tags_dtor(t->tags, t->tags_num);
	efree(t->tags);
}
/* }}} */

static void php_timer_hash_dtor(void *data) /* {{{ */
{
	pinba_timer_t *t = *(pinba_timer_t **)data;

	if (t) {
		php_pinba_timer_dtor(t);
		efree(t);
		*(pinba_timer_t **)data = NULL;
	}
}
/* }}} */

static int php_pinba_tags_to_hashed_string(pinba_timer_t *timer, char **hashed_tags, int *hashed_tags_len TSRMLS_DC) /* {{{ */
{
	int i;
	char *buf;
	int buf_len, wrote_len;

	*hashed_tags = NULL;
	*hashed_tags_len = 0;

	if (!timer->tags_num) {
		return FAILURE;
	}

	buf_len = 4096;
	wrote_len = 0;
	buf = (char *)emalloc(buf_len + 1);

	for (i = 0; i < timer->tags_num; i++) {
		if (buf_len <= (wrote_len + timer->tags[i]->name_len + 2 + timer->tags[i]->value_len + 1)) {
			buf_len = wrote_len + timer->tags[i]->name_len + 2 + timer->tags[i]->value_len + 1 + 4096;
			buf = (char *)erealloc(buf, buf_len + 1);
		}
		memcpy(buf + wrote_len, timer->tags[i]->name, timer->tags[i]->name_len);
		wrote_len += timer->tags[i]->name_len;

		memcpy(buf + wrote_len, "=>", 2);
		wrote_len += 2;

		memcpy(buf + wrote_len, timer->tags[i]->value, timer->tags[i]->value_len);
		wrote_len += timer->tags[i]->value_len;
		
		memcpy(buf + wrote_len, ",", 1);
		wrote_len += 1;
	}

	buf[wrote_len] = '\0';


	*hashed_tags = buf;
	*hashed_tags_len = wrote_len;
	return SUCCESS;	
}
/* }}} */

static void php_timer_resource_dtor(zend_rsrc_list_entry *entry TSRMLS_DC) /* {{{ */
{
	pinba_timer_t *t = (pinba_timer_t *)entry->ptr;

	php_pinba_timer_stop(t);
	/* php_pinba_timer_dtor(t); all timers are destroyed at once */

	/* but we don't need the user data anymore */
	if (t->data) {
		zval_ptr_dtor(&t->data);
		t->data = NULL;
	}

	if (!t->deleted) {
		if (zend_hash_index_exists(&PINBA_G(timers), t->rsrc_id) == 0) {
			zend_hash_index_update(&PINBA_G(timers), t->rsrc_id, &t, sizeof(pinba_timer_t *), NULL);
		}
	} else {
		php_pinba_timer_dtor(t);
		efree(t);
	}
}
/* }}} */

static int php_pinba_timer_stop_helper(zend_rsrc_list_entry *le, void *le_type TSRMLS_DC) /* {{{ */
{
	if (le->type == le_pinba_timer) {
		/* remove not looking at the refcount */
		return ZEND_HASH_APPLY_REMOVE;
	}
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

static int sapi_ub_write_counter(const char *str, unsigned int length TSRMLS_DC) /* {{{ */
{
	PINBA_G(tmp_req_data).doc_size += length;
	return PINBA_G(old_sapi_ub_write)(str, length TSRMLS_CC);
}
/* }}} */

static int php_pinba_init_socket (TSRMLS_D) /* {{{ */
{
	struct addrinfo *ai_list;
	struct addrinfo *ai_ptr;
	struct addrinfo  ai_hints;
	int fd;
	int status;

	if (PINBA_G(server_host) == NULL || PINBA_G(server_port) == NULL) {
		return -1;
	}

	memset(&ai_hints, 0, sizeof(ai_hints));
	ai_hints.ai_flags     = 0;
#ifdef AI_ADDRCONFIG
	ai_hints.ai_flags    |= AI_ADDRCONFIG;
#endif
	ai_hints.ai_family    = AF_UNSPEC;
	ai_hints.ai_socktype  = SOCK_DGRAM;
	ai_hints.ai_addr      = NULL;
	ai_hints.ai_canonname = NULL;
	ai_hints.ai_next      = NULL;

	ai_list = NULL;
	status = getaddrinfo(PINBA_G(server_host), PINBA_G(server_port), &ai_hints, &ai_list);
	if (status != 0) {
		/* TODO: error reporting */
		return -1;
	}

	fd = -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next) {
		fd = socket(ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
		if (fd < 0) {
			continue;
		}

		if (pinba_socket >= 0) {
			close(pinba_socket);
		}

		pinba_socket = fd;

		memcpy(&PINBA_G(collector_sockaddr), ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		PINBA_G(collector_sockaddr_len) = ai_ptr->ai_addrlen;
		break;
	}

	freeaddrinfo(ai_list);

	return ((fd >= 0) ? 0 : -1);
} /* }}} */

static inline int php_pinba_req_data_send(pinba_req_data record, HashTable *timers, long flags TSRMLS_DC) /* {{{ */
{
	int ret = SUCCESS, timers_num, dict_cnt = 0;
	HashTable dict;
	HashTable timers_uniq;
	HashPosition pos;
	Pinba::Request *request;
	int i;

	/* no socket -> bail out */
	if (pinba_socket < 0) {
		return FAILURE;
	}

	timers_num = zend_hash_num_elements(timers);
	if (timers_num > 0) {
		pinba_timer_t *t, *old_t, **t_el, **old_t_el;
		char *hashed_tags;
		int hashed_tags_len;

		/* make sure we send aggregated timers to the server */
		zend_hash_init(&timers_uniq, 10, NULL, NULL, 0);
		zend_hash_init(&dict, 10, NULL, NULL, 0);

		for (zend_hash_internal_pointer_reset_ex(timers, &pos);
				zend_hash_get_current_data_ex(timers, (void **) &t_el, &pos) == SUCCESS;
				zend_hash_move_forward_ex(timers, &pos)) {
			t = *t_el;
			if (php_pinba_tags_to_hashed_string(t, &hashed_tags, &hashed_tags_len TSRMLS_CC) != SUCCESS) {
				continue;
			}

			/* aggregate only stopped timers */
			if ((flags & PINBA_FLUSH_ONLY_STOPPED_TIMERS) != 0 && t->started) {
				continue;
			}

			if (zend_hash_find(&timers_uniq, hashed_tags, hashed_tags_len + 1, (void **)&old_t_el) == SUCCESS) {
				old_t = *old_t_el;
				timeradd(&old_t->value, &t->value, &old_t->value);
				old_t->hit_count++;
			} else {
				zend_hash_add(&timers_uniq, hashed_tags, hashed_tags_len + 1, t_el, sizeof(pinba_timer_t *), NULL);
			}
			efree(hashed_tags);
		}

		/* create our temporary dictionary and add ids to timers */
		for (zend_hash_internal_pointer_reset_ex(&timers_uniq, &pos);
				zend_hash_get_current_data_ex(&timers_uniq, (void **) &t_el, &pos) == SUCCESS;
				zend_hash_move_forward_ex(&timers_uniq, &pos)) {
			t = *t_el;
			for (i = 0; i < t->tags_num; i++) {
				int *id;

				if (zend_hash_find(&dict, t->tags[i]->name, t->tags[i]->name_len + 1, (void **)&id) != SUCCESS) {
					ret = zend_hash_add(&dict, t->tags[i]->name, t->tags[i]->name_len + 1, &dict_cnt, sizeof(int), NULL);

					if (ret != SUCCESS) {
						break;
					}

					t->tags[i]->name_id = dict_cnt;
					dict_cnt++;
				} else {
					t->tags[i]->name_id = *id;
				}

				if (zend_hash_find(&dict, t->tags[i]->value, t->tags[i]->value_len + 1, (void **)&id) != SUCCESS) {
					ret = zend_hash_add(&dict, t->tags[i]->value, t->tags[i]->value_len + 1, &dict_cnt, sizeof(int), NULL);
					
					if (ret != SUCCESS) {
						break;
					}
					
					t->tags[i]->value_id = dict_cnt;
					dict_cnt++;
				} else {
					t->tags[i]->value_id = *id;
				}
			}
		}
	}

	request = new Pinba::Request;

	request->set_hostname(PINBA_G(host_name));
	request->set_server_name(record.server_name);
	request->set_script_name(record.script_name);
	request->set_request_count(record.req_count);
	request->set_document_size(record.doc_size);
	request->set_memory_peak(record.mem_peak_usage);
	request->set_request_time(timeval_to_float(record.req_time));
	request->set_ru_utime(timeval_to_float(record.ru_utime));
	request->set_ru_stime(timeval_to_float(record.ru_stime));
	request->set_status(SG(sapi_headers).http_response_code);

	/* timers */
	if (timers_num > 0) {
		pinba_timer_t *t, **t_el;
		int *id;

		for (zend_hash_internal_pointer_reset_ex(&dict, &pos);
				zend_hash_get_current_data_ex(&dict, (void **) &id, &pos) == SUCCESS;
				zend_hash_move_forward_ex(&dict, &pos)) {
			char *str;
			uint str_len;
			ulong dummy;

			if (zend_hash_get_current_key_ex(&dict, &str, &str_len, &dummy, 0, &pos) == HASH_KEY_IS_STRING) {
				request->add_dictionary(str);
			} else {
				continue;
			}
		}
		zend_hash_destroy(&dict);

		for (zend_hash_internal_pointer_reset_ex(&timers_uniq, &pos);
				zend_hash_get_current_data_ex(&timers_uniq, (void **) &t_el, &pos) == SUCCESS;
				zend_hash_move_forward_ex(&timers_uniq, &pos)) {
			
			t = *t_el;

			for (i = 0; i < t->tags_num; i++) {
				request->add_timer_tag_name(t->tags[i]->name_id);
				request->add_timer_tag_value(t->tags[i]->value_id);
			}
			
			request->add_timer_tag_count(i);
			request->add_timer_hit_count(t->hit_count);
			request->add_timer_value(timeval_to_float(t->value));
		}
		zend_hash_destroy(&timers_uniq);
	}

	if (ret == SUCCESS) {
		size_t total_sent = 0;
		ssize_t sent;
		string data;
		bool res;

		res = request->SerializeToString(&data);

		while (total_sent < data.size()) {
			int flags = 0;

			sent = sendto(pinba_socket, data.c_str() + total_sent, data.size() - total_sent, flags,
					(struct sockaddr *) &PINBA_G(collector_sockaddr), PINBA_G(collector_sockaddr_len));
			if (sent < 0) {
				ret = FAILURE;
				break;
			}
			total_sent += sent;
		}
	} else {
		ret = FAILURE;
	}

	delete request;
	return ret;
}
/* }}} */

static inline void php_pinba_req_data_dtor(pinba_req_data *record) /* {{{ */
{
	if (record->server_name) {
		efree(record->server_name);
	}

	if (record->script_name) {
		efree(record->script_name);
	}

	memset(record, 0, sizeof(pinba_req_data));
}
/* }}} */

static void php_pinba_flush_data(const char *custom_script_name, long flags TSRMLS_DC) /* {{{ */
{
	struct timeval req_finish;
	struct rusage u;
	pinba_req_data req_data;
	int status;

#if PHP_MAJOR_VERSION >= 5 
	PINBA_G(tmp_req_data).mem_peak_usage = zend_memory_peak_usage(1 TSRMLS_CC);
#elif PHP_MAJOR_VERSION == 4 && MEMORY_LIMIT
	PINBA_G(tmp_req_data).mem_peak_usage = AG(allocated_memory_peak);
#else
	/* no data available */
	PINBA_G(tmp_req_data).mem_peak_usage = 0;
#endif
	if ((flags & PINBA_FLUSH_ONLY_STOPPED_TIMERS) == 0) {
		/* stop all running timers */
		zend_hash_apply_with_argument(&EG(regular_list), (apply_func_arg_t) php_pinba_timer_stop_helper, (void *)(long)le_pinba_timer TSRMLS_CC);
	}
	/* prevent any further access to the timers */
	PINBA_G(timers_stopped) = 1;

	if (!PINBA_G(enabled) || !PINBA_G(server_host) || !PINBA_G(server_port)) {
		/* no server host/port set, exit */
		zend_hash_clean(&PINBA_G(timers));
		PINBA_G(timers_stopped) = 0;
		return;
	}

	status = php_pinba_init_socket(TSRMLS_C);
	if (status != 0) {
		PINBA_G(timers_stopped) = 0;
		return;
	}

	/* compute how many time the request took */
	if (gettimeofday(&req_finish, 0) == 0) {
		timersub(&req_finish, &(PINBA_G(tmp_req_data).req_start), &req_data.req_time);
	}

	/* get rusage */
	if (getrusage(RUSAGE_SELF, &u) == 0) {
		timersub(&u.ru_utime, &(PINBA_G(tmp_req_data).ru_utime), &req_data.ru_utime);
		timersub(&u.ru_stime, &(PINBA_G(tmp_req_data).ru_stime), &req_data.ru_stime);
	}

	timeval_cvt(&req_data.req_start, &PINBA_G(tmp_req_data).req_start);
	req_data.req_count = ++PINBA_G(tmp_req_data).req_count;
	req_data.doc_size = PINBA_G(tmp_req_data).doc_size;
	req_data.mem_peak_usage = PINBA_G(tmp_req_data).mem_peak_usage;

	req_data.server_name = NULL;
	req_data.script_name = NULL;

	if (PINBA_G(server_name)) {
		req_data.server_name = estrdup(PINBA_G(server_name));
	}

	if (!req_data.server_name) {
		req_data.server_name = estrdup("unknown");
	}

	if (custom_script_name) {
		req_data.script_name = estrdup(custom_script_name);
	} else if (PINBA_G(script_name)) {
		req_data.script_name = estrdup(PINBA_G(script_name));
	}

	if (!req_data.script_name) {
		req_data.script_name = estrdup("unknown");
	}

	php_pinba_req_data_send(req_data, &PINBA_G(timers), flags TSRMLS_CC);
	php_pinba_req_data_dtor(&req_data);

	PINBA_G(timers_stopped) = 0;
	zend_hash_clean(&PINBA_G(timers));
}
/* }}} */

static int php_pinba_key_compare(const void *a, const void *b TSRMLS_DC) /* {{{ */
{
	Bucket *f;
	Bucket *s;
	zval result;
	zval first;
	zval second;

	f = *((Bucket **) a);
	s = *((Bucket **) b);

	if (f->nKeyLength == 0) {
		Z_TYPE(first) = IS_LONG;
		Z_LVAL(first) = f->h;
	} else {
		Z_TYPE(first) = IS_STRING;
		Z_STRVAL(first) = f->arKey;
		Z_STRLEN(first) = f->nKeyLength - 1;
	}

	if (s->nKeyLength == 0) {
		Z_TYPE(second) = IS_LONG;
		Z_LVAL(second) = s->h;
	} else {
		Z_TYPE(second) = IS_STRING;
		Z_STRVAL(second) = s->arKey;
		Z_STRLEN(second) = s->nKeyLength - 1;
	}

	if (string_compare_function(&result, &first, &second TSRMLS_CC) == FAILURE) {
		return 0;
	}

	if (Z_TYPE(result) == IS_DOUBLE) {
		if (Z_DVAL(result) < 0) {
			return -1;
		} else if (Z_DVAL(result) > 0) {
			return 1;
		} else {
			return 0;
		}
	}

	convert_to_long(&result);

	if (Z_LVAL(result) < 0) {
		return -1;
	} else if (Z_LVAL(result) > 0) {
		return 1;
	}

	return 0;
}
/* }}} */

static int php_pinba_array_to_tags(zval *array, pinba_timer_tag_t ***tags TSRMLS_DC) /* {{{ */
{
	int num, i = 0;
	zval **value;
	char *tag_name, *value_str;
	uint tag_name_len, value_str_len;
	ulong dummy;

	num = zend_hash_num_elements(Z_ARRVAL_P(array));
	if (!num) {
		return FAILURE;
	}

	/* sort array, we'll use this when computing tags hash */
	zend_hash_sort(Z_ARRVAL_P(array), zend_qsort, php_pinba_key_compare, 0 TSRMLS_CC);

	*tags = (pinba_timer_tag_t **)ecalloc(num, sizeof(pinba_timer_tag_t *));
	for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(array));
			zend_hash_get_current_data(Z_ARRVAL_P(array), (void **) &value) == SUCCESS;
			zend_hash_move_forward(Z_ARRVAL_P(array))) {

		switch (Z_TYPE_PP(value)) {
			case IS_NULL:
			case IS_STRING:
			case IS_BOOL:
			case IS_LONG:
			case IS_DOUBLE:
				SEPARATE_ZVAL(value);
				convert_to_string_ex(value);
				value_str = estrndup(Z_STRVAL_PP(value), Z_STRLEN_PP(value));
				value_str_len = Z_STRLEN_PP(value);
				break;
			default:
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "tags cannot have non-scalar values");
				php_pinba_timer_tags_dtor(*tags, i);
				efree(*tags);
				return FAILURE;
		}

		if (zend_hash_get_current_key_ex(Z_ARRVAL_P(array), &tag_name, &tag_name_len, &dummy, 1, NULL) == HASH_KEY_IS_STRING) {
			(*tags)[i] = (pinba_timer_tag_t *)emalloc(sizeof(pinba_timer_tag_t));
			(*tags)[i]->name = tag_name;
			(*tags)[i]->name_len = tag_name_len - 1;
			(*tags)[i]->value = value_str;
			(*tags)[i]->value_len = value_str_len;
		} else {
			if (value_str) {
				efree(value_str);
			}
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "tags can only have string names (i.e. tags array cannot contain numeric indexes)");
			php_pinba_timer_tags_dtor(*tags, i);
			efree(*tags);
			return FAILURE;
		}
		i++;
	}
	return SUCCESS;	
}
/* }}} */

static pinba_timer_t *php_pinba_timer_ctor(pinba_timer_tag_t **tags, int tags_num TSRMLS_DC) /* {{{ */
{
	struct timeval now;
	pinba_timer_t *t;
	
	t = (pinba_timer_t *)ecalloc(1, sizeof(pinba_timer_t));
	t->tags_num = tags_num;
	t->tags = tags;

	gettimeofday(&now, 0);
	timeval_cvt(&t->start, &now);

	return t;
}
/* }}} */

static void php_pinba_get_timer_info(pinba_timer_t *t, zval *info TSRMLS_DC) /* {{{ */
{
	zval *tags;
	pinba_timer_tag_t *tag;
	struct timeval tmp;
	int i;

	array_init(info);
	
	if (t->started) {
		gettimeofday(&tmp, 0);
		timersub(&tmp, &t->start, &tmp);
		timeradd(&t->value, &tmp, &tmp);
	} else {
		timeval_cvt(&tmp, &t->value);
	}
	add_assoc_double_ex(info, "value", sizeof("value"), timeval_to_float(tmp));

	MAKE_STD_ZVAL(tags);
	array_init(tags);

	for (i = 0; i < t->tags_num; i++) {
		tag = t->tags[i];
		add_assoc_stringl_ex(tags, tag->name, tag->name_len + 1, tag->value, tag->value_len, 1);
	}

	add_assoc_zval_ex(info, "tags", sizeof("tags"), tags);
	add_assoc_bool_ex(info, "started", sizeof("started"), t->started ? 1 : 0);
	if (t->data) {
		add_assoc_zval_ex(info, "data", sizeof("data"), t->data);
		zval_add_ref(&t->data);
	} else {
		add_assoc_null_ex(info, "data", sizeof("data"));
	}
	
	add_assoc_double_ex(info, "ru_utime", sizeof("ru_utime"), timeval_to_float(t->ru_utime));
	add_assoc_double_ex(info, "ru_stime", sizeof("ru_stime"), timeval_to_float(t->ru_stime));
}
/* }}} */

/* }}} */

/* {{{ proto resource pinba_timer_start(array tags[, array data])
   Start user timer */
static PHP_FUNCTION(pinba_timer_start)
{
	zval *tags_array, *data = NULL;
	pinba_timer_t *t = NULL;
	pinba_timer_tag_t **tags;
	int tags_num;
	struct rusage u;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a|a", &tags_array, &data) != SUCCESS) {
		return;
	}

	tags_num = zend_hash_num_elements(Z_ARRVAL_P(tags_array));

	if (!tags_num) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "tags array cannot be empty");
		RETURN_FALSE;
	}

	if (php_pinba_array_to_tags(tags_array, &tags TSRMLS_CC) != SUCCESS) {
		RETURN_FALSE;
	}

	t = php_pinba_timer_ctor(tags, tags_num TSRMLS_CC);

	if (data) {
		MAKE_STD_ZVAL(t->data);
		*(t->data) = *data;
		zval_copy_ctor(t->data);
		INIT_PZVAL(t->data);
	}

	t->started = 1;
	t->hit_count = 1;

	t->rsrc_id = zend_list_insert(t, le_pinba_timer);

	if (getrusage(RUSAGE_SELF, &u) == 0) {
		timeval_cvt(&t->tmp_ru_utime, &u.ru_utime);
		timeval_cvt(&t->tmp_ru_stime, &u.ru_stime);
	}
	/* refcount++ so that the timer is shut down only on request finish if not stopped manually */
	zend_list_addref(t->rsrc_id);
	RETURN_RESOURCE(t->rsrc_id);
}
/* }}} */

/* {{{ proto resource pinba_timer_add(array tags, float value[, array data])
   Create user timer with a value */
static PHP_FUNCTION(pinba_timer_add)
{
	zval *tags_array, *data = NULL;
	pinba_timer_t *t = NULL;
	pinba_timer_tag_t **tags;
	int tags_num;
	double value;
	unsigned long time_l;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ad|a", &tags_array, &value, &data) != SUCCESS) {
		return;
	}

	tags_num = zend_hash_num_elements(Z_ARRVAL_P(tags_array));

	if (!tags_num) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "tags array cannot be empty");
		RETURN_FALSE;
	}

	if (php_pinba_array_to_tags(tags_array, &tags TSRMLS_CC) != SUCCESS) {
		RETURN_FALSE;
	}

	t = php_pinba_timer_ctor(tags, tags_num TSRMLS_CC);

	if (data) {
		MAKE_STD_ZVAL(t->data);
		*(t->data) = *data;
		zval_copy_ctor(t->data);
		INIT_PZVAL(t->data);
	}

	t->started = 0;
	t->hit_count = 1;
	time_l = (unsigned long)(value * 1000000.0);
	t->value.tv_sec = time_l / 1000000;
	t->value.tv_usec = time_l % 1000000;

	t->rsrc_id = zend_list_insert(t, le_pinba_timer);

	/* refcount++ so that the timer is shut down only on request finish if not stopped manually */
	zend_list_addref(t->rsrc_id);
	RETURN_RESOURCE(t->rsrc_id);
}
/* }}} */

/* {{{ proto bool pinba_timer_stop(resource timer)
   Stop user timer */
static PHP_FUNCTION(pinba_timer_stop)
{
	zval *timer;
	pinba_timer_t *t;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &timer) != SUCCESS) {
		return;
	}
	
	PHP_ZVAL_TO_TIMER(timer, t);

	if (!t->started) {
		php_error_docref(NULL TSRMLS_CC, E_NOTICE, "timer is already stopped");
		RETURN_FALSE;
	}

	php_pinba_timer_stop(t);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_timer_delete(resource timer)
   Delete user timer */
static PHP_FUNCTION(pinba_timer_delete)
{
	zval *timer;
	pinba_timer_t *t;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &timer) != SUCCESS) {
		return;
	}
	
	PHP_ZVAL_TO_TIMER(timer, t);

	if (t->started) {
		php_pinba_timer_stop(t);
	}

	t->deleted = 1;
	zend_list_delete(t->rsrc_id);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_timer_data_merge(resource timer, array data)
   Merge timer data with new data */
static PHP_FUNCTION(pinba_timer_data_merge)
{
	zval *timer, *data, *tmp;
	pinba_timer_t *t;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ra", &timer, &data) != SUCCESS) {
		return;
	}
	
	PHP_ZVAL_TO_TIMER(timer, t);

	if (!t->data) {
		MAKE_STD_ZVAL(t->data);
		*(t->data) = *data;
		zval_copy_ctor(t->data);
		INIT_PZVAL(t->data);
	} else {
		zend_hash_merge(Z_ARRVAL_P(t->data), Z_ARRVAL_P(data), (void (*)(void *pData)) zval_add_ref, (void *) &tmp, sizeof(zval *), 1);
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_timer_data_replace(resource timer, array data)
   Replace timer data with new one */
static PHP_FUNCTION(pinba_timer_data_replace)
{
	zval *timer, *data;
	pinba_timer_t *t;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ra!", &timer, &data) != SUCCESS) {
		return;
	}
	
	PHP_ZVAL_TO_TIMER(timer, t);

	if (Z_TYPE_P(data) == IS_NULL) {
		/* reset */
		if (t->data) {
			zval_ptr_dtor(&t->data);
			t->data = NULL;
		}
	} else {
		if (t->data) {
			zval_ptr_dtor(&t->data);
			t->data = NULL;
		}

		MAKE_STD_ZVAL(t->data);
		*(t->data) = *data;
		zval_copy_ctor(t->data);
		INIT_PZVAL(t->data);
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_timer_tags_merge(resource timer, array tags)
   Merge timer data with new data */
static PHP_FUNCTION(pinba_timer_tags_merge)
{
	zval *timer, *tags;
	pinba_timer_t *t;
	pinba_timer_tag_t **new_tags;
	int i, j, tags_num;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ra", &timer, &tags) != SUCCESS) {
		return;
	}

	PHP_ZVAL_TO_TIMER(timer, t);

	tags_num = zend_hash_num_elements(Z_ARRVAL_P(tags));

	if (!tags_num) {
		RETURN_TRUE;
	}

	if (php_pinba_array_to_tags(tags, &new_tags TSRMLS_CC) != SUCCESS) {
		RETURN_FALSE;
	}

	for (i = 0; i < tags_num; i++) {
		int found = 0;
		for (j = 0; j < t->tags_num; j++) {
			if (t->tags[j]->name_len == new_tags[i]->name_len && memcmp(t->tags[j]->name, new_tags[i]->name, new_tags[i]->name_len) == 0) {
				found = 1;
				break;
			}
		}
		if (found == 1) {
			/* replace */
			efree(t->tags[j]->value);
			t->tags[j]->value = estrndup(new_tags[i]->value, new_tags[i]->value_len);
			t->tags[j]->value_len = new_tags[i]->value_len;
		} else {
			/* add */
			pinba_timer_tag_t *tag;

			t->tags = (pinba_timer_tag_t **)erealloc(t->tags, (t->tags_num + 1) * sizeof(pinba_timer_tag_t *));
			tag = (pinba_timer_tag_t *)emalloc(sizeof(pinba_timer_tag_t));
			tag->value = estrndup(new_tags[i]->value, new_tags[i]->value_len);
			tag->value_len = new_tags[i]->value_len;
			tag->name = estrndup(new_tags[i]->name, new_tags[i]->name_len);
			tag->name_len = new_tags[i]->name_len;
			t->tags[t->tags_num] = tag;
			t->tags_num++;
		}
	}

	php_pinba_timer_tags_dtor(new_tags, tags_num);
	efree(new_tags);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_timer_tags_replace(resource timer, array tags)
   Replace timer data with new one */
static PHP_FUNCTION(pinba_timer_tags_replace)
{
	zval *timer, *tags;
	pinba_timer_t *t;
	pinba_timer_tag_t **new_tags;
	int tags_num;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ra", &timer, &tags) != SUCCESS) {
		return;
	}
	
	PHP_ZVAL_TO_TIMER(timer, t);

	tags_num = zend_hash_num_elements(Z_ARRVAL_P(tags));

	if (!tags_num) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "tags array cannot be empty");
		RETURN_TRUE;
	}

	if (php_pinba_array_to_tags(tags, &new_tags TSRMLS_CC) != SUCCESS) {
		RETURN_FALSE;
	}
	
	php_pinba_timer_tags_dtor(t->tags, t->tags_num);
	efree(t->tags);
	t->tags = new_tags;
	t->tags_num = tags_num;

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_flush([string custom_script_name[, int flags]])
   Flush the data */
static PHP_FUNCTION(pinba_flush)
{
	zval **script_name = NULL;
	long flags = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|Zl", &script_name, &flags) != SUCCESS) {
		return;
	}

	if (script_name && Z_TYPE_PP(script_name) != IS_NULL) {
		convert_to_string_ex(script_name);
		php_pinba_flush_data(Z_STRVAL_PP(script_name), flags TSRMLS_CC);
	} else {
		php_pinba_flush_data(NULL, flags TSRMLS_CC);
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto array pinba_get_info()
   Get request info */
static PHP_FUNCTION(pinba_get_info)
{
	zval *timers, *timer_info;
	struct timeval tmp;
	struct rusage u;
	HashPosition pos;
	zend_rsrc_list_entry *le;
	pinba_timer_t *t;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") != SUCCESS) {
		return;
	}

	array_init(return_value);

#if PHP_MAJOR_VERSION >= 5 
	add_assoc_long_ex(return_value, "mem_peak_usage", sizeof("mem_peak_usage"), zend_memory_peak_usage(1 TSRMLS_CC));
#elif PHP_MAJOR_VERSION == 4 && MEMORY_LIMIT
	add_assoc_long_ex(return_value, "mem_peak_usage", sizeof("mem_peak_usage"), AG(allocated_memory_peak));
#else
	add_assoc_long_ex(return_value, "mem_peak_usage", sizeof("mem_peak_usage"), 0);
#endif

	if (gettimeofday(&tmp, 0) == 0) {
		timersub(&tmp, &(PINBA_G(tmp_req_data).req_start), &tmp);
		add_assoc_double_ex(return_value, "req_time", sizeof("req_time"), timeval_to_float(tmp));
	} else {
		add_assoc_double_ex(return_value, "req_time", sizeof("req_time"), 0);
	}

	if (getrusage(RUSAGE_SELF, &u) == 0) {
		timersub(&u.ru_utime, &(PINBA_G(tmp_req_data).ru_utime), &tmp);
		add_assoc_double_ex(return_value, "ru_utime", sizeof("ru_utime"), timeval_to_float(tmp));
		timersub(&u.ru_stime, &(PINBA_G(tmp_req_data).ru_stime), &tmp);
		add_assoc_double_ex(return_value, "ru_stime", sizeof("ru_stime"), timeval_to_float(tmp));
	} else {
		add_assoc_double_ex(return_value, "ru_utime", sizeof("ru_utime"), 0);
		add_assoc_double_ex(return_value, "ru_stime", sizeof("ru_stime"), 0);
	}

	add_assoc_long_ex(return_value, "req_count", sizeof("req_count"), ++PINBA_G(tmp_req_data).req_count);
	add_assoc_long_ex(return_value, "doc_size", sizeof("doc_size"), PINBA_G(tmp_req_data).doc_size);

	if (PINBA_G(server_name)) {
		add_assoc_string_ex(return_value, "server_name", sizeof("server_name"), PINBA_G(server_name), 1);
	} else {
		add_assoc_string_ex(return_value, "server_name", sizeof("server_name"), "unknown", 1);
	}

	if (PINBA_G(script_name)) {
		add_assoc_string_ex(return_value, "script_name", sizeof("script_name"), PINBA_G(script_name), 1);
	} else {
		add_assoc_string_ex(return_value, "script_name", sizeof("script_name"), "unknown", 1);
	}

	MAKE_STD_ZVAL(timers);
	array_init(timers);

	for (zend_hash_internal_pointer_reset_ex(&EG(regular_list), &pos);
			zend_hash_get_current_data_ex((&EG(regular_list)), (void **) &le, &pos) == SUCCESS;
			zend_hash_move_forward_ex(&EG(regular_list), &pos)) {
		if (le->type == le_pinba_timer) {
			t = (pinba_timer_t *)le->ptr;

			if (t->deleted) {
				continue;
			}

			MAKE_STD_ZVAL(timer_info);
			php_pinba_get_timer_info(t, timer_info TSRMLS_CC);
			add_next_index_zval(timers, timer_info);
		}
	}
	add_assoc_zval_ex(return_value, "timers", sizeof("timers"), timers);
}
/* }}} */

/* {{{ proto array pinba_timer_get_info(resource timer)
   Get timer data */
static PHP_FUNCTION(pinba_timer_get_info)
{
	zval *timer;
	pinba_timer_t *t;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &timer) != SUCCESS) {
		return;
	}

	PHP_ZVAL_TO_TIMER(timer, t);

	php_pinba_get_timer_info(t, return_value TSRMLS_CC);
}
/* }}} */

/* {{{ proto bool pinba_timers_stop()
   Stop all timers */
static PHP_FUNCTION(pinba_timers_stop)
{
	zend_rsrc_list_entry *le;
	pinba_timer_t *t;
	HashPosition pos;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") != SUCCESS) {
		return;
	}

	for (zend_hash_internal_pointer_reset_ex(&EG(regular_list), &pos);
			zend_hash_get_current_data_ex((&EG(regular_list)), (void **) &le, &pos) == SUCCESS;
			zend_hash_move_forward_ex(&EG(regular_list), &pos)) {
		if (le->type == le_pinba_timer) {
			t = (pinba_timer_t *)le->ptr;
			php_pinba_timer_stop(t);
		}
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_script_name_set(string custom_script_name)
   Set custom script name */
static PHP_FUNCTION(pinba_script_name_set)
{
	char *script_name;
	int script_name_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &script_name, &script_name_len) != SUCCESS) {
		return;
	}

	if (PINBA_G(script_name)) {
		efree(PINBA_G(script_name));
	}

	PINBA_G(script_name) = estrndup(script_name, script_name_len);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_hostname_set(string custom_hostname)
   Set custom hostname */
static PHP_FUNCTION(pinba_hostname_set)
{
	char *hostname;
	int hostname_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &hostname, &hostname_len) != SUCCESS) {
		return;
	}

	if (hostname_len < sizeof(PINBA_G(host_name))) {
		memcpy(PINBA_G(host_name), hostname, hostname_len);
		PINBA_G(host_name)[hostname_len] = '\0';
	} else {
		memcpy(PINBA_G(host_name), hostname, sizeof(PINBA_G(host_name)) - 1);
		PINBA_G(host_name)[sizeof(PINBA_G(host_name))] = '\0';
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ pinba_functions[]
 */
function_entry pinba_functions[] = {
	PHP_FE(pinba_timer_start, NULL)
	PHP_FE(pinba_timer_add, NULL)
	PHP_FE(pinba_timer_stop, NULL)
	PHP_FE(pinba_timer_delete, NULL)
	PHP_FE(pinba_timer_data_merge, NULL)
	PHP_FE(pinba_timer_data_replace, NULL)
	PHP_FE(pinba_timer_tags_merge, NULL)
	PHP_FE(pinba_timer_tags_replace, NULL)
	PHP_FE(pinba_flush, NULL)
	PHP_FE(pinba_get_info, NULL)
	PHP_FE(pinba_timer_get_info, NULL)
	PHP_FE(pinba_timers_stop, NULL)
	PHP_FE(pinba_script_name_set, NULL)
	PHP_FE(pinba_hostname_set, NULL)
	{NULL, NULL, NULL}
};
/* }}} */

static PHP_INI_MH(OnUpdateCollectorAddress) /* {{{ */
{
	char *copy;
	char *new_node;
	char *new_service;

	if (new_value == NULL || new_value[0] == 0) {
		return FAILURE;
	}

	copy = strdup(new_value);
	if (copy == NULL) {
		return FAILURE;
	}

	new_node = NULL;
	new_service = NULL;

	/* '[' <node> ']' [':' <service>] */
	if (copy[0] == '[') {
		char *endptr;

		new_node = copy + 1;

		endptr = strchr(new_node, ']');
		if (endptr == NULL) {
			free(copy);
			return FAILURE;
		}
		*endptr = 0;
		endptr++;

		if (*endptr != ':' && *endptr != 0) {
			free(copy);
			return FAILURE;
		}

		if (*endptr != 0) {
			new_service = endptr + 1;
		}

		if (new_service != NULL && *new_service == 0) {
			new_service = NULL;
		}
	}
	/* <ipv4 node> [':' <service>] */
	else if ((strchr(copy, ':') == NULL) /* no colon */
			|| (strchr(copy, ':') == strrchr(copy, ':'))) { /* exactly one colon */
		char *endptr = strchr(copy, ':');

		if (endptr != NULL) {
			*endptr = 0;
			new_service = endptr + 1;
		}
		new_node = copy;
	}
	/* <ipv6 node> */
	else { /* multiple colons */
		new_node = copy;
	}

	if (PINBA_G(server_host)) {
		free(PINBA_G(server_host));
	}
	if (PINBA_G(server_port)) {
		free(PINBA_G(server_port));
	}

	PINBA_G(server_host) = strdup(new_node);
	if (new_service == NULL) {
		PINBA_G(server_port) = strdup(PINBA_COLLECTOR_DEFAULT_PORT);
	} else {
		PINBA_G(server_port) = strdup(new_service);
	}
	free(copy);

	/* Sets "collector_address", I assume */
	return OnUpdateString(entry, new_value, new_value_length, mh_arg1, mh_arg2, mh_arg3, stage TSRMLS_CC);
}
/* }}} */

/* {{{ PHP_INI
 */
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("pinba.server", NULL, PHP_INI_SYSTEM|PHP_INI_PERDIR, OnUpdateCollectorAddress, collector_address, zend_pinba_globals, pinba_globals)
    STD_PHP_INI_ENTRY("pinba.enabled", "0", PHP_INI_ALL, OnUpdateBool, enabled, zend_pinba_globals, pinba_globals)
PHP_INI_END()
/* }}} */

/* {{{ php_pinba_init_globals
 */
static void php_pinba_init_globals(zend_pinba_globals *globals)
{
	memset(globals, 0, sizeof(*globals));

	globals->timers_stopped = 0;
	globals->server_name = NULL;
	globals->script_name = NULL;
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
static PHP_MINIT_FUNCTION(pinba)
{
	ZEND_INIT_MODULE_GLOBALS(pinba, php_pinba_init_globals, NULL);
	REGISTER_INI_ENTRIES();

	le_pinba_timer = zend_register_list_destructors_ex(php_timer_resource_dtor, NULL, "pinba timer", module_number);

	REGISTER_LONG_CONSTANT("PINBA_FLUSH_ONLY_STOPPED_TIMERS", PINBA_FLUSH_ONLY_STOPPED_TIMERS, CONST_CS | CONST_PERSISTENT);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
static PHP_MSHUTDOWN_FUNCTION(pinba)
{
	UNREGISTER_INI_ENTRIES();

	if (pinba_socket > 0) {
		close(pinba_socket);
	}
	if (PINBA_G(server_host)) {
		free(PINBA_G(server_host));
	}
	if (PINBA_G(server_port)) {
		free(PINBA_G(server_port));
	}
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION
 */
static PHP_RINIT_FUNCTION(pinba)
{
	zval **tmp;
	struct timeval t;
	struct rusage u;

	PINBA_G(timers_stopped) = 0;
	
	if (gettimeofday(&t, 0) == 0) {
		timeval_cvt(&(PINBA_G(tmp_req_data).req_start), &t);
	} else {
		return FAILURE;
	}

	if (getrusage(RUSAGE_SELF, &u) == 0) {
		timeval_cvt(&(PINBA_G(tmp_req_data).ru_utime), &u.ru_utime);
		timeval_cvt(&(PINBA_G(tmp_req_data).ru_stime), &u.ru_stime);
	} else {
		return FAILURE;
	}

	zend_hash_init(&PINBA_G(timers), 10, NULL, php_timer_hash_dtor, 0);

	PINBA_G(tmp_req_data).doc_size = 0;
	PINBA_G(tmp_req_data).mem_peak_usage= 0;

	PINBA_G(server_name) = NULL;
	PINBA_G(script_name) = NULL;

	gethostname(PINBA_G(host_name), sizeof(PINBA_G(host_name)));
	PINBA_G(host_name)[sizeof(PINBA_G(host_name))] = '\0';

#if PHP_MAJOR_VERSION >= 5
	zend_is_auto_global("_SERVER", sizeof("_SERVER") - 1 TSRMLS_CC);
#endif

	if (PG(http_globals)[TRACK_VARS_SERVER] && zend_hash_find(HASH_OF(PG(http_globals)[TRACK_VARS_SERVER]), "SCRIPT_NAME", sizeof("SCRIPT_NAME"), (void **) &tmp) != FAILURE && Z_TYPE_PP(tmp) == IS_STRING && Z_STRLEN_PP(tmp) > 0) {
		PINBA_G(script_name) = estrndup(Z_STRVAL_PP(tmp), Z_STRLEN_PP(tmp));
	}
	
	if (PG(http_globals)[TRACK_VARS_SERVER] && zend_hash_find(HASH_OF(PG(http_globals)[TRACK_VARS_SERVER]), "SERVER_NAME", sizeof("SERVER_NAME"), (void **) &tmp) != FAILURE && Z_TYPE_PP(tmp) == IS_STRING && Z_STRLEN_PP(tmp) > 0) {
		PINBA_G(server_name) = estrndup(Z_STRVAL_PP(tmp), Z_STRLEN_PP(tmp));
	}

	PINBA_G(old_sapi_ub_write) = OG(php_header_write);
	OG(php_header_write) = sapi_ub_write_counter;

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
static PHP_RSHUTDOWN_FUNCTION(pinba)
{
	php_pinba_flush_data(NULL, 0 TSRMLS_CC);

	zend_hash_destroy(&PINBA_G(timers));
	OG(php_header_write) = PINBA_G(old_sapi_ub_write);

	if (PINBA_G(server_name)) {
		efree(PINBA_G(server_name));
	}
	if (PINBA_G(script_name)) {
		efree(PINBA_G(script_name));
	}
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
static PHP_MINFO_FUNCTION(pinba)
{
	string version;

	version = google::protobuf::internal::VersionString(GOOGLE_PROTOBUF_VERSION);

	php_info_print_table_start();
	php_info_print_table_header(2, "Pinba support", "enabled");
	php_info_print_table_row(2, "Extension version", PHP_PINBA_VERSION);
	php_info_print_table_row(2, "Google Protocol Buffers version", version.c_str());
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */

/* {{{ pinba_module_entry
 */
zend_module_entry pinba_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"pinba",
	pinba_functions,
	PHP_MINIT(pinba),
	PHP_MSHUTDOWN(pinba),
	PHP_RINIT(pinba),
	PHP_RSHUTDOWN(pinba),
	PHP_MINFO(pinba),
#if ZEND_MODULE_API_NO >= 20010901
	PHP_PINBA_VERSION,
#endif
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
