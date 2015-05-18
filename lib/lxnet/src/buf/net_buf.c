
/*
 * Copyright (C) lcinx
 * lcinx@163.com
 */

#include "net_buf.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "net_bufpool.h"
#include "block_list.h"
#include "log.h"
#include "net_thread_buf.h"
#include "net_compress.h"


static bool s_enable_errorlog = false;

enum enum_some {
	enum_unknow = 0,
	enum_compress,
	enum_uncompress,
	enum_encrypt,
	enum_decrypt,
};

struct block_size {
	size_t bigblocksize;
	size_t smallblocksize;
};
static struct block_size s_blockinfo;


struct net_buf {
	bool isbigbuf;				/* big or small flag. */
	char compress_falg;
	char crypt_falg;
	bool use_tgw;
	volatile bool already_do_tgw;

	size_t raw_size_for_encrypt;
	size_t raw_size_for_compress;

	dofunc_f dofunc;
	void (*release_logicdata)(void *logicdata);
	void *do_logicdata;

	long io_limitsize;			/* io handle limit size. */

	struct blocklist iolist;	/* io block list. */

	struct blocklist logiclist;	/* if use compress/uncompress, logic block list is can use. */
};

static inline bool buf_is_use_compress(struct net_buf *self) {
	return (self->compress_falg == enum_compress);
}

static inline bool buf_is_use_uncompress(struct net_buf *self) {
	return (self->compress_falg == enum_uncompress);
}

static inline bool buf_is_use_encrypt(struct net_buf *self) {
	return (self->crypt_falg == enum_encrypt);
}

static inline bool buf_is_use_decrypt(struct net_buf *self) {
	return (self->crypt_falg == enum_decrypt);
}

static void buf_real_release(struct net_buf *self) {

	if (self->release_logicdata && self->do_logicdata) {
		self->release_logicdata(self->do_logicdata);
	}

	self->release_logicdata = NULL;
	self->do_logicdata = NULL;

	blocklist_release(&self->iolist);
	blocklist_release(&self->logiclist);
}

static void *create_small_block_f(void *arg, size_t size) {
	return bufpool_createsmallblock();
}

static void release_small_block_f(void *arg, void *bobj) {
	bufpool_releasesmallblock(bobj);
}

static void *create_big_block_f(void *arg, size_t size) {
	return bufpool_createbigblock();
}

static void release_big_block_f(void *arg, void *bobj) {
	bufpool_releasebigblock(bobj);
}


static void buf_init(struct net_buf *self, bool isbigbuf) {
	assert(self != NULL);

	self->isbigbuf = isbigbuf;
	self->compress_falg = enum_unknow;
	self->crypt_falg = enum_unknow;
	self->use_tgw = false;
	self->already_do_tgw = false;

	self->raw_size_for_encrypt = 0;
	self->raw_size_for_compress = 0;

	self->dofunc = NULL;
	self->release_logicdata = NULL;
	self->do_logicdata = NULL;

	self->io_limitsize = 0;

	if (isbigbuf) {
		blocklist_init(&self->iolist, 
					   create_big_block_f, release_big_block_f, 
					   NULL, s_blockinfo.bigblocksize);
		blocklist_init(&self->logiclist, 
					   create_big_block_f, release_big_block_f, 
					   NULL, s_blockinfo.bigblocksize);
	} else {
		blocklist_init(&self->iolist, 
					   create_small_block_f, release_small_block_f, 
					   NULL, s_blockinfo.smallblocksize);
		blocklist_init(&self->logiclist, 
					   create_small_block_f, release_small_block_f, 
					   NULL, s_blockinfo.smallblocksize);
	}
}

/* 
 * create buf.
 * bigbuf --- big or small buf, if is true, then is big buf, or else is small buf.
 * */
struct net_buf *buf_create(bool bigbuf) {
	struct net_buf *self = (struct net_buf *)bufpool_createbuf();
	if (!self) {
		log_error("	if (!self)");
		return NULL;
	}
	buf_init(self, bigbuf);
	return self;
}

/*
 * set buf encrypt function or decrypt function, and some logic data.
 * */
void buf_setdofunc(struct net_buf *self, dofunc_f func, void (*release_logicdata)(void *logicdata), void *logicdata) {
	assert(func != NULL);
	if (!self || !func)
		return;
	self->dofunc = func;
	self->release_logicdata = release_logicdata;
	self->do_logicdata = logicdata;
}

/* release buf. */
void buf_release(struct net_buf *self) {
	if (!self)
		return;
	buf_real_release(self);
	bufpool_releasebuf(self);
}


