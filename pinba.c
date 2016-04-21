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
	long flags;
	int collectors_initialized:1;
	int data_sent:1;
	zend_object std;
} pinba_client_t;

ZEND_DECLARE_MODULE_GLOBALS(pinba)

#ifdef COMPILE_DL_PINBA
ZEND_GET_MODULE(pinba)
#endif

static int le_pinba_timer;
size_t (*old_sapi_ub_write) (const char *, size_t);

#if ZEND_MODULE_API_NO > 20020429
#define ONUPDATELONGFUNC OnUpdateLong
#else
#define ONUPDATELONGFUNC OnUpdateInt
#endif

#define PINBA_FLUSH_ONLY_STOPPED_TIMERS (1<<0)
#define PINBA_FLUSH_RESET_DATA (1<<1)
#define PINBA_ONLY_RUNNING_TIMERS (1<<2)
#define PINBA_AUTO_FLUSH (1<<3)

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
	zval data;
	struct timeval tmp_ru_utime;
	struct timeval tmp_ru_stime;
	struct timeval ru_utime;
	struct timeval ru_stime;
	unsigned deleted:1;
} pinba_timer_t;
/* }}} */

#define PHP_ZVAL_TO_TIMER(zval, timer) \
	            timer = (pinba_timer_t *)zend_fetch_resource(Z_RES_P(zval), "pinba timer", le_pinba_timer);	\
				if (!timer) {																		\
					RETURN_FALSE;																	\
				}

static inline pinba_client_t  *php_pinba_client_object(zend_object *obj) {
	return (pinba_client_t *)((char*)(obj) - XtOffsetOf(pinba_client_t, std));
}

#define Z_PINBACLIENT_P(zv) php_pinba_client_object(Z_OBJ_P(zv))

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

static int php_pinba_key_compare(const void *a, const void *b);

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

static void php_timer_hash_dtor(zval *zv) /* {{{ */
{
	pinba_timer_t *t = Z_PTR_P(zv);

	if (t) {
		php_pinba_timer_dtor(t);
		efree(t);
	}
}
/* }}} */

static void php_tag_hash_dtor(zval *zv) /* {{{ */
{
	char *tag = Z_PTR_P(zv);

	if (tag) {
		efree(tag);
	}
}
/* }}} */

