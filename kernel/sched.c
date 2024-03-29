/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <signal.h>
#include <linux/sys.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void);
extern int system_call(void);

union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];
};

static union task_union init_task = {INIT_TASK,};

long volatile jiffies=0;
long startup_time=0;
struct task_struct *current = &(init_task.task), *last_task_used_math = NULL;

struct task_struct * task[NR_TASKS] = {&(init_task.task), };

long user_stack [ PAGE_SIZE>>2 ] ;

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
void math_state_restore()
{
	if (last_task_used_math)
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	if (current->used_math)
		__asm__("frstor %0"::"m" (current->tss.i387));
	else {
		__asm__("fninit"::);
		current->used_math=1;
	}
	last_task_used_math=current;
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 *
 * 该函数主要做的三件事：
 * 1. int next = get_max_counter_from_runnanle();	获取剩余时间片(counter值)最大且在runnable状态(state=0)的进程号next；
 * 2. refesh_all_thread_counter();					如果所有的时间片都为0，则将所有的进程(不止是runnable状态的进程)的counter重新赋值，然后再执行步骤1；
 * 3. switch_to(next);								获得一个进程号next，调用swich_to，切换到还进程中去执行。
 */
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */

	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			if ((*p)->alarm && (*p)->alarm < jiffies) {
					(*p)->signal |= (1<<(SIGALRM-1));
					(*p)->alarm = 0;
				}
			if ((*p)->signal && (*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		while (--i) {
			if (!*--p)
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
		if (c) break;
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;					
		//根据时间片=时间片/2+优先级 来进行排序，最大的先执行 称为[优先级时间片轮转算法]
	}
	switch_to(next);
}

int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	if (tmp)
		tmp->state=0;
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	*p=NULL;
	if (tmp)
		tmp->state=0;
}

void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;
		*p=NULL;
	}
}

void do_timer(long cpl)
{
	// CPL(Current Privilege Level)，表示正在执行代码所在段的特权级别，存在于cs段的低两位
	if (cpl)				//cpl用来表示被中断程序的特权。0表示内核进程；1表示用户进程
		current->utime++;	//utime表示用户程序时间
	else
		current->stime++;	//stime表示内核程序时间
	/* 1.先将当前进程的时间片-1，如果剩余时间片仍>0，直接返回 */
	if ((--current->counter)>0) return;
	current->counter=0;		//如果-1之后时间片<0，则将时间片赋值为0
	if (!cpl) return;		//如果是内核进程，则直接返回
	/* 2.若没有剩余时间片，则进行调度 */
	schedule();
}

int sys_alarm(long seconds)
{
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return seconds;
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

int sys_signal(long signal,long addr,long restorer)
{
	long i;

	switch (signal) {
		case SIGHUP: case SIGINT: case SIGQUIT: case SIGILL:
		case SIGTRAP: case SIGABRT: case SIGFPE: case SIGUSR1:
		case SIGSEGV: case SIGUSR2: case SIGPIPE: case SIGALRM:
		case SIGCHLD:
			i=(long) current->sig_fn[signal-1];
			current->sig_fn[signal-1] = (fn_ptr) addr;
			current->sig_restorer = (fn_ptr) restorer;
			return i;
		default: return -1;
	}
}

void sched_init(void)
{
	int i;
	struct desc_struct * p;

	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));	//初始化第一个进程的tss
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) {									//将进程数组清零
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
	ltr(0);
	lldt(0);
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	set_intr_gate(0x20,&timer_interrupt);	//0x20为时钟中断，当该中断来临时，CPU会查找中断向量表中0x20处的函数地址，即_timer_interrupt
	outb(inb_p(0x21)&~0x01,0x21);
	set_system_gate(0x80,&system_call);
}
