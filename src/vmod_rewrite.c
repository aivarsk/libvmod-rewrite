/*
    Copyright (C) 2013 Aivars Kalvans <aivars.kalvans@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published
    by the Free Software Foundation; either version 3 of the License,
    or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, see <http://www.gnu.org/licenses>.

    Additional permission under GNU GPL version 3 section 7

    If you modify this Program, or any covered work, by linking or
    combining it with libvmod-example (or a modified version of that
    library), containing parts covered by the terms of Aivars Kalvans
    the licensors of this Program grant you additional permission to
    convey the resulting work. {Corresponding Source for a non-source
    form of such a combination shall include the source code for the
    parts of libvmod-example used as well as that of the covered work.}
*/

#include <stdio.h>
#define DEBUG(fmt, args...) do { \
    FILE *f = fopen("/tmp/vmod_zzzz.log", "a"); \
    fprintf(f, fmt, ##args); \
    fclose(f); \
} while (0)

#include <stdlib.h>
#include <regex.h>

#include "vrt.h"
#include "vqueue.h"
#include "vsha256.h"
#include "bin/varnishd/cache.h"
#include "bin/varnishd/stevedore.h"
#include "vcc_if.h"

int init_function(struct vmod_priv *priv, const struct VCL_conf *conf) {
    return 0;
}

struct buf_t {
    char *ptr;
    size_t len;
    size_t size;
};

#define BUF_GROW(buf) do { \
    (buf)->size *= 2; \
    (buf)->ptr = realloc((buf)->ptr, (buf)->size); \
    AN((buf)->ptr); \
} while (0)

#define BUF_RESERVE(buf, n) do { \
    while ((buf)->size <= (buf)->len + (n)) { \
        BUF_GROW(buf); \
    } \
} while (0)

static void _set_content_len(struct sess *sp, struct http *rsp, size_t len) {
    char *header;

    http_Unset(rsp, H_Content_Length);
    /* Header must outlive our plugin */
    header = WS_Alloc(sp->wrk->ws, 32);
    sprintf(header, "Content-Length: %jd", (intmax_t)len);
    http_SetH(rsp, rsp->nhd++, header);
}

static void _fix_content_len(struct sess *sp, struct http *rsp, size_t len) {
    if (http_GetHdr(rsp, H_Content_Length, NULL)) {
        _set_content_len(sp, rsp, len);
    }
}

static struct buf_t _object_read(struct sess *sp) {
    struct storage *st;
    size_t len;
    struct buf_t buf = {NULL, 0, 4 * 1024};

    BUF_GROW(&buf);
    CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);

    if (!sp->obj->gziped) {

        VTAILQ_FOREACH(st, &sp->obj->store, list) {
            CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
            CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);

            BUF_RESERVE(&buf, st->len);
            memcpy(buf.ptr + buf.len, st->ptr, st->len);
            buf.len += st->len;
        }
    } else {
	struct vgz *vg;
	char obuf[params->gzip_stack_buffer];
	ssize_t obufl = 0;
	const void *dp;
	size_t dl;

	vg = VGZ_NewUngzip(sp, "U D -");

        VTAILQ_FOREACH(st, &sp->obj->store, list) {
            CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
            CHECK_OBJ_NOTNULL(st, STORAGE_MAGIC);

            BUF_RESERVE(&buf, st->len * 2);
	    VGZ_Ibuf(vg, st->ptr, st->len);
            do {
	        VGZ_Obuf(vg, buf.ptr + buf.len, buf.size - buf.len);
                if (buf.len < buf.size) {
                    VGZ_Gunzip(vg, &dp, &dl);
                    buf.len += dl;
                } else {
                    BUF_RESERVE(&buf, st->len);
                }
	    } while (!VGZ_IbufEmpty(vg));
        }
	VGZ_Destroy(&vg);

        sp->wrk->res_mode &= ~RES_GUNZIP;
        sp->wrk->res_mode &= ~RES_CHUNKED;
        sp->wrk->res_mode |= RES_LEN;

        http_Unset(sp->wrk->resp, H_Content_Encoding);

        _set_content_len(sp, sp->wrk->resp, buf.len);
        http_GetHdr(sp->wrk->resp, H_Content_Length, &sp->wrk->h_content_length);
    }
    buf.ptr[buf.len] = '\0';
    return buf;
}

