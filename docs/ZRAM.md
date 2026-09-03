# Zram

`tb330fu-zram.service` is the sole owner of native zram setup and swap activation.
The Android/vendor runtime graph does not create, configure, tune, initialize, or activate zram.

| Component | Role |
|---|---|
| `zram.ko` | Kernel provider. It is a deferred module, outside the native early-load manifest. |
| `tb330fu-devd.service` | Creates `/dev/zram0` from kernel uevents because this kernel has no devtmpfs. |
| `src/zram/tb330fu-zram` | Idempotent setup and activation helper. |
| `systemd/tb330fu-zram.service` | Orders and runs the helper during native boot. |

The helper creates the `zram0` kernel device when needed, selects `zstd`, requests a 2G disk size, initializes swap, and activates it with priority 100.
`tb330fu-devd` materializes `/dev/zram0` from the resulting kernel uevent.
The kernel supplies the resulting runtime `disksize`; the helper requires it to be nonzero but does not encode an exact byte count.
No writeback, backing device, or zram-local tuning is configured.
Generic VM policy such as swappiness, page-cluster, MGLRU, and unrelated sysctls is not copied from Android init.

The service runs after `tb330fu-devd.service` and does not use a systemd `.device` unit.
The generator-created `dev-zram0.swap` unit is masked on the native target and is not part of the native ownership path.
If needed, the helper loads `zram.ko`, creates `zram0` through the kernel control interface, and waits for devd to materialize the node.
It never manually creates the node.
An active incompatible zram device fails without reset.
An inactive stale device may be reset.

The service has no stop teardown.
Stopping it does not run swapoff, reset, or hot-remove; teardown is an explicit maintenance operation.

## Post-deployment verification

```bash
systemctl status tb330fu-zram.service
cat /proc/swaps
cat /sys/block/zram0/comp_algorithm
cat /sys/block/zram0/disksize
```

Expected results are `/dev/zram0` with priority `100`, `[zstd]` selected, and a nonzero kernel-provided `disksize`.