static int php_pinba_tags_to_hashed_string(pinba_timer_tag_t **tags, int tags_num, char **hashed_tags, size_t *hashed_tags_len) /* {{{ */
{
	int i;
	char *buf;
	size_t buf_len, wrote_len;

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

static void php_timer_resource_dtor(zend_resource *entry) /* {{{ */
{
	pinba_timer_t *t = (pinba_timer_t *)entry->ptr;

	php_pinba_timer_stop(t);
	/* php_pinba_timer_dtor(t); all timers are destroyed at once */

	/* but we don't need the user data anymore */
	if (!Z_ISUNDEF(t->data)) {
		zval_ptr_dtor(&t->data);
	}

	if (!t->deleted && !PINBA_G(in_rshutdown)) {
		/* do nothing */
	} else {
		php_pinba_timer_dtor(t);
		efree(t);
	}
}
/* }}} */

static int php_pinba_timer_stop_helper(zval *zv, void *arg) /* {{{ */
{
	long flags = (long)arg;

	if (Z_RES_TYPE_P(zv) == le_pinba_timer) {
		pinba_timer_t *t = (pinba_timer_t *)Z_RES_VAL_P(zv);

		if (t->deleted || ((flags & PINBA_FLUSH_ONLY_STOPPED_TIMERS) != 0 && t->started)) {
			return ZEND_HASH_APPLY_KEEP;
		} else {
			php_pinba_timer_stop(t);
			t->deleted = 1; /* ignore next time */
		}

		if (zend_hash_index_exists(&PINBA_G(timers), t->rsrc_id) == 0) {
			zend_hash_index_update_ptr(&PINBA_G(timers), t->rsrc_id, t);
		}
		return ZEND_HASH_APPLY_KEEP;

	}
	return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

static size_t sapi_ub_write_counter(const char *str, size_t length) /* {{{ */
{
	PINBA_G(tmp_req_data).doc_size += length;
#if PHP_VERSION_ID < 50400
	return PINBA_G(old_sapi_ub_write)(str, length);
#else
	return old_sapi_ub_write(str, length);
#endif
}
/* }}} */

static int php_pinba_init_socket(pinba_collector *collectors, int n_collectors) /* {{{ */
{
	struct addrinfo *ai_list;
	struct addrinfo *ai_ptr = NULL;
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
		fd = -1; /* need to put -1 into collector->fd */
		if (status != 0) {
			php_error_docref(NULL, E_WARNING, "failed to resolve Pinba server hostname '%s': %s", collector->host, gai_strerror(status));
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

static inline int php_pinba_dict_find_or_add(HashTable *ht, char *word, size_t word_len) /* {{{ */
{
	size_t id, cnt;

	id = (size_t)zend_hash_str_find_ptr(ht, word, word_len);
	if (!id) {
		cnt = zend_hash_num_elements(ht) + 1;

		if (zend_hash_str_add_ptr(ht, word, word_len, (void *)cnt) == NULL) {
			return -1;
		}
		return cnt - 1;
	}
	return id - 1;
}
/* }}} */

static inline char *_pinba_fetch_global_var(char *name, int name_len) /* {{{ */
{
	char *res;
	zval *tmp;

	if (!PINBA_G(in_rshutdown) && (Z_TYPE(PG(http_globals)[TRACK_VARS_SERVER]) == IS_ARRAY || zend_is_auto_global_str(ZEND_STRL("_SERVER")))) {
		tmp = zend_hash_str_find(HASH_OF(&PG(http_globals)[TRACK_VARS_SERVER]), name, name_len);
		if (tmp && Z_TYPE_P(tmp) == IS_STRING && Z_STRLEN_P(tmp) > 0) {
			res = strdup(Z_STRVAL_P(tmp));
			return res;
		}
	}

	res = strdup("unknown");
	return res;
}
/* }}} */

static inline Pinba__Request *php_create_pinba_packet(pinba_client_t *client, const char *custom_script_name, int flags) /* {{{ */
{
	HashTable dict, *tags, *timers, timers_uniq;
	HashPosition pos;
	Pinba__Request *request;
	char hostname[256], *tag_value;
	pinba_req_data *req_data = &PINBA_G(tmp_req_data);
	int timers_num, tags_cnt, *tag_ids = NULL, *tag_value_ids = NULL, i, n;
	size_t id;

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
			request->server_name = _pinba_fetch_global_var("SERVER_NAME", sizeof("SERVER_NAME")-1);
		}

		if (custom_script_name) {
			request->script_name = strdup(custom_script_name);
		} else if (client->script_name) {
			request->script_name = strdup(client->script_name);
		} else {
			request->script_name = _pinba_fetch_global_var("SCRIPT_NAME", sizeof("SCRIPT_NAME")-1);
		}

		tags = &client->tags;
		timers = &client->timers;
	} else {
		struct timeval ru_utime = {0, 0}, ru_stime = {0, 0};
		struct rusage u;

#if PHP_MAJOR_VERSION >= 5
		req_data->mem_peak_usage = zend_memory_peak_usage(1);
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
			request->server_name = _pinba_fetch_global_var("SERVER_NAME", sizeof("SERVER_NAME")-1);
		}

		if (custom_script_name) {
			request->script_name = strdup(custom_script_name);
		} else if (PINBA_G(script_name)) {
			request->script_name = strdup(PINBA_G(script_name));
		} else {
			request->script_name = _pinba_fetch_global_var("SCRIPT_NAME", sizeof("SCRIPT_NAME")-1);
		}

		tags = &PINBA_G(tags);
		timers = &PINBA_G(timers);
	}

	zend_hash_init(&dict, 10, NULL, NULL, 0);
	tags_cnt = zend_hash_num_elements(tags);

	if (tags_cnt) {
		int tag_num = 0;

		zend_hash_sort(tags, php_pinba_key_compare, 0);

		tag_ids = ecalloc(tags_cnt, sizeof(int));
		tag_value_ids = ecalloc(tags_cnt, sizeof(int));

		for (zend_hash_internal_pointer_reset_ex(tags, &pos);
				(tag_value = zend_hash_get_current_data_ptr_ex(tags, &pos)) != NULL;
				zend_hash_move_forward_ex(tags, &pos)) {
			zend_string *key = NULL;
			zend_ulong num_key;
			int word_id;

			word_id = php_pinba_dict_find_or_add(&dict, tag_value, strlen(tag_value));
			if (word_id < 0) {
				continue;
			}

			tag_value_ids[tag_num] = word_id;

			if (zend_hash_get_current_key_ex(tags, &key, &num_key, &pos) == HASH_KEY_IS_STRING) {
				word_id = php_pinba_dict_find_or_add(&dict, key->val, key->len);
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
		pinba_timer_t *t, *old_t;
		char *hashed_tags;
		size_t hashed_tags_len;

		/* make sure we send aggregated timers to the server */
		zend_hash_init(&timers_uniq, 10, NULL, NULL, 0);

		for (zend_hash_internal_pointer_reset_ex(timers, &pos);
				(t = zend_hash_get_current_data_ptr_ex(timers, &pos)) != NULL;
				zend_hash_move_forward_ex(timers, &pos)) {

			/* aggregate only stopped timers */
			if ((flags & PINBA_FLUSH_ONLY_STOPPED_TIMERS) != 0 && t->started) {
				continue;
			}

			if (php_pinba_tags_to_hashed_string(t->tags, t->tags_num, &hashed_tags, &hashed_tags_len) != SUCCESS) {
				continue;
			}

			old_t = zend_hash_str_find_ptr(&timers_uniq, hashed_tags, hashed_tags_len);
			if (old_t != NULL) {
				timeradd(&old_t->value, &t->value, &old_t->value);
				old_t->hit_count++;
			} else {
				zend_hash_str_add_ptr(&timers_uniq, hashed_tags, hashed_tags_len, t);
			}
			efree(hashed_tags);
		}

		/* create our temporary dictionary and add ids to timers */
		for (zend_hash_internal_pointer_reset_ex(&timers_uniq, &pos);
				(t = zend_hash_get_current_data_ptr_ex(&timers_uniq, &pos)) != NULL;
				zend_hash_move_forward_ex(&timers_uniq, &pos)) {
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
			(id = (size_t)zend_hash_get_current_data_ex(&dict, &pos) != 0);
			zend_hash_move_forward_ex(&dict, &pos)) {
		zend_string *str;
		zend_ulong num_key;

		if (zend_hash_get_current_key_ex(&dict, &str, &num_key, &pos) == HASH_KEY_IS_STRING) {
			request->dictionary[n] = strndup(str->val, str->len);
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
		pinba_timer_t *t;

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
				(t = zend_hash_get_current_data_ptr_ex(&timers_uniq, &pos)) != NULL;
				zend_hash_move_forward_ex(&timers_uniq, &pos)) {

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

static inline int php_pinba_req_data_send(pinba_client_t *client, const char *custom_script_name, int flags) /* {{{ */
{
	int ret = SUCCESS;
	Pinba__Request *request;

	request = php_create_pinba_packet(client, custom_script_name, flags);

	if (request) {
		int i, n_collectors, data_len;
		ssize_t sent;
		pinba_collector *collectors;
		char *data;

		if (client) {
			/* disable AUTO_FLUSH if data has been sent manually */
			client->data_sent = 1;
		}

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
				php_error_docref(NULL, E_WARNING, "failed to send data to Pinba server: %s", strerror(errno));
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

static void php_pinba_flush_data(const char *custom_script_name, long flags) /* {{{ */
{

	/* stop all running timers */
	zend_hash_apply_with_argument(&EG(regular_list), (apply_func_arg_t) php_pinba_timer_stop_helper, (void *)flags);

	/* prevent any further access to the timers */
	PINBA_G(timers_stopped) = 1;

	if (!PINBA_G(enabled) || PINBA_G(n_collectors) == 0) {
		/* disabled or no collectors defined, exit */
		zend_hash_clean(&PINBA_G(timers));
		PINBA_G(timers_stopped) = 0;
		return;
	}

	if (php_pinba_init_socket(PINBA_G(collectors), PINBA_G(n_collectors)) != SUCCESS) {
		PINBA_G(timers_stopped) = 0;
		return;
	}

	php_pinba_req_data_send(NULL, custom_script_name, flags);

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

		zend_hash_clean(&PINBA_G(tags));
	}

	PINBA_G(timers_stopped) = 0;
	zend_hash_clean(&PINBA_G(timers));
}
/* }}} */

static int php_pinba_key_compare(const void *a, const void *b) /* {{{ */
{
	Bucket *f;
	Bucket *s;
	zval first;
	zval second;

	f = (Bucket *) a;
	s = (Bucket *) b;

	if (f->key == NULL) {
		ZVAL_LONG(&first, f->h);
	} else {
		ZVAL_STR(&first, f->key);
	}

	if (s->key == NULL) {
		ZVAL_LONG(&second, s->h);
	} else {
		ZVAL_STR(&second, s->key);
	}

	return string_compare_function(&first, &second);
}
/* }}} */

static int php_pinba_array_to_tags(HashTable *array, pinba_timer_tag_t ***tags) /* {{{ */
{
	int num, i = 0;
	zval *value;
	zend_string *tag_name_str;

	num = zend_hash_num_elements(array);
	if (!num) {
		return FAILURE;
	}

	/* sort array, we'll use this when computing tags hash */
	zend_hash_sort(array, php_pinba_key_compare, 0);

	*tags = (pinba_timer_tag_t **)ecalloc(num, sizeof(pinba_timer_tag_t *));
	ZEND_HASH_FOREACH_STR_KEY_VAL_IND(array, tag_name_str, value) {
		zend_string *str;

		switch (Z_TYPE_P(value)) {
			case IS_NULL:
			case IS_STRING:
			case IS_TRUE:
			case IS_FALSE:
			case IS_LONG:
			case IS_DOUBLE:
				str = zval_get_string(value);
				break;
			default:
				php_error_docref(NULL, E_WARNING, "tags cannot have non-scalar values");
				php_pinba_timer_tags_dtor(*tags, i);
				efree(*tags);
				return FAILURE;
		}

		if (tag_name_str) {
			(*tags)[i] = (pinba_timer_tag_t *)emalloc(sizeof(pinba_timer_tag_t));
			(*tags)[i]->name = estrndup(tag_name_str->val, tag_name_str->len);
			(*tags)[i]->name_len = tag_name_str->len;
			(*tags)[i]->value = estrndup(str->val, str->len);
			(*tags)[i]->value_len = str->len;
			zend_string_release(str);
		} else {
			zend_string_release(str);
			php_error_docref(NULL, E_WARNING, "tags can only have string names (i.e. tags array cannot contain numeric indexes)");
			php_pinba_timer_tags_dtor(*tags, i);
			efree(*tags);
			return FAILURE;
		}
		i++;
	} ZEND_HASH_FOREACH_END();
	return SUCCESS;
}
/* }}} */

static pinba_timer_t *php_pinba_timer_ctor(pinba_timer_tag_t **tags, int tags_num) /* {{{ */
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

static void php_pinba_get_timer_info(pinba_timer_t *t, zval *info) /* {{{ */
{
	zval tags;
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
	add_assoc_double(info, "value", timeval_to_float(tmp));

	array_init(&tags);

	for (i = 0; i < t->tags_num; i++) {
		tag = t->tags[i];
		add_assoc_stringl(&tags, tag->name, tag->value, tag->value_len);
	}

	add_assoc_zval(info, "tags", &tags);
	add_assoc_bool(info, "started", t->started ? 1 : 0);
	if (!Z_ISUNDEF(t->data)) {
		add_assoc_zval(info, "data", &t->data);
		zval_add_ref(&t->data);
	} else {
		add_assoc_null(info, "data");
	}

	add_assoc_double(info, "ru_utime", timeval_to_float(t->ru_utime));
	add_assoc_double(info, "ru_stime", timeval_to_float(t->ru_stime));
}
/* }}} */

static void pinba_client_free_storage(zend_object *object) /* {{{ */
{
	int i;
	pinba_client_t *client = (pinba_client_t *) php_pinba_client_object(object);

	if (!client->data_sent && (client->flags & PINBA_AUTO_FLUSH) != 0) {
		if (client->collectors_initialized || php_pinba_init_socket(client->collectors, client->n_collectors) != FAILURE) {
			php_pinba_req_data_send(client, NULL, client->flags);
		}
	}

	zend_object_std_dtor(&client->std);

	if (client->n_servers > 0) {
		for (i = 0; i < client->n_servers; i++) {
			efree(client->servers[i]);
		}
		efree(client->servers);
	}

	php_pinba_cleanup_collectors(client->collectors, &client->n_collectors);

	if (client->hostname) {
		efree(client->hostname);
	}

	if (client->server_name) {
		efree(client->server_name);
	}

	if (client->script_name) {
		efree(client->script_name);
	}

	if (client->schema) {
		efree(client->schema);
	}

	zend_hash_destroy(&client->timers);
	zend_hash_destroy(&client->tags);
}
/* }}} */

zend_object *pinba_client_new(zend_class_entry *ce) /* {{{ */
{
	pinba_client_t *intern;

	intern = ecalloc(1, sizeof(pinba_client_t) + zend_object_properties_size(ce));

	zend_object_std_init(&(intern->std), ce);
	object_properties_init(&intern->std, ce);
	intern->std.handlers = &pinba_client_handlers;

	zend_hash_init(&intern->timers, 0, NULL, php_timer_hash_dtor, 0);
	zend_hash_init(&intern->tags, 0, NULL, php_tag_hash_dtor, 0);
	return &intern->std;
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
	zend_resource *rsrc;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "h|al", &tags_array, &data, &hit_count) != SUCCESS) {
		return;
	}

	tags_num = zend_hash_num_elements(tags_array);

	if (!tags_num) {
		php_error_docref(NULL, E_WARNING, "tags array cannot be empty");
		RETURN_FALSE;
	}

	if (hit_count <= 0) {
		php_error_docref(NULL, E_WARNING, "hit_count must be greater than 0 (%ld was passed)", hit_count);
		RETURN_FALSE;
	}

	if (php_pinba_array_to_tags(tags_array, &tags) != SUCCESS) {
		RETURN_FALSE;
	}

	t = php_pinba_timer_ctor(tags, tags_num);

	if (data && zend_hash_num_elements(Z_ARRVAL_P(data)) > 0) {
		ZVAL_DUP(&t->data, data);
	}

	t->started = 1;
	t->hit_count = hit_count;

	rsrc = zend_register_resource(t, le_pinba_timer);
	t->rsrc_id = rsrc->handle;

	if (getrusage(RUSAGE_SELF, &u) == 0) {
		timeval_cvt(&t->tmp_ru_utime, &u.ru_utime);
		timeval_cvt(&t->tmp_ru_stime, &u.ru_stime);
	}
	/* refcount++ so that the timer is shut down only on request finish if not stopped manually */
	GC_REFCOUNT(rsrc)++;
	RETURN_RES(rsrc);
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
	zend_resource *rsrc;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "hd|a", &tags_array, &value, &data) != SUCCESS) {
		return;
	}

	tags_num = zend_hash_num_elements(tags_array);

	if (!tags_num) {
		php_error_docref(NULL, E_WARNING, "tags array cannot be empty");
		RETURN_FALSE;
	}

	if (php_pinba_array_to_tags(tags_array, &tags) != SUCCESS) {
		RETURN_FALSE;
	}

	if (value < 0) {
		php_error_docref(NULL, E_WARNING, "negative time value passed (%f), changing it to 0", value);
		value = 0;
	}

	t = php_pinba_timer_ctor(tags, tags_num);

	if (data) {
		ZVAL_DUP(&t->data, data);
	}

	t->started = 0;
	t->hit_count = 1;
	time_l = (unsigned long)(value * 1000000.0);
	t->value.tv_sec = time_l / 1000000;
	t->value.tv_usec = time_l % 1000000;

	rsrc = zend_register_resource(t, le_pinba_timer);
	t->rsrc_id = rsrc->handle;

	/* refcount++ so that the timer is shut down only on request finish if not stopped manually */
	GC_REFCOUNT(rsrc)++;
	RETURN_RES(rsrc);
}
/* }}} */

/* {{{ proto bool pinba_timer_stop(resource timer)
   Stop user timer */
static PHP_FUNCTION(pinba_timer_stop)
{
	zval *timer;
	pinba_timer_t *t;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &timer) != SUCCESS) {
		return;
	}

	PHP_ZVAL_TO_TIMER(timer, t);

	if (!t->started) {
		php_error_docref(NULL, E_NOTICE, "timer is already stopped");
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

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &timer) != SUCCESS) {
		return;
	}

	PHP_ZVAL_TO_TIMER(timer, t);

	if (t->started) {
		php_pinba_timer_stop(t);
	}

	t->deleted = 1;
	zend_list_delete(Z_RES_P(timer));
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_timer_data_merge(resource timer, array data)
   Merge timer data with new data */
static PHP_FUNCTION(pinba_timer_data_merge)
{
	zval *timer, *data;
	pinba_timer_t *t;

	if (PINBA_G(timers_stopped)) {
		php_error_docref(NULL, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ra", &timer, &data) != SUCCESS) {
		return;
	}

	PHP_ZVAL_TO_TIMER(timer, t);

	if (Z_ISUNDEF(t->data)) {
		ZVAL_DUP(&t->data, data);
	} else {
		zend_hash_merge(Z_ARRVAL_P(&t->data), Z_ARRVAL_P(data), zval_add_ref, 1);
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
		php_error_docref(NULL, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ra!", &timer, &data) != SUCCESS) {
		return;
	}

	PHP_ZVAL_TO_TIMER(timer, t);

	if (!data) {
		/* reset */
		if (!Z_ISUNDEF(t->data)) {
			zval_ptr_dtor(&t->data);
			ZVAL_UNDEF(&t->data);
		}
	} else {
		if (!Z_ISUNDEF(t->data)) {
			zval_ptr_dtor(&t->data);
		}
		ZVAL_DUP(&t->data, data);
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
		php_error_docref(NULL, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rh", &timer, &tags) != SUCCESS) {
		return;
	}

	PHP_ZVAL_TO_TIMER(timer, t);

	tags_num = zend_hash_num_elements(tags);

	if (!tags_num) {
		RETURN_TRUE;
	}

	if (php_pinba_array_to_tags(tags, &new_tags) != SUCCESS) {
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
		php_error_docref(NULL, E_WARNING, "all timers have already been stopped");
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "rh", &timer, &tags) != SUCCESS) {
		return;
	}

	PHP_ZVAL_TO_TIMER(timer, t);

	tags_num = zend_hash_num_elements(tags);

	if (!tags_num) {
		php_error_docref(NULL, E_WARNING, "tags array cannot be empty");
		RETURN_TRUE;
	}

	if (php_pinba_array_to_tags(tags, &new_tags) != SUCCESS) {
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
	long flags = 0;
	char *script_name = NULL;
	size_t script_name_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|sl", &script_name, &script_name_len, &flags) != SUCCESS) {
		return;
	}

	if (script_name && script_name_len > 0) {
		php_pinba_flush_data(script_name, flags);
	} else {
		php_pinba_flush_data(NULL, flags);
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto array pinba_get_info()
   Get request info */
static PHP_FUNCTION(pinba_get_info)
{
	zval timers, timer_info, tags;
	struct timeval tmp;
	struct rusage u;
	HashPosition pos;
	zval *zv;
	pinba_timer_t *t;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "") != SUCCESS) {
		return;
	}

	array_init(return_value);

	add_assoc_long(return_value, "mem_peak_usage", zend_memory_peak_usage(1));

	if (PINBA_G(request_time) > 0) {
		/* use custom request time */
		add_assoc_double(return_value, "req_time", PINBA_G(request_time));
	} else {
		if (gettimeofday(&tmp, 0) == 0) {
			timersub(&tmp, &(PINBA_G(tmp_req_data).req_start), &tmp);
			add_assoc_double(return_value, "req_time", timeval_to_float(tmp));
		} else {
			add_assoc_double(return_value, "req_time", 0);
		}
	}

	if (getrusage(RUSAGE_SELF, &u) == 0) {
		timersub(&u.ru_utime, &(PINBA_G(tmp_req_data).ru_utime), &tmp);
		add_assoc_double(return_value, "ru_utime", timeval_to_float(tmp));
		timersub(&u.ru_stime, &(PINBA_G(tmp_req_data).ru_stime), &tmp);
		add_assoc_double(return_value, "ru_stime", timeval_to_float(tmp));
	} else {
		add_assoc_double(return_value, "ru_utime", 0);
		add_assoc_double(return_value, "ru_stime", 0);
	}

	add_assoc_long(return_value, "req_count", PINBA_G(tmp_req_data).req_count + 1);
	add_assoc_long(return_value, "doc_size", PINBA_G(tmp_req_data).doc_size);

	if (PINBA_G(schema)) {
		add_assoc_string(return_value, "schema", PINBA_G(schema));
	} else {
		add_assoc_string(return_value, "schema", (char *)"unknown");
	}

	if (PINBA_G(server_name)) {
		add_assoc_string(return_value, "server_name", PINBA_G(server_name));
	} else {
		add_assoc_string(return_value, "server_name", (char *)"unknown");
	}

	if (PINBA_G(script_name)) {
		add_assoc_string(return_value, "script_name", PINBA_G(script_name));
	} else {
		add_assoc_string(return_value, "script_name", (char *)"unknown");
	}

	add_assoc_string(return_value, "hostname", PINBA_G(host_name));

	array_init(&timers);

	for (zend_hash_internal_pointer_reset_ex(&EG(regular_list), &pos);
			(zv = zend_hash_get_current_data_ex((&EG(regular_list)), &pos)) != NULL;
			zend_hash_move_forward_ex(&EG(regular_list), &pos)) {
		zend_resource *rsrc = Z_RES_P(zv);
		if (rsrc->type == le_pinba_timer) {
			t = (pinba_timer_t *)rsrc->ptr;

			if (t->deleted) {
				continue;
			}

			php_pinba_get_timer_info(t, &timer_info);
			add_next_index_zval(&timers, &timer_info);
		}
	}
	add_assoc_zval(return_value, "timers", &timers);

	array_init(&tags);

	for (zend_hash_internal_pointer_reset_ex(&PINBA_G(tags), &pos);
			(zv = zend_hash_get_current_data_ex(&PINBA_G(tags), &pos)) != NULL;
			zend_hash_move_forward_ex(&PINBA_G(tags), &pos)) {
		zend_string *key;
		zend_ulong dummy;
		char *tag_value = Z_PTR_P(zv);

		if (zend_hash_get_current_key_ex(&PINBA_G(tags), &key, &dummy, &pos) == HASH_KEY_IS_STRING) {
			add_assoc_string_ex(&tags, key->val, key->len, tag_value);
		} else {
			continue;
		}
	}
	add_assoc_zval(return_value, "tags", &tags);
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

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &flags) != SUCCESS) {
		return;
	}

	request = php_create_pinba_packet(NULL, NULL, flags);
	if (!request) {
		RETURN_FALSE;
	}

	PINBA_PACK(request, data, data_len);
	RETVAL_STRINGL(data, data_len);
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

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &timer) != SUCCESS) {
		return;
	}

	PHP_ZVAL_TO_TIMER(timer, t);

	php_pinba_get_timer_info(t, return_value);
}
/* }}} */

