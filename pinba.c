/*
 * Authors: Antony Dovgal <tony@daylessday.org>
 *          Florian Forster <ff at octo.it>  (IPv6 support)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

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

#ifdef HAVE_MALLOC_H
# include <malloc.h>
#endif

#include "php_pinba.h"

#include "pinba.pb-c.h"

zend_class_entry *pinba_client_ce;
static zend_object_handlers pinba_client_handlers;

typedef struct {
	zend_object std;
	char **servers;
	int n_servers;
	char *hostname;
	char *server_name;
	char *script_name;
	char *schema;
	size_t request_count;
	size_t document_size;
	size_t memory_peak;
	double rusage[2];
	float request_time;
	unsigned int status;
	size_t memory_footprint;
	HashTable tags;
	HashTable timers;
	pinba_collector collectors[PINBA_COLLECTORS_MAX];
	unsigned int n_collectors;
} pinba_client_t;

ZEND_DECLARE_MODULE_GLOBALS(pinba)

#ifdef COMPILE_DL_PINBA
ZEND_GET_MODULE(pinba)
#endif

static int le_pinba_timer;
int (*old_sapi_ub_write) (const char *, unsigned int TSRMLS_DC);

#if ZEND_MODULE_API_NO > 20020429
#define ONUPDATELONGFUNC OnUpdateLong
#else
#define ONUPDATELONGFUNC OnUpdateInt
#endif

#define PINBA_FLUSH_ONLY_STOPPED_TIMERS (1<<0)
#define PINBA_FLUSH_RESET_DATA (1<<1)
#define PINBA_ONLY_RUNNING_TIMERS (1<<2)

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
#define float_to_timeval(f, t)										\
	do {															\
		(t).tv_sec = (int)(f);										\
		(t).tv_usec = (int)((f - (double)(t).tv_sec) * 1000000.0);	\
	} while(0);

#ifndef timersub
# define timersub(a, b, result)										\
	do {															\
		(result)->tv_sec = (a)->tv_sec - (b)->tv_sec;				\
		(result)->tv_usec = (a)->tv_usec - (b)->tv_usec;			\
		if ((result)->tv_usec < 0) {								\
			--(result)->tv_sec;										\
			(result)->tv_usec += 1000000;							\
		}															\
	} while (0)
#endif

#ifndef timeradd
# define timeradd(a, b, result)										\
	do {															\
		(result)->tv_sec = (a)->tv_sec + (b)->tv_sec;				\
		(result)->tv_usec = (a)->tv_usec + (b)->tv_usec;			\
		if ((result)->tv_usec >= 1000000) {							\
			(result)->tv_sec++;										\
			(result)->tv_usec -= 1000000;							\
		}															\
	} while (0)
#endif

static int php_pinba_key_compare(const void *a, const void *b TSRMLS_DC);

/* {{{ internal funcs */

static inline pinba_collector* php_pinba_collector_add(pinba_collector *collectors, unsigned int *n_collectors) /* {{{ */
{
	if (*n_collectors >= PINBA_COLLECTORS_MAX) {
		return NULL;
	}

	return &collectors[(*n_collectors)++];
}
/* }}} */

static inline void php_pinba_cleanup_collectors(pinba_collector *collectors, unsigned int *n_collectors) /* {{{ */
{
	int i;

	for (i = 0; i < *n_collectors; i++) {
		pinba_collector *collector = &collectors[i];

		if (collector->fd >= 0) {
			close(collector->fd);
		}

		if (collector->host) {
			free(collector->host);
		}

		if (collector->port) {
			free(collector->port);
		}
	}
	*n_collectors = 0;
}
/* }}} */

