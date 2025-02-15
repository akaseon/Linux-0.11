/*
 *  linux/fs/namei.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <string.h> 
#include <fcntl.h>
#include <errno.h>
#include <const.h>
#include <sys/stat.h>

#define ACC_MODE(x) ("\004\002\006\377"[(x)&O_ACCMODE])

/*
 * comment out this line if you want names > NAME_LEN chars to be
 * truncated. Else they will be disallowed.
 */
/* #define NO_TRUNCATE */

#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4

/*
 *	permission()
 *
 * is used to check for read/write/execute permissions on a file.
 * I don't know if we should look at just the euid or both euid and
 * uid, but that should be easily changed.
 */
static int permission(struct m_inode * inode,int mask)
{
	int mode = inode->i_mode;

/* special case: not even root can read/write a deleted file */
	if (inode->i_dev && !inode->i_nlinks)
		return 0;
	else if (current->euid==inode->i_uid)
		mode >>= 6;
	else if (current->egid==inode->i_gid)
		mode >>= 3;
	if (((mode & mask & 0007) == mask) || suser())
		return 1;
	return 0;
}

/*
 * ok, we cannot use strncmp, as the name is not in our data space.
 * Thus we'll have to use match. No big problem. Match also makes
 * some sanity tests.
 *
 * NOTE! unlike strncmp, match returns 1 for success, 0 for failure.
 */
static int match(int len,const char * name,struct dir_entry * de)
{
	register int same ;

	if (!de || !de->inode || len > NAME_LEN)
		return 0;
	// 내가 찾을 글자 수 이상 데이터가 존재하면 
	if (len < NAME_LEN && de->name[len])
		return 0;

	//TODO 어셈 해석 불가. 의미는 대충 파악 
	__asm__("cld\n\t"
		"fs ; repe ; cmpsb\n\t"
		"setz %%al"
		:"=a" (same)
		:"0" (0),"S" ((long) name),"D" ((long) de->name),"c" (len)
		);
	return same;
}

/*
 *	find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * This also takes care of the few special cases due to '..'-traversal
 * over a pseudo-root and a mount point.
 */
