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
#define PHP_PINBA_VERSION "1.1.0-dev"

typedef struct _pinba_req_data { /* {{{ */
	char *server_name;
	char *script_name;
	unsigned int req_count;
	unsigned int doc_size;
	unsigned int mem_peak_usage;
	struct timeval req_start;
	struct timeval req_time;
	struct timeval ru_utime;
	struct timeval ru_stime;
	unsigned int memory_footprint;
} pinba_req_data;
/* }}} */

ZEND_BEGIN_MODULE_GLOBALS(pinba) /* {{{ */
	struct sockaddr_storage collector_sockaddr;
	size_t collector_sockaddr_len;
	char *collector_address;
	char *server_host;
	char *server_port;
	int (*old_sapi_ub_write) (const char *, unsigned int TSRMLS_DC);
	char host_name[128];
	char *server_name;
	char *script_name;
	HashTable timers;
	pinba_req_data tmp_req_data;
	zend_bool timers_stopped;
	zend_bool in_rshutdown;
	zend_bool enabled;
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