static inline int php_pinba_parse_server(char *address, char **host, char **port) /* {{{ */
{
	char *new_node, *new_service = NULL;

	*host = *port = NULL;

	if (address[0] == 0) { /* empty string, just skip */
		return FAILURE;
	}

	/* '[' <node> ']' [':' <service>] */
	if (address[0] == '[') {
		char *endptr;

		new_node = address + 1;

		endptr = strchr(new_node, ']');
		if (endptr == NULL) {
			return FAILURE;
		}
		*endptr = 0;
		endptr++;

		if (*endptr != ':' && *endptr != 0) {
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
	else if ((strchr(address, ':') == NULL) /* no colon */
			|| (strchr(address, ':') == strrchr(address, ':'))) { /* exactly one colon */
		char *endptr = strchr(address, ':');

		if (endptr != NULL) {
			*endptr = 0;
			new_service = endptr + 1;
		}
		new_node = address;
	}
	/* <ipv6 node> */
	else { /* multiple colons */
		new_node = address;
	}
	*host = new_node;
	*port = new_service;
	return SUCCESS;
}
/* }}} */

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

static void php_tag_hash_dtor(void *data) /* {{{ */
{
	char *tag = *(char **)data;

	if (tag) {
		efree(tag);
	}
}
/* }}} */

static int php_pinba_tags_to_hashed_string(pinba_timer_tag_t **tags, int tags_num, char **hashed_tags, int *hashed_tags_len TSRMLS_DC) /* {{{ */
{
	int i;
	char *buf;
	int buf_len, wrote_len;

	*hashed_tags = NULL;
	*hashed_tags_len = 0;

	if (!tags_num) {
		return FAILURE;
	}

	buf_len = 4096;
	wrote_len = 0;
	buf = (char *)emalloc(buf_len + 1);

	for (i = 0; i < tags_num; i++) {
		if (buf_len <= (wrote_len + tags[i]->name_len + 2 + tags[i]->value_len + 1)) {
			buf_len = wrote_len + tags[i]->name_len + 2 + tags[i]->value_len + 1 + 4096;
			buf = (char *)erealloc(buf, buf_len + 1);
		}
		memcpy(buf + wrote_len, tags[i]->name, tags[i]->name_len);
		wrote_len += tags[i]->name_len;

		memcpy(buf + wrote_len, "=>", 2);
		wrote_len += 2;

		memcpy(buf + wrote_len, tags[i]->value, tags[i]->value_len);
		wrote_len += tags[i]->value_len;

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

	if (!t->deleted && !PINBA_G(in_rshutdown)) {
		if (zend_hash_index_exists(&PINBA_G(timers), t->rsrc_id) == 0) {
			zend_hash_index_update(&PINBA_G(timers), t->rsrc_id, &t, sizeof(pinba_timer_t *), NULL);
		}
	} else {
		php_pinba_timer_dtor(t);
		efree(t);
	}
}
/* }}} */

static int php_pinba_timer_stop_helper(zend_rsrc_list_entry *le, void *arg TSRMLS_DC) /* {{{ */
{
	long flags = (long)arg;

	if (le->type == le_pinba_timer) {
		pinba_timer_t *t = (pinba_timer_t *)le->ptr;

		if ((flags & PINBA_FLUSH_ONLY_STOPPED_TIMERS) && t->started) {
			return ZEND_HASH_APPLY_KEEP;
		} else {
			/* remove not looking at the refcount */
			return ZEND_HASH_APPLY_REMOVE;
		}
	}
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

static int sapi_ub_write_counter(const char *str, unsigned int length TSRMLS_DC) /* {{{ */
{
	PINBA_G(tmp_req_data).doc_size += length;
#if PHP_VERSION_ID < 50400
	return PINBA_G(old_sapi_ub_write)(str, length TSRMLS_CC);
#else
	return old_sapi_ub_write(str, length TSRMLS_CC);
#endif
}
/* }}} */

static int php_pinba_init_socket(pinba_collector *collectors, int n_collectors TSRMLS_DC) /* {{{ */
{
	struct addrinfo *ai_list;
	struct addrinfo *ai_ptr;
	struct addrinfo  ai_hints;
	int i;
	int fd;
	int status;
	int n_fds;

	if (n_collectors == 0) {
		return FAILURE;
	}

	n_fds = 0;
	for (i = 0; i < n_collectors; i++) {
		pinba_collector *collector = &collectors[i];

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
		status = getaddrinfo(collector->host, collector->port, &ai_hints, &ai_list);
		if (status != 0) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "failed to resolve Pinba server hostname '%s': %s", collector->host, gai_strerror(status));
			fd = -1; /* need to put -1 into collector->fd */
		}
		else {
			for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next) {
				fd = socket(ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
				if (fd >= 0) {
					break;
				}
			}
		}

		if (collector->fd >= 0) {
			close(collector->fd);
		}
		collector->fd = fd;

		if (fd < 0) {
			continue; /* skip this one in case others are good */
		}

		memcpy(&collector->sockaddr, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		collector->sockaddr_len = ai_ptr->ai_addrlen;

		freeaddrinfo(ai_list);

		n_fds++;
	}

	return (n_fds > 0) ? SUCCESS : FAILURE;
} /* }}} */

static inline int php_pinba_dict_find_or_add(HashTable *ht, char *word, int word_len) /* {{{ */
{
	int *id, cnt;

	if (zend_hash_find(ht, word, word_len + 1, (void **)&id) != SUCCESS) {
		cnt = zend_hash_num_elements(ht);

		if (zend_hash_add(ht, word, word_len + 1, &cnt, sizeof(int), NULL) != SUCCESS) {
			return -1;
		}
		return cnt;
	}
	return *id;
}
/* }}} */

static inline char *_pinba_fetch_global_var(char *name, int name_size TSRMLS_DC) /* {{{ */
{
	char *res;
	zval **tmp;

	if (PG(http_globals)[TRACK_VARS_SERVER] &&
			zend_hash_find(HASH_OF(PG(http_globals)[TRACK_VARS_SERVER]), name, name_size, (void **) &tmp) != FAILURE &&
			Z_TYPE_PP(tmp) == IS_STRING && Z_STRLEN_PP(tmp) > 0) {

		res = strdup(Z_STRVAL_PP(tmp));
	} else {
		res = strdup("unknown");
	}
	return res;
}
/* }}} */

static inline Pinba__Request *php_create_pinba_packet(pinba_client_t *client, const char *custom_script_name, int flags TSRMLS_DC) /* {{{ */
{
	HashTable dict, *tags, *timers, timers_uniq;
	HashPosition pos;
	Pinba__Request *request;
	char hostname[256], **tag_value;
	pinba_req_data *req_data = &PINBA_G(tmp_req_data);
	int timers_num, tags_cnt, *tag_ids, *tag_value_ids, i, n, *id;

	request = malloc(sizeof(Pinba__Request));
	if (!request) {
		return NULL;
	}

	pinba__request__init(request);

	if (client) {
		request->memory_peak = client->memory_peak;
		request->request_count = client->request_count;
		request->document_size = client->document_size;
		request->request_time = client->request_time;
		request->ru_utime = client->rusage[0];
		request->ru_stime = client->rusage[1];
		request->status = client->status;
		request->has_status = 1;
		request->memory_footprint = client->memory_footprint;
		request->has_memory_footprint = 1;

		if (client->schema && client->schema[0] != '\0') {
			request->schema = strdup(client->schema);
		}

		if (client->hostname) {
			request->hostname = strdup(client->hostname);
		} else {
			gethostname(hostname, sizeof(hostname));
			hostname[sizeof(hostname) - 1] = '\0';
			request->hostname = strdup(hostname);
		}

		if (client->server_name) {
			request->server_name = strdup(client->server_name);
		} else {
			request->server_name = _pinba_fetch_global_var("SERVER_NAME", sizeof("SERVER_NAME") TSRMLS_CC);
		}

		if (custom_script_name) {
			request->script_name = strdup(custom_script_name);
		} else if (client->script_name) {
			request->script_name = strdup(client->script_name);
		} else {
			request->script_name = _pinba_fetch_global_var("SCRIPT_NAME", sizeof("SCRIPT_NAME") TSRMLS_CC);
		}

		tags = &client->tags;
		timers = &client->timers;
	} else {
		struct timeval ru_utime, ru_stime;
		struct rusage u;

#if PHP_MAJOR_VERSION >= 5
		req_data->mem_peak_usage = zend_memory_peak_usage(1 TSRMLS_CC);
#elif PHP_MAJOR_VERSION == 4 && MEMORY_LIMIT
		req_data->mem_peak_usage = AG(allocated_memory_peak);
#else
		/* no data available */
		req_data->mem_peak_usage = 0;
#endif

		request->memory_peak = req_data->mem_peak_usage;
		request->request_count = req_data->req_count;
		request->document_size = req_data->doc_size;

		if (PINBA_G(request_time) > 0) {
			request->request_time = PINBA_G(request_time);
		} else {
			struct timeval request_finish, req_time;

			gettimeofday(&request_finish, 0);
			timersub(&request_finish, &req_data->req_start, &req_time);
			request->request_time = timeval_to_float(req_time);
		}

		if (getrusage(RUSAGE_SELF, &u) == 0) {
			timersub(&u.ru_utime, &req_data->ru_utime, &ru_utime);
			timersub(&u.ru_stime, &req_data->ru_stime, &ru_stime);
		}
		request->ru_utime = timeval_to_float(ru_utime);
		request->ru_stime = timeval_to_float(ru_stime);

		request->status = SG(sapi_headers).http_response_code;
		request->has_status = 1;
#if defined(HAVE_MALLOC_H) && defined(HAVE_MALLINFO)
		{
			struct mallinfo info;

			info = mallinfo();
			request->memory_footprint = info.arena + info.hblkhd;
		}
#else
		request->memory_footprint = 0;
#endif
		request->has_memory_footprint = 1;

		if (PINBA_G(schema)[0] != '\0') {
			request->schema = strdup(PINBA_G(schema));
		}

		if (PINBA_G(host_name)[0] != '\0') {
			request->hostname = strdup(PINBA_G(host_name));
		} else {
			gethostname(hostname, sizeof(hostname));
			hostname[sizeof(hostname) - 1] = '\0';
			request->hostname = strdup(hostname);
		}

		if (PINBA_G(server_name)) {
			request->server_name = strdup(PINBA_G(server_name));
		} else {
			request->server_name = _pinba_fetch_global_var("SERVER_NAME", sizeof("SERVER_NAME") TSRMLS_CC);
		}

		if (custom_script_name) {
			request->script_name = strdup(custom_script_name);
		} else if (PINBA_G(script_name)) {
			request->script_name = strdup(PINBA_G(script_name));
		} else {
			request->script_name = _pinba_fetch_global_var("SCRIPT_NAME", sizeof("SCRIPT_NAME") TSRMLS_CC);
		}

		tags = &PINBA_G(tags);
		timers = &PINBA_G(timers);
	}

	zend_hash_init(&dict, 10, NULL, NULL, 0);
	tags_cnt = zend_hash_num_elements(tags);

	if (tags_cnt) {
		int tag_num = 0;

		zend_hash_sort(tags, zend_qsort, php_pinba_key_compare, 0 TSRMLS_CC);

		tag_ids = ecalloc(tags_cnt, sizeof(int));
		tag_value_ids = ecalloc(tags_cnt, sizeof(int));

		for (zend_hash_internal_pointer_reset_ex(tags, &pos);
				zend_hash_get_current_data_ex(tags, (void **) &tag_value, &pos) == SUCCESS;
				zend_hash_move_forward_ex(tags, &pos)) {
			char *key = NULL;
			uint key_len = 0;
			ulong dummy = 0;
			int word_id;

			word_id = php_pinba_dict_find_or_add(&dict, *tag_value, strlen(*tag_value));
			if (word_id < 0) {
				continue;
			}

			tag_value_ids[tag_num] = word_id;

			if (zend_hash_get_current_key_ex(tags, &key, &key_len, &dummy, 0, &pos) == HASH_KEY_IS_STRING) {
				word_id = php_pinba_dict_find_or_add(&dict, key, key_len - 1);
				if (word_id < 0) {
					continue;
				}
				tag_ids[tag_num] = word_id;
			}
			tag_num++;
		}
		tags_cnt = tag_num;
	}

	timers_num = zend_hash_num_elements(timers);
	if (timers_num > 0) {
		pinba_timer_t *t, *old_t, **t_el, **old_t_el;
		char *hashed_tags;
		int hashed_tags_len;

		/* make sure we send aggregated timers to the server */
		zend_hash_init(&timers_uniq, 10, NULL, NULL, 0);

		for (zend_hash_internal_pointer_reset_ex(timers, &pos);
				zend_hash_get_current_data_ex(timers, (void **) &t_el, &pos) == SUCCESS;
				zend_hash_move_forward_ex(timers, &pos)) {
			t = *t_el;

			/* aggregate only stopped timers */
			if ((flags & PINBA_FLUSH_ONLY_STOPPED_TIMERS) != 0 && t->started) {
				continue;
			}

			if (php_pinba_tags_to_hashed_string(t->tags, t->tags_num, &hashed_tags, &hashed_tags_len TSRMLS_CC) != SUCCESS) {
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
				int word_id;

				word_id = php_pinba_dict_find_or_add(&dict, t->tags[i]->name, t->tags[i]->name_len);
				if (word_id < 0) {
					break;
				}
				t->tags[i]->name_id = word_id;

				word_id = php_pinba_dict_find_or_add(&dict, t->tags[i]->value, t->tags[i]->value_len);
				if (word_id < 0) {
					break;
				}
				t->tags[i]->value_id = word_id;
			}
		}
	}

	n = zend_hash_num_elements(&dict);

	request->dictionary = malloc(sizeof(char *) * n);
	if (!request->dictionary) {
		pinba__request__free_unpacked(request, NULL);
		return NULL;
	}

	n = 0;
	for (zend_hash_internal_pointer_reset_ex(&dict, &pos);
			zend_hash_get_current_data_ex(&dict, (void **) &id, &pos) == SUCCESS;
			zend_hash_move_forward_ex(&dict, &pos)) {
		char *str;
		uint str_len;
		ulong dummy;

		if (zend_hash_get_current_key_ex(&dict, &str, &str_len, &dummy, 0, &pos) == HASH_KEY_IS_STRING) {
			request->dictionary[n] = strdup(str);
			n++;
		} else {
			continue;
		}
	}
	zend_hash_destroy(&dict);
	request->n_dictionary = n;

	if (tags_cnt) {
		request->tag_name = malloc(sizeof(unsigned int) * tags_cnt);
		request->n_tag_name = tags_cnt;

		request->tag_value = malloc(sizeof(unsigned int) * tags_cnt);
		request->n_tag_value = tags_cnt;

		for (i = 0; i < tags_cnt; i++) {
			request->tag_name[i] = tag_ids[i];
			request->tag_value[i] = tag_value_ids[i];
		}
		efree(tag_ids);
		efree(tag_value_ids);
	}

	/* timers */
	if (timers_num > 0) {
		pinba_timer_t *t, **t_el;

		n = zend_hash_num_elements(&timers_uniq);
		request->timer_hit_count = malloc(sizeof(unsigned int) * n);
		request->timer_tag_count = malloc(sizeof(unsigned int) * n);
		request->timer_ru_stime = malloc(sizeof(float) * n);
		request->timer_ru_utime = malloc(sizeof(float) * n);
		request->timer_tag_name = NULL;
		request->timer_tag_value = NULL;
		request->timer_value = malloc(sizeof(float) * n);

		if (!request->timer_hit_count || !request->timer_tag_count || !request->timer_value || !request->timer_ru_stime || !request->timer_ru_utime) {
			pinba__request__free_unpacked(request, NULL);
			return NULL;
		}

		n = 0;
		for (zend_hash_internal_pointer_reset_ex(&timers_uniq, &pos);
				zend_hash_get_current_data_ex(&timers_uniq, (void **) &t_el, &pos) == SUCCESS;
				zend_hash_move_forward_ex(&timers_uniq, &pos)) {

			t = *t_el;

			request->timer_tag_name = realloc(request->timer_tag_name, sizeof(unsigned int) * (request->n_timer_tag_name + t->tags_num));
			request->timer_tag_value = realloc(request->timer_tag_value, sizeof(unsigned int) * (request->n_timer_tag_value + t->tags_num));

			if (!request->timer_tag_name || !request->timer_tag_value) {
				pinba__request__free_unpacked(request, NULL);
				return NULL;
			}

			for (i = 0; i < t->tags_num; i++) {
				request->timer_tag_name[request->n_timer_tag_name + i] = t->tags[i]->name_id;
				request->timer_tag_value[request->n_timer_tag_value + i] = t->tags[i]->value_id;
			}

			request->n_timer_tag_name += i;
			request->n_timer_tag_value += i;

			request->timer_tag_count[n] = i;
			request->timer_hit_count[n] = t->hit_count;
			request->timer_value[n] = timeval_to_float(t->value);
			request->timer_ru_utime[n] = timeval_to_float(t->ru_utime);
			request->timer_ru_stime[n] = timeval_to_float(t->ru_stime);
			n++;
		}
		request->n_timer_tag_count = n;
		request->n_timer_hit_count = n;
		request->n_timer_ru_utime = n;
		request->n_timer_ru_stime = n;
		request->n_timer_value = n;
		zend_hash_destroy(&timers_uniq);
	}

	return request;
}
/* }}} */

#define PINBA_PACK(request, data, data_len) \
	unsigned char _pad[256];					\
	ProtobufCBufferSimple _buf = PROTOBUF_C_BUFFER_SIMPLE_INIT (_pad);	\
	ProtobufCBuffer *_buffer = (ProtobufCBuffer *) &_buf;					\
	pinba__request__pack_to_buffer((request), _buffer);					\
	(data) = (char *)_buf.data;											\
	(data_len) = _buf.len;												\

#define PINBA_FREE_BUFFER() \
		PROTOBUF_C_BUFFER_SIMPLE_CLEAR(&_buf)

static inline int php_pinba_req_data_send(pinba_client_t *client, const char *custom_script_name, int flags TSRMLS_DC) /* {{{ */
{
	int ret = SUCCESS;
	Pinba__Request *request;

	request = php_create_pinba_packet(client, custom_script_name, flags TSRMLS_CC);

	if (request) {
		int i, n_collectors, data_len;
		ssize_t sent;
		pinba_collector *collectors;
		char *data;

		PINBA_PACK(request, data, data_len);

		if (client) {
			n_collectors = client->n_collectors;
			collectors = client->collectors;
		} else {
			n_collectors = PINBA_G(n_collectors);
			collectors = PINBA_G(collectors);
		}

		for (i = 0; i < n_collectors; i++) {
			pinba_collector *collector = &collectors[i];

			if (collector->fd < 0) {
				continue;
			}

			sent = sendto(collector->fd, data, data_len, 0, (struct sockaddr *) &collector->sockaddr, collector->sockaddr_len);
			if (sent < data_len) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "failed to send data to Pinba server: %s", strerror(errno));
				ret = FAILURE;
			}
		}

		PINBA_FREE_BUFFER();
		pinba__request__free_unpacked(request, NULL);
	} else {
		ret = FAILURE;
	}

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

	/* stop all running timers */
	zend_hash_apply_with_argument(&EG(regular_list), (apply_func_arg_t) php_pinba_timer_stop_helper, (void *)flags TSRMLS_CC);

	/* prevent any further access to the timers */
	PINBA_G(timers_stopped) = 1;

	if (!PINBA_G(enabled) || PINBA_G(n_collectors) == 0) {
		/* disabled or no collectors defined, exit */
		zend_hash_clean(&PINBA_G(timers));
		PINBA_G(timers_stopped) = 0;
		return;
	}

	if (php_pinba_init_socket(PINBA_G(collectors), PINBA_G(n_collectors) TSRMLS_CC) != SUCCESS) {
		PINBA_G(timers_stopped) = 0;
		return;
	}

	php_pinba_req_data_send(NULL, custom_script_name, flags TSRMLS_CC);

	if (flags & PINBA_FLUSH_RESET_DATA) {
		struct timeval t;
		struct rusage u;

		if (gettimeofday(&t, 0) == 0) {
			timeval_cvt(&(PINBA_G(tmp_req_data).req_start), &t);
		}

		if (getrusage(RUSAGE_SELF, &u) == 0) {
			timeval_cvt(&(PINBA_G(tmp_req_data).ru_utime), &u.ru_utime);
			timeval_cvt(&(PINBA_G(tmp_req_data).ru_stime), &u.ru_stime);
		}

		PINBA_G(tmp_req_data).doc_size = 0;
		PINBA_G(tmp_req_data).mem_peak_usage= 0;
		PINBA_G(tmp_req_data).req_count = 0;
	}

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
		Z_STRVAL(first) = (char *)f->arKey;
		Z_STRLEN(first) = f->nKeyLength - 1;
	}

	if (s->nKeyLength == 0) {
		Z_TYPE(second) = IS_LONG;
		Z_LVAL(second) = s->h;
	} else {
		Z_TYPE(second) = IS_STRING;
		Z_STRVAL(second) = (char *)s->arKey;
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

static int php_pinba_array_to_tags(HashTable *array, pinba_timer_tag_t ***tags TSRMLS_DC) /* {{{ */
{
	int num, i = 0;
	zval **value;
	char *tag_name, *value_str;
	uint tag_name_len, value_str_len;
	ulong dummy;

	num = zend_hash_num_elements(array);
	if (!num) {
		return FAILURE;
	}

	/* sort array, we'll use this when computing tags hash */
	zend_hash_sort(array, zend_qsort, php_pinba_key_compare, 0 TSRMLS_CC);

	*tags = (pinba_timer_tag_t **)ecalloc(num, sizeof(pinba_timer_tag_t *));
	for (zend_hash_internal_pointer_reset(array);
			zend_hash_get_current_data(array, (void **) &value) == SUCCESS;
			zend_hash_move_forward(array)) {

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

		if (zend_hash_get_current_key_ex(array, &tag_name, &tag_name_len, &dummy, 1, NULL) == HASH_KEY_IS_STRING) {
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

static void pinba_client_free_storage(void *object TSRMLS_DC) /* {{{ */
{
	int i;
	pinba_client_t *client = (pinba_client_t *) object;
	zend_object_std_dtor(&client->std TSRMLS_CC);

	if (client->n_servers > 0) {
		for (i = 0; i < client->n_servers; i++) {
			efree(client->servers[i]);
		}
		efree(client->servers);
	}

	php_pinba_cleanup_collectors(client->collectors, &client->n_collectors);

	STR_FREE(client->hostname);
	STR_FREE(client->server_name);
	STR_FREE(client->script_name);
	STR_FREE(client->schema);

	zend_hash_destroy(&client->timers);
	zend_hash_destroy(&client->tags);
	efree(object);
}
/* }}} */

zend_object_value pinba_client_new(zend_class_entry *ce TSRMLS_DC) /* {{{ */
{
	zend_object_value retval;
	pinba_client_t *intern;
#if PHP_VERSION_ID < 50399
	zval *tmp;
#endif

	intern = ecalloc(1, sizeof(pinba_client_t));

	zend_object_std_init(&(intern->std), ce TSRMLS_CC);

#if PHP_VERSION_ID < 50399
	zend_hash_copy(intern->std.properties, &ce->default_properties, (copy_ctor_func_t) zval_add_ref, (void *) &tmp, sizeof(zval *));
#else
	object_properties_init(&intern->std, ce);
#endif

	retval.handle = zend_objects_store_put(intern, (zend_objects_store_dtor_t) zend_objects_destroy_object, (zend_objects_free_object_storage_t)             pinba_client_free_storage, NULL TSRMLS_CC);
	retval.handlers = &pinba_client_handlers;

	zend_hash_init(&intern->timers, 0, NULL, php_timer_hash_dtor, 0);
	zend_hash_init(&intern->tags, 0, NULL, php_tag_hash_dtor, 0);
	return retval;
}
/* }}} */

/* }}} */

/* {{{ proto resource pinba_timer_start(array tags[[, array data], int hit_count])
   Start user timer */
static PHP_FUNCTION(pinba_timer_start)
{
	HashTable *tags_array;
	zval *data = NULL;
	pinba_timer_t *t = NULL;
	pinba_timer_tag_t **tags;
	int tags_num;
	long hit_count = 1;
	struct rusage u;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "h|al", &tags_array, &data, &hit_count) != SUCCESS) {
		return;
	}

	tags_num = zend_hash_num_elements(tags_array);

	if (!tags_num) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "tags array cannot be empty");
		RETURN_FALSE;
	}

	if (hit_count <= 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "hit_count must be greater than 0 (%ld was passed)", hit_count);
		RETURN_FALSE;
	}

	if (php_pinba_array_to_tags(tags_array, &tags TSRMLS_CC) != SUCCESS) {
		RETURN_FALSE;
	}

	t = php_pinba_timer_ctor(tags, tags_num TSRMLS_CC);

	if (data && zend_hash_num_elements(Z_ARRVAL_P(data)) > 0) {
		MAKE_STD_ZVAL(t->data);
		*(t->data) = *data;
		zval_copy_ctor(t->data);
		INIT_PZVAL(t->data);
	}

	t->started = 1;
	t->hit_count = hit_count;

#if PHP_VERSION_ID >= 50400
	t->rsrc_id = zend_list_insert(t, le_pinba_timer TSRMLS_CC);
#else
	t->rsrc_id = zend_list_insert(t, le_pinba_timer);
#endif

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
	HashTable *tags_array;
	zval *data = NULL;
	pinba_timer_t *t = NULL;
	pinba_timer_tag_t **tags;
	int tags_num;
	double value;
	unsigned long time_l;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "hd|a", &tags_array, &value, &data) != SUCCESS) {
		return;
	}

	tags_num = zend_hash_num_elements(tags_array);

	if (!tags_num) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "tags array cannot be empty");
		RETURN_FALSE;
	}

	if (php_pinba_array_to_tags(tags_array, &tags TSRMLS_CC) != SUCCESS) {
		RETURN_FALSE;
	}

	if (value < 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "negative time value passed (%f), changing it to 0", value);
		value = 0;
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