static struct buffer_head * find_entry(struct m_inode ** dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int entries;
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;
	struct super_block * sb;

#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN) //파일명 길이가 14가 넘으면 null로 
		return NULL;
#else
	if (namelen > NAME_LEN) //파일명 길이가 14가 넘으면 14로 
		namelen = NAME_LEN;
#endif

	//TODO i_size ... 
	entries = (*dir)->i_size / (sizeof (struct dir_entry));
	*res_dir = NULL;
	if (!namelen)
		return NULL;
/* check for '..', as we might have to do some "magic" for it */
	// .. 이면 예시 /mnt/a/g/.. 
	if (namelen==2 && get_fs_byte(name)=='.' && get_fs_byte(name+1)=='.') 
	{
		/* '..' in a pseudo-root results in a faked '.' (just change namelen) */
		if ((*dir) == current->root) 
			namelen=1; // 내가 루트인데 .. 을 주면 . 으로 변경
		else if ((*dir)->i_num == ROOT_INO) 
		{
/* '..' over a mount-point results in 'dir' being exchanged for the mounted
   directory-inode. NOTE! We set mounted, so that we can iput the new dir */
			sb=get_super((*dir)->i_dev);
			if (sb->s_imount) 
			{
				iput(*dir);
				(*dir)=sb->s_imount;
				(*dir)->i_count++;
			}
		}
	}

	if (!(block = (*dir)->i_zone[0]))
		return NULL;
	if (!(bh = bread((*dir)->i_dev,block)))
		return NULL;
	i = 0;
	de = (struct dir_entry *) bh->b_data;
	while (i < entries) //현재 디렉터리내의 엔트리들
	{
		//현재 논리 븐럭에서 원하는 디렉토리 엔트리를 찾지 못하면 
		if ( (char *)de >= BLOCK_SIZE+bh->b_data ) 
		{
			brelse(bh);
			bh = NULL;
			if (!(block = bmap(*dir,i/DIR_ENTRIES_PER_BLOCK)) ||
			    !(bh = bread((*dir)->i_dev,block))) 
			{
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
        //이름이 동일한지 검사.
		if (match(namelen,name,de)) {
			*res_dir = de;
			return bh; // 찾은 경우
		}
		de++;
		i++;
	}
	brelse(bh);
	return NULL;// 못찾은 경우
}

/*
 *	add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
static struct buffer_head * add_entry(struct m_inode * dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;

	*res_dir = NULL;

#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
    
	if (!namelen)
		return NULL;

	if ( !(block = dir->i_zone[0]) )
		return NULL;

	if (!(bh = bread(dir->i_dev,block)))
		return NULL;

	i = 0; // de 를 해당 위치에 저장할수 있는지 확인
	de = (struct dir_entry *) bh->b_data;
	while (1) 
    {
        // de 를 저장할 곳이 없으면
		if ( (char *)de >= (BLOCK_SIZE + bh->b_data) ) 
        {
			brelse(bh);
			bh = NULL;
            //#define DIR_ENTRIES_PER_BLOCK ((BLOCK_SIZE)/(sizeof (struct dir_entry)))
            // dir->i_zone[1~] 에 할당
            // i 가 DIR_ENTRIES_PER_BLOCK 보다 클것이다.
			block = create_block( dir, i/DIR_ENTRIES_PER_BLOCK );
			if (!block)
				return NULL;

			if ( !(bh = bread(dir->i_dev,block)) ) 
            {
				i += DIR_ENTRIES_PER_BLOCK;
                // bh 가 null 이므로 continue 하면 죽을것 같다.
                // Null pointer dereference
				continue;
			}

			de = (struct dir_entry *) bh->b_data;
		}

        // 맨 뒤에 추가 하게 되면 p.299 case 1 
		if ( (i*sizeof(struct dir_entry)) >= dir->i_size) 
        {
            // size 를 계산을 다시 해준다.
			de->inode=0;
			dir->i_size = (i+1)*sizeof(struct dir_entry);
			dir->i_dirt = 1;
			dir->i_ctime = CURRENT_TIME;
		}

        // 빈 item 이면 
		if (!de->inode) 
        {
			dir->i_mtime = CURRENT_TIME;

            // 이름을 설정 한다
			for ( i=0; i < NAME_LEN ; i++ )
				de->name[i]=(i<namelen)?get_fs_byte(name+i):0;

			bh->b_dirt = 1;
			*res_dir = de;

			return bh;
		}

		de++;
		i++;
	}

	brelse(bh);

	return NULL;
}

/*
 *	get_dir()
 *
 * Getdir traverses the pathname until it hits the topmost directory.
 * It returns NULL on failure.
 */
static struct m_inode * get_dir(const char * pathname)
{
	char c;
	const char * thisname;
	struct m_inode * inode;
	struct buffer_head * bh;
	int namelen,inr,idev;
	struct dir_entry * de;

	//현재 루트 i-node가 없거나 참조 카운터가 0인 경우 
	if (!current->root || !current->root->i_count)
		panic("No root inode");
	//현재 디렉토리의 i-node가 없거나 참조 카운터가 0인 경우 
	if (!current->pwd || !current->pwd->i_count)
		panic("No cwd inode");
	// root 에서 파일을 읽으면(절대경로) /dev/tty0 
	if ((c=get_fs_byte(pathname))=='/')
	{
		inode = current->root;
		pathname++;
	// 상대경로 이면 ./home
	} 
	else if (c)
		inode = current->pwd;
	// 잘못 들어온거
	else
		return NULL;	/* empty name is bad */

	inode->i_count++;

	while (1) //최종 디렉터리를 찾을 때 까지 반복 한다.
	{
		thisname = pathname;
		if (!S_ISDIR(inode->i_mode) ||
			!permission(inode,MAY_EXEC))
		{
			iput(inode);
			return NULL;
		}

		// mnt/user/user1/user2/hello.tx1 
		// for 수행 후 > user/user1/user2/hello.tx1 
		for(namelen=0; (c=get_fs_byte(pathname++)) && (c!='/'); namelen++)
			/* nothing */ ;
		
		//파일이면 
		if (!c)
			return inode; //디렉터리의 아이노드

        //한 디렉터리 안의 엔트리들중 동일한 이름을 찾는다.
		if (!(bh = find_entry(&inode,thisname,namelen,&de))) 
		{
			iput(inode);
			return NULL;
		}

		inr = de->inode;
		idev = inode->i_dev;

		brelse(bh);
		iput(inode);//반납 
		//하위 디렉터리의 아이노드를 찾는다.
		if (!(inode = iget(idev,inr)))
			return NULL;
	}
}

/*
 *	dir_namei()
 *
 * dir_namei() returns the inode of the directory of the
 * specified name, and the name within that directory.
 */
static struct m_inode * dir_namei(const char * pathname,
	int * namelen, const char ** name)
{
	char c;
	const char * basename;
	struct m_inode * dir;

	if (!(dir = get_dir(pathname)))
		return NULL;

	basename = pathname;
	while ((c=get_fs_byte(pathname++)))
		if (c=='/')
			basename=pathname;

	*namelen = pathname-basename-1;
	*name = basename;
	return dir;
}

/*
 *	namei()
 *
 * is used by most simple commands to get the inode of a specified name.
 * Open, link etc use their own routines, but this is enough for things
 * like 'chmod' etc.
 */
struct m_inode * namei(const char * pathname)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!(dir = dir_namei(pathname,&namelen,&basename)))
		return NULL;
	if (!namelen)			/* special case: '/usr/' etc */
		return dir;
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return NULL;
	}
	inr = de->inode;
	dev = dir->i_dev;
	brelse(bh);
	iput(dir);
	dir=iget(dev,inr);
	if (dir) {
		dir->i_atime=CURRENT_TIME;
		dir->i_dirt=1;
	}
	return dir;
}

