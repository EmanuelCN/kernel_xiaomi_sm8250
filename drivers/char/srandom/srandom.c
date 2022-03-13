#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/slab.h>             /* For kmalloc */
#include <linux/gfp.h>
#include <linux/vmalloc.h>          /* For vmalloc */
#include <linux/uaccess.h>          /* For copy_to_user */
#include <linux/miscdevice.h>       /* For misc_register (the /dev/srandom) device */
#include <linux/time.h>             /* For getnstimeofday/ktime_get_real_ts64 */
#include <linux/proc_fs.h>          /* For /proc filesystem */
#include <linux/seq_file.h>         /* For seq_print */
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/kthread.h>

#define DRIVER_AUTHOR "Jonathan Senkerik <josenk@jintegrate.co>"
#define DRIVER_DESC   "Improved random number generator."
#define rndArraySize 67             /* Size of Array.  Must be >= 64. (actual size used will be 64, anything greater is thrown away). Recommended prime.*/
#define numberOfRndArrays  16       /* Number of 512b Array (Must be power of 2) */
#define SDEVICE_NAME "srandom"      /* Dev name as it appears in /proc/devices */
#define APP_VERSION "1.40.0"
#define THREAD_SLEEP_VALUE 11       /* Amount of time the background thread should sleep between each operation. Recommended prime */
#define PAID 0

//#define DEBUG_CONNECTIONS 0
//#define DEBUG_READ 0
//#define DEBUG_WRITE 0
//#define DEBUG_PRNG_SEED 0
//#define DEBUG_NEXT_BUFFER 0

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
    #define COPY_TO_USER raw_copy_to_user
    #define COPY_FROM_USER raw_copy_from_user
#else
    #define COPY_TO_USER copy_to_user
    #define COPY_FROM_USER copy_from_user
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,8,0)
    #define KTIME_GET_NS ktime_get_real_ts64
    #define TIMESPEC timespec64
#else
    #define KTIME_GET_NS getnstimeofday
    #define TIMESPEC timespec
#endif


/*
 * Copyright (C) 2015 Jonathan Senkerik
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Prototypes
 */
static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t sdevice_read(struct file *, char *, size_t, loff_t *);
static ssize_t sdevice_write(struct file *, const char *, size_t, loff_t *);
static uint64_t xorshft64(void);
static uint64_t xorshft128(void);
static int nextbuffer(void);
static void update_sarray(int);
static void seed_PRND_s0(void);
static void seed_PRND_s1(void);
static void seed_PRND_x(void);
static int proc_read(struct seq_file *m, void *v);
static int proc_open(struct inode *inode, struct  file *file);
static int work_thread(void *data);


/*
 * Global variables are declared as static, so are global within the file.
 */
static struct file_operations sfops = {
        .owner   = THIS_MODULE,
        .open    = device_open,
        .read    = sdevice_read,
        .write   = sdevice_write,
        .release = device_release
};

static struct miscdevice srandom_dev = {
        MISC_DYNAMIC_MINOR,
        "srandom",
        &sfops
};


#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,8,0)
static struct proc_ops proc_fops={
      .proc_open = proc_open,
      .proc_release = single_release,
      .proc_read = seq_read,
      .proc_lseek = seq_lseek
};
#else
static const struct file_operations proc_fops = {
        .owner   = THIS_MODULE,
        .read    = seq_read,
        .open    = proc_open,
        .llseek  = seq_lseek,
        .release = single_release,
};
#endif


static struct mutex UpArr_mutex;
static struct mutex Open_mutex;
static struct mutex ArrBusy_mutex;
static struct mutex UpPos_mutex;

static struct task_struct *kthread;


/*
 * Global variables
 */
uint64_t x;                             /* Used for xorshft64 */
uint64_t s[ 2 ];                        /* Used for xorshft128 */
uint64_t (*prngArrays)[numberOfRndArrays + 1];  /* Array of Array of SECURE RND numbers */
uint16_t ArraysBusyFlags = 0;             /* Binary Flags for Busy Arrays */
int      arraysBufferPosition = 0;        /* Array reserved to determine which buffer to use */
uint64_t tm_seed;
struct   TIMESPEC ts;

