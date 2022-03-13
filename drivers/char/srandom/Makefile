TARGET_MODULE:=srandom
obj-m += $(TARGET_MODULE).o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) clean

load:
	insmod ./$(TARGET_MODULE).ko

unload:
	rmmod $(TARGET_MODULE).ko

install:
	mkdir -p /lib/modules/$(shell uname -r)/kernel/drivers/$(TARGET_MODULE)
	install -m 644  ./$(TARGET_MODULE).ko /lib/modules/$(shell uname -r)/kernel/drivers/$(TARGET_MODULE)
	install -m 644  ./11-$(TARGET_MODULE).rules /etc/udev/rules.d/
	install -m 755  ./$(TARGET_MODULE) /usr/bin/$(TARGET_MODULE)
	install -m 644  ./$(TARGET_MODULE).conf /etc/modules-load.d/
	depmod
	udevadm trigger
	@echo "Install Success."

uninstall:
	rm -f /lib/modules/$(shell uname -r)/kernel/drivers/$(TARGET_MODULE)/$(TARGET_MODULE).ko
	rm -f /etc/udev/rules.d/11-$(TARGET_MODULE).rules
	rm -f /etc/modules-load.d/$(TARGET_MODULE).conf
	depmod
	rm -f /usr/bin/$(TARGET_MODULE)
	@test -c /dev/srandom|| echo "Reboot required to complete uninstall."
	@echo "Uninstalled."
