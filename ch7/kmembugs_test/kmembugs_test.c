/*
 * ch7/kmembugs_test/kmembugs_test.c
 ***************************************************************
 * This program is part of the source code released for the book
 *  "Linux Kernel Debugging"
 *  (c) Author: Kaiwan N Billimoria
 *  Publisher:  Packt
 *  GitHub repository:
 *  https://github.com/PacktPublishing/Linux-Kernel-Debugging
 *
 * From: Ch 7: Debugging kernel memory issues
 ****************************************************************
 * Brief Description:
 * This kernel module has buggy functions, each of which represents a simple
 * test case. They're deliberately selected to be the ones that are typically
 * NOT caught by KASAN!
 *
 * IMP:
 * By default, KASAN will turn off reporting after the very first error
 * encountered; we can change this behavior (and therefore test more easily)
 * by passing the kernel parameter kasan_multi_shot. Even easier, we can simply
 * first invoke the function kasan_save_enable_multi_shot() - which has the
 * same effect - and on unload restore it by invoking the
 * kasan_restore_multi_shot()! (note they require GPL licensing!).
 *
 * For details, please refer the book, Ch 7.
 */
#define pr_fmt(fmt) "%s:%s(): " fmt, KBUILD_MODNAME, __func__
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/debugfs.h>

MODULE_AUTHOR("Kaiwan N Billimoria");
MODULE_DESCRIPTION("kmembugs_test: a few additional test cases for KASAN/UBSAN");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION("0.1");

static bool use_kasan_multishot;
module_param(use_kasan_multishot, bool, 0);
MODULE_PARM_DESC(use_kasan_multishot, "Set to 1 to run test cases for KASAN (default=0)");

#ifdef CONFIG_KASAN
static bool kasan_multishot;
#endif

int debugfs_simple_intf_init(void);
extern struct dentry *gparent;

/* The UMR - Uninitialized Memory Read - testcase */
int umr(void)
{
	volatile int x, y;
	/* Recent gcc does have the ability to detect this!
	 * To get warnings on this, you require to:
	 * a) use some optimization level besides 0 (-On, n != 0)
	 * b) pass the -Wuninitialized or -Wall compiler option.
	 * The gcc warning, when generated, typically shows up as:
	 *  warning: 'x' is used uninitialized in this function [-Wuninitialized]
	 *
	 * It carries a number of caveats though; pl see:
	 * https://gcc.gnu.org/wiki/Better_Uninitialized_Warnings
	 * Also see gcc(1) for the -Wmaybe-uninitialized option.
	 */
	pr_info("testcase 1: UMR (val=%d)\n", x);
	y = x;

	return x;
}

/* The UAR - Use After Return - testcase */
static void *uar(void)
{
#define NUM_ALLOC  64
	volatile char name[NUM_ALLOC];
	volatile int i;

	pr_info("testcase 2: UAR:\n");
	for (i = 0; i < NUM_ALLOC - 1; i++)
		name[i] = 'x';
	name[i] = '\0';

	return (void *)name;
	/*
	 * Here too, at the point of return, gcc emits a warning!
	 *  warning: function returns address of local variable [-Wreturn-local-addr]
	 * Good stuff.
	 */
}

/* A simple memory leakage testcase 1 */
static void leak_simple1(void)
{
	char *p = NULL;

	p = kzalloc(1520, GFP_KERNEL);
	if (unlikely(!p))
		return;

	if (0)			// test: ensure it isn't freed
		kfree(p);
}

/* A simple memory leakage testcase 2.
 * The caller's to free the memory...
 */
static void *leak_simple2(void)
{
	volatile char *q = NULL;
	volatile int i;
	volatile char heehee[] = "leaky!!";

#define NUM_ALLOC2	8
	q = kmalloc(NUM_ALLOC2, GFP_KERNEL);
	for (i = 0; i < NUM_ALLOC2 - 1; i++)
		q[i] = heehee[i];	// 'x';
	q[i] = '\0';

	return (void *)q;
}

