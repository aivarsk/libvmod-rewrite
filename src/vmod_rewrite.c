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

#define DEBUG(fmt, args...) do { \
    FILE *f = fopen("/tmp/vmod_rewrite.log", "a"); \
    fprintf(f, fmt, ##args); \
    fclose(f); \
} while (0)

#include <stdio.h>
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
    }
    BUF_RESERVE(&buf, 1);
    buf.ptr[buf.len] = '\0';
    return buf;
}

static void _object_write(struct sess *sp, struct buf_t buf) {
    struct vsb *vsb;
    char *header;

    CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
    CHECK_OBJ_NOTNULL(sp->obj, OBJECT_MAGIC);
    vsb = SMS_Makesynth(sp->obj);
    AN(vsb);
    VSB_bcpy(vsb, buf.ptr, buf.len);
    SMS_Finish(sp->obj);


    http_Unset(sp->wrk->resp, H_Content_Length);

    /* Header must outlive our plugin */
    header = WS_Alloc(sp->wrk->ws, 32);
    sprintf(header, "Content-Length: %jd", (intmax_t)buf.len);
    http_SetH(sp->wrk->resp, sp->wrk->resp->nhd++, header);
    http_GetHdr(sp->wrk->resp, H_Content_Length, &sp->wrk->h_content_length);
}

static int _object_rewrite(struct buf_t *buf, regex_t *re_search, const char *str_replace) {
    int rewrote = 0;
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
        int sub, so, n, i, diff;
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

        rewrote = 1;
        diff = replacement.len - (pmatch[0].rm_eo - pmatch[0].rm_so);
        BUF_RESERVE(buf, diff);

        memmove(buf->ptr + (buf_pos + pmatch[0].rm_so + replacement.len),
                buf->ptr + (buf_pos + pmatch[0].rm_eo), 
                buf->len - (buf_pos + pmatch[0].rm_eo));
        memcpy(buf->ptr + (buf_pos + pmatch[0].rm_so), replacement.ptr, replacement.len);
        buf->len += diff;

        /* Advance position inside document so we don't process the same data again and again */
        BUF_RESERVE(buf,1);
        buf->ptr[buf->len] = '\0';
        buf_pos = (buf_pos + pmatch[0].rm_eo) + diff;
    }

    free(replacement.ptr);
    return rewrote;
}

void vmod_rewrite_re(struct sess *sp, const char *search, const char *replace) {
    struct buf_t buf;
    regex_t re_search;

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

    if (_object_rewrite(&buf, &re_search, replace)) {
        _object_write(sp, buf);
    }

    regfree(&re_search);
}