/* {{{ proto bool pinba_timers_stop()
   Stop all timers */
static PHP_FUNCTION(pinba_timers_stop)
{
	zval *zv;
	pinba_timer_t *t;
	HashPosition pos;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "") != SUCCESS) {
		return;
	}

	for (zend_hash_internal_pointer_reset_ex(&EG(regular_list), &pos);
			(zv = zend_hash_get_current_data_ex((&EG(regular_list)), &pos)) != NULL;
			zend_hash_move_forward_ex(&EG(regular_list), &pos)) {
		zend_resource *rsrc = Z_RES_P(zv);

		if (rsrc->type == le_pinba_timer) {
			t = (pinba_timer_t *)rsrc->ptr;
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
	zval *zv;
	HashPosition pos;
	pinba_timer_t *t;
	long flag = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &flag) != SUCCESS) {
		return;
	}

	array_init(return_value);
	for (zend_hash_internal_pointer_reset_ex(&EG(regular_list), &pos);
			(zv = zend_hash_get_current_data_ex((&EG(regular_list)), &pos)) != NULL;
			zend_hash_move_forward_ex(&EG(regular_list), &pos)) {
		zend_resource *rsrc = Z_RES_P(zv);

		if (rsrc->type == le_pinba_timer) {
			t = (pinba_timer_t *)rsrc->ptr;
			if (t->deleted || ((flag & PINBA_FLUSH_ONLY_STOPPED_TIMERS) != 0 && t->started)) {
				continue;
			}
			/* refcount++ */
			GC_REFCOUNT(rsrc)++;
			add_next_index_resource(return_value, rsrc);
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
	size_t script_name_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &script_name, &script_name_len) != SUCCESS) {
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
	size_t hostname_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &hostname, &hostname_len) != SUCCESS) {
		return;
	}

	if (hostname_len < sizeof(PINBA_G(host_name))) {
		memcpy(PINBA_G(host_name), hostname, hostname_len);
		PINBA_G(host_name)[hostname_len] = '\0';
	} else {
		memcpy(PINBA_G(host_name), hostname, sizeof(PINBA_G(host_name)) - 1);
		PINBA_G(host_name)[sizeof(PINBA_G(host_name)) - 1] = '\0';
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_schema_set(string custom_schema)
   Set custom schema */
static PHP_FUNCTION(pinba_schema_set)
{
	char *schema;
	size_t schema_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &schema, &schema_len) != SUCCESS) {
		return;
	}

	if (schema_len < sizeof(PINBA_G(schema))) {
		memcpy(PINBA_G(schema), schema, schema_len);
		PINBA_G(schema)[schema_len] = '\0';
	} else {
		memcpy(PINBA_G(schema), schema, sizeof(PINBA_G(schema)) - 1);
		PINBA_G(schema)[sizeof(PINBA_G(schema)) - 1] = '\0';
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool pinba_server_name_set(string custom_server_name)
   Set custom server name */
static PHP_FUNCTION(pinba_server_name_set)
{
	char *server_name;
	size_t server_name_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &server_name, &server_name_len) != SUCCESS) {
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

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "d", &time) != SUCCESS) {
		return;
	}

	if (time < 0) {
		php_error_docref(NULL, E_WARNING, "negative request time value passed (%f), changing it to 0", time);
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
	size_t tag_len, value_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &tag, &tag_len, &value, &value_len) != SUCCESS) {
		return;
	}

	if (tag_len < 1) {
		php_error_docref(NULL, E_WARNING, "tag name cannot be empty");
		RETURN_FALSE;
	}

	/* store the copy */
	value = estrndup(value, value_len);

	zend_hash_str_update_ptr(&PINBA_G(tags), tag, tag_len, value);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto string pinba_tag_get(string tag)
   Get previously set request tag value */