/*
 *	open_namei()
 *
 * namei for open - this is in fact almost the whole open-routine.
 */
int open_namei(const char * pathname, int flag, int mode,
	struct m_inode ** res_inode)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir, *inode;
	struct buffer_head * bh;
	struct dir_entry * de;
	
	//파일이 읽기 전용이고, 파일 크기가 0(기존파일 제거 플래그)
	//이면 쓰기전용으로 설정한다. 
	if ((flag & O_TRUNC) && !(flag & O_ACCMODE))
		flag |= O_WRONLY; //쓰기 전용모드 

	mode &= 0777 & ~current->umask;
	//일반 파일로 설정한다. 
	mode |= I_REGULAR;
	//dir_namei 에서 최종 디렉터리를 가져온다. 
	if (!(dir = dir_namei(pathname,&namelen,&basename)))
		return -ENOENT;
	
	if (!namelen) //파일이 아닌 디렉토리인 경우.
	{			/* special case: '/usr/' etc */
		//추정 디렉터리 오픈 시 하위 3개 플래그를 사용하면 안됨
		if (!(flag & (O_ACCMODE|O_CREAT|O_TRUNC))) 
		{
			*res_inode=dir;
			return 0;
		}
		iput(dir);
		return -EISDIR;
	}

//struct buffer_head {
//	char * b_data;			/* pointer to data block (1024 bytes) */
//	unsigned long b_blocknr;	/* block number */
//	unsigned short b_dev;		/* device (0 = free) */
//	unsigned char b_uptodate;
//	unsigned char b_dirt;		/* 0-clean,1-dirty */
//	unsigned char b_count;		/* users using this block */
//	unsigned char b_lock;		/* 0 - ok, 1 -locked */
//	struct task_struct * b_wait;
//	struct buffer_head * b_prev;
//	struct buffer_head * b_next;
//	struct buffer_head * b_prev_free;
//	struct buffer_head * b_next_free;
//};
//struct dir_entry {
//	unsigned short inode;
//	char name[NAME_LEN];
//};
    // bh : file 의 메타 정보(dir에 저장되어 있는)
    // de : bh 안에 있는 file  정보
    // dir 에서 읽고 싶은 파일의 정보를 읽는다.
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) // 파일이 존재하지 않으면
	{
		if (!(flag & O_CREAT)) 
		{
			iput(dir);
			return -ENOENT;
		}

		if (!permission(dir,MAY_WRITE)) 
		{
			iput(dir);
			return -EACCES;
		}

		inode = new_inode(dir->i_dev);
		if (!inode) 
		{
			iput(dir);
			return -ENOSPC;
		}

		inode->i_uid = current->euid;
		inode->i_mode = mode;
		inode->i_dirt = 1;
		bh = add_entry(dir,basename,namelen,&de);
		if (!bh) 
		{
			inode->i_nlinks--;
			iput(inode);
			iput(dir);
			return -ENOSPC;
		}
		de->inode = inode->i_num;
		bh->b_dirt = 1;
		brelse(bh);
		iput(dir);
		*res_inode = inode;
		return 0;
	}
	inr = de->inode;
	dev = dir->i_dev;

    // de 사용이 끝난 이후에 bh 를 해제 한다.
    // de 가 bh의 일부분을 가리킨다.
	brelse(bh);
	iput(dir);

	if (flag & O_EXCL)
		return -EEXIST;

    // 읽고 싶은 file 의 inode 를 읽는다.
	if (!(inode=iget(dev,inr)))
		return -EACCES;
    
	if ( ( S_ISDIR(inode->i_mode ) && ( flag & O_ACCMODE ) ) ||
	    !permission( inode,ACC_MODE(flag) ) )
    {
		iput(inode);
		return -EPERM;
	}
	inode->i_atime = CURRENT_TIME;
	if (flag & O_TRUNC)
		truncate(inode);
	*res_inode = inode;
	return 0;
}

