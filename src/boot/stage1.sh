#!/bin/sh

PATH=/bin:/sbin
export PATH

INTERNAL_MMC_CONTROLLER="11230000.mmc"
SD_MMC_CONTROLLER="11240000.mmc"
MODULE_DIR="/lib/modules"
SYSFS_ROOT="${SYSFS_ROOT:-/sys}"
DEV_ROOT="${DEV_ROOT:-/dev}"
MMC_DISCOVERY_TIMEOUT="${MMC_DISCOVERY_TIMEOUT:-30}"
bb() {
    /bin/busybox "$@"
}

kmsg() {
    echo "TB330FU INIT: $*" > /dev/kmsg 2>/dev/null || \
        echo "TB330FU INIT: $*" > /dev/console 2>/dev/null || true
}

make_static_nodes() {
    bb mkdir -p /dev /dev/pts
    bb mknod /dev/kmsg    c 1 11 2>/dev/null || true
    bb mknod /dev/console c 5 1  2>/dev/null || true
    bb mknod /dev/tty     c 5 0  2>/dev/null || true
    bb mknod /dev/ptmx    c 5 2  2>/dev/null || true
    bb mknod /dev/null    c 1 3  2>/dev/null || true
    bb mknod /dev/zero    c 1 5  2>/dev/null || true
    bb mknod /dev/random  c 1 8  2>/dev/null || true
    bb mknod /dev/urandom c 1 9  2>/dev/null || true
    bb chmod 600 /dev/kmsg 2>/dev/null || true
    bb chmod 600 /dev/console 2>/dev/null || true
    bb chmod 666 /dev/tty /dev/ptmx /dev/null /dev/zero 2>/dev/null || true
    bb chmod 666 /dev/random /dev/urandom 2>/dev/null || true
}

mount_early() {
    bb mkdir -p /proc /sys /dev /dev/pts /newroot
    bb mount -t proc proc /proc || true
    bb mount -t sysfs sysfs /sys || true
    bb mount -t tmpfs -o mode=0755,nosuid tmpfs /dev || true
    make_static_nodes
    bb mount -t devpts -o mode=0620,ptmxmode=0666 devpts /dev/pts || true
}

module_loaded() {
    modname="${1%.ko}"
    modname="$(echo "$modname" | bb tr '-' '_')"
    [ -d "/sys/module/$modname" ]
}

load_one() {
    module="$1"
    kmsg "insmod $module"
    if bb insmod "$MODULE_DIR/$module" 2>/dev/kmsg; then
        kmsg "loaded $module"
        return 0
    fi
    if module_loaded "$module"; then
        kmsg "$module already loaded"
        return 0
    fi
    kmsg "FAILED to load $module"
    return 1
}

log_watchdog_state() {
    for watchdog in "$SYSFS_ROOT"/class/watchdog/watchdog*; do
        [ -e "$watchdog" ] || continue
        kmsg "watchdog sysfs: $(bb basename "$watchdog")"
        for field in identity timeout state nowayout; do
            [ -r "$watchdog/$field" ] || continue
            value="$(bb cat "$watchdog/$field" 2>/dev/null)"
            kmsg "watchdog $field=${value:-empty}"
        done
    done
}

load_stock_early_modules() {
    # Stock Android first-stage init uses libmodprobe, which consumes the
    # normal modules.load request list plus modules.dep/modules.softdep.  The
    # native initramfs intentionally keeps its static init and BusyBox shell;
    # modules.load.native is the equivalent dependency-resolved request order
    # generated from those preserved stock metadata files.  No module options
    # file exists in the oracle.
    failed=0
    kmsg "loading stock normal-boot module set"
    while IFS= read -r module || [ -n "$module" ]; do
        case "$module" in
            ''|'#'*) continue ;;
        esac
        load_one "$module" || failed=1
    done < "$MODULE_DIR/modules.load.native"
    log_watchdog_state
    [ "$failed" -eq 0 ]
}

driver_name() {
    device="$1"
    link="$SYSFS_ROOT/bus/platform/devices/$device/driver"
    [ -L "$link" ] || return 1
    bb basename "$(bb readlink "$link" 2>/dev/null)"
}

wait_driver() {
    device="$1"
    expected="$2"
    timeout="$3"
    count=0
    while [ "$count" -lt "$timeout" ]; do
        observed="$(driver_name "$device" 2>/dev/null || true)"
        if [ "$observed" = "$expected" ]; then
            kmsg "provider ready: $device -> $expected"
            return 0
        fi
        count=$((count + 1))
        bb sleep 1
    done
    observed="$(driver_name "$device" 2>/dev/null || true)"
    kmsg "provider timeout: $device expected $expected observed ${observed:-unbound}"
    return 1
}