#if 0
#define NUM_ALLOC3	42
static int oob_array_dynmem(void)
{
	volatile char *arr, x, y;

	arr = kmalloc(NUM_ALLOC3, GFP_KERNEL);
	if (!arr)
		return -ENOMEM;

	pr_info("Allocated %d bytes via kmalloc(), *actual amt* alloc'ed is %ld bytes\n",
		NUM_ALLOC3, ksize((char *)arr));
	x = arr[40];		// valid and within bounds
	y = arr[50];		// valid but NOT within bounds
	/* hey, there's also a lurking UMR defect here! arr[] has random content;
	 * Neither KASAN/UBSAN nor the compiler catches it...
	 */
	pr_info("x=0x%x y=0x%x\n", x, y);

	kfree((char *)arr);
	return 0;
}
#endif

#define READ	0
#define WRITE	1

static char global_arr[8];

/*
 * OOB on static (compile-time) mem: OOB read/write (right) overflow
 * Covers both read/write overflow on both static global and local/stack memory
 */
static int static_mem_oob_right(int mode)
{
	volatile char w, x, y, z;
	volatile char local_arr[20];

	if (mode == READ) {
		w = global_arr[ARRAY_SIZE(global_arr) - 2];	// valid and within bounds
		x = global_arr[ARRAY_SIZE(global_arr) + 2];	// invalid, not within bounds

		y = local_arr[ARRAY_SIZE(local_arr) - 5];	// valid and within bounds but random content!
		z = local_arr[ARRAY_SIZE(local_arr) + 5];	// invalid, not within bounds
		/* hey, there's also a lurking UMR defect here! local_arr[] has random content;
		 * KASAN/UBSAN don't seem to catch it; the compiler does! via a warning:
		 *  [...]warning: 'arr[20]' is used uninitialized in this function [-Wuninitialized]
		 *  142 |  x = arr[20]; // valid and within bounds
		 *      |      ~~~^~~~
		    ^^^^^^^^^^^^^^^^ ?? NOT getting gcc warning now!!
		 */
		//pr_info("global mem: w=0x%x x=0x%x; local mem: y=0x%x z=0x%x\n", w, x, y, z);
	}
	else if (mode == WRITE) {
		global_arr[ARRAY_SIZE(global_arr) - 2] = 'w';	// valid and within bounds
		global_arr[ARRAY_SIZE(global_arr) + 2] = 'x';	// invalid, not within bounds

		local_arr[ARRAY_SIZE(local_arr) - 5] = 'y';	// valid and within bounds but random content!
		local_arr[ARRAY_SIZE(local_arr) + 5] = 'z';	// invalid, not within bounds

	}
	return 0;
}

/*
 * OOB on static (compile-time) mem: OOB read/write (left) underflow
 * Covers both read/write overflow on both static global and local/stack memory
 */
static int static_mem_oob_left(int mode)
{
	volatile char w, x, y, z;
	volatile char local_arr[20];

	if (mode == READ) {
		w = global_arr[-2];	// invalid, not within bounds
		x = global_arr[2];	// valid, within bounds

		y = local_arr[-5];	// invalid, not within bounds and random!
		z = local_arr[5];	// valid, within bounds but random
		/* hey, there's also a lurking UMR defect here! local_arr[] has random content;
		 * KASAN/UBSAN don't seem to catch it; the compiler does! via a warning:
		 *  [...]warning: 'arr[20]' is used uninitialized in this function [-Wuninitialized]
		 *  142 |  x = arr[20]; // valid and within bounds
		 *      |      ~~~^~~~
		 */
		pr_info("global mem: w=0x%x x=0x%x; local mem: y=0x%x z=0x%x\n", w, x, y, z);
	}

	return 0;
}