static PHP_FUNCTION(pinba_tag_get)
{
	char *tag, *value;
	size_t tag_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &tag, &tag_len) != SUCCESS) {
		return;
	}

	value = zend_hash_str_find_ptr(&PINBA_G(tags), tag, tag_len);
	if (!value) {
		RETURN_FALSE;
	}
	RETURN_STRING(value);
}
/* }}} */

/* {{{ proto bool pinba_tag_delete(string tag)
   Delete previously set request tag */
static PHP_FUNCTION(pinba_tag_delete)
{
	char *tag;
	size_t tag_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &tag, &tag_len) != SUCCESS) {
		return;
	}

	if (zend_hash_str_del(&PINBA_G(tags), tag, tag_len) == FAILURE) {
		RETURN_FALSE;
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto array pinba_tags_get(string tag)
   List all request tags */
static PHP_FUNCTION(pinba_tags_get)
{
	char *value;
	HashPosition pos;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "") != SUCCESS) {
		return;
	}

	array_init(return_value);
	for (zend_hash_internal_pointer_reset_ex(&PINBA_G(tags), &pos);
			(value = zend_hash_get_current_data_ptr_ex(&PINBA_G(tags), &pos)) != NULL;
			zend_hash_move_forward_ex(&PINBA_G(tags), &pos)) {
		zend_string *key;
		zend_ulong dummy;

		if (zend_hash_get_current_key_ex(&PINBA_G(tags), &key, &dummy, &pos) == HASH_KEY_IS_STRING) {
			add_assoc_string_ex(return_value, key->val, key->len, value);
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
	zval *tmp;
	pinba_collector *new_collector;
	pinba_client_t *client;
	long flags = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "h|l", &servers, &flags) != SUCCESS) {
		return;
	}
	client = Z_PINBACLIENT_P(getThis());
	client->flags = flags;

	for (zend_hash_internal_pointer_reset(servers);
		 (tmp = zend_hash_get_current_data(servers)) != NULL;
		 zend_hash_move_forward(servers)) {
		char *host, *port, *address_copy;
		zend_string *str = zval_get_string(tmp);

		address_copy = estrndup(str->val, str->len);
		if (php_pinba_parse_server(address_copy, &host, &port) != SUCCESS) {
			efree(address_copy);
			zend_string_release(str);
			continue;
		}
		zend_string_release(str);

		new_collector = php_pinba_collector_add(client->collectors, &client->n_collectors);
		if (new_collector == NULL) {
			efree(address_copy);
			break;
		}

		new_collector->host = strdup(host);
		new_collector->port = (port == NULL) ? strdup(PINBA_COLLECTOR_DEFAULT_PORT) : strdup(port);
		new_collector->fd = -1; /* set invalid fd */
		efree(address_copy);
	}
}
/* }}} */

