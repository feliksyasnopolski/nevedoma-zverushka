# USB NCM gadget

## Purpose

`tb330fu-usb-gadget.service` creates the late native Linux management link.
It provides an NCM USB gadget and does not implement Android USB policy, role switching, ADB, ACM, RNDIS, or USB storage.

## Architecture

```text
systemd
    |
tb330fu-usb-gadget.service
    |
/usr/libexec/tb330fu-usb-gadget
    |
configfs: tb330fu_ncm / ncm.usb0
    |
musb-hdrc UDC
    |
usb0: 192.168.7.2/24
```

The helper owns creation of the `tb330fu_ncm` configfs gadget, its one NCM function, UDC binding, and the gadget-side IPv4 address.
`usb0` is unmanaged by NetworkManager.
No DHCP or networkd address configuration is used.
The host-side contract is `192.168.7.1/24` with host MAC `02:00:00:00:00:02`; the tablet-side MAC is `02:00:00:00:00:01`.

## Gadget contract

| Attribute | Value |
|---|---|
| Configfs gadget | `tb330fu_ncm` |
| Configuration | `configs/c.1` |
| Function | `functions/ncm.usb0` |
| Function link | `configs/c.1/ncm.usb0` |
| UDC | discovered from `/sys/class/udc`; observed `musb-hdrc` |
| VID:PID | `0x1d6b:0x0104` |
| USB version/device version | `0x0200` / `0x0100` |
| Manufacturer/product/serial | `Lenovo` / `TB330FU Ubuntu NCM` / `tb330fu-linux-ncm-001` |
| Configuration string | `NCM network` |
| Maximum power | `250` mA |
| Network interface | `usb0` |
| Address | `192.168.7.2/24` |

The helper records available USB role and Type-C state for diagnostics but does not force a role.
It discovers the first available UDC instead of hardcoding its name.

## Startup and failure behavior

The service is enabled for `multi-user.target`.
It waits for `systemd-udev-settle.service` and `network-pre.target`, then the helper waits eight seconds before touching configfs.
The helper starts or verifies SSH on TCP port 22 after configuring `usb0`.

The helper refuses to run if `tb330fu_ncm` already exists.
It therefore does not silently reconfigure or unbind an active gadget.
It has no automatic teardown; the oneshot unit remains active after successful setup.
Teardown is manual maintenance and is outside normal service stop behavior.

USB controller and gadget state can affect suspend and wake behavior.
Use Wi-Fi/mDNS for tablet access during suspend or USB investigations.
Do not unbind/rebind the UDC as part of ordinary verification.

The retained Android-init corpus still contains `g1` configfs actions for Android modes, including RNDIS, ACM, ADB, and factory/META paths.
Those are compatibility-corpus inputs, not dependencies of this native NCM service.
The native service does not remove or modify them; any future ownership cleanup requires a separate graph review.

## Verification

Read-only checks on the tablet:

```bash
systemctl status tb330fu-usb-gadget.service
find /sys/kernel/config/usb_gadget/tb330fu_ncm -maxdepth 4 -print
cat /sys/kernel/config/usb_gadget/tb330fu_ncm/UDC
cat /sys/class/udc/*/uevent
ip addr show usb0
nmcli device show usb0
```

Expected state is an active/exited service, `UDC=musb-hdrc` on the observed platform, an NCM function linked into `configs/c.1`, and `usb0` at `192.168.7.2/24`.
NetworkManager should report the interface unmanaged.