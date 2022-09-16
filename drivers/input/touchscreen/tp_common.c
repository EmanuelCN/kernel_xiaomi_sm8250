#include <linux/input/tp_common.h>

bool capacitive_keys_enabled;
struct kobject *touchpanel_kobj;

#define TS_ENABLE_FOPS(type)                                                   \
	int tp_common_set_##type##_ops(struct tp_common_ops *ops)              \
	{                                                                      \
		static struct kobj_attribute kattr =                           \
			__ATTR(type, (S_IWUSR | S_IRUGO), NULL, NULL);         \
		kattr.show = ops->show;                                        \
		kattr.store = ops->store;                                      \
		return sysfs_create_file(touchpanel_kobj, &kattr.attr);        \
	}

#define TS_ENABLE_NOTIFY(type)                                                 \
	void tp_common_notify_##type(void)                                     \
	{                                                                      \
		sysfs_notify(touchpanel_kobj, NULL, __stringify(type));        \
	}

TS_ENABLE_FOPS(capacitive_keys)
TS_ENABLE_FOPS(double_tap)
TS_ENABLE_FOPS(reversed_keys)
TS_ENABLE_FOPS(fod_status)
TS_ENABLE_FOPS(fp_state)
TS_ENABLE_NOTIFY(fp_state)

static int __init tp_common_init(void)
{
	touchpanel_kobj = kobject_create_and_add("touchpanel", NULL);
	if (!touchpanel_kobj)
		return -ENOMEM;

	return 0;
}

core_initcall(tp_common_init);