/*
 * Global counters
 */
int16_t  sdevOpenCurrent;          /* srandom device current open count */
int32_t  sdevOpenTotal;            /* srandom device total open count */
uint64_t generatedCount;           /* Total generated (512byte) */


/*
 * This function is called when the module is loaded
 */
int mod_init(void)
{
        int16_t C,arraysPosition;

        sdevOpenCurrent = 0;
        sdevOpenTotal   = 0;
        generatedCount  = 0;

        mutex_init(&UpArr_mutex);
        mutex_init(&Open_mutex);
        mutex_init(&ArrBusy_mutex);
        mutex_init(&UpPos_mutex);

        /*
         * Entropy Initialize #1
         */
        KTIME_GET_NS(&ts);
        x    = (uint64_t)ts.tv_nsec;
        s[0] = xorshft64();
        s[1] = xorshft64();

        /*
         * Register char device
         */
        if (misc_register(&srandom_dev))
                printk(KERN_INFO "[srandom] mod_init /dev/srandom driver registion failed..\n");
        else
                printk(KERN_INFO "[srandom] mod_init /dev/srandom driver registered..\n");

        /*
         * Create /proc/srandom
         */
        // if (! proc_create("srandom", 0, NULL, &proc_fops))
        if (! proc_create("srandom", 0, NULL, &proc_fops))
                printk(KERN_INFO "[srandom] mod_init /proc/srandom registion failed..\n");
        else
                printk(KERN_INFO "[srandom] mod_init /proc/srandom registion regisered..\n");

        printk(KERN_INFO "[srandom] mod_init Module version         : "APP_VERSION"\n");
        if (PAID == 0) {
                printk(KERN_INFO "-----------------------:----------------------\n");
                printk(KERN_INFO "Please support my work and efforts contributing\n");
                printk(KERN_INFO "to the Linux community.  A $25 payment per\n");
                printk(KERN_INFO "server would be highly appreciated.\n");
        }
        printk(KERN_INFO "-----------------------:----------------------\n");
        printk(KERN_INFO "Author                 : Jonathan Senkerik\n");
        printk(KERN_INFO "Website                : http://www.jintegrate.co\n");
        printk(KERN_INFO "github                 : http://github.com/josenk/srandom\n");
        if (PAID == 0) {
                printk(KERN_INFO "Paypal                 : josenk@jintegrate.co\n");
                printk(KERN_INFO "Bitcoin                : 1MTNg7SqcEWs5uwLKwNiAfYqBfnKFJu65p\n");
                printk(KERN_INFO "Commercial Invoice     : Avail on request.\n");
        }


        prngArrays = kmalloc((numberOfRndArrays + 1) * rndArraySize * sizeof(uint64_t), GFP_KERNEL);
        while (!prngArrays) {
                printk(KERN_INFO "[srandom] mod_init kmalloc failed to allocate initial memory.  retrying...\n");
                prngArrays = kmalloc((numberOfRndArrays + 1) * rndArraySize * sizeof(uint64_t), GFP_KERNEL);
        }

        /*
         * Entropy Initialize #2
         */
        seed_PRND_s0();
        seed_PRND_s1();
        seed_PRND_x();

        /*
         * Init the sarray
         */
        for (arraysPosition = 0;arraysPosition <= numberOfRndArrays ;arraysPosition++) {
                for (C = 0;C <= rndArraySize;C++) {
                        prngArrays[arraysPosition][C] = xorshft128();
                }
                update_sarray(arraysPosition);
        }

        kthread = kthread_create(work_thread, NULL, "mykthread");
        wake_up_process(kthread);

        return 0;
}

/*
 * This function is called when the module is unloaded
 */
void mod_exit(void)
{
        kthread_stop(kthread);

        misc_deregister(&srandom_dev);

        remove_proc_entry("srandom", NULL);

        printk(KERN_INFO "[srandom] mod_exit srandom deregisered..\n");
}


/*
 * This function is called when a process tries to open the device file. "dd if=/dev/srandom"
 */