#if PHP_VERSION_ID >= 50400
	t->rsrc_id = zend_list_insert(t, le_pinba_timer TSRMLS_CC);
#else
	t->rsrc_id = zend_list_insert(t, le_pinba_timer);
#endif

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

/* {{{ proto bool pinba_timer_epoch(array tags[, array data])
   Stop user timer as it if it were started at the very beginning */
static PHP_FUNCTION(pinba_timer_epoch)
{
	zval *tags_array, *data = NULL;
	pinba_timer_t *t = NULL;
	pinba_timer_tag_t **tags;
	int tags_num;

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

    /* Create the timer as if it was started from the beginning */
	t = php_pinba_timer_ctor(tags, tags_num TSRMLS_CC);
	t->started = 1;
	t->hit_count = 1;
    timeval_cvt(&t->tmp_ru_utime, &(PINBA_G(tmp_req_data).ru_utime));
    timeval_cvt(&t->tmp_ru_stime, &(PINBA_G(tmp_req_data).ru_stime));
	timeval_cvt(&t->start, &(PINBA_G(tmp_req_data).req_start));

	if (data) {
		MAKE_STD_ZVAL(t->data);
		*(t->data) = *data;
		zval_copy_ctor(t->data);
		INIT_PZVAL(t->data);
	}

#if PHP_VERSION_ID >= 50400
	t->rsrc_id = zend_list_insert(t, le_pinba_timer TSRMLS_CC);
#else
	t->rsrc_id = zend_list_insert(t, le_pinba_timer);
#endif

    /* Stop the timer right now */
	php_pinba_timer_stop(t);

	zend_list_addref(t->rsrc_id);
	RETURN_RESOURCE(t->rsrc_id);
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
	HashTable *tags;
	zval *timer;
	pinba_timer_t *t;
	pinba_timer_tag_t **new_tags;
	int i, j, tags_num;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rh", &timer, &tags) != SUCCESS) {
		return;
	}

	PHP_ZVAL_TO_TIMER(timer, t);

	tags_num = zend_hash_num_elements(tags);

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
	HashTable *tags;
	zval *timer;
	pinba_timer_t *t;
	pinba_timer_tag_t **new_tags;
	int tags_num;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rh", &timer, &tags) != SUCCESS) {
		return;
	}

	PHP_ZVAL_TO_TIMER(timer, t);

	tags_num = zend_hash_num_elements(tags);

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
	zval *timers, *timer_info, *tags;
	struct timeval tmp;
	struct rusage u;
	HashPosition pos;
	zend_rsrc_list_entry *le;
	pinba_timer_t *t;
	char **tag_value;

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

	if (PINBA_G(request_time) > 0) {
		/* use custom request time */
		add_assoc_double_ex(return_value, "req_time", sizeof("req_time"), PINBA_G(request_time));
	} else {
		if (gettimeofday(&tmp, 0) == 0) {
			timersub(&tmp, &(PINBA_G(tmp_req_data).req_start), &tmp);
			add_assoc_double_ex(return_value, "req_time", sizeof("req_time"), timeval_to_float(tmp));
		} else {
			add_assoc_double_ex(return_value, "req_time", sizeof("req_time"), 0);
		}
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

	add_assoc_long_ex(return_value, "req_count", sizeof("req_count"), PINBA_G(tmp_req_data).req_count + 1);
	add_assoc_long_ex(return_value, "doc_size", sizeof("doc_size"), PINBA_G(tmp_req_data).doc_size);

	if (PINBA_G(schema)) {
		add_assoc_string_ex(return_value, "schema", sizeof("schema"), PINBA_G(schema), 1);
	} else {
		add_assoc_string_ex(return_value, "schema", sizeof("schema"), (char *)"unknown", 1);
	}

	if (PINBA_G(server_name)) {
		add_assoc_string_ex(return_value, "server_name", sizeof("server_name"), PINBA_G(server_name), 1);
	} else {
		add_assoc_string_ex(return_value, "server_name", sizeof("server_name"), (char *)"unknown", 1);
	}

	if (PINBA_G(script_name)) {
		add_assoc_string_ex(return_value, "script_name", sizeof("script_name"), PINBA_G(script_name), 1);
	} else {
		add_assoc_string_ex(return_value, "script_name", sizeof("script_name"), (char *)"unknown", 1);
	}

	add_assoc_string(return_value, "hostname", PINBA_G(host_name), 1);

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

	MAKE_STD_ZVAL(tags);
	array_init(tags);

	for (zend_hash_internal_pointer_reset_ex(&PINBA_G(tags), &pos);
			zend_hash_get_current_data_ex(&PINBA_G(tags), (void **) &tag_value, &pos) == SUCCESS;
			zend_hash_move_forward_ex(&PINBA_G(tags), &pos)) {
		char *key;
		uint key_len;
		ulong dummy;

		if (zend_hash_get_current_key_ex(&PINBA_G(tags), &key, &key_len, &dummy, 0, &pos) == HASH_KEY_IS_STRING) {
			add_assoc_string(tags, key, *tag_value, 1);
		} else {
			continue;
		}
	}
	add_assoc_zval_ex(return_value, "tags", sizeof("tags"), tags);
}
/* }}} */