/**
Writes updated buf to object store and updates response object.

XXX not clear if STV_alloc always returns a block >= requested size
*/
static void _object_write(struct sess *sp, struct buf_t buf) {
    size_t pos = 0, l;
    struct storage *st;

    /* For varnish unittests */
    if (buf.len == 0) {
        return;
    }

    /* Create a new object and copy data to it */
    sp->obj = STV_NewObject(sp, TRANSIENT_STORAGE, buf.len, &sp->wrk->exp, 0);
    while (pos < buf.len) {
        l = buf.len - pos;
        st = STV_alloc(sp, l);

        if (l <= st->space) {
            memcpy(st->ptr, buf.ptr + pos, l);
            st->len = l;
        } else {
            memcpy(st->ptr, buf.ptr + pos, st->space);
            st->len = st->space;
        }

        pos += st->len;
        VTAILQ_INSERT_TAIL(&sp->obj->store, st, list);
    }
    if (st->len < st->space) {
        STV_trim(st, st->len);
    }

    /* XXX: hmmm... */
    sp->obj->len = buf.len;
    sp->obj->gziped = 0;
    sp->obj->xid = sp->xid;
    sp->obj->response = sp->err_code;

    //if (sp->obj->objcore != NULL) {
    //  EXP_Insert(sp->obj);
    //}

    /* If Content-Length header is present, update it to actual length */
    _fix_content_len(sp, sp->wrk->resp, buf.len);
}

static void _object_rewrite(struct buf_t *buf, regex_t *re_search, const char *str_replace) {
    size_t buf_pos;
    struct buf_t replacement;
    regmatch_t pmatch[10];

    /* Temporary buffer for creating replacement string */
    replacement.size = 1024;
    replacement.ptr = NULL;
    BUF_GROW(&replacement);

    buf_pos = 0;
    while (regexec(re_search, buf->ptr + buf_pos,
                sizeof(pmatch) / sizeof(pmatch[0]), pmatch, 0) == 0) {

        const char *pos;
        int sub, so, n, i;
        int idx_char;

        /* Create a replacement string for matched pattern.
           It's not static since replacement can use parts of matched pattern.
        */
        replacement.len = 0;
        for (pos = str_replace; *pos; pos++) {

            idx_char = *(pos + 1) - '0';
            if (*pos == '\\' && idx_char >= 0 && idx_char <= 9) {
                so = pmatch[idx_char].rm_so;
                if (so < 0) {
                    break;
                }

                n = pmatch[idx_char].rm_eo - so;

                BUF_RESERVE(&replacement, n);
                memcpy(replacement.ptr + replacement.len, buf->ptr + buf_pos + so, n);
                replacement.len += n;

                pos++;
            } else {
                BUF_RESERVE(&replacement, 1);
                replacement.ptr[replacement.len++] = *pos;
            }
        }

        /* Insert replacement string into document */

        n = pmatch[0].rm_eo - pmatch[0].rm_so;
        BUF_RESERVE(buf, replacement.len - n);

        memmove(buf->ptr + (buf_pos + pmatch[0].rm_so + replacement.len),
                buf->ptr + (buf_pos + pmatch[0].rm_eo), 
                buf->len - (buf_pos + pmatch[0].rm_eo));
        memcpy(buf->ptr + (buf_pos + pmatch[0].rm_so), replacement.ptr, replacement.len);
        buf->len += n;

        /* Advance position inside document so we don't process the same data again and again */
        buf->ptr[buf->len] = '\0';
        buf_pos = (buf_pos + pmatch[0].rm_eo) + n;
    }

    free(replacement.ptr);
}

void vmod_rewrite_re(struct sess *sp, const char *search, const char *replace) {
    struct buf_t buf;
    regex_t re_search;

    DEBUG("AAAAAAAAAAAAAAAAAAAA\n");

    if (sp->step != STP_PREPRESP) {
        /* Can be called only from vcl_deliver */
        abort();
        return;
    }

    if (regcomp(&re_search, search, REG_EXTENDED) != 0) {
        abort();
        return;
    }

    buf = _object_read(sp);

    _object_rewrite(&buf, &re_search, replace);

    _object_write(sp, buf);
    DEBUG("Buf: %s\n", buf.ptr);

    regfree(&re_search);
}
