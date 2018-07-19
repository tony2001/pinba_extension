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

#ifndef PHP_PINBA_H
#define PHP_PINBA_H

#include <netinet/in.h>

extern zend_module_entry pinba_module_entry;
#define phpext_pinba_ptr &pinba_module_entry

#ifdef ZTS
#include "TSRM.h"
#endif

#define PINBA_COLLECTOR_DEFAULT_PORT "30002"
#define PINBA_COLLECTORS_MAX 8
#define PHP_PINBA_VERSION "1.1.1"

typedef struct _pinba_req_data { /* {{{ */
	char *server_name;
	char *script_name;
	size_t req_count;
	size_t doc_size;
	size_t  mem_peak_usage;
	struct timeval req_start;
	struct timeval req_time;
	struct timeval ru_utime;
	struct timeval ru_stime;
	size_t memory_footprint;
} pinba_req_data;
/* }}} */

typedef struct _pinba_collector {
	struct sockaddr_storage sockaddr;
	size_t                  sockaddr_len; /* shouldn't this be socken_t ? */
	time_t                  sockaddr_time; /* time last resolved */
	char *host;
	char *port;
	int fd;
} pinba_collector;

ZEND_BEGIN_MODULE_GLOBALS(pinba) /* {{{ */
	pinba_collector collectors[PINBA_COLLECTORS_MAX];
	unsigned int n_collectors; /* number of collectors we got from ini file */
	char *collector_address; /* this is a lil broken, contains last address only */
#if PHP_VERSION_ID < 50400
	int (*old_sapi_ub_write) (const char *, unsigned int TSRMLS_DC);
#endif
	char host_name[128];
	char schema[17];
	char *server_name;
	char *script_name;
	double request_time;
	HashTable timers;
	HashTable tags;
	pinba_req_data tmp_req_data;
	zend_bool timers_stopped;
	zend_bool in_rshutdown;
	zend_bool enabled;
	zend_bool auto_flush;
	time_t resolve_interval; /* seconds */
ZEND_END_MODULE_GLOBALS(pinba)
/* }}} */

#ifdef ZTS
#define PINBA_G(v) TSRMG(pinba_globals_id, zend_pinba_globals *, v)
#else
#define PINBA_G(v) (pinba_globals.v)
#endif

#endif	/* PHP_PINBA_H */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * End:
 */