regulator_present() {
    wanted="$1"
    for name_file in "$SYSFS_ROOT"/class/regulator/*/name; do
        [ -r "$name_file" ] || continue
        observed="$(bb cat "$name_file" 2>/dev/null)"
        [ "$observed" = "$wanted" ] && return 0
    done
    return 1
}

wait_regulator_name() {
    wanted="$1"
    timeout="$2"
    count=0
    while [ "$count" -lt "$timeout" ]; do
        if regulator_present "$wanted"; then
            kmsg "regulator ready: $wanted"
            return 0
        fi
        count=$((count + 1))
        bb sleep 1
    done
    kmsg "regulator timeout: $wanted"
    return 1
}

wait_for_providers() {
    wait_driver 10000000.topckgen clk-mt6768 20 || return 1
    wait_driver 10001000.infracfg_ao clk-mt6768 20 || return 1
    wait_driver 10005000.pinctrl mt6768-pinctrl 20 || return 1
    wait_driver 10012000.dvfsrc mtk-dvfsrc 20 || return 1
    wait_driver mt6358-regulator mt6358-regulator 20 || return 1
    wait_regulator_name dvfsrc-vcore 20 || return 1
    wait_regulator_name vmc 20 || return 1
    wait_regulator_name mt6358_vmch_eint_low 20 || return 1
}

wait_for_controller() {
    _wfc_controller="$1"
    count=0
    while [ "$count" -lt 20 ]; do
        if [ -L "$SYSFS_ROOT/bus/platform/devices/$_wfc_controller/driver" ]; then
            kmsg "MMC controller $_wfc_controller bound"
            return 0
        fi
        count=$((count + 1))
        bb sleep 1
    done
    kmsg "MMC controller $_wfc_controller did not bind"
    return 1
}

read_charger_boot_mode() {
    _boot_mode_bytes="$(bb hexdump -v -s 8 -n 4 \
        -e '4/1 "%u "' "$SYSFS_ROOT/firmware/devicetree/base/chosen/atag,boot" \
        2>/dev/null)" || return 1
    set -- $_boot_mode_bytes
    [ "$#" -eq 4 ] || return 1
    case "$1$2$3$4" in
        *[!0-9]*) return 1 ;;
    esac
    # The native AArch64 DT value is little-endian: byte 8 is the low byte.
    BOOT_MODE=$(($1 | ($2 << 8) | ($3 << 16) | ($4 << 24)))
    return 0
}

platform_driver_state() {
    device="$1"
    observed="$(driver_name "$device" 2>/dev/null || true)"
    kmsg "MMC state: $device driver=${observed:-unbound}"
}

log_mmc_failure_state() {
    platform_driver_state 11230000.mmc
    platform_driver_state 11240000.mmc
    [ -d "$SYSFS_ROOT/class/mmc_host/mmc0" ] && mmc0=present || mmc0=absent
    [ -d "$SYSFS_ROOT/class/mmc_host/mmc1" ] && mmc1=present || mmc1=absent
    kmsg "MMC hosts: mmc0=$mmc0 mmc1=$mmc1"
    blocks="$(bb ls "$SYSFS_ROOT/class/block" 2>/dev/null | bb tr '\n' ' ')"
    kmsg "block devices: ${blocks:-none}"
}

create_block_node() {
    _cbn_sysnode="$1"
    _cbn_name="$(bb basename "$_cbn_sysnode")"
    _cbn_devno="$(bb cat "$_cbn_sysnode/dev" 2>/dev/null)"
    _cbn_major="${_cbn_devno%:*}"
    _cbn_minor="${_cbn_devno#*:}"
    case "$_cbn_major" in
        ''|*[!0-9]*) return 1 ;;
    esac
    case "$_cbn_minor" in
        ''|*[!0-9]*) return 1 ;;
    esac
    bb mknod "$DEV_ROOT/$_cbn_name" b "$_cbn_major" "$_cbn_minor" 2>/dev/null || \
        [ -b "$DEV_ROOT/$_cbn_name" ] || return 1
    bb chmod 600 "$DEV_ROOT/$_cbn_name" 2>/dev/null || true
    kmsg "created $DEV_ROOT/$_cbn_name ($_cbn_major:$_cbn_minor)"
    return 0
}

create_char_node() {
    _ccn_sysnode="$1"
    _ccn_node="$2"
    _ccn_parent="$3"
    _ccn_mode="$4"
    _ccn_devno="$(bb cat "$_ccn_sysnode/dev" 2>/dev/null)" || return 1
    _ccn_major="${_ccn_devno%:*}"
    _ccn_minor="${_ccn_devno#*:}"
    case "$_ccn_major" in
        ''|*[!0-9]*) return 1 ;;
    esac
    case "$_ccn_minor" in
        ''|*[!0-9]*) return 1 ;;
    esac
    bb mkdir -p "$_ccn_parent" || return 1
    bb mknod "$_ccn_node" c "$_ccn_major" "$_ccn_minor" 2>/dev/null || \
        [ -c "$_ccn_node" ] || return 1
    bb chmod "$_ccn_mode" "$_ccn_node" 2>/dev/null || return 1
    kmsg "character node $_ccn_node ($_ccn_major:$_ccn_minor) mode=$_ccn_mode"
    return 0
}

wait_for_host() {
    _wfh_controller="$1"
    _wfh_role="$2"
    _wfh_count=0
    while [ "$_wfh_count" -lt "$MMC_DISCOVERY_TIMEOUT" ]; do
        for _wfh_sysnode in \
            "$SYSFS_ROOT/bus/platform/devices/$_wfh_controller/mmc_host"/mmc*; do
            [ -d "$_wfh_sysnode" ] || continue
            DISCOVERED_MMC_HOST="$(bb basename "$_wfh_sysnode")"
            kmsg "$_wfh_role MMC host: $DISCOVERED_MMC_HOST"
            return 0
        done
        _wfh_count=$((_wfh_count + 1))
        bb sleep 1
    done
    kmsg "$_wfh_role host not found for $_wfh_controller"
    return 1
}

wait_for_disk() {
    _wfd_controller="$1"
    _wfd_host="$2"
    _wfd_role="$3"
    _wfd_count=0
    while [ "$_wfd_count" -lt "$MMC_DISCOVERY_TIMEOUT" ]; do
        for _wfd_sysnode in \
            "$SYSFS_ROOT/bus/platform/devices/$_wfd_controller/mmc_host/$_wfd_host"/"$_wfd_host":*/block/mmcblk*; do
            [ -r "$_wfd_sysnode/dev" ] || continue
            DISCOVERED_MMC_DISK="$(bb basename "$_wfd_sysnode")"
            kmsg "$_wfd_role MMC disk: $DISCOVERED_MMC_DISK"
            return 0
        done
        _wfd_count=$((_wfd_count + 1))
        bb sleep 1
    done
    kmsg "disk not found for $_wfd_role host $_wfd_host"
    return 1
}