static int device_open(struct inode *inode, struct file *file)
{
        while (mutex_lock_interruptible(&Open_mutex));

        sdevOpenCurrent++;
        sdevOpenTotal++;
        mutex_unlock(&Open_mutex);

        #ifdef DEBUG_CONNECTIONS
        printk(KERN_INFO "[srandom] device_open (current open) :%d\n",sdevOpenCurrent);
        printk(KERN_INFO "[srandom] device_open (total open)   :%d\n",sdevOpenTotal);
        #endif

        return 0;
}


/*
 * Called when a process closes the device file.
 */
static int device_release(struct inode *inode, struct file *file)
{
        while (mutex_lock_interruptible(&Open_mutex));

        sdevOpenCurrent--;
        mutex_unlock(&Open_mutex);

        #ifdef DEBUG_CONNECTIONS
        printk(KERN_INFO "[srandom] device_release (current open) :%d\n", sdevOpenCurrent);
        #endif

        return 0;
}

/*
 * Called when a process reads from the device.
 */
static ssize_t sdevice_read(struct file * file, char * buf, size_t requestedCount, loff_t *ppos)
{
        int arraysPosition;
        int Block, ret;
        char *new_buf;                 /* Buffer to hold numbers to send */
        bool isVMalloc = 0;


        #ifdef DEBUG_READ
        printk(KERN_INFO "[srandom] sdevice_read requestedCount:%zu\n", requestedCount);
        #endif


        new_buf = kmalloc((requestedCount + 512) * sizeof(uint8_t), GFP_KERNEL|__GFP_NOWARN);
        while (!new_buf) {
                #ifdef DEBUG_READ
                printk(KERN_INFO "[srandom] using vmalloc to allocate large blocksize.\n");
                #endif

                isVMalloc = 1;
                new_buf = vmalloc((requestedCount + 512) * sizeof(uint8_t));
        }


        /*
         * Select a RND array
         */
        while (mutex_lock_interruptible(&ArrBusy_mutex));

        arraysPosition = nextbuffer();

        while ((ArraysBusyFlags & 1 << arraysPosition) == (1 << arraysPosition)) {
                arraysPosition += 1;
                if (arraysPosition >= numberOfRndArrays) {
                        arraysPosition = 0;
                }
        }
    
        /*
         * Mark the Arry as busy by setting the flag
         */
        ArraysBusyFlags += (1 << arraysPosition);
        mutex_unlock(&ArrBusy_mutex);


        /*
         * Send the Array of RND to USER
         */

        for (Block = 0; Block <= (requestedCount / 512); Block++) {
                #ifdef DEBUG_READ
                printk(KERN_INFO "[srandom] Block:%u\n", Block);
                #endif

                memcpy(new_buf + (Block * 512), prngArrays[arraysPosition], 512);
                update_sarray(arraysPosition);

        }

        /*
         * Send new_buf to device
         */
        ret = COPY_TO_USER(buf, new_buf, requestedCount);
        
        /*
         * Free allocated memory
         */
        if (isVMalloc) {
                vfree(new_buf);
        } else {
                kfree(new_buf);
        }
             

        /*
         * Clear ArraysBusyFlags
         */
        if (mutex_lock_interruptible(&ArrBusy_mutex))
                return -ERESTARTSYS;
        ArraysBusyFlags -= (1 << arraysPosition);
        mutex_unlock(&ArrBusy_mutex);

        

        /*
         * return how many chars we sent
         */

        return requestedCount;
}


/*
 * Called when someone tries to write to /dev/srandom device
 */
static ssize_t sdevice_write(struct file *file, const char __user *buf, size_t receivedCount, loff_t *ppos)
{

        char *newdata;
        int result;

        #ifdef DEBUG_CONNECTIONS
        printk(KERN_INFO "[srandom] sdevice_write receivedCount:%zu\n", receivedCount);
        #endif

        /*
         * Allocate memory to read from device
         */
        newdata = kmalloc(receivedCount, GFP_KERNEL);
        while (!newdata) {
                newdata = kmalloc(receivedCount, GFP_KERNEL);
        }

        result = COPY_FROM_USER(newdata, buf, receivedCount);

        /*
         * Free memory
         */
        kfree(newdata);

        #ifdef DEBUG_WRITE
        printk(KERN_INFO "[srandom] sdevice_write COPT_FROM_USER receivedCount:%zu \n", receivedCount);
        #endif

        return receivedCount;
}



