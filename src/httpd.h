/* Part of owntone-mini, derived from OwnTone (GPL v2 or later, see COPYING).
 * Modified for owntone-mini. Modifications Copyright (C) 2026 James Pearce */

#ifndef __HTTPD_H__
#define __HTTPD_H__

#include <event2/buffer.h>

/*
 * Gzips an evbuffer
 *
 * @in  in       Data to be compressed
 * @return       Compressed data - must be freed by caller
 */
struct evbuffer *
httpd_gzip_deflate(struct evbuffer *in);

int
httpd_init(void);

void
httpd_deinit(void);

#endif /* !__HTTPD_H__ */
