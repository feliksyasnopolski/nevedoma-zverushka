// SPDX-License-Identifier: GPL-2.0
#include <linux/build_bug.h>
#include <linux/fs.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include "../../src/sensors/tb330fu_sensor_feed.h"

#define TB330FU_SCALE_MICRO 1

struct tb330fu_values {
	struct mutex lock;
	__s32 accel[3];
	__s32 gyro[3];
	__s32 light;
	__u64 steps;
};

static struct tb330fu_values values;

static int tb330fu_read_raw(struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan, int *val, int *val2, long mask)
{
	int ret = -EINVAL;
	mutex_lock(&values.lock);
	if (mask == IIO_CHAN_INFO_RAW || mask == IIO_CHAN_INFO_PROCESSED) {
		if (chan->type == IIO_ACCEL)
			*val = values.accel[chan->scan_index], ret = IIO_VAL_INT;
		else if (chan->type == IIO_ANGL_VEL)
			*val = values.gyro[chan->scan_index], ret = IIO_VAL_INT;
		else if (chan->type == IIO_LIGHT)
			*val = values.light, ret = IIO_VAL_INT;
		else if (chan->type == IIO_STEPS)
			*val = (int)min_t(__u64, values.steps, INT_MAX), ret = IIO_VAL_INT;
	} else if (mask == IIO_CHAN_INFO_SCALE) {
		if (chan->type == IIO_ACCEL || chan->type == IIO_ANGL_VEL) {
			*val = 0; *val2 = TB330FU_SCALE_MICRO;
			ret = IIO_VAL_INT_PLUS_MICRO;
		}
	}
	mutex_unlock(&values.lock);
	return ret;
}

static const struct iio_info tb330fu_info = { .read_raw = tb330fu_read_raw };

static const struct iio_mount_matrix tb330fu_accel_mount_matrix = {
	.rotation = { "-1", "0", "0", "0", "-1", "0", "0", "0", "1" },
};

static const struct iio_mount_matrix *
tb330fu_get_accel_mount_matrix(const struct iio_dev *indio_dev,
	const struct iio_chan_spec *chan)
{
	return &tb330fu_accel_mount_matrix;
}

static ssize_t tb330fu_show_mount_matrix(struct iio_dev *indio_dev,
	uintptr_t priv, const struct iio_chan_spec *chan, char *buf)
{
	const struct iio_mount_matrix *matrix =
		tb330fu_get_accel_mount_matrix(indio_dev, chan);
	return sysfs_emit(buf, "%s, %s, %s; %s, %s, %s; %s, %s, %s\n",
		matrix->rotation[0], matrix->rotation[1], matrix->rotation[2],
		matrix->rotation[3], matrix->rotation[4], matrix->rotation[5],
		matrix->rotation[6], matrix->rotation[7], matrix->rotation[8]);
}

static const struct iio_chan_spec_ext_info accel_ext_info[] = {
	{ .name = "mount_matrix", .shared = IIO_SHARED_BY_ALL,
	  .read = tb330fu_show_mount_matrix },
	{ }
};

#define TB330FU_VEC3_CHANNEL(_type, _mod, _index) { \
	.type = (_type), .modified = 1, .channel2 = IIO_MOD_##_mod, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
	.ext_info = accel_ext_info, \
	.scan_index = (_index), }

static const struct iio_chan_spec accel_channels[] = {
	TB330FU_VEC3_CHANNEL(IIO_ACCEL, X, 0),
	TB330FU_VEC3_CHANNEL(IIO_ACCEL, Y, 1),
	TB330FU_VEC3_CHANNEL(IIO_ACCEL, Z, 2),
};
static const struct iio_chan_spec gyro_channels[] = {
	TB330FU_VEC3_CHANNEL(IIO_ANGL_VEL, X, 0),
	TB330FU_VEC3_CHANNEL(IIO_ANGL_VEL, Y, 1),
	TB330FU_VEC3_CHANNEL(IIO_ANGL_VEL, Z, 2),
};
static const struct iio_chan_spec light_channels[] = {{
	.type = IIO_LIGHT, .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	.scan_index = 0,
}};
static const struct iio_chan_spec steps_channels[] = {{
	.type = IIO_STEPS, .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	.scan_index = 0,
}};