/*
 * Update the sarray with new random numbers
 */
void update_sarray(int arraysPosition)
{
        int16_t C;
        int64_t X, Y, Z1, Z2, Z3;

        /*
         * This function must run exclusivly
         */
        while (mutex_lock_interruptible(&UpArr_mutex));

        generatedCount++;

        Z1 = xorshft64();
        Z2 = xorshft64();
        Z3 = xorshft64();
        if ((Z1 & 1) == 0) {
                #ifdef DEBUG_UPDATE_ARRAYS
                printk(KERN_INFO "[srandom] update_sarray 0\n");
                #endif

                for (C = 0;C < (rndArraySize -4) ;C = C + 4) {
                        X=xorshft128();
                        Y=xorshft128();
                        prngArrays[arraysPosition][C]     = prngArrays[arraysPosition][C + 1] ^ X ^ Y;
                        prngArrays[arraysPosition][C + 1] = prngArrays[arraysPosition][C + 2] ^ Y ^ Z1;
                        prngArrays[arraysPosition][C + 2] = prngArrays[arraysPosition][C + 3] ^ X ^ Z2;
                        prngArrays[arraysPosition][C + 3] = X ^ Y ^ Z3;
                }
        } else {
                #ifdef DEBUG_UPDATE_ARRAYS
                printk(KERN_INFO "[srandom] update_sarray 1\n");
                #endif

                for (C = 0;C < (rndArraySize -4) ;C = C + 4) {
                        X=xorshft128();
                        Y=xorshft128();
                        prngArrays[arraysPosition][C]     = prngArrays[arraysPosition][C + 1] ^ X ^ Z2;
                        prngArrays[arraysPosition][C + 1] = prngArrays[arraysPosition][C + 2] ^ X ^ Y;
                        prngArrays[arraysPosition][C + 2] = prngArrays[arraysPosition][C + 3] ^ Y ^ Z3;
                        prngArrays[arraysPosition][C + 3] = X ^ Y ^ Z1;
                }
        }

        mutex_unlock(&UpArr_mutex);

        #ifdef DEBUG_UPDATE_ARRAYS
        printk(KERN_INFO "[srandom] update_sarray arraysPosition:%d, X:%llu, Y:%llu, Z1:%llu, Z2:%llu, Z3:%llu,\n", arraysPosition, X, Y, Z1, Z2, Z3);
        #endif

}


/*
 *  Seeding the xorshft's
 */
 void seed_PRND_s0(void)
 {
         KTIME_GET_NS(&ts);
         s[0] = (s[0] << 31) ^ (uint64_t)ts.tv_nsec;
         #ifdef DEBUG_PRNG_SEED
         printk(KERN_INFO "[srandom] seed_PRNG_s0 x:%llu, s[0]:%llu, s[1]:%llu\n", x, s[0], s[1]);
         #endif
 }
void seed_PRND_s1(void)
{
        KTIME_GET_NS(&ts);
        s[1] = (s[1] << 24) ^ (uint64_t)ts.tv_nsec;
        #ifdef DEBUG_PRNG_SEED
        printk(KERN_INFO "[srandom] seed_PRNG_s1 x:%llu, s[0]:%llu, s[1]:%llu\n", x, s[0], s[1]);
        #endif
}
void seed_PRND_x(void)
{
        KTIME_GET_NS(&ts);
        x = (x << 32) ^ (uint64_t)ts.tv_nsec;
        #ifdef DEBUG_PRNG_SEED
        printk(KERN_INFO "[srandom] seed_PRNG_x x:%llu, s[0]:%llu, s[1]:%llu\n", x, s[0], s[1]);
        #endif
}



/*
 * PRNG functions
 */
uint64_t xorshft64(void)
{
        uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
}
uint64_t xorshft128(void)
{
        uint64_t s1 = s[0];
        const uint64_t s0 = s[1];
        s[0] = s0;
        s1 ^= s1 << 23;
        return (s[ 1 ] = (s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26))) + s0;
}

