#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/random.h>
#include <linux/cred.h>
#include <linux/sus_su.h>

#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
extern bool susfs_is_log_enabled __read_mostly;
#define SUSFS_LOGI(fmt, ...) if (susfs_is_log_enabled) pr_info("susfs_sus_su:[%u][%u][%s] " fmt, current_uid().val, current->pid, __func__, ##__VA_ARGS__)
#define SUSFS_LOGE(fmt, ...) if (susfs_is_log_enabled) pr_err("susfs_sus_su:[%u][%u][%s]" fmt, current_uid().val, current->pid, __func__, ##__VA_ARGS__)
#else
#define SUSFS_LOGI(fmt, ...)
#define SUSFS_LOGE(fmt, ...)
#endif

#define FIFO_SIZE 1024
#define MAX_DRV_NAME 255

static int cur_maj_dev_num = -1;
static char fifo_buffer[FIFO_SIZE];
static struct cdev sus_su_cdev;
static const char *sus_su_token = "!@#$SU_IS_SUS$#@!-pRE6W9BKXrJr1hEKyvDq0CvWziVKbatT8yzq06fhtrEGky2tVS7Q2QTjhtMfVMGV";
static char rand_drv_path[MAX_DRV_NAME+1] = "/dev/";
static bool is_sus_su_enabled_before = false;

extern bool susfs_is_allow_su(void);
extern void ksu_escape_to_root(void);

static void gen_rand_drv_name(char *buffer, size_t min_length, size_t max_length) {
    const char *symbols = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-+@#:=";
    size_t symbols_length = strlen(symbols);
    size_t length, i;
    unsigned int rand_value;

    // Determine the random length of the string
    get_random_bytes(&rand_value, sizeof(rand_value));
    length = min_length + (rand_value % (max_length - min_length + 1));

    for (i = 0; i < length; ++i) {
        get_random_bytes(&rand_value, sizeof(rand_value));
        buffer[i] = symbols[rand_value % symbols_length];
    }
    buffer[length] = '\0'; // Null-terminate the string
}

static int fifo_open(struct inode *inode, struct file *file) {
    return 0;
}

static int fifo_release(struct inode *inode, struct file *file) {
    return 0;
}

static ssize_t fifo_read(struct file *file, char __user *buf, size_t len, loff_t *offset) {
    return 0;
}

static ssize_t fifo_write(struct file *file, const char __user *buf, size_t len, loff_t *offset) {
    int sus_su_token_len = strlen(sus_su_token);

    if (!susfs_is_allow_su()) {
        SUSFS_LOGE("root is not allowed for uid: '%d', pid: '%d'\n", current_uid().val, current->pid);
        return 0;
    }

    if (copy_from_user(fifo_buffer, buf, sus_su_token_len+1)) {
        SUSFS_LOGE("copy_from_user() failed, uid: '%d', pid: '%d'\n", current_uid().val, current->pid);
        return 0;
    }

    if (!memcmp(fifo_buffer, sus_su_token, sus_su_token_len+1)) {
        SUSFS_LOGI("granting root access for uid: '%d', pid: '%d'\n", current_uid().val, current->pid);
        ksu_escape_to_root();
    } else {
        SUSFS_LOGI("wrong token! deny root access for uid: '%d', pid: '%d'\n", current_uid().val, current->pid);
    }
    memset(fifo_buffer, 0, FIFO_SIZE);
    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = fifo_open,
    .release = fifo_release,
    .read = fifo_read,
    .write = fifo_write,
};

int sus_su_fifo_init(int *maj_dev_num, char *drv_path) {
    if (cur_maj_dev_num > 0) {
        SUSFS_LOGE("'%s' is already registered\n", rand_drv_path);
        return -1;
    }

    // generate a random driver name if it is executed for the first time
    if (!is_sus_su_enabled_before) {
        // min length 192, max length 248, just make sure max length doesn't exceeds 255
        gen_rand_drv_name(rand_drv_path+5, 192, 248);
    }

    cur_maj_dev_num = register_chrdev(0, rand_drv_path+5, &fops);
    if (cur_maj_dev_num < 0) {
        SUSFS_LOGE("Failed to register character device\n");
        return -1;
    }

    cdev_init(&sus_su_cdev, &fops);
    if (cdev_add(&sus_su_cdev, MKDEV(cur_maj_dev_num, 0), 1) < 0) {
        unregister_chrdev(cur_maj_dev_num, rand_drv_path+5);
        SUSFS_LOGE("Failed to add cdev\n");
        return -1;
    }

    strncpy(drv_path, rand_drv_path, strlen(rand_drv_path));
    *maj_dev_num = cur_maj_dev_num;
    SUSFS_LOGI("'%s' registered with major device number %d\n", rand_drv_path, cur_maj_dev_num);
    
    if (!is_sus_su_enabled_before)
        is_sus_su_enabled_before = true;

    return 0;
}

int sus_su_fifo_exit(int *maj_dev_num, char *drv_path) {
    if (cur_maj_dev_num < 0) {
        SUSFS_LOGE("'%s' was already unregistered before\n", rand_drv_path);
        return 0;
    }

    cdev_del(&sus_su_cdev);
    unregister_chrdev(cur_maj_dev_num, rand_drv_path+5);
    cur_maj_dev_num = -1;
    *maj_dev_num = cur_maj_dev_num;
    strncpy(drv_path, rand_drv_path, strlen(rand_drv_path));
    SUSFS_LOGI("'%s' unregistered\n", rand_drv_path);
    return 0;
}
