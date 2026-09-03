// SPDX-License-Identifier: GPL-2.0
/* Minimal TB330FU HCI transport shim.  No Bluetooth protocol logic. */
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <linux/un.h>
#include <net/sock.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#define SHIM_MAX_FRAME (1 + HCI_MAX_FRAME_SIZE)
#define SHIM_SOCKET_PATH "/tmp/tb330fu-hci.sock"

static struct hci_dev *shim_hdev;
static struct task_struct *shim_thread;
static struct socket *shim_sock;
static atomic64_t tx_cmd, tx_acl, tx_sco, tx_iso, tx_drop;
static atomic64_t rx_event, rx_acl, rx_sco, rx_iso, rx_drop;

static void shim_count_tx(u8 type)
{
	switch (type) {
	case HCI_COMMAND_PKT: atomic64_inc(&tx_cmd); break;
	case HCI_ACLDATA_PKT: atomic64_inc(&tx_acl); break;
	case HCI_SCODATA_PKT: atomic64_inc(&tx_sco); break;
	case HCI_ISODATA_PKT: atomic64_inc(&tx_iso); break;
	default: atomic64_inc(&tx_drop); break;
	}
}

static void shim_count_rx(u8 type)
{
	switch (type) {
	case HCI_EVENT_PKT: atomic64_inc(&rx_event); break;
	case HCI_ACLDATA_PKT: atomic64_inc(&rx_acl); break;
	case HCI_SCODATA_PKT: atomic64_inc(&rx_sco); break;
	case HCI_ISODATA_PKT: atomic64_inc(&rx_iso); break;
	default: atomic64_inc(&rx_drop); break;
	}
}

static int shim_send_frame(u8 type, const void *data, size_t len)
{
	struct msghdr msg = { };
	struct kvec iov[2];
	u8 header = type;
	int ret;

	if (!shim_sock || len > HCI_MAX_FRAME_SIZE)
		return -ENOTCONN;
	iov[0].iov_base = &header;
	iov[0].iov_len = 1;
	iov[1].iov_base = (void *)data;
	iov[1].iov_len = len;
	ret = kernel_sendmsg(shim_sock, &msg, iov, 2, len + 1);
	return ret == len + 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int shim_open(struct hci_dev *hdev)
{
	return 0;
}

static int shim_close(struct hci_dev *hdev)
{
	return 0;
}

static int shim_flush(struct hci_dev *hdev)
{
	return 0;
}

static int shim_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	u8 type = hci_skb_pkt_type(skb);
	int ret;

	shim_count_tx(type);
	ret = shim_send_frame(type, skb->data, skb->len);
	if (ret)
		atomic64_inc(&tx_drop);
	else
		kfree_skb(skb);
	return ret;
}

static int shim_submit_rx(u8 type, const u8 *payload, size_t len)
{
	struct sk_buff *skb;
	int ret;

	if (type != HCI_EVENT_PKT && type != HCI_ACLDATA_PKT &&
	    type != HCI_SCODATA_PKT && type != HCI_ISODATA_PKT) {
		atomic64_inc(&rx_drop);
		return -EINVAL;
	}
	if (!len || len > HCI_MAX_FRAME_SIZE) {
		atomic64_inc(&rx_drop);
		return -EMSGSIZE;
	}
	skb = bt_skb_alloc(len, GFP_KERNEL);
	if (!skb) {
		atomic64_inc(&rx_drop);
		return -ENOMEM;
	}
	hci_skb_pkt_type(skb) = type;
	skb_put_data(skb, payload, len);
	shim_count_rx(type);
	ret = hci_recv_frame(shim_hdev, skb);
	if (ret) {
		atomic64_inc(&rx_drop);
		kfree_skb(skb);
	}
	return ret;
}

static int shim_connect(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int ret;

	pr_info("tb330fu_hci: TRN 02 before sock_create_kern\n");
	strscpy(addr.sun_path, SHIM_SOCKET_PATH, sizeof(addr.sun_path));
	ret = sock_create_kern(&init_net, AF_UNIX, SOCK_SEQPACKET, 0,
				       &shim_sock);
	pr_info("tb330fu_hci: TRN 03 after sock_create_kern ret=%d\n", ret);
	if (ret)
		return ret;
	pr_info("tb330fu_hci: TRN 04 before kernel_connect\n");
	ret = kernel_connect(shim_sock, (struct sockaddr *)&addr,
				     sizeof(addr.sun_family) + strlen(addr.sun_path), 0);
	pr_info("tb330fu_hci: TRN 05 after kernel_connect ret=%d\n", ret);
	if (ret) {
		sock_release(shim_sock);
		shim_sock = NULL;
	}
	return ret;
}

