/* TB330FU camera transport: the producer and consumer contract is I420/YU12. */
static const struct v4l2l_format formats[] = {
	{
		.name = "12 bpp YU12 4:2:0",
		.fourcc = V4L2_PIX_FMT_YUV420,
		.depth = 12,
		.flags = FORMAT_FLAGS_PLANAR,
	},
};