#define SET_METHOD_STR(name, attr_name)														\
static PHP_METHOD(PinbaClient, name)														\
{																							\
	pinba_client_t *client;																	\
	char *value;																			\
	size_t value_len;																		\
																							\
	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &value, &value_len) != SUCCESS) {		\
		return;																				\
	}																						\
	client = Z_PINBACLIENT_P(getThis());													\
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
	if (zend_parse_parameters(ZEND_NUM_ARGS(), type_letter, &value) != SUCCESS) { \
		return;																				\
	}																						\
	client = Z_PINBACLIENT_P(getThis());													\
																							\
	if (value < 0) {																		\
		php_error_docref(NULL, E_WARNING, #attr_name " cannot be less than zero"); \
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
	zval *tmp;
	int i;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "h", &rusage) != SUCCESS) {
		return;
	}
	client = Z_PINBACLIENT_P(getThis());

	if (zend_hash_num_elements(rusage) != 2) {
		php_error_docref(NULL, E_WARNING, "rusage array must contain exactly 2 elements");
		RETURN_FALSE;
	}

	for (zend_hash_internal_pointer_reset(rusage), i = 0;
		 ((tmp = zend_hash_get_current_data(rusage)) != NULL) && i < 2;
		 zend_hash_move_forward(rusage), i++) {

		client->rusage[i] = zval_get_double(tmp);
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
	size_t tag_len, value_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &tag, &tag_len, &value, &value_len) != SUCCESS) {
		return;
	}
	client = Z_PINBACLIENT_P(getThis());

	/* store the copy */
	value = estrndup(value, value_len);
	zend_hash_str_update_ptr(&client->tags, tag, tag_len, value);
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
	size_t hashed_tags_len, i, tags_num;
	zval *tmp;
	pinba_timer_t *timer;
	pinba_timer_tag_t **new_tags;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "hd|hl", &tags, &value, &rusage, &hit_count) != SUCCESS) {
		return;
	}
	client = Z_PINBACLIENT_P(getThis());

	tags_num = zend_hash_num_elements(tags);
	if (!tags_num) {
		php_error_docref(NULL, E_WARNING, "timer tags array cannot be empty");
		RETURN_FALSE;
	}

	if (value < 0) {
		php_error_docref(NULL, E_WARNING, "timer value cannot be less than 0");
		RETURN_FALSE;
	}

	if (hit_count < 0) {
		php_error_docref(NULL, E_WARNING, "timer hit count cannot be less than 0");
		RETURN_FALSE;
	}

	if (rusage && zend_hash_num_elements(rusage) != 2) {
		php_error_docref(NULL, E_WARNING, "rusage array must contain exactly 2 elements");
		RETURN_FALSE;
	}

	if (rusage) {
		for (zend_hash_internal_pointer_reset(rusage), i = 0;
				((tmp = zend_hash_get_current_data(rusage)) != NULL) && i < 2;
				zend_hash_move_forward(rusage), i++) {

			if (i == 0) {
				ru_utime = zval_get_double(tmp);
			} else {
				ru_stime = zval_get_double(tmp);
			}
		}
	}

	if (php_pinba_array_to_tags(tags, &new_tags) != SUCCESS) {
		RETURN_FALSE;
	}

	if (php_pinba_tags_to_hashed_string(new_tags, tags_num, &hashed_tags, &hashed_tags_len) != SUCCESS) {
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
		pinba_timer_t *old_t;

		old_t = zend_hash_str_find_ptr(&client->timers, hashed_tags, hashed_tags_len);
		if (old_t != NULL) {
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
			zend_hash_str_add_ptr(&client->timers, hashed_tags, hashed_tags_len, timer);
		}
	} else {
		zend_hash_str_update_ptr(&client->timers, hashed_tags, hashed_tags_len, timer);
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

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &flags) != SUCCESS) {
		return;
	}
	client = Z_PINBACLIENT_P(getThis());

	if (client->n_collectors == 0) {
		RETURN_FALSE;
	}

	/* no need to reinitialize collectors on every send */
	if (!client->collectors_initialized) {
		if (php_pinba_init_socket(client->collectors, client->n_collectors) != SUCCESS) {
			RETURN_FALSE;
		}
		client->collectors_initialized = 1;
	}

	/* if set, flags override flags set in the constructor */
	if (!flags && client->flags > 0) {
		flags = client->flags;
	}

	php_pinba_req_data_send(client, NULL, flags);
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

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &flags) != SUCCESS) {
		return;
	}
	client = Z_PINBACLIENT_P(getThis());

	request = php_create_pinba_packet(client, NULL, flags);
	if (!request) {
		RETURN_FALSE;
	}

	PINBA_PACK(request, data, data_len);
	RETVAL_STRINGL(data, data_len);
	PINBA_FREE_BUFFER();
	pinba__request__free_unpacked(request, NULL);
}
/* }}} */

