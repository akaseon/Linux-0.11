/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/io.h>

extern int end;
extern void put_super(int);
extern void invalidate_inodes(int);

struct buffer_head * start_buffer = (struct buffer_head *) &end;
struct buffer_head * hash_table[NR_HASH];
static struct buffer_head * free_list;
static struct task_struct * buffer_wait = NULL;
int NR_BUFFERS = 0;

static inline void wait_on_buffer(struct buffer_head * bh)
{
	cli();      // interrupt 막고 
    // b_lock이 풀릴때까지 loop
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	sti();      // interrupt 풀고
}

int sys_sync(void)
{
	int i;
	struct buffer_head * bh;

	sync_inodes();		/* write out inodes into buffers */

    // 실제 데이터를 디스크에 내리는 작업은 아래서 수행한다.
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		wait_on_buffer(bh);
		if (bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

    // 먼저 bh 를 디스크에 내린다.
    // 이유는 이 시점에서는 이미 bh 를 다 사용했다 
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}

    // sys_sync
	sync_inodes();
	bh = start_buffer;
    //sync_inodes 에서 버퍼에 쓰기 때문에 한번 더 버퍼를 sync 한다.
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

static void inline invalidate_buffers(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev)
			bh->b_uptodate = bh->b_dirt = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
void check_disk_change(int dev)
{
	int i;

    // 2 : floppy
	if (MAJOR(dev) != 2)
		return;

	if (!floppy_change(dev & 0x03))
		return;

	for (i=0 ; i<NR_SUPER ; i++)
		if (super_block[i].s_dev == dev)
			put_super(super_block[i].s_dev);
    
	invalidate_inodes(dev);
	invalidate_buffers(dev);
}

#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]

static inline void remove_from_queues(struct buffer_head * bh)
{
/* remove from hash-queue */
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
/* remove from free list */
	if (!(bh->b_prev_free) || !(bh->b_next_free))
        // 초기화시 환형 자료구조이기 때문에
        // prev, next 가 null 이면 메모리 깨짐
		panic("Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	if (free_list == bh)
        // free_list 포인터가 가리키는 버퍼가 사용해야할 버퍼 이면 
        // 다음 버퍼로free_list 포인터가 가리키도록 변경 
		free_list = bh->b_next_free;
}

static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	hash(bh->b_dev,bh->b_blocknr) = bh;
	bh->b_next->b_prev = bh;
}

/*
#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]
*/
static struct buffer_head * find_buffer(int dev, int block)
{		
	struct buffer_head * tmp;

    // 충돌이 나면 여러번 실행
	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
    {
        if (tmp->b_dev==dev && tmp->b_blocknr==block)
			return tmp;
    }

    return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

	for (;;) {
		if (!(bh=find_buffer(dev,block)))
			return NULL;
		bh->b_count++;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_blocknr == block)
			return bh;
		bh->b_count--;
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 */
/*
 * unsigned char b_dirt;		 0-clean,1-dirty 
 * unsigned char b_lock;		 0 - ok, 1 -locked 
 */
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)
/* BADNESS 는dirty 하지 않고 lock이 안걸려 있고
 * #define BADNESS(bh) (((bh)->b_dirt*2)+(bh)->b_lock)
 * [dirty][lock]
 */
// buffer_head 를 할당받거나
// 같은 dev, block 정보를 가지는 buffer_head 를 받는다.
// 이 함수는 실패 되지 않는다.
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp, * bh;

repeat:
	if (bh = get_hash_table(dev,block))
		return bh;

    /* free_list 초기화는 buffer_init() 에서 수행 
     * 최초수행시 start_buffer 를 가리키고 있음
     */
    tmp = free_list;
	do {
		if (tmp->b_count) // 참조 카운트
        	//unsigned char b_count;		/* users using this block */
			continue;

		if (!bh || BADNESS(tmp) < BADNESS(bh)) {
			bh = tmp;
			if (!BADNESS(tmp))
                // dirty 하지 않고 lock 도 없고
                // 깨끗한
				break;
		}
/* and repeat until we find something good */
	} while ((tmp = tmp->b_next_free) != free_list);

	if (!bh) {
        // 위 loop 에서 참조 카운트 문제로 모두다 continue
        // 사용할수 있는 버퍼를 찾지 못하면
		sleep_on(&buffer_wait);
		goto repeat;
	}

	wait_on_buffer(bh); // lock 이 풀릴때까지 대기 
	if (bh->b_count)
		goto repeat;
    // dirty 이면
	while (bh->b_dirt) {
		sync_dev(bh->b_dev);
		wait_on_buffer(bh);
		if (bh->b_count)
			goto repeat;//맨위로 다시 가서 리턴
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
    // 사용할 버퍼를 다시 찾아본다.
    // 해쉬 테이블에 연결된 버퍼인지 확인
    // 다른 프로세스가 이 블록을 캐쉬에 추가 할수 있다
	if (find_buffer(dev,block))
    {
        // hash 테이블에 등록되어 있으면 위로 올라가 
        // buffer 를 다시 찾는다.
		goto repeat;
    }

/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
	bh->b_count=1;
	bh->b_dirt=0;
	bh->b_uptodate=0;
    remove_from_queues(bh);
	bh->b_dev=dev;// 새 블록넘버를 설정한다
	bh->b_blocknr=block;//새 버퍼 블록에 블록 넘버를 설정한다
	insert_into_queues(bh);
	return bh;
}

void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	if (!(buf->b_count--))
		panic("Trying to free free buffer");
	wake_up(&buffer_wait);
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
struct buffer_head * bread(int dev,int block)
{
	struct buffer_head * bh;

	if (!(bh=getblk(dev,block)))
		panic("bread: getblk returned NULL\n");
	if (bh->b_uptodate)
		return bh;//최신이면 읽어오지 않고 그냥 리턴
	// 디스크에서 읽어온다
    ll_rw_block(READ,bh);
	// 스케쥴링이 일어나다
    wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;//최신이면 리턴
	brelse(bh);
	return NULL;
}

#define COPYBLK(from,to) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"movsl\n\t" \
	::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
	)

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 */
void bread_page(unsigned long address,int dev,int b[4])
{
	struct buffer_head * bh[4];
	int i;

    // 데이터 읽기
	for (i=0 ; i<4 ; i++)
    {
		if (b[i])  // 읽을 데이터가 있으면 
        {
			if ((bh[i] = getblk(dev,b[i])))
            {
				if (!bh[i]->b_uptodate) // 데이터를 채워넣어야 하면
                {
					ll_rw_block(READ,bh[i]);
                }
            }
            else
            {
                /* 이 부분은 실행 할수 없다. */
            }
		} 
        else
			bh[i] = NULL;
    }

    // 읽은 데이터를 주소에 복사
	for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
    {
		if (bh[i]) 
        {
			wait_on_buffer(bh[i]);

			if (bh[i]->b_uptodate)
            {
				COPYBLK((unsigned long) bh[i]->b_data,address);
            }

			brelse(bh[i]);
		}
    }
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
struct buffer_head * breada(int dev,int first, ...)
{
	va_list args;
	struct buffer_head * bh, *tmp;

	va_start(args,first);
	if (!(bh=getblk(dev,first)))
		panic("bread: getblk returned NULL\n");
	if (!bh->b_uptodate)
		ll_rw_block(READ,bh);
	while ((first=va_arg(args,int))>=0) {
		tmp=getblk(dev,first);
		if (tmp) {
			if (!tmp->b_uptodate)
				ll_rw_block(READA,bh);
			tmp->b_count--;
		}
	}
	va_end(args);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}

void buffer_init(long buffer_end)
{
	struct buffer_head * h = start_buffer;
	void * b;
	int i;

	if (buffer_end == 1<<20)
		b = (void *) (640*1024);
	else
		b = (void *) buffer_end;
	while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) {
		h->b_dev = 0;
		h->b_dirt = 0;
		h->b_count = 0;
		h->b_lock = 0;
		h->b_uptodate = 0;
		h->b_wait = NULL;
		h->b_next = NULL;
		h->b_prev = NULL;
		h->b_data = (char *) b;
		h->b_prev_free = h-1;
		h->b_next_free = h+1;
		h++;
		NR_BUFFERS++;
		if (b == (void *) 0x100000)
			b = (void *) 0xA0000;
	}
	h--;
	free_list = start_buffer;
	free_list->b_prev_free = h;
	h->b_next_free = free_list;
	for (i=0;i<NR_HASH;i++)
		hash_table[i]=NULL;
}	