/* set buf handle limit size. */
void buf_set_limitsize(struct net_buf *self, int limit_len) {
	assert(limit_len > 0);
	if (!self)
		return;
	if (limit_len <= 0)
		self->io_limitsize = 0;
	else
		self->io_limitsize = limit_len;
}

void buf_usecompress(struct net_buf *self) {
	if (!self)
		return;
	self->compress_falg = enum_compress;
}

void buf_useuncompress(struct net_buf *self) {
	if (!self)
		return;
	self->compress_falg = enum_uncompress;
}

void buf_useencrypt(struct net_buf *self) {
	if (!self)
		return;
	self->crypt_falg = enum_encrypt;
}

void buf_usedecrypt(struct net_buf *self) {
	if (!self)
		return;
	self->crypt_falg = enum_decrypt;
}

void buf_use_tgw(struct net_buf *self) {
	if (!self)
		return;
	self->use_tgw = true;
}

void buf_set_raw_datasize(struct net_buf *self, size_t size) {
	if (!self)
		return;
	self->raw_size_for_encrypt = size;
	self->raw_size_for_compress = size;
}

long buf_get_data_size(struct net_buf *self) {
	if (!self)
		return 0;

	return self->iolist.datasize + self->logiclist.datasize;
}

/* push len, if is more than the limit, return true.*/
bool buf_add_islimit(struct net_buf *self, size_t len) {
	assert(len < _MAX_MSG_LEN);
	if (!self)
		return true;
	if (self->io_limitsize == 0)
		return false;
	/* limit compare as io datasize or logic datasize.*/
	if ((self->io_limitsize <= self->iolist.datasize) || 
			(self->io_limitsize <= (self->logiclist.datasize + len)))
		return true;
	return false;
}

/* test limit, buffer data as limit */
static bool buf_islimit(struct net_buf *self) {
	if (!self)
		return true;
	if (self->io_limitsize == 0)
		return false;
	/* limit compare as io datasize or logic datasize.*/
	if (self->io_limitsize <= self->logiclist.datasize || 
			self->io_limitsize <= self->iolist.datasize)
		return true;
	return false;
}

/* test can recv data, if not recv, return true. */
bool buf_can_not_recv(struct net_buf *self) {
	return buf_islimit(self);
}

/* test has data for send, if not has data,  return true. */
bool buf_can_not_send(struct net_buf *self) {
	if (!self)
		return true;
	return ((self->iolist.datasize <= 0) && (self->logiclist.datasize <= 0));
}

/*
 * some recv interface.
 *
 * */

/* get write buffer info. */
struct buf_info buf_getwritebufinfo(struct net_buf *self) {
	struct buf_info writebuf;
	writebuf.buf = NULL;
	writebuf.len = 0;

	if (buf_islimit(self))
		return writebuf;
	if (buf_is_use_uncompress(self))
		return blocklist_get_write_bufinfo(&self->iolist);
	else
		return blocklist_get_write_bufinfo(&self->logiclist);
}

static bool buf_try_parse_tgw(struct blocklist *lst, char **buf, int *len) {
	const int maxchecksize = 256;
	char tgwbuf[] = "\r\n\r\n";
	int findidx = 0;
	struct block *bk;
	int num = 0;
	int i;
	int canreadsize;
	char *f;
	
	*buf = NULL;
	*len = 0;
	for (bk = lst->head; bk; bk = bk->next) {
		f = block_getreadbuf(bk);
		canreadsize = block_getreadsize(bk);
		for (i = 0; i < canreadsize; ++i) {
			if (num >= maxchecksize)
				return false;

			if (f[i] == tgwbuf[findidx])
				findidx++;
			else
				findidx = 0;

			num++;
			if (findidx == 4) {
				blocklist_add_read(lst, num);
				if (i + 1 < canreadsize) {
					*buf = &f[i + 1];
					*len = canreadsize - (i + 1);
				}
				return true;
			}
		}
	}

	return false;
}

/* add write position. */
void buf_addwrite(struct net_buf *self, char *buf, int len) {
	char *tmpbuf = buf;
	int newlen = len;
	struct blocklist *lst;
	assert(len > 0);
	if (!self)
		return;

	if (buf_is_use_uncompress(self))
		lst = &self->iolist;
	else
		lst = &self->logiclist;

	if (self->use_tgw && (!self->already_do_tgw)) {
		if (buf_try_parse_tgw(lst, &tmpbuf, &newlen))
			self->already_do_tgw = true;
	}

	/* decrypt opt. */
	if (buf_is_use_decrypt(self)) {
		if (tmpbuf && (newlen > 0))
			self->dofunc(self->do_logicdata, tmpbuf, newlen);
	}

	/* end change data size. */
	blocklist_add_write(lst, len);
}