// dynamic mem: OOB read/write overflow
static int dynamic_mem_oob_right(int mode)
{
	volatile char *kptr, ch = 0;
	size_t sz = 123;

	kptr = (char *)kmalloc(sz, GFP_KERNEL);
	if (!kptr)
		return -ENOMEM;

	if (mode == READ)
		ch = kptr[sz];
	else if (mode == WRITE)
		kptr[sz] = 'x';

	kfree((char *)kptr);
	return 0;
}

static int __init kmembugs_test_init(void)
{
	int i, stat, numtimes = 1;	// would you like to try a number of times? :)

	pr_info("Testing for ");
	if (use_kasan_multishot) {
		pr_info("KASAN\n");
		/*
		 * Realize that the kasan_save_enable_multi_shot() / kasan_restore_multi_shot()
		 * pair of functions work only on a kernel that has CONFIG_KASAN=y. Also,
		 * we're expecting Generic KASAN enabled.
		 */
#ifdef CONFIG_KASAN_GENERIC
		kasan_multishot = kasan_save_enable_multi_shot();
#else
		pr_warn("Attempting to test for KASAN on a non-KASAN-enabled kernel!\n");
		return -EINVAL;
#endif
	}
	if (IS_ENABLED(CONFIG_UBSAN))
		pr_info("UBSAN\n");

	stat = debugfs_simple_intf_init();
	if (stat < 0)
		return stat;





	return 0;		/* success */


	for (i = 0; i < numtimes; i++) {
		int umr_ret;
		char *res1 = NULL, *res2 = NULL;

		// 1. Run the UMR - Uninitialized Memory Read - testcase
		umr_ret = umr();
		//pr_info("testcase 1: UMR (val=%d)\n", umr_ret);

		// 2. Run the UAR - Use After Return - testcase
		res1 = uar();
		pr_info("testcase 2: UAR: res1 = \"%s\"\n",
			res1 == NULL ? "<whoops, it's NULL; UAR!>" : (char *)res1);

		// 3. Run the UAF - Use After Free - testcase

		//---------- 4. OOB accesses on static memory (read/write under/overflow)
		pr_info
		    ("testcases set 4: simple OOB accesses on static memory (read/write under/overflow)\n");
		pr_info(" 4.1: static (compile-time) mem: OOB read (right) overflow\n");
//		static_mem_oob_right(READ);
		pr_info(" 4.2: static (compile-time) mem: OOB write (right) overflow\n");
		static_mem_oob_right(WRITE);
		pr_info(" 4.3: static (compile-time) mem: OOB read (left) underflow\n");
//		static_mem_oob_left(READ);
		pr_info(" 4.4: static (compile-time) mem: OOB write (left) underflow\n");
		static_mem_oob_left(WRITE);

		/*
		   oob_array_dynmem();

		   // 5. OOB static array access
		   pr_info("testcase 6: simple OOB memory access on static (compile-time) memory array\n");
		   oob_array_staticmem();
		 */

		// 6.1. memory leak 1
		pr_info("testcase 6.1: simple memory leak testcase 1\n");
		leak_simple1();

		// 6.2. memory leak 2: caller's to free the memory!
		pr_info("testcase 6.2: simple memory leak testcase 2\n");
		res2 = (char *)leak_simple2();
		pr_info(" res2 = \"%s\"\n",
			res2 == NULL ? "<whoops, it's NULL; UAR!>" : (char *)res2);
		if (0)		// test: ensure it isn't freed by the caller
			kfree(res2);
	}

	return 0;		/* success */
}

static void __exit kmembugs_test_exit(void)
{
#ifdef CONFIG_KASAN_GENERIC
	if (use_kasan_multishot)
		kasan_restore_multi_shot(kasan_multishot);
#endif
	debugfs_remove_recursive(gparent);
	pr_info("removed\n");
}

module_init(kmembugs_test_init);
module_exit(kmembugs_test_exit);