/* {{{ proto string pinba_get_data([int flags])
    */
static PHP_FUNCTION(pinba_get_data)
{
	Pinba__Request *request;
	long flags = 0;
	char *data;
	int data_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &flags) != SUCCESS) {
		return;
	}

	request = php_create_pinba_packet(NULL, NULL, flags TSRMLS_CC);
	if (!request) {
		RETURN_FALSE;
	}

	PINBA_PACK(request, data, data_len);
	RETVAL_STRINGL(data, data_len, 1);
	PINBA_FREE_BUFFER();
	pinba__request__free_unpacked(request, NULL);
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

/* {{{ proto bool pinba_timers_get()
   Get timers */
static PHP_FUNCTION(pinba_timers_get)
{
	zend_rsrc_list_entry *le;
	HashPosition pos;
	pinba_timer_t *t;
	long flag = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &flag) != SUCCESS) {
		return;
	}

	array_init(return_value);
	for (zend_hash_internal_pointer_reset_ex(&EG(regular_list), &pos);
			zend_hash_get_current_data_ex((&EG(regular_list)), (void **) &le, &pos) == SUCCESS;
			zend_hash_move_forward_ex(&EG(regular_list), &pos)) {
		if (le->type == le_pinba_timer) {
			t = (pinba_timer_t *)le->ptr;
			if ((flag & PINBA_FLUSH_ONLY_STOPPED_TIMERS) && t->started) {
				continue;
			}
			/* refcount++ */
			zend_list_addref(t->rsrc_id);
			add_next_index_resource(return_value, t->rsrc_id);
		}
	}
	return;
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

