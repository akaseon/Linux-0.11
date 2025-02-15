/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * This handles all read/write requests to block devices
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 */
struct request request[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 */
// 빈 req 가 없으면 wait_for_request 에 달아 놓는다.
struct task_struct * wait_for_request = NULL;

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 */
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
	{ NULL, NULL },		/* no_dev */
	{ NULL, NULL },		/* dev mem */
	{ NULL, NULL },		/* dev fd */
	{ NULL, NULL },		/* dev hd */
	{ NULL, NULL },		/* dev ttyx */
	{ NULL, NULL },		/* dev tty */
	{ NULL, NULL }		/* dev lp */
};

static inline void lock_buffer(struct buffer_head * bh)
{
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	bh->b_lock=1;
	sti();
}

static inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk("ll_rw_block.c: buffer not locked\n\r");
	bh->b_lock = 0;
	wake_up(&bh->b_wait);
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 */
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
	struct request * tmp;

	req->next = NULL;
	cli();//인터럽트 중지 

	if (req->bh)
		req->bh->b_dirt = 0;

    // dev 에 이전의 request 없으면
	if (!(tmp = dev->current_request)) {
		dev->current_request = req;
		sti();//인터럽트 재시작 
		(dev->request_fn)();
		return;
	}

    // dev 에 이전의 request 가 있으면 아래를 수행 

    // 엘레베이터 알고리즘 적용하여
    // 적절한 위치를 찾는다.
    /* 우선순위 
     1. cmd  (read/write)
     2. dev
     3. sector
     */
	for ( ; tmp->next ; tmp=tmp->next)
		if ( (IN_ORDER(tmp,req) || !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next) )
			break;

	req->next=tmp->next;
	tmp->next=req;
	sti();
}

static void make_request(int major,int rw, struct buffer_head * bh)
{
	struct request * req;
	int rw_ahead;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
	if ( (rw_ahead = (rw == READA || rw == WRITEA)) )
    {
		if (bh->b_lock)
			return;
		if (rw == READA)
			rw = READ;
		else
			rw = WRITE;
	}

	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W/RA/WA");

	lock_buffer(bh);
    //write 일때 dirt가 아니면 무시.
    //uptodate 가 1 일때 READ면 무시. uptodate가 1로 세팅되어 있으면 최신 데이터.
	if ((rw == WRITE && !bh->b_dirt) || 
        (rw == READ && bh->b_uptodate)) {
		unlock_buffer(bh);
		return;
	}
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third
 * of the requests are only for reads.
 */
/*
 NR_REQUEST 32
struct request {
	int dev;		 -1 if no request 
	int cmd;		 READ or WRITE 
	int errors;
	unsigned long sector;
	unsigned long nr_sectors;
	char * buffer;
	struct task_struct * waiting;
	struct buffer_head * bh;
	struct request * next;
};
*/
	if (rw == READ)
		req = request+NR_REQUEST; // req = &req[NR_REQUEST]
	else
		req = request+((NR_REQUEST*2)/3);

/* find an empty request */
	while (--req >= request)
    {
		if (req->dev<0)
			break;
    }

/* if none found, sleep on new requests: check for rw_ahead */
	if (req < request)  
    {
		if (rw_ahead) {
			unlock_buffer(bh);
			return;
		}
        ///*
        // * used to wait on when there are no free requests
        // */
        //struct task_struct * wait_for_request = NULL;
		sleep_on(&wait_for_request);
		goto repeat;
	}

/* fill up the request-info, and add it to the queue */
	req->dev = bh->b_dev;
	req->cmd = rw;
	req->errors=0;
	req->sector = bh->b_blocknr<<1;
	req->nr_sectors = 2;        // 읽을 섹터 수
	req->buffer = bh->b_data;   // 버퍼 블록 주소
	req->waiting = NULL;
	req->bh = bh;
	req->next = NULL;

	add_request(major+blk_dev,req); // add_request( &blk_dev[major], req );
}

void ll_rw_block(int rw, struct buffer_head * bh)
{
	unsigned int major;
/*
#define MAJOR(a) (((unsigned)(a))>>8)
#define MINOR(a) ((a)&0xff)
*/
	if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV ||
	    !(blk_dev[major].request_fn)) {
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}
	make_request(major,rw,bh);
}

void blk_dev_init(void)
{
	int i;

	for (i=0 ; i<NR_REQUEST ; i++) {
		request[i].dev = -1;
		request[i].next = NULL;
	}
}