/*
 *  This function returns the next sarray to use/read.
 */
int nextbuffer(void)
{
        uint8_t position = (int)((arraysBufferPosition * 4) / 64 );
        uint8_t roll = arraysBufferPosition % 16;
        uint8_t nextbuffer = (prngArrays[numberOfRndArrays][position] >> (roll * 4)) & (numberOfRndArrays -1);

        #ifdef DEBUG_NEXT_BUFFER
        printk(KERN_INFO "[srandom] nextbuffer raw:%lld, position:%d, roll:%d, nextbuffer:%d,  arraysBufferPosition:%d\n", prngArrays[numberOfRndArrays][position], position, roll, nextbuffer, arraysBufferPosition);
        #endif

        while (mutex_lock_interruptible(&UpPos_mutex));
        arraysBufferPosition ++;
        mutex_unlock(&UpPos_mutex);

        if (arraysBufferPosition >= 1021) {
                while (mutex_lock_interruptible(&UpPos_mutex));
                arraysBufferPosition = 0;
                mutex_unlock(&UpPos_mutex);

                update_sarray(numberOfRndArrays);
        }

        return nextbuffer;
}

/*
 *  The Kernel thread doing background tasks.
 */
int work_thread(void *data)
{
        int iteration = 0;

        while (!kthread_should_stop()) {

                if (iteration <= numberOfRndArrays) {
                  update_sarray(iteration);
                }
                else if (iteration == numberOfRndArrays + 1) {
                  seed_PRND_s0();
                }
                else if (iteration == numberOfRndArrays + 2) {
                  seed_PRND_s1();
                }
                else if (iteration == numberOfRndArrays + 3) {
                  seed_PRND_x();
                }
                else {
                  iteration = -1;
                }

                iteration++;

                ssleep(THREAD_SLEEP_VALUE);
        }

        do_exit(0);
        return 0;
 }

/*
 * This function is called when reading /proc filesystem
 */
int proc_read(struct seq_file *m, void *v)
{
        seq_printf(m, "-----------------------:----------------------\n");
        seq_printf(m, "Device                 : /dev/"SDEVICE_NAME"\n");
        seq_printf(m, "Module version         : "APP_VERSION"\n");
        seq_printf(m, "Current open count     : %d\n",sdevOpenCurrent);
        seq_printf(m, "Total open count       : %d\n",sdevOpenTotal);
        seq_printf(m, "Total K bytes          : %llu\n",generatedCount / 2);
        if (PAID == 0) {
                seq_printf(m, "-----------------------:----------------------\n");
                seq_printf(m, "Please support my work and efforts contributing\n");
                seq_printf(m, "to the Linux community.  A $25 payment per\n");
                seq_printf(m, "server would be highly appreciated.\n");
        }
        seq_printf(m, "-----------------------:----------------------\n");
        seq_printf(m, "Author                 : Jonathan Senkerik\n");
        seq_printf(m, "Website                : http://www.jintegrate.co\n");
        seq_printf(m, "github                 : http://github.com/josenk/srandom\n");
        if (PAID == 0) {
                seq_printf(m, "Paypal                 : josenk@jintegrate.co\n");
                seq_printf(m, "Bitcoin                : 1GEtkAm97DphwJbJTPyywv6NbqJKLMtDzA\n");
                seq_printf(m, "Commercial Invoice     : Avail on request.\n");
        }
        return 0;
}


int proc_open(struct inode *inode, struct  file *file)
{
        return single_open(file, proc_read, NULL);
}


module_init(mod_init);
module_exit(mod_exit);


/*
    Stack Guard
*/
unsigned long __stack_chk_guard;
void __stack_chk_guard_setup(void)
{
        KTIME_GET_NS(&ts);
        __stack_chk_guard = (uint64_t)ts.tv_nsec;
}

void __stack_chk_fail(void)
{
        printk(KERN_INFO "[srandom] Stack Guard check Failed!\n");
}


/*
 *  Module license information
 */
MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