/* {{{ proto bool pinba_schema_set(string custom_schema)
   Set custom schema */
static PHP_FUNCTION(pinba_schema_set)
{
	char *schema;
	int schema_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &schema, &schema_len) != SUCCESS) {
		return;
	}

	if (schema_len < sizeof(PINBA_G(schema))) {
		memcpy(PINBA_G(schema), schema, schema_len);
		PINBA_G(schema)[schema_len] = '\0';
	} else {
		memcpy(PINBA_G(schema), schema, sizeof(PINBA_G(schema)) - 1);
		PINBA_G(schema)[sizeof(PINBA_G(schema))] = '\0';
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_server_name_set(string custom_server_name)
   Set custom server name */
static PHP_FUNCTION(pinba_server_name_set)
{
	char *server_name;
	int server_name_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &server_name, &server_name_len) != SUCCESS) {
		return;
	}

	if (PINBA_G(server_name)) {
		efree(PINBA_G(server_name));
	}

	PINBA_G(server_name) = estrndup(server_name, server_name_len);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_request_time_set(float time)
   Set custom request time */
static PHP_FUNCTION(pinba_request_time_set)
{
	double time;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "d", &time) != SUCCESS) {
		return;
	}

	if (time < 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "negative request time value passed (%f), changing it to 0", time);
		time = 0;
	}

	PINBA_G(request_time) = time;
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_tag_set(string tag, string value)
   Set request tag */