static struct iio_dev *devices[4];
static const char * const names[4] = {
	"tb330fu-accelerometer", "tb330fu-gyroscope", "tb330fu-light",
	"tb330fu-pedometer",
};

static ssize_t tb330fu_feed_write(struct file *file, const char __user *buf,
	size_t count, loff_t *ppos)
{
	struct tb330fu_sensor_sample sample;
	if (count != sizeof(sample)) return -EINVAL;
	if (copy_from_user(&sample, buf, sizeof(sample))) return -EFAULT;
	if (sample.version != TB330FU_SENSOR_ABI_VERSION || !sample.timestamp_ns)
		return -EPROTO;
	if (sample.kind < TB330FU_SENSOR_ACCEL || sample.kind > TB330FU_SENSOR_STEPS)
		return -EINVAL;
	mutex_lock(&values.lock);
	switch (sample.kind) {
	case TB330FU_SENSOR_ACCEL:
		values.accel[0] = sample.vec3.x; values.accel[1] = sample.vec3.y;
		values.accel[2] = sample.vec3.z; break;
	case TB330FU_SENSOR_GYRO:
		values.gyro[0] = sample.vec3.x; values.gyro[1] = sample.vec3.y;
		values.gyro[2] = sample.vec3.z; break;
	case TB330FU_SENSOR_LIGHT: values.light = sample.light; break;
	case TB330FU_SENSOR_STEPS: values.steps = sample.steps; break;
	}
	mutex_unlock(&values.lock);
	return sizeof(sample);
}

static const struct file_operations feed_fops = {
	.owner = THIS_MODULE, .write = tb330fu_feed_write, .llseek = no_llseek,
};
static struct miscdevice feed_device = {
	.minor = MISC_DYNAMIC_MINOR, .name = "tb330fu-sensor-feed",
	.fops = &feed_fops, .mode = 0600,
};

static int tb330fu_make_iio(unsigned int index, const struct iio_chan_spec *channels,
	unsigned int num_channels)
{
	struct iio_dev *dev = iio_device_alloc(NULL, 0);
	if (!dev) return -ENOMEM;
	dev->name = names[index]; dev->info = &tb330fu_info;
	dev->channels = channels; dev->num_channels = num_channels;
	dev->modes = INDIO_DIRECT_MODE;
	if (iio_device_register(dev)) { iio_device_free(dev); return -EINVAL; }
	devices[index] = dev;
	return 0;
}

static int __init tb330fu_init(void)
{
	BUILD_BUG_ON(sizeof(struct tb330fu_sensor_sample) != 32);
	mutex_init(&values.lock);
	if (misc_register(&feed_device)) return -EINVAL;
	if (tb330fu_make_iio(0, accel_channels, ARRAY_SIZE(accel_channels)) ||
	    tb330fu_make_iio(1, gyro_channels, ARRAY_SIZE(gyro_channels)) ||
	    tb330fu_make_iio(2, light_channels, ARRAY_SIZE(light_channels)) ||
	    tb330fu_make_iio(3, steps_channels, ARRAY_SIZE(steps_channels))) {
		int i; for (i = 0; i < ARRAY_SIZE(devices); ++i)
			if (devices[i]) { iio_device_unregister(devices[i]); iio_device_free(devices[i]); }
		misc_deregister(&feed_device); return -EINVAL;
	}
	return 0;
}

static void __exit tb330fu_exit(void)
{
	int i; for (i = 0; i < ARRAY_SIZE(devices); ++i)
		if (devices[i]) { iio_device_unregister(devices[i]); iio_device_free(devices[i]); }
	misc_deregister(&feed_device);
}
module_init(tb330fu_init); module_exit(tb330fu_exit);
MODULE_DESCRIPTION("TB330FU Android sensor feed to standard IIO devices");
MODULE_LICENSE("GPL");
