#ifndef TB330FU_SENSOR_FEED_H
#define TB330FU_SENSOR_FEED_H

#include <linux/types.h>

#define TB330FU_SENSOR_ABI_VERSION 1u

enum tb330fu_sensor_kind {
	TB330FU_SENSOR_ACCEL = 1,
	TB330FU_SENSOR_GYRO = 2,
	TB330FU_SENSOR_LIGHT = 3,
	TB330FU_SENSOR_STEPS = 4,
};

struct tb330fu_sensor_sample {
	__u32 version;
	__u32 kind;
	__u64 timestamp_ns;
	union {
		struct { __s32 x, y, z; } vec3;
		__s32 light;
		__u64 steps;
	};
};

#endif
