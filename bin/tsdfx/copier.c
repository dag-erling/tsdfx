/*-
 * Copyright (c) 2014 Universitetet i Oslo
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef RUNNING_SHA
#include <tsd/sha1.h>
#endif

#include <tsd/strutil.h>

#include "tsdfx_log.h"

static mode_t mumask;

/* XXX make this a configuration parameter */
#define BLOCKSIZE	1024576

struct copyfile {
	char		 name[1024];
	int		 fd;
	int		 mode;
	struct stat	 st;
#ifdef RUNNING_SHA
	sha1_ctx	 sha_ctx;
	char		 digest[SHA1_DIGEST_LEN];
#endif
	off_t		 offset;
	size_t		 bufsize, buflen;
	char		 buf[];
};

static struct copyfile *copyfile_open(const char *, int, int);
static int copyfile_read(struct copyfile *);
static int copyfile_compare(struct copyfile *, struct copyfile *);
static int copyfile_comparestat(struct copyfile *, struct copyfile *);
static void copyfile_copy(struct copyfile *, struct copyfile *);
static void copyfile_copystat(struct copyfile *, struct copyfile *);
static int copyfile_write(struct copyfile *);
static int copyfile_advance(struct copyfile *);
static int copyfile_finish(struct copyfile *);
static void copyfile_close(struct copyfile *);

/* open a file and populate the state structure */
static struct copyfile *
copyfile_open(const char *fn, int mode, int perm)
{
	struct copyfile *cf;

	if ((cf = calloc(1, sizeof *cf + BLOCKSIZE)) == NULL)
		goto fail;
	strlcpy(cf->name, fn, sizeof cf->name);
	cf->mode = mode;
	cf->bufsize = BLOCKSIZE;
	if ((cf->fd = open(fn, mode, perm)) < 0)
		goto fail;
	if (fstat(cf->fd, &cf->st) != 0)
		goto fail;
#ifdef RUNNING_SHA
	if ((cf->sha_ctx = sha1_init()) == NULL)
		goto fail;
#endif
	return (cf);
fail:
	if (cf != NULL)
		copyfile_close(cf);
	return (NULL);
}

/* read a block */
static int
copyfile_read(struct copyfile *cf)
{
	ssize_t rlen;

	if ((rlen = read(cf->fd, cf->buf, cf->bufsize)) < 0) {
		warn("%s: read()", cf->name);
		return (-1);
	}
	if ((cf->buflen = (size_t)rlen) < cf->bufsize)
		memset(cf->buf + cf->buflen, 0, cf->bufsize - cf->buflen);
	return (0);
}

/* compare buffer length and content */
static int
copyfile_compare(struct copyfile *src, struct copyfile *dst)
{

	if (src->buflen != dst->buflen)
		return (-1);
	if (memcmp(src->buf, dst->buf, src->buflen) != 0)
		return (-1);
	return (0);
}

/* compare file size and times */
static int
copyfile_comparestat(struct copyfile *src, struct copyfile *dst)
{

	if (src->st.st_size != dst->st.st_size)
		return (-1);
	if (src->st.st_mtime != dst->st.st_mtime)
		return (-1);
	return (0);
}

/* copy buffer from one state structure to another */
static void
copyfile_copy(struct copyfile *src, struct copyfile *dst)
{

	assert(dst->bufsize >= src->bufsize);
	memcpy(dst->buf, src->buf, src->buflen);
	if ((dst->buflen = src->buflen) < dst->bufsize)
		memset(dst->buf + dst->buflen, 0, dst->bufsize - dst->buflen);
}

/* copy mode + times from one state structure to another */
static void
copyfile_copystat(struct copyfile *src, struct copyfile *dst)
{

	dst->st.st_mode = src->st.st_mode;
	dst->st.st_atim = src->st.st_atim;
	dst->st.st_mtim = src->st.st_mtim;
}