static PHP_FUNCTION(pinba_tag_set)
{
	char *tag, *value;
	int tag_len, value_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &tag, &tag_len, &value, &value_len) != SUCCESS) {
		return;
	}

	if (tag_len < 1) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "tag name cannot be empty");
		RETURN_FALSE;
	}

	/* store the copy */
	value = estrndup(value, value_len);

	zend_hash_update(&PINBA_G(tags), tag, tag_len + 1, &value, sizeof(char *), NULL);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto string pinba_tag_get(string tag)
   Get previously set request tag value */
static PHP_FUNCTION(pinba_tag_get)
{
	char *tag, **value;
	int tag_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &tag, &tag_len) != SUCCESS) {
		return;
	}

	if (zend_hash_find(&PINBA_G(tags), tag, tag_len + 1, (void **)&value) == FAILURE) {
		RETURN_FALSE;
	}
	RETURN_STRING(*value, 1);
}
/* }}} */

/* {{{ proto bool pinba_tag_delete(string tag)
   Delete previously set request tag */
static PHP_FUNCTION(pinba_tag_delete)
{
	char *tag;
	int tag_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &tag, &tag_len) != SUCCESS) {
		return;
	}

	if (zend_hash_del(&PINBA_G(tags), tag, tag_len + 1) == FAILURE) {
		RETURN_FALSE;
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto array pinba_tags_get(string tag)
   List all request tags */
static PHP_FUNCTION(pinba_tags_get)
{
	char **value;
	HashPosition pos;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") != SUCCESS) {
		return;
	}

	array_init(return_value);
	for (zend_hash_internal_pointer_reset_ex(&PINBA_G(tags), &pos);
			zend_hash_get_current_data_ex(&PINBA_G(tags), (void **) &value, &pos) == SUCCESS;
			zend_hash_move_forward_ex(&PINBA_G(tags), &pos)) {
		char *key;
		uint key_len;
		ulong dummy;

		if (zend_hash_get_current_key_ex(&PINBA_G(tags), &key, &key_len, &dummy, 0, &pos) == HASH_KEY_IS_STRING) {
			add_assoc_string(return_value, key, *value, 1);
		} else {
			continue;
		}
	}
}
/* }}} */


/* {{{ proto PinbaClient::__construct(servers)
    */
static PHP_METHOD(PinbaClient, __construct)
{
	HashTable *servers;
	zval **tmp;
	pinba_collector *new_collector;
	pinba_client_t *client;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "h", &servers) != SUCCESS) {
		return;
	}

	client = (pinba_client_t *)zend_object_store_get_object(getThis() TSRMLS_CC);

	for (zend_hash_internal_pointer_reset(servers);
		 zend_hash_get_current_data(servers, (void **)&tmp) == SUCCESS;
		 zend_hash_move_forward(servers)) {
		char *host, *port;

		convert_to_string_ex(tmp);

		if (php_pinba_parse_server(Z_STRVAL_PP(tmp), &host, &port) != SUCCESS) {
			continue;
		}

		new_collector = php_pinba_collector_add(client->collectors, &client->n_collectors);
		if (new_collector == NULL) {
			break;
		}
		new_collector->host = strdup(host);
		new_collector->port = (port == NULL) ? strdup(PINBA_COLLECTOR_DEFAULT_PORT) : strdup(port);
		new_collector->fd = -1; /* set invalid fd */
	}
}
/* }}} */

#define SET_METHOD_STR(name, attr_name)														\
static PHP_METHOD(PinbaClient, name)														\
{																							\
	pinba_client_t *client;																	\
	char *value;																			\
	int value_len;																			\
																							\
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &value, &value_len) != SUCCESS) { \
		return;																				\
	}																						\
	client = (pinba_client_t *)zend_object_store_get_object(getThis() TSRMLS_CC);			\
																							\
	if (client->attr_name) {																\
		efree(client->attr_name);															\
	}																						\
	client->attr_name = estrndup(value, value_len);											\
	RETURN_TRUE;																			\
}

SET_METHOD_STR(setHostname, hostname);
SET_METHOD_STR(setServername, server_name);
SET_METHOD_STR(setScriptname, script_name);
SET_METHOD_STR(setSchema, schema);

#define SET_METHOD_NUM(name, attr_name, type, type_letter)									\
static PHP_METHOD(PinbaClient, name)														\
{																							\
	pinba_client_t *client;																	\
	type value;																				\
																							\
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, type_letter, &value) != SUCCESS) { \
		return;																				\
	}																						\
	client = (pinba_client_t *)zend_object_store_get_object(getThis() TSRMLS_CC);			\
																							\
	if (value < 0) {																		\
		php_error_docref(NULL TSRMLS_CC, E_WARNING, #attr_name " cannot be less than zero"); \
		RETURN_FALSE;																		\
	}																						\
	client->attr_name = value;																\
	RETURN_TRUE;																			\
}

SET_METHOD_NUM(setRequestCount, request_count, long, "l");
SET_METHOD_NUM(setMemoryFootprint, memory_footprint, long, "l");
SET_METHOD_NUM(setMemoryPeak, memory_peak, long, "l");
SET_METHOD_NUM(setDocumentSize, document_size, long, "l");
SET_METHOD_NUM(setStatus, status, long, "l");
SET_METHOD_NUM(setRequestTime, request_time, double, "d");

/* {{{ proto bool PinbaClient::setRusage(array rusage)
    */
static PHP_METHOD(PinbaClient, setRusage)
{
	HashTable *rusage;
	pinba_client_t *client;
	zval **tmp;
	int i;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "h", &rusage) != SUCCESS) {
		return;
	}
	client = (pinba_client_t *)zend_object_store_get_object(getThis() TSRMLS_CC);

	if (zend_hash_num_elements(rusage) != 2) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "rusage array must contain exactly 2 elements");
		RETURN_FALSE;
	}

	for (zend_hash_internal_pointer_reset(rusage), i = 0;
		 zend_hash_get_current_data(rusage, (void **)&tmp) == SUCCESS && i < 2;
		 zend_hash_move_forward(rusage), i++) {

		convert_to_double_ex(tmp);
		client->rusage[i] = Z_DVAL_PP(tmp);
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool PinbaClient::setTag(string name, string value)
    */
static PHP_METHOD(PinbaClient, setTag)
{
	pinba_client_t *client;
	char *tag, *value;
	int tag_len, value_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &tag, &tag_len, &value, &value_len) != SUCCESS) {
		return;
	}
	client = (pinba_client_t *)zend_object_store_get_object(getThis() TSRMLS_CC);

	/* store the copy */
	value = estrndup(value, value_len);
	zend_hash_update(&client->tags, tag, tag_len + 1, (void **)&value, sizeof(char *), NULL);
	RETURN_TRUE;
}
/* }}} */