static int shim_thread_fn(void *unused)
{
	u8 frame[SHIM_MAX_FRAME];
	struct msghdr msg;
	struct kvec iov;
	int ret;

	pr_info("tb330fu_hci: TRN 01 transport thread enter\n");
	while (!kthread_should_stop()) {
		if (!shim_sock) {
			ret = shim_connect();
			if (ret) {
				if (msleep_interruptible(100) || kthread_should_stop())
					break;
				continue;
			}
			pr_info("tb330fu_hci_shim: userspace endpoint connected: %s\n",
					SHIM_SOCKET_PATH);
		}
		memset(&msg, 0, sizeof(msg));
		iov.iov_base = frame;
		iov.iov_len = sizeof(frame);
		pr_info("tb330fu_hci: TRN 06 before sock_recvmsg\n");
		iov_iter_kvec(&msg.msg_iter, ITER_DEST, &iov, 1, sizeof(frame));
		ret = sock_recvmsg(shim_sock, &msg, 0);
		if (ret <= 0) {
			sock_release(shim_sock);
			shim_sock = NULL;
			continue;
		}
		if (ret < 2 || shim_submit_rx(frame[0], frame + 1, ret - 1))
			atomic64_inc(&rx_drop);
	}
	if (shim_sock) {
		sock_release(shim_sock);
		shim_sock = NULL;
	}
	return 0;
}

static int __init shim_init(void)
{
	int ret;

	pr_info("tb330fu_hci: INIT 01 enter\n");
	pr_info("tb330fu_hci: INIT 02 before hci_alloc_dev_priv\n");
	shim_hdev = hci_alloc_dev();
	pr_info("tb330fu_hci: INIT 03 after hci_alloc_dev_priv hdev=%p\n",
		shim_hdev);
	if (!shim_hdev)
		return -ENOMEM;
	pr_info("tb330fu_hci: INIT 04 before hci_dev field initialization\n");
	shim_hdev->bus = HCI_VIRTUAL;
	shim_hdev->open = shim_open;
	shim_hdev->close = shim_close;
	shim_hdev->flush = shim_flush;
	shim_hdev->send = shim_send;
	set_bit(HCI_QUIRK_NON_PERSISTENT_SETUP, &shim_hdev->quirks);
	set_bit(HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2, &shim_hdev->quirks);
	pr_info("tb330fu_hci: INIT 05 after hci_dev field initialization\n");
	/*
	 * Establish the backend before publishing hci0.  hci_register_dev()
	 * can synchronously cause the Bluetooth core to send HCI Reset through
	 * shim_send(), so registering first races the transport kthread.
	 */
	pr_info("tb330fu_hci: INIT 06 before initial kernel_connect\n");
	ret = shim_connect();
	pr_info("tb330fu_hci: INIT 07 after initial kernel_connect ret=%d\n", ret);
	if (ret) {
		pr_err("tb330fu_hci: transport not ready, refusing hci_register_dev ret=%d\n",
		       ret);
		shim_sock = NULL;
		hci_free_dev(shim_hdev);
		shim_hdev = NULL;
		return ret;
	}
	pr_info("tb330fu_hci: INIT 08 transport ready before hci_register_dev\n");
	ret = hci_register_dev(shim_hdev);
	pr_info("tb330fu_hci: INIT 09 after hci_register_dev ret=%d\n", ret);
	if (ret) {
		sock_release(shim_sock);
		shim_sock = NULL;
		hci_free_dev(shim_hdev);
		shim_hdev = NULL;
		return ret;
	}
	pr_info("tb330fu_hci: INIT 10 before kthread_run\n");
	shim_thread = kthread_run(shim_thread_fn, NULL, "tb330fu-hci");
	pr_info("tb330fu_hci: INIT 11 after kthread_run thread=%p\n",
		shim_thread);
	if (IS_ERR(shim_thread)) {
		ret = PTR_ERR(shim_thread);
		shim_thread = NULL;
		hci_unregister_dev(shim_hdev);
		sock_release(shim_sock);
		shim_sock = NULL;
		hci_free_dev(shim_hdev);
		shim_hdev = NULL;
		return ret;
	}
	pr_info("tb330fu_hci: INIT 12 complete\n");
	return 0;
}

static void __exit shim_exit(void)
{
	pr_info("tb330fu_hci: EXIT 01 enter\n");
	if (shim_thread)
		pr_info("tb330fu_hci: EXIT 02 before kthread_stop\n");
	if (shim_thread)
		kthread_stop(shim_thread);
	pr_info("tb330fu_hci: EXIT 03 before hci_unregister_dev\n");
	if (shim_hdev) {
		hci_unregister_dev(shim_hdev);
		pr_info("tb330fu_hci: EXIT 04 after hci_unregister_dev\n");
		hci_free_dev(shim_hdev);
		shim_hdev = NULL;
	}
	pr_info("tb330fu_hci: EXIT 05 complete\n");
	pr_info("tb330fu_hci_shim: unloaded tx(cmd=%lld acl=%lld sco=%lld iso=%lld drop=%lld) rx(event=%lld acl=%lld sco=%lld iso=%lld drop=%lld)\n",
		atomic64_read(&tx_cmd), atomic64_read(&tx_acl),
		atomic64_read(&tx_sco), atomic64_read(&tx_iso), atomic64_read(&tx_drop),
		atomic64_read(&rx_event), atomic64_read(&rx_acl),
		atomic64_read(&rx_sco), atomic64_read(&rx_iso), atomic64_read(&rx_drop));
}

module_init(shim_init);
module_exit(shim_exit);
MODULE_DESCRIPTION("TB330FU minimal userspace-backed Bluetooth HCI transport");
MODULE_LICENSE("GPL");
