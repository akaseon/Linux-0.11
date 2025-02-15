/*
 *  linux/kernel/exit.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <asm/segment.h>

int sys_pause(void);
int sys_close(int fd);

void release(struct task_struct * p)
{
	int i;

	if (!p)
		return;
	for (i=1 ; i<NR_TASKS ; i++)
		if (task[i]==p) {
			task[i]=NULL;
			free_page((long)p);
			schedule();
			return;
		}
	panic("trying to release non-existent task");
}

static inline int send_sig(long sig,struct task_struct * p,int priv)
{
    // validate
	if (!p || sig<1 || sig>32)
		return -EINVAL;

    // 권한 체크- 같은 그룹이거나 euid : effetive user id가 같거나 슈퍼유저 
	if (priv || (current->euid==p->euid) || suser())
    {
		p->signal |= (1<<(sig-1));
    }
	else
    {
		return -EPERM;
    }

	return 0;
}

static void kill_session(void)
{
	struct task_struct **p = NR_TASKS + task;
	
	while (--p > &FIRST_TASK) 
    {
		if (*p && (*p)->session == current->session)
        {
            // SIGHUP : 터미널이 연결 종료될때 발생
			(*p)->signal |= 1<<(SIGHUP-1);
        }
	}
}

/*
 * XXX need to check permissions needed to send signals to process
 * groups, etc. etc.  kill() permissions semantics are tricky!
 */
int sys_kill(int pid,int sig)
{
	struct task_struct **p = NR_TASKS + task;
	int err, retval = 0;

    if (!pid) // pid 가 0인 경우, 자식
        while (--p > &FIRST_TASK) 
        {//pgrp 프로세스 그룹?
            if (*p && (*p)->pgrp == current->pid) 
                if ((err=send_sig(sig,*p,1)))// priv = 1 (권한)
                    retval = err;
        } 
    else if (pid>0) // 0보다 클때
        while (--p > &FIRST_TASK)
        {
            if (*p && (*p)->pid == pid) 
                if ((err=send_sig(sig,*p,0)))// priv = 0 (권한없음)
                    retval = err;
        } 
    else if (pid == -1) // 모든 프로세스
        while (--p > &FIRST_TASK) 
        {
            if ((err = send_sig(sig,*p,0)))// priv = 0 (권한없음)
                retval = err;
        } 
    else 
        while (--p > &FIRST_TASK)
            if (*p && (*p)->pgrp == -pid)// 특정 그룹
                if ((err = send_sig(sig,*p,0)))
                    retval = err;
        return retval;
}

static void tell_father(int pid)
{
	int i;

	if (pid)
    {
		for (i=0;i<NR_TASKS;i++) 
        {
			if (!task[i])
				continue;
			if (task[i]->pid != pid)
				continue;

            // 부모에게 sigchild signal
			task[i]->signal |= (1<<(SIGCHLD-1));
			return;
		}
    }
/* if we don't find any fathers, we just release ourselves */
/* This is not really OK. Must change it to make father 1 */
	printk("BAD BAD - no father found\n\r");
	release(current);
}

int do_exit(long code)
{
	int i;

    // 코드와 데이터 세그먼트가 있던 페이지를 해제 한다.
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));

    // 자식들 먼저 처리
	for (i=0 ; i<NR_TASKS ; i++)
    {
		if (task[i] && task[i]->father == current->pid) 
        {
            // init 으로 부모를 변경
            task[i]->father = 1;
            // 자식이 좀비이면
			if (task[i]->state == TASK_ZOMBIE)
            {
                // sigchild 를 inti 에 signal 발생
				/* assumption task[1] is always init */
				(void) send_sig(SIGCHLD, task[1], 1);
            }
		}
    }
    
    // 파일 close
	for (i=0 ; i<NR_OPEN ; i++)
    {
		if (current->filp[i])
			sys_close(i);
    }

	iput(current->pwd);
	current->pwd=NULL;
	iput(current->root);
	current->root=NULL;
	iput(current->executable);
	current->executable=NULL;

	if (current->leader && current->tty >= 0)
		tty_table[current->tty].pgrp = 0;

	if (last_task_used_math == current)
		last_task_used_math = NULL;

	if (current->leader)
		kill_session();

	current->state = TASK_ZOMBIE;

    // exit code 를 설정
	current->exit_code = code;
    
    // sigchild 를 부모에게 전달 
	tell_father(current->father);

	schedule();

	return (-1);	/* just to suppress warnings */
}

int sys_exit(int error_code)
{
	return do_exit((error_code&0xff)<<8);
}

int sys_waitpid(pid_t pid,unsigned long * stat_addr, int options)
{
	int flag, code;
	struct task_struct ** p;

	//추정1 스택 어드레스의 write의 권한 확인.
	//추정2 커널모드에서 유저영역(Process1) 스택에 데이터를 사용
	verify_area(stat_addr,4);
repeat:
	flag=0;
	
	//테스크를 전부 뒤지면서 기다릴 자식할 프로세스를 찾는다.
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p) 
	{
		//프로세스가 없거나 찾은 프로세스가 현재 프로세스이면 
		if (!*p || *p == current)
			continue;
		// 현재 프로세스가 찾은 프로세스의 부모가 아닌 경우 
		if ((*p)->father != current->pid)
			continue;

		if (pid>0) 
		{
			if ((*p)->pid != pid)
				continue;
		} 
		else if (!pid) 
		{
			if ((*p)->pgrp != current->pgrp)
				continue;
		} 
		else if (pid != -1) 
		{
			if ((*p)->pgrp != -pid)
				continue;
		}

		switch ((*p)->state) 
		{
			case TASK_STOPPED:
				if (!(options & WUNTRACED))
					continue;
				put_fs_long(0x7f,stat_addr);
				return (*p)->pid;
			case TASK_ZOMBIE:
				current->cutime += (*p)->utime;
				current->cstime += (*p)->stime;
				flag = (*p)->pid;
				code = (*p)->exit_code;
				release(*p);                 // 자식 Task를 해제
				put_fs_long(code,stat_addr); // code 를 user 영역으로
				return flag;
			default:
				// 아직 stop 되지 않은 child가 존재
				//Running 상태
				flag=1;
				continue;
		}
	}

	if (flag) 
	{
		//자식이 종료되지 않더라도 리턴
		if (options & WNOHANG)
			return 0;

		current->state=TASK_INTERRUPTIBLE;
		schedule();// 종료 될때까지 스케쥴 프로세스 2로 전환 

        // sigchild 
        // check -> clear
		//if (!(current->signal = (current->signal & ~(1<<(SIGCHLD-1)))))
		if (!(current->signal &= ~(1<<(SIGCHLD-1))))
			goto repeat;
		else
			return -EINTR;
	}

	return -ECHILD;
}