int sys_mknod(const char * filename, int mode, int dev)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;
	
	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(filename,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	inode = new_inode(dir->i_dev);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_mode = mode;
	if (S_ISBLK(mode) || S_ISCHR(mode))
		inode->i_zone[0] = dev;
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
	inode->i_dirt = 1;
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		inode->i_nlinks=0;
		iput(inode);
		return -ENOSPC;
	}
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

int sys_mkdir(const char * pathname, int mode)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh, *dir_block;
	struct dir_entry * de;

	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(pathname,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	inode = new_inode(dir->i_dev);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_size = 32;
	inode->i_dirt = 1;
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
	if (!(inode->i_zone[0]=new_block(inode->i_dev))) {
		iput(dir);
		inode->i_nlinks--;
		iput(inode);
		return -ENOSPC;
	}
	inode->i_dirt = 1;
	if (!(dir_block=bread(inode->i_dev,inode->i_zone[0]))) {
		iput(dir);
		free_block(inode->i_dev,inode->i_zone[0]);
		inode->i_nlinks--;
		iput(inode);
		return -ERROR;
	}
	de = (struct dir_entry *) dir_block->b_data;
	de->inode=inode->i_num;
	strcpy(de->name,".");
	de++;
	de->inode = dir->i_num;
	strcpy(de->name,"..");
	inode->i_nlinks = 2;
	dir_block->b_dirt = 1;
	brelse(dir_block);
	inode->i_mode = I_DIRECTORY | (mode & 0777 & ~current->umask);
	inode->i_dirt = 1;
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		free_block(inode->i_dev,inode->i_zone[0]);
		inode->i_nlinks=0;
		iput(inode);
		return -ENOSPC;
	}
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	dir->i_nlinks++;
	dir->i_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int empty_dir(struct m_inode * inode)
{
	int nr,block;
	int len;
	struct buffer_head * bh;
	struct dir_entry * de;

	len = inode->i_size / sizeof (struct dir_entry);
	if (len<2 || !inode->i_zone[0] ||
	    !(bh=bread(inode->i_dev,inode->i_zone[0]))) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	de = (struct dir_entry *) bh->b_data;
	if (de[0].inode != inode->i_num || !de[1].inode || 
	    strcmp(".",de[0].name) || strcmp("..",de[1].name)) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	nr = 2;
	de += 2;
	while (nr<len) {
		if ((void *) de >= (void *) (bh->b_data+BLOCK_SIZE)) {
			brelse(bh);
			block=bmap(inode,nr/DIR_ENTRIES_PER_BLOCK);
			if (!block) {
				nr += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			if (!(bh=bread(inode->i_dev,block)))
				return 0;
			de = (struct dir_entry *) bh->b_data;
		}
		if (de->inode) {
			brelse(bh);
			return 0;
		}
		de++;
		nr++;
	}
	brelse(bh);
	return 1;
}

int sys_rmdir(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!suser())
		return -EPERM;
	if (!(dir = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	if (!(inode = iget(dir->i_dev, de->inode))) {
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if ((dir->i_mode & S_ISVTX) && current->euid &&
	    inode->i_uid != current->euid) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (inode->i_dev != dir->i_dev || inode->i_count>1) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (inode == dir) {	/* we may not delete ".", but "../dir" is ok */
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTDIR;
	}
	if (!empty_dir(inode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTEMPTY;
	}
	if (inode->i_nlinks != 2)
		printk("empty directory has nlink!=2 (%d)",inode->i_nlinks);
	de->inode = 0;
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks=0;
	inode->i_dirt=1;
	dir->i_nlinks--;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_dirt=1;
	iput(dir);
	iput(inode);
	return 0;
}

int sys_unlink(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!(dir = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);//파일의 디렉토리 엔트리를 찾는다.
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	if (!(inode = iget(dir->i_dev, de->inode))) {
		iput(dir);
		brelse(bh);
		return -ENOENT;
	}
	if ((dir->i_mode & S_ISVTX) && !suser() &&//S_ISVTX는 sticky bit로 삭제나 변경이 가능한지 확인
	    current->euid != inode->i_uid &&
	    current->euid != dir->i_uid) {// 유저 프로세스가 파일 쓰기 권한이 없는 경우
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if (!inode->i_nlinks) {
		printk("Deleting nonexistent file (%04x:%d), %d\n",
			inode->i_dev,inode->i_num,inode->i_nlinks);
		inode->i_nlinks=1;
	}
	de->inode = 0;
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks--;
	inode->i_dirt = 1;
	inode->i_ctime = CURRENT_TIME;
	iput(inode);
	iput(dir);
	return 0;
}

int sys_link(const char * oldname, const char * newname)
{
	struct dir_entry * de;
	struct m_inode * oldinode, * dir;
	struct buffer_head * bh;
	const char * basename;
	int namelen;

	oldinode=namei(oldname);
	if (!oldinode)
		return -ENOENT;
	if (S_ISDIR(oldinode->i_mode)) {
		iput(oldinode);
		return -EPERM;
	}
	dir = dir_namei(newname,&namelen,&basename);
	if (!dir) {
		iput(oldinode);
		return -EACCES;
	}
	if (!namelen) {
		iput(oldinode);
		iput(dir);
		return -EPERM;
	}
	if (dir->i_dev != oldinode->i_dev) {
		iput(dir);
		iput(oldinode);
		return -EXDEV;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		iput(oldinode);
		return -EACCES;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		iput(oldinode);
		return -EEXIST;
	}
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		iput(oldinode);
		return -ENOSPC;
	}
	de->inode = oldinode->i_num;
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir);
	oldinode->i_nlinks++;
	oldinode->i_ctime = CURRENT_TIME;
	oldinode->i_dirt = 1;
	iput(oldinode);
	return 0;
}