/* write a block at the previous offset */
static int
copyfile_write(struct copyfile *cf)
{
	ssize_t wlen;

	if (lseek(cf->fd, cf->offset, SEEK_SET) != cf->offset) {
		warn("%s: lseek()", cf->name);
		return (-1);
	}
	if ((wlen = write(cf->fd, cf->buf, cf->buflen)) != (ssize_t)cf->buflen) {
		warn("%s: write()", cf->name);
		return (-1);
	}
	return (0);
}

/* update the offset and running digest */
static int
copyfile_advance(struct copyfile *cf)
{

#ifdef RUNNING_SHA
	sha1_update(cf->sha_ctx, cf->buf, cf->buflen);
#endif
	cf->offset += cf->buflen;
	if (fstat(cf->fd, &cf->st) != 0)
		return (-1);
	return (0);
}


/* truncate at current offset, finalize digest */
static int
copyfile_finish(struct copyfile *cf)
{
	struct timeval times[2];
	int mode;

	if (cf->mode & O_RDWR) {
		if (ftruncate(cf->fd, cf->offset) != 0) {
			warn("%s: ftruncate()", cf->name);
			return (-1);
		}
		mode = (cf->st.st_mode & 0777) | 0600; // force u+rw
		mode &= ~mumask; // apply umask
		if (fchmod(cf->fd, mode) != 0) {
			warn("%s: fchmod(%04o)", cf->name, mode);
			return (-1);
		}
		times[0].tv_sec = cf->st.st_atim.tv_sec;
		times[0].tv_usec = cf->st.st_atim.tv_nsec / 1000;
		times[1].tv_sec = cf->st.st_mtim.tv_sec;
		times[1].tv_usec = cf->st.st_mtim.tv_nsec / 1000;
		if (futimes(cf->fd, times) != 0) {
			warn("%s: futimes()", cf->name);
			return (-1);
		}
	}
#ifdef RUNNING_SHA
	sha1_final(cf->sha_ctx, cf->digest);
	cf->sha_ctx = NULL;
#endif
	return (0);
}

/* close */
static void
copyfile_close(struct copyfile *cf)
{

	if (cf->fd >= 0)
		close(cf->fd);
#ifdef RUNNING_SHA
	if (cf->sha_ctx != NULL)
		sha1_discard(cf->sha_ctx);
#endif
	memset(cf, 0, sizeof *cf + cf->bufsize);
	free(cf);
}

/* read from both files, compare and write if necessary */
int
tsdfx_copier(const char *srcfn, const char *dstfn)
{
	struct copyfile *src, *dst;

	VERBOSE("%s to %s", srcfn, dstfn);

	/* what's my umask? */
	umask(mumask = umask(0));

	/* open source and destination files */
	src = dst = NULL;
	if ((src = copyfile_open(srcfn, O_RDONLY, 0)) == NULL ||
	    (dst = copyfile_open(dstfn, O_RDWR|O_CREAT, 0600)) == NULL)
		goto fail;

	/* compare size and times */
	if (copyfile_comparestat(src, dst) == 0) {
		VERBOSE("size and mtime match");
		copyfile_close(src);
		copyfile_close(dst);
		return (0);
	}

	/* XXX truncating dst to the length of src if it is longer might
	 * save a few cycles */

	/* loop over the input and compare with the destination */
	for (;;) {
		if (copyfile_read(src) != 0 ||
		    copyfile_read(dst) != 0)
			goto fail;
		if (src->buflen == 0) {
			/* end of source file */
			/* XXX add code to wait for additional data */
			copyfile_copystat(src, dst);
			if (copyfile_finish(src) != 0 ||
			    copyfile_finish(dst) != 0)
				goto fail;
			copyfile_close(src);
			copyfile_close(dst);
			return (0);
		}
		if (copyfile_compare(src, dst) != 0) {
			/* input and output differ */
			copyfile_copy(src, dst);
			if (copyfile_write(dst) != 0)
				goto fail;
		}
		if (copyfile_advance(src) != 0 ||
		    copyfile_advance(dst) != 0)
			goto fail;
	}
	/* not reached */
fail:
	ERROR("failed to copy %s to %s", srcfn, dstfn);
	if (src != NULL)
		copyfile_close(src);
	if (dst != NULL)
		copyfile_close(dst);
	return (-1);
}