static void php_pinba_client_timer_add_set(INTERNAL_FUNCTION_PARAMETERS, int add) /* {{{ */
{
	pinba_client_t *client;
	long hit_count = 1;
	double value, ru_utime = 0, ru_stime = 0;
	HashTable *tags, *rusage = NULL;
	char *hashed_tags;
	int hashed_tags_len, i, tags_num;
	zval **tmp;
	pinba_timer_t *timer;
	pinba_timer_tag_t **new_tags;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "hd|hl", &tags, &value, &rusage, &hit_count) != SUCCESS) {
		return;
	}
	client = (pinba_client_t *)zend_object_store_get_object(getThis() TSRMLS_CC);

	tags_num = zend_hash_num_elements(tags);
	if (!tags_num) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "timer tags array cannot be empty");
		RETURN_FALSE;
	}

	if (value < 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "timer value cannot be less than 0");
		RETURN_FALSE;
	}

	if (hit_count < 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "timer hit count cannot be less than 0");
		RETURN_FALSE;
	}

	if (rusage && zend_hash_num_elements(rusage) != 2) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "rusage array must contain exactly 2 elements");
		RETURN_FALSE;
	}

	if (rusage) {
		for (zend_hash_internal_pointer_reset(rusage), i = 0;
				zend_hash_get_current_data(rusage, (void **)&tmp) == SUCCESS && i < 2;
				zend_hash_move_forward(rusage), i++) {

			convert_to_double_ex(tmp);
			if (i == 0) {
				ru_utime = Z_DVAL_PP(tmp);
			} else {
				ru_stime = Z_DVAL_PP(tmp);
			}
		}
	}

	if (php_pinba_array_to_tags(tags, &new_tags TSRMLS_CC) != SUCCESS) {
		RETURN_FALSE;
	}

	if (php_pinba_tags_to_hashed_string(new_tags, tags_num, &hashed_tags, &hashed_tags_len TSRMLS_CC) != SUCCESS) {
		php_pinba_timer_tags_dtor(new_tags, tags_num);
		efree(new_tags);
		RETURN_FALSE;
	}

	timer = ecalloc(1, sizeof(pinba_timer_t));
	float_to_timeval(value, timer->value);
	float_to_timeval(ru_utime, timer->ru_utime);
	float_to_timeval(ru_stime, timer->ru_stime);
	timer->tags = new_tags;
	timer->tags_num = tags_num;
	timer->hit_count = hit_count;

	if (add) {
		pinba_timer_t *old_t, **old_t_pp;

		if (zend_hash_find(&client->timers, hashed_tags, hashed_tags_len + 1, (void **)&old_t_pp) == SUCCESS) {
			old_t = *old_t_pp;
			timeradd(&old_t->value, &timer->value, &old_t->value);
			timeradd(&old_t->ru_utime, &timer->ru_utime, &old_t->ru_utime);
			timeradd(&old_t->ru_stime, &timer->ru_stime, &old_t->ru_stime);
			if (timer->hit_count) {
				old_t->hit_count += timer->hit_count;
			} else {
				old_t->hit_count++;
			}
			php_pinba_timer_dtor(timer);
			efree(timer);
		} else {
			zend_hash_add(&client->timers, hashed_tags, hashed_tags_len + 1, (void **)&timer, sizeof(pinba_timer_t *), NULL);
		}
	} else {
		zend_hash_update(&client->timers, hashed_tags, hashed_tags_len + 1, (void **)&timer, sizeof(pinba_timer_t *), NULL);
	}
	efree(hashed_tags);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool PinbaClient::setTimer(array tags, float value[, array rusage[, int hit_count]])
    */
static PHP_METHOD(PinbaClient, setTimer)
{
	php_pinba_client_timer_add_set(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ proto bool PinbaClient::addTimer(array tags, float value[, array rusage[, int hit_count]])
    */
static PHP_METHOD(PinbaClient, addTimer)
{
	php_pinba_client_timer_add_set(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ proto bool PinbaClient::send([int flags])
    */
static PHP_METHOD(PinbaClient, send)
{
	pinba_client_t *client;
	long flags = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &flags) != SUCCESS) {
		return;
	}
	client = (pinba_client_t *)zend_object_store_get_object(getThis() TSRMLS_CC);

	if (client->n_collectors == 0) {
		RETURN_FALSE;
	}

	if (php_pinba_init_socket(client->collectors, client->n_collectors TSRMLS_CC) != SUCCESS) {
		RETURN_FALSE;
	}

	php_pinba_req_data_send(client, NULL, flags TSRMLS_CC);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto string PinbaClient::getData([int flags])
    */
static PHP_METHOD(PinbaClient, getData)
{
	pinba_client_t *client;
	Pinba__Request *request;
	long flags = 0;
	char *data;
	int data_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &flags) != SUCCESS) {
		return;
	}
	client = (pinba_client_t *)zend_object_store_get_object(getThis() TSRMLS_CC);

	request = php_create_pinba_packet(client, NULL, flags TSRMLS_CC);
	if (!request) {
		RETURN_FALSE;
	}

	PINBA_PACK(request, data, data_len);
	RETVAL_STRINGL(data, data_len, 1);
	PINBA_FREE_BUFFER();
	pinba__request__free_unpacked(request, NULL);
}
/* }}} */


/* {{{ pinba_functions[]
 */
zend_function_entry pinba_functions[] = {
	PHP_FE(pinba_timer_start, NULL)
	PHP_FE(pinba_timer_add, NULL)
	PHP_FE(pinba_timer_stop, NULL)
	PHP_FE(pinba_timer_epoch, NULL)
	PHP_FE(pinba_timer_delete, NULL)
	PHP_FE(pinba_timer_data_merge, NULL)
	PHP_FE(pinba_timer_data_replace, NULL)
	PHP_FE(pinba_timer_tags_merge, NULL)
	PHP_FE(pinba_timer_tags_replace, NULL)
	PHP_FE(pinba_flush, NULL)
	PHP_FE(pinba_get_info, NULL)
	PHP_FE(pinba_get_data, NULL)
	PHP_FE(pinba_timer_get_info, NULL)
	PHP_FE(pinba_timers_stop, NULL)
	PHP_FE(pinba_timers_get, NULL)
	PHP_FE(pinba_script_name_set, NULL)
	PHP_FE(pinba_hostname_set, NULL)
	PHP_FE(pinba_server_name_set, NULL)
	PHP_FE(pinba_schema_set, NULL)
	PHP_FE(pinba_request_time_set, NULL)
	PHP_FE(pinba_tag_set, NULL)
	PHP_FE(pinba_tag_get, NULL)
	PHP_FE(pinba_tag_delete, NULL)
	PHP_FE(pinba_tags_get, NULL)
	{NULL, NULL, NULL}
};
/* }}} */

/* {{{ arginfo */
ZEND_BEGIN_ARG_INFO_EX(arginfo___construct, 0, 0, 1)
	ZEND_ARG_INFO(0, servers)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_setvalue, 0, 0, 1)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_settag, 0, 0, 2)
	ZEND_ARG_INFO(0, tag)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_settimer, 0, 0, 2)
	ZEND_ARG_INFO(0, tags)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, rusage)
	ZEND_ARG_INFO(0, hit_count)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_send, 0, 0, 0)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_getdata, 0, 0, 0)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ pinba_client_methods[]
 */
