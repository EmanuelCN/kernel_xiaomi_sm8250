#ifndef __KSU_H_SUS_SU
#define __KSU_H_SUS_SU

#include "../../drivers/kernelsu/core_hook.h"

int sus_su_fifo_init(int *maj_dev_num, char *drv_path);
int sus_su_fifo_exit(int *maj_dev_num, char *drv_path);

#endif