/* 
 * recv end, do something, if return flase, then close connect.
 * */
bool buf_recv_end_do(struct net_buf *self) {
	if (!self)
		return false;
	if (buf_is_use_uncompress(self)) {
		/* get a compress packet, uncompress it, and then push the queue. */
		struct blocklist *lst = &self->iolist;
		int res;
		struct buf_info srcbuf;
		struct buf_info resbuf;
		bool pushresult;
		struct buf_info compressbuf = threadbuf_get_compress_buf();
		struct buf_info msgbuf = threadbuf_get_msg_buf();
		char *quicklzbuf = threadbuf_get_quicklz_buf();
		for (;;) {
			res = blocklist_get_message(lst, msgbuf.buf, msgbuf.len);
			if (res == 0)
				break;

			if (res < 0) {
				if (s_enable_errorlog) {
					log_error("msg length error. max message len:%d, message len:%d", (int)lst->message_maxlen, (int)lst->message_len);
				}
				return false;
			}

			srcbuf.buf = msgbuf.buf;
			srcbuf.len = res;

			/* uncompress function will be responsible for header length of removed. */
			resbuf = compressmgr_uncompressdata(compressbuf.buf, compressbuf.len, quicklzbuf, srcbuf.buf, srcbuf.len);
			
			/* if return null, then uncompress error,
			 * uncompress error, probably because the uncompress buffer is less than uncompress data length. */
			if (!resbuf.buf) {
				log_error("uncompress buf is too small!");
				return false;
			}
			assert(resbuf.len > 0);
			pushresult = blocklist_put_data(&self->logiclist, resbuf.buf, resbuf.len);
			assert(pushresult);
			if (!pushresult) {
				log_error("if (!pushresult)");
				return false;
			}
		}
	}
	return true;
}

/*
 * some send interface.
 *
 * */

/* get read buffer info. */
struct buf_info buf_getreadbufinfo(struct net_buf *self) {
	struct buf_info readbuf;
	struct blocklist *lst;

	readbuf.buf = NULL;
	readbuf.len = 0;

	if (!self)
		return readbuf;

	if (buf_is_use_compress(self))
		lst = &self->iolist;
	else
		lst = &self->logiclist;

	readbuf = blocklist_get_read_bufinfo(lst);
	if (readbuf.len > 0) {
		if (buf_is_use_encrypt(self)) {
			/* encrypt */
			struct buf_info encrybuf = block_get_do_process(lst->head);
			assert(encrybuf.len >= 0);
			if (self->raw_size_for_encrypt <= encrybuf.len) {
				encrybuf.len -= self->raw_size_for_encrypt;
				encrybuf.buf = &encrybuf.buf[self->raw_size_for_encrypt];
				self->raw_size_for_encrypt = 0;
				self->dofunc(self->do_logicdata, encrybuf.buf, encrybuf.len);
			} else {
				self->raw_size_for_encrypt -= encrybuf.len;
			}
		}
	}
	return readbuf;
}

/* add read position. */
void buf_addread(struct net_buf *self, int len) {
	assert(len > 0);
	if (!self)
		return;
	if (buf_is_use_compress(self))
		blocklist_add_read(&self->iolist, len);
	else
		blocklist_add_read(&self->logiclist, len);
}

/* before send, do something. */
void buf_send_before_do(struct net_buf *self) {
	if (!self)
		return;
	if (buf_is_use_compress(self)) {
		/* get all can read data, compress it. (compress data header is compress function do.)*/
		bool pushresult = false;
		struct buf_info resbuf;
		struct buf_info srcbuf;
		struct buf_info compressbuf = threadbuf_get_compress_buf();
		char *quicklzbuf = threadbuf_get_quicklz_buf();
		for (;;) {
			srcbuf = blocklist_get_read_bufinfo(&self->logiclist);
			srcbuf.len = min(srcbuf.len, self->logiclist.message_maxlen);
			assert(srcbuf.len >= 0);
			if ((srcbuf.len <= 0) || (!srcbuf.buf))
				break;
			if (self->raw_size_for_compress != 0) {
				if (self->raw_size_for_compress <= srcbuf.len) {
					srcbuf.len = self->raw_size_for_compress;
					self->raw_size_for_compress = 0;
				} else {
					self->raw_size_for_compress -= srcbuf.len;
				}

				resbuf.len = srcbuf.len;
				resbuf.buf = srcbuf.buf;
			} else {
				resbuf = compressmgr_do_compressdata(compressbuf.buf, quicklzbuf, srcbuf.buf, srcbuf.len);
			}

			assert(resbuf.len > 0);
			pushresult = blocklist_put_data(&self->iolist, resbuf.buf, resbuf.len);
			assert(pushresult);
			if (!pushresult)
				log_error("if (!pushresult)");
			blocklist_add_read(&self->logiclist, srcbuf.len);
		}
	}
}

