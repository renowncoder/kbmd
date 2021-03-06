/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2020 Joyent, Inc.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/debug.h>
#include <sys/types.h>
#include <umem.h>
#include <unistd.h>
#include "common.h"
#include "ecustr.h"
#include "envlist.h"
#include "pivy/errf.h"

char panicstr[256];

typedef struct kbmlog {
	mutex_t	kbmlog_lock;
	int	kbmlog_fd;
} kbmlog_t;

static kbmlog_t kbmlog = {
	.kbmlog_lock = ERRORCHECKMUTEX,
	.kbmlog_fd = STDERR_FILENO
};

bunyan_logger_t *blog;
__thread bunyan_logger_t *tlog;

/*
 * Debug builds are automatically wired up for umem debugging.
 */
#ifdef  DEBUG
const char *
_umem_debug_init(void)
{
	/*
	 * Per umem_debug(3MALLOC), "default" is equivalent to
	 * audit,contents,guards
	 */
	return ("default,verbose");
}

const char *
_umem_logging_init(void)
{
	return ("fail,contents");
}
#else
const char *
_umem_debug_init(void)
{
	return ("guards");
}
#endif  /* DEBUG */

static __NORETURN int
nomem(void)
{
	panic("out of memory");
}

void __NORETURN
panic(const char *msg, ...)
{
	int n;
	va_list ap;

	va_start(ap, msg);
	n = vsnprintf(panicstr, sizeof (panicstr), msg, ap);
	va_end(ap);

	(void) write(STDERR_FILENO, "PANIC: ", 7);
	(void) write(STDERR_FILENO, panicstr, n);
	if (n > 0 && panicstr[n - 1] != '\n')
		(void) write(STDERR_FILENO, "\n", 1);
	(void) fsync(STDERR_FILENO);
	abort();
}

void
alloc_init(void)
{
	ecustr_init();
	envlist_init();
	umem_nofail_callback(nomem);
}

void
tohex(const uint8_t *restrict bytes, size_t len, char *restrict str,
    size_t slen)
{
	static const char hexdigits[] = "0123456789ABCDEF";
	size_t i, j;

	if (slen < 2) {
		bzero(str, slen);
		return;
	}
	VERIFY3U(len, <, (SIZE_MAX - 1) / 2);

	if (len * 2 + 1 > slen) {
		panic("tohex output buffer too small "
		    "(%zu bytes, need at least %zu bytes)", slen, len * 2 + 1);
	}

	for (i = j = 0; i < len; i++) {
		uint8_t v = bytes[i];
		str[j++] = hexdigits[v >> 4];
		str[j++] = hexdigits[v & 0x0f];
	}
	str[j] = '\0';
}

void
guidtohex(const uint8_t *restrict guid, char *restrict str, size_t slen)
{
	tohex(guid, GUID_LEN, str, slen);
}

errf_t *
ecalloc(size_t n, size_t sz, void *p)
{
	void **pp = p;

	if ((*pp = calloc(n, sz)) == NULL)
		return (errfno("calloc", errno, ""));
	return (ERRF_OK);
}

errf_t *
eparse_ulong(const char *str, ulong_t *valp)
{
	char *endptr = NULL;
	ulong_t val = 0;

	*valp = 0;

	errno = 0;
	val = strtoul(str, &endptr, 10);
	if (errno != 0) {
		return (errfno("strtoul", errno,
		    "cannot parse '%s' as a number", str));
	}

	if (*endptr != '\0') {
		return (errfno("strtoul", EINVAL,
		    "'%s' has trailing characters", str));
	}

	*valp = val;
	return (ERRF_OK);
}

static int
kbm_stream_log(nvlist_t *nvl, const char *js, void *arg)
{
	struct kbmlog *klog = arg;
	int ret;

	mutex_enter(&klog->kbmlog_lock);
	ret = bunyan_stream_fd(nvl, js, (void *)(uintptr_t)klog->kbmlog_fd);
	mutex_exit(&klog->kbmlog_lock);

	return (ret);
}

errf_t *
init_log(bunyan_level_t level)
{
	int rc;

	rc = bunyan_init(getprogname(), &blog);
	if (rc != 0)
		return (errfno("bunyan_init", rc, "cannot initialize logger"));

	rc = bunyan_stream_add(blog, "stderr", level, kbm_stream_log, &kbmlog);
	if (rc != 0) {
		return (errfno("bunyan_stream_add", rc,
		    "cannot add stderr stream to logger"));
	}

	return (ERRF_OK);
}
