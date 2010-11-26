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
  +----------------------------------------------------------------------+

  $Id: php_pinba.h,v 1.9.2.3 2009/04/28 10:40:57 tony Exp $ 
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
#define PHP_PINBA_VERSION "1.0.0-dev"

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