wait_for_partition() {
    _wfp_disk="$1"
    _wfp_number="$2"
    _wfp_role="$3"
    _wfp_partition="${_wfp_disk}p$_wfp_number"
    _wfp_count=0
    while [ "$_wfp_count" -lt "$MMC_DISCOVERY_TIMEOUT" ]; do
        if [ -r "$SYSFS_ROOT/class/block/$_wfp_partition/dev" ]; then
            DISCOVERED_MMC_PARTITION="$_wfp_partition"
            return 0
        fi
        _wfp_count=$((_wfp_count + 1))
        bb sleep 1
    done
    kmsg "partition $_wfp_number not found for $_wfp_role disk $_wfp_disk"
    return 1
}

discover_controller_root() {
    _dcr_controller="$1"
    _dcr_partition_number="$2"
    _dcr_role="$3"
    wait_for_controller "$_dcr_controller" || return 1
    wait_for_host "$_dcr_controller" "$_dcr_role" || return 1
    _dcr_host="$DISCOVERED_MMC_HOST"
    wait_for_disk "$_dcr_controller" "$_dcr_host" "$_dcr_role" || return 1
    _dcr_disk="$DISCOVERED_MMC_DISK"
    if ! create_block_node "$SYSFS_ROOT/class/block/$_dcr_disk"; then
        kmsg "mknod failed for $_dcr_role disk $_dcr_disk"
        return 1
    fi
    wait_for_partition "$_dcr_disk" "$_dcr_partition_number" "$_dcr_role" || return 1
    _dcr_partition="$DISCOVERED_MMC_PARTITION"
    if ! create_block_node "$SYSFS_ROOT/class/block/$_dcr_partition"; then
        kmsg "mknod failed for $_dcr_role partition $_dcr_partition"
        return 1
    fi
    ROOT_NODE="$DEV_ROOT/$_dcr_partition"
    return 0
}

select_root() {
    if discover_controller_root "$INTERNAL_MMC_CONTROLLER" 52 internal; then
        ROOT_SOURCE=internal
        kmsg "TB330FU EARLY: selected internal root $ROOT_NODE (p52)"
        return 0
    fi
    kmsg "TB330FU EARLY: internal root unavailable; trying SD rescue"
    if discover_controller_root "$SD_MMC_CONTROLLER" 1 SD-rescue; then
        ROOT_SOURCE=sd
        kmsg "TB330FU EARLY: selected SD rescue root $ROOT_NODE (p1)"
        return 0
    fi
    return 1
}