/* push packet into the buffer. */
bool buf_pushmessage(struct net_buf *self, const char *msgbuf, int len) {
	assert(msgbuf != NULL);
	assert(len > 0);
	if (!self || (len <= 0))
		return false;
	return blocklist_put_message(&self->logiclist, msgbuf, len);
}

/* get packet from the buffer, if error, then needclose is true. */
char *buf_getmessage(struct net_buf *self, bool *needclose, char *buf, size_t bufsize) {
	struct buf_info dst;
	int res;
	if (!self || !needclose)
		return NULL;
	if (self->use_tgw && (!self->already_do_tgw))
		return NULL;

	if (!buf || bufsize <= 0) {
		dst = threadbuf_get_msg_buf();
	} else {
		if (bufsize < _MAX_MSG_LEN) {
			assert(false && "why bufsize < _MAX_MSG_LEN");
			return NULL;
		}
		
		dst.buf = buf;
		dst.len = (int)bufsize;
	}

	res = blocklist_get_message(&self->logiclist, dst.buf, dst.len);
	if (res == 0) {
		return NULL;
	} else if (res > 0) {
		return dst.buf;
	} else {
		*needclose = true;
		if (s_enable_errorlog) {
			log_error("msg length error. max message len:%d, message len:%d", (int)self->logiclist.message_maxlen, (int)self->logiclist.message_len);
		}
		return NULL;
	}
}

/* get data from the buffer, if error, then needclose is true. */
char *buf_getdata(struct net_buf *self, bool *needclose, char *buf, int bufsize, int *datalen) {
	struct blocklist *lst;
	if (!self || !needclose)
		return NULL;
	if (self->use_tgw && (!self->already_do_tgw))
		return NULL;

	if (!buf || bufsize <= 0 || !datalen)
		return NULL;

	lst = &self->logiclist;
	if (blocklist_get_datasize(lst) <= 0)
		return NULL;
	
	if (!blocklist_get_data(lst, buf, bufsize, datalen)) {
		*needclose = true;
		return NULL;
	}

	return buf;
}

/* 
 * create and init buf pool.
 * bigbufnum --- is bigbuf num.
 * bigbufsize --- is bigbuf size.
 * smallbufnum --- is small buf num.
 * smallbufsize --- is small buf size.
 * bufnum --- is buf num.
 * 
 * because a socket need 2 buf, so bufnum is *2, in init function.
 *
 * this function be able to call private thread buffer etc.
 * */
bool bufmgr_init(size_t bigbufnum, size_t bigbufsize, size_t smallbufnum, size_t smallbufsize, size_t bufnum) {
	if ((bigbufnum == 0) || (bigbufsize == 0) || (smallbufnum == 0) || (smallbufsize == 0))
		return false;

	if (!threadbuf_init(_MAX_MSG_LEN + 512, _MAX_MSG_LEN + 512))
		return false;

	bigbufsize += sizeof(struct block);
	smallbufsize += sizeof(struct block);

	if (!bufpool_init(bigbufnum, bigbufsize, 
					 smallbufnum, smallbufsize, 
					 bufnum * 2, sizeof(struct net_buf))) {
		return false;
	}

	s_blockinfo.bigblocksize = bigbufsize;
	s_blockinfo.smallblocksize = smallbufsize;
	return true;
}

/* release some buf. */
void bufmgr_release() {
	bufpool_release();
	threadbuf_release();
}

/* get some buf memroy info. */
void bufmgr_meminfo(char *buf, size_t bufsize) {
	bufpool_meminfo(buf, bufsize);
}

/* enable/disable errorlog, and return before value. */
bool buf_set_enable_errorlog(bool flag) {
	bool old = s_enable_errorlog;
	s_enable_errorlog = flag;
	return old;
}

/* get now enable or disable errorlog. */
bool buf_get_enable_errorlog() {
	return s_enable_errorlog;
}