zend_function_entry pinba_client_methods[] = {
	PHP_ME(PinbaClient, __construct, arginfo___construct, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setHostname, arginfo_setvalue, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setScriptname, arginfo_setvalue, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setServername, arginfo_setvalue, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setRequestCount, arginfo_setvalue, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setDocumentSize, arginfo_setvalue, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setMemoryPeak, arginfo_setvalue, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setMemoryFootprint, arginfo_setvalue, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setRusage, arginfo_setvalue, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setRequestTime, arginfo_setvalue, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setStatus, arginfo_setvalue, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setSchema, arginfo_setvalue, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setTag, arginfo_settag, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, setTimer, arginfo_settimer, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, addTimer, arginfo_settimer, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, send, arginfo_send, ZEND_ACC_PUBLIC)
	PHP_ME(PinbaClient, getData, arginfo_getdata, ZEND_ACC_PUBLIC)
	{NULL, NULL, NULL}
};
/* }}} */


static PHP_INI_MH(OnUpdateCollectorAddress) /* {{{ */
{
	char *copy; /* copy of the passed value, so that we can mangle it at will */
	char *address; /* address, split off the space separated string */
	char *new_node;
	char *new_service;
	char *tmp;
	pinba_collector *new_collector;

	if (new_value == NULL || new_value[0] == 0) {
		return FAILURE;
	}

	copy = strdup(new_value);
	if (copy == NULL) {
		return FAILURE;
	}

	php_pinba_cleanup_collectors(PINBA_G(collectors), &PINBA_G(n_collectors));

	for (tmp = copy; (address = strsep(&tmp, ", ")); /**/) {
		if (php_pinba_parse_server(address, &new_node, &new_service) != SUCCESS) {
			free(copy);
			return FAILURE;
		}

		new_collector = php_pinba_collector_add(PINBA_G(collectors), &PINBA_G(n_collectors));
		if (new_collector == NULL) {
			/* TODO: log that max collectors has been reached and recompilation is required (will never happen) */
			free(copy);
			return FAILURE;
		}
		new_collector->host = strdup(new_node);
		new_collector->port = (new_service == NULL) ? strdup(PINBA_COLLECTOR_DEFAULT_PORT) : strdup(new_service);
		new_collector->fd = -1; /* set invalid fd */
	}

	free(copy);

	/* Sets "collector_address", I assume */
	return OnUpdateString(entry, new_value, new_value_length, mh_arg1, mh_arg2, mh_arg3, stage TSRMLS_CC);
}
/* }}} */

/* {{{ PHP_INI
 */
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("pinba.server", NULL, PHP_INI_ALL, OnUpdateCollectorAddress, collector_address, zend_pinba_globals, pinba_globals)
    STD_PHP_INI_ENTRY("pinba.enabled", "0", PHP_INI_ALL, OnUpdateBool, enabled, zend_pinba_globals, pinba_globals)
    STD_PHP_INI_ENTRY("pinba.auto_flush", "1", PHP_INI_ALL, OnUpdateBool, auto_flush, zend_pinba_globals, pinba_globals)
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
	zend_class_entry ce;

	ZEND_INIT_MODULE_GLOBALS(pinba, php_pinba_init_globals, NULL);
	REGISTER_INI_ENTRIES();

	le_pinba_timer = zend_register_list_destructors_ex(php_timer_resource_dtor, NULL, "pinba timer", module_number);

	REGISTER_LONG_CONSTANT("PINBA_FLUSH_ONLY_STOPPED_TIMERS", PINBA_FLUSH_ONLY_STOPPED_TIMERS, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("PINBA_FLUSH_RESET_DATA", PINBA_FLUSH_RESET_DATA, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("PINBA_ONLY_STOPPED_TIMERS", PINBA_FLUSH_ONLY_STOPPED_TIMERS, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("PINBA_ONLY_RUNNING_TIMERS", PINBA_ONLY_RUNNING_TIMERS, CONST_CS | CONST_PERSISTENT);

#if PHP_VERSION_ID >= 50400
	old_sapi_ub_write = sapi_module.ub_write;
	sapi_module.ub_write = sapi_ub_write_counter;
#endif

	INIT_CLASS_ENTRY(ce, "PinbaClient", pinba_client_methods);
	pinba_client_ce = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	pinba_client_ce->create_object = pinba_client_new;

	memcpy(&pinba_client_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	pinba_client_handlers.clone_obj = NULL;
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
static PHP_MSHUTDOWN_FUNCTION(pinba)
{
	UNREGISTER_INI_ENTRIES();

	php_pinba_cleanup_collectors(PINBA_G(collectors), &PINBA_G(n_collectors));
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
	PINBA_G(in_rshutdown) = 0;
	PINBA_G(request_time) = 0;

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
	zend_hash_init(&PINBA_G(tags), 10, NULL, php_tag_hash_dtor, 0);

	PINBA_G(tmp_req_data).doc_size = 0;
	PINBA_G(tmp_req_data).mem_peak_usage= 0;

	PINBA_G(server_name) = NULL;
	PINBA_G(script_name) = NULL;

	gethostname(PINBA_G(host_name), sizeof(PINBA_G(host_name)));
	PINBA_G(host_name)[sizeof(PINBA_G(host_name)) - 1] = '\0';

#if PHP_MAJOR_VERSION >= 5
	zend_is_auto_global("_SERVER", sizeof("_SERVER") - 1 TSRMLS_CC);
#endif

	if (PG(http_globals)[TRACK_VARS_SERVER] && zend_hash_find(HASH_OF(PG(http_globals)[TRACK_VARS_SERVER]), "SCRIPT_NAME", sizeof("SCRIPT_NAME"), (void **) &tmp) != FAILURE && Z_TYPE_PP(tmp) == IS_STRING && Z_STRLEN_PP(tmp) > 0) {
		PINBA_G(script_name) = estrndup(Z_STRVAL_PP(tmp), Z_STRLEN_PP(tmp));
	}

	if (PG(http_globals)[TRACK_VARS_SERVER] && zend_hash_find(HASH_OF(PG(http_globals)[TRACK_VARS_SERVER]), "SERVER_NAME", sizeof("SERVER_NAME"), (void **) &tmp) != FAILURE && Z_TYPE_PP(tmp) == IS_STRING && Z_STRLEN_PP(tmp) > 0) {
		PINBA_G(server_name) = estrndup(Z_STRVAL_PP(tmp), Z_STRLEN_PP(tmp));
	}

#if PHP_VERSION_ID < 50400
	if (OG(php_header_write) != sapi_ub_write_counter) {
		PINBA_G(old_sapi_ub_write) = OG(php_header_write);
		OG(php_header_write) = sapi_ub_write_counter;
	}
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
static PHP_RSHUTDOWN_FUNCTION(pinba)
{
	if (PINBA_G(auto_flush)) {
		php_pinba_flush_data(NULL, 0 TSRMLS_CC);
	}

	zend_hash_destroy(&PINBA_G(timers));
	zend_hash_destroy(&PINBA_G(tags));

#if PHP_VERSION_ID < 50400
	OG(php_header_write) = PINBA_G(old_sapi_ub_write);
#endif

	if (PINBA_G(server_name)) {
		efree(PINBA_G(server_name));
		PINBA_G(server_name) = NULL;
	}
	if (PINBA_G(script_name)) {
		efree(PINBA_G(script_name));
		PINBA_G(script_name) = NULL;
	}
	PINBA_G(in_rshutdown) = 1;
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
static PHP_MINFO_FUNCTION(pinba)
{

	php_info_print_table_start();
	php_info_print_table_header(2, "Pinba support", "enabled");
	php_info_print_table_row(2, "Extension version", PHP_PINBA_VERSION);
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