mount_selected_root() {
    _msr_options="$1"
    if bb mount -t ext4 -o "$_msr_options" "$ROOT_NODE" /newroot; then
        return 0
    fi
    if [ "$ROOT_SOURCE" = internal ]; then
        kmsg "TB330FU EARLY: internal root unusable; trying SD rescue"
        if discover_controller_root "$SD_MMC_CONTROLLER" 1 SD-rescue; then
            ROOT_SOURCE=sd
            kmsg "TB330FU EARLY: selected SD rescue root $ROOT_NODE (p1)"
            bb mount -t ext4 -o "$_msr_options" "$ROOT_NODE" /newroot && return 0
        fi
    fi
    return 1
}

hold_pid1() {
    reason="$1"
    kmsg "$reason; PID 1 will remain alive"
    bb sync 2>/dev/null || true
    while true; do
        bb sleep 10
    done
}

kmsg "TB330FU EARLY: init started"
mount_early
kmsg "TB330FU EARLY: pseudo-filesystems ready"

CHARGER_BOOT=0
if read_charger_boot_mode; then
    kmsg "TB330FU EARLY: atag boot mode=0x$(printf '%x' "$BOOT_MODE")"
    [ "$BOOT_MODE" -eq 8 ] && CHARGER_BOOT=1
else
    kmsg "TB330FU EARLY: atag boot mode unavailable; using normal boot"
fi

load_stock_early_modules || hold_pid1 "154-module early closure load failure"
kmsg "TB330FU EARLY: module closure loaded (154 modules)"
wait_for_providers || hold_pid1 "MMC provider readiness failure"
select_root || { log_mmc_failure_state; hold_pid1 "internal and SD root discovery failure"; }

if [ "$CHARGER_BOOT" -eq 1 ]; then
    bb mkdir -p /newroot
    kmsg "TB330FU CHARGER: mounting Ubuntu root $ROOT_NODE ro"
    if ! mount_selected_root ro; then
        hold_pid1 "charger ext4 root mount failed"
    fi
    [ -x /newroot/usr/libexec/tb330fu-charger ] || \
        hold_pid1 "charger binary missing or not executable"
    [ -d /newroot/proc ] && [ -d /newroot/sys ] && [ -d /newroot/dev ] || \
        hold_pid1 "charger root mountpoints missing"
    if ! create_char_node "$SYSFS_ROOT/class/drm/card0" \
        "$DEV_ROOT/dri/card0" "$DEV_ROOT/dri" 0600; then
        hold_pid1 "charger DRM node creation failed"
    fi
    if ! create_char_node "$SYSFS_ROOT/class/misc/pmsg0" \
        "$DEV_ROOT/pmsg0" "$DEV_ROOT" 0222; then
        kmsg "charger pmsg node creation failed; continuing without pmsg"
    fi
    kmsg "TB330FU CHARGER: switching root and execing charger"
    bb mount --move /proc /newroot/proc || hold_pid1 "failed to move /proc for charger"
    bb mount --move /sys /newroot/sys || hold_pid1 "failed to move /sys for charger"
    bb mount --move /dev /newroot/dev || hold_pid1 "failed to move /dev for charger"
    exec /bin/switch_root /newroot /usr/libexec/tb330fu-charger
    hold_pid1 "charger switch_root returned unexpectedly"
fi

bb mkdir -p /newroot
kmsg "TB330FU EARLY: mounting Ubuntu root $ROOT_NODE rw"
if ! mount_selected_root rw; then
    hold_pid1 "ext4 root mount failed"
fi
kmsg "TB330FU EARLY: Ubuntu root mounted"

[ -e /newroot/sbin/init ] || hold_pid1 "required init path missing: /newroot/sbin/init"
[ -x /newroot/sbin/init ] || hold_pid1 "/newroot/sbin/init is not executable"
kmsg "TB330FU EARLY: /newroot/sbin/init ready"
bb mkdir -p /newroot/proc /newroot/sys /newroot/dev
kmsg "TB330FU EARLY: switching root to Ubuntu"
bb mount --move /proc /newroot/proc || hold_pid1 "failed to move /proc"
bb mount --move /sys /newroot/sys || hold_pid1 "failed to move /sys"
bb mount --move /dev /newroot/dev || hold_pid1 "failed to move /dev"
exec /bin/switch_root /newroot /sbin/init

hold_pid1 "switch_root returned unexpectedly"