/* {{{ arginfo */

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_timer_start, 0, 0, 1)
	ZEND_ARG_INFO(0, tags)
	ZEND_ARG_INFO(0, data)
	ZEND_ARG_INFO(0, hit_count)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_timer_add, 0, 0, 2)
	ZEND_ARG_INFO(0, tags)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, data)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_timer_stop, 0, 0, 1)
	ZEND_ARG_INFO(0, timer)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_timer_delete, 0, 0, 1)
	ZEND_ARG_INFO(0, timer)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_timer_data_merge, 0, 0, 2)
	ZEND_ARG_INFO(0, timer)
	ZEND_ARG_INFO(0, data)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_timer_data_replace, 0, 0, 2)
	ZEND_ARG_INFO(0, timer)
	ZEND_ARG_INFO(0, data)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_timer_tags_merge, 0, 0, 2)
	ZEND_ARG_INFO(0, timer)
	ZEND_ARG_INFO(0, tags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_timer_tags_replace, 0, 0, 2)
	ZEND_ARG_INFO(0, timer)
	ZEND_ARG_INFO(0, tags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_flush, 0, 0, 0)
	ZEND_ARG_INFO(0, custom_script_name)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_get_info, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_get_data, 0, 0, 0)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_timer_get_info, 0, 0, 1)
	ZEND_ARG_INFO(0, timer)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_timers_stop, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_timers_get, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_script_name_set, 0, 0, 1)
	ZEND_ARG_INFO(0, custom_script_name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_hostname_set, 0, 0, 1)
	ZEND_ARG_INFO(0, custom_hostname)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_schema_set, 0, 0, 1)
	ZEND_ARG_INFO(0, custom_schema)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_server_name_set, 0, 0, 1)
	ZEND_ARG_INFO(0, custom_server_name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_request_time_set, 0, 0, 1)
	ZEND_ARG_INFO(0, request_time)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_tag_set, 0, 0, 2)
	ZEND_ARG_INFO(0, tag)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_tag_get, 0, 0, 1)
	ZEND_ARG_INFO(0, tag)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_tag_delete, 0, 0, 1)
	ZEND_ARG_INFO(0, tag)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pinba_tags_get, 0, 0, 0)
ZEND_END_ARG_INFO()

/* }}} */

#define PINBA_FUNC(func) PHP_FE(func, arginfo_ ## func)

/* {{{ pinba_functions[]
 */
zend_function_entry pinba_functions[] = {
	PINBA_FUNC(pinba_timer_start)
	PINBA_FUNC(pinba_timer_add)
	PINBA_FUNC(pinba_timer_stop)
	PINBA_FUNC(pinba_timer_delete)
	PINBA_FUNC(pinba_timer_data_merge)
	PINBA_FUNC(pinba_timer_data_replace)
	PINBA_FUNC(pinba_timer_tags_merge)
	PINBA_FUNC(pinba_timer_tags_replace)
	PINBA_FUNC(pinba_flush)
	PINBA_FUNC(pinba_get_info)
	PINBA_FUNC(pinba_get_data)
	PINBA_FUNC(pinba_timer_get_info)
	PINBA_FUNC(pinba_timers_stop)
	PINBA_FUNC(pinba_timers_get)
	PINBA_FUNC(pinba_script_name_set)
	PINBA_FUNC(pinba_hostname_set)
	PINBA_FUNC(pinba_server_name_set)
	PINBA_FUNC(pinba_schema_set)
	PINBA_FUNC(pinba_request_time_set)
	PINBA_FUNC(pinba_tag_set)
	PINBA_FUNC(pinba_tag_get)
	PINBA_FUNC(pinba_tag_delete)
	PINBA_FUNC(pinba_tags_get)
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

	if (new_value == NULL) {
		return FAILURE;
	}

	copy = strndup(new_value->val, new_value->len);
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
	return OnUpdateString(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage);
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
	REGISTER_LONG_CONSTANT("PINBA_AUTO_FLUSH", PINBA_AUTO_FLUSH, CONST_CS | CONST_PERSISTENT);

#if PHP_VERSION_ID >= 50400
	old_sapi_ub_write = sapi_module.ub_write;
	sapi_module.ub_write = sapi_ub_write_counter;
#endif

	INIT_CLASS_ENTRY(ce, "PinbaClient", pinba_client_methods);
	pinba_client_ce = zend_register_internal_class_ex(&ce, NULL);
	pinba_client_ce->create_object = pinba_client_new;

	memcpy(&pinba_client_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	pinba_client_handlers.dtor_obj = zend_objects_destroy_object;
	pinba_client_handlers.free_obj = pinba_client_free_storage;
	pinba_client_handlers.clone_obj = NULL;
	pinba_client_handlers.offset = XtOffsetOf(pinba_client_t, std);
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
	zval *tmp;
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

	zend_hash_init(&PINBA_G(timers), 10, NULL, NULL, 0);
	zend_hash_init(&PINBA_G(tags), 10, NULL, php_tag_hash_dtor, 0);

	PINBA_G(tmp_req_data).doc_size = 0;
	PINBA_G(tmp_req_data).mem_peak_usage= 0;

	PINBA_G(server_name) = NULL;
	PINBA_G(script_name) = NULL;

	gethostname(PINBA_G(host_name), sizeof(PINBA_G(host_name)));
	PINBA_G(host_name)[sizeof(PINBA_G(host_name)) - 1] = '\0';

	if (zend_is_auto_global_str("_SERVER", sizeof("_SERVER") - 1)) {
		tmp = zend_hash_str_find(HASH_OF(&PG(http_globals)[TRACK_VARS_SERVER]), "SCRIPT_NAME", sizeof("SCRIPT_NAME")-1);
		if (tmp && Z_TYPE_P(tmp) == IS_STRING && Z_STRLEN_P(tmp) > 0) {
			PINBA_G(script_name) = estrndup(Z_STRVAL_P(tmp), Z_STRLEN_P(tmp));
		}

		tmp = zend_hash_str_find(HASH_OF(&PG(http_globals)[TRACK_VARS_SERVER]), "SERVER_NAME", sizeof("SERVER_NAME")-1);
		if (tmp != NULL && Z_TYPE_P(tmp) == IS_STRING && Z_STRLEN_P(tmp) > 0) {
			PINBA_G(server_name) = estrndup(Z_STRVAL_P(tmp), Z_STRLEN_P(tmp));
		}
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
static PHP_RSHUTDOWN_FUNCTION(pinba)
{
	if (PINBA_G(auto_flush)) {
		php_pinba_flush_data(NULL, 0);
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
