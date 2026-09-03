#include "generated_ueventd_policy.h"
const struct dev_policy tb_dev_policy[] = {
 {"/dev/null","/system/etc/ueventd.rc",438u,0,0,38u}, /* root:root */
 {"/dev/zero","/system/etc/ueventd.rc",438u,0,0,39u}, /* root:root */
 {"/dev/full","/system/etc/ueventd.rc",438u,0,0,40u}, /* root:root */
 {"/dev/ptmx","/system/etc/ueventd.rc",438u,0,0,41u}, /* root:root */
 {"/dev/tty","/system/etc/ueventd.rc",438u,0,0,42u}, /* root:root */
 {"/dev/random","/system/etc/ueventd.rc",438u,0,0,43u}, /* root:root */
 {"/dev/urandom","/system/etc/ueventd.rc",438u,0,0,44u}, /* root:root */
 {"/dev/hw_random","/system/etc/ueventd.rc",256u,1078,1078,46u}, /* prng_seeder:prng_seeder */
 {"/dev/ashmem*","/system/etc/ueventd.rc",438u,0,0,47u}, /* root:root */
 {"/dev/binder","/system/etc/ueventd.rc",438u,0,0,48u}, /* root:root */
 {"/dev/hwbinder","/system/etc/ueventd.rc",438u,0,0,49u}, /* root:root */
 {"/dev/vndbinder","/system/etc/ueventd.rc",438u,0,0,50u}, /* root:root */
 {"/dev/vfio/*","/system/etc/ueventd.rc",438u,0,0,51u}, /* root:root */
 {"/dev/pmsg0","/system/etc/ueventd.rc",146u,0,1007,53u}, /* root:log */
 {"/dev/dma_heap/system","/system/etc/ueventd.rc",292u,1000,1000,54u}, /* system:system */
 {"/dev/dma_heap/system-uncached","/system/etc/ueventd.rc",292u,1000,1000,55u}, /* system:system */
 {"/dev/dma_heap/system-secure","/system/etc/ueventd.rc",292u,1000,1000,56u}, /* system:system */
 {"/dev/dri/*","/system/etc/ueventd.rc",438u,0,1003,59u}, /* root:graphics */
 {"/dev/uhid","/system/etc/ueventd.rc",432u,1071,1071,62u}, /* uhid:uhid */
 {"/dev/uinput","/system/etc/ueventd.rc",432u,1071,1071,63u}, /* uhid:uhid */
 {"/dev/rtc0","/system/etc/ueventd.rc",416u,1000,1000,64u}, /* system:system */
 {"/dev/tty0","/system/etc/ueventd.rc",432u,0,1000,65u}, /* root:system */
 {"/dev/graphics/*","/system/etc/ueventd.rc",432u,0,1003,66u}, /* root:graphics */
 {"/dev/input/*","/system/etc/ueventd.rc",432u,0,1004,67u}, /* root:input */
 {"/dev/v4l-touch*","/system/etc/ueventd.rc",432u,0,1004,68u}, /* root:input */
 {"/dev/snd/*","/system/etc/ueventd.rc",432u,1000,1041,69u}, /* system:audio */
 {"/dev/bus/usb/*","/system/etc/ueventd.rc",432u,0,1018,70u}, /* root:usb */
 {"/dev/mtp_usb","/system/etc/ueventd.rc",432u,0,1024,71u}, /* root:mtp */
 {"/dev/usb_accessory","/system/etc/ueventd.rc",432u,0,1018,72u}, /* root:usb */
 {"/dev/tun","/system/etc/ueventd.rc",432u,1000,1016,73u}, /* system:vpn */
 {"/dev/hidraw*","/system/etc/ueventd.rc",432u,1000,1000,74u}, /* system:system */
 {"/dev/ppp","/system/etc/ueventd.rc",432u,1001,1016,77u}, /* radio:vpn */
 {"/dev/kvm","/system/etc/ueventd.rc",438u,0,0,79u}, /* root:root */
 {"/dev/vhost-vsock","/system/etc/ueventd.rc",438u,0,0,80u}, /* root:root */
 {"/dev/block/mmcblk0","/vendor/etc/ueventd.rc",432u,0,1000,2u}, /* root:system */
 {"/dev/block/mmcblk0boot0","/vendor/etc/ueventd.rc",432u,0,1000,3u}, /* root:system */
 {"/dev/block/mmcblk0boot1","/vendor/etc/ueventd.rc",432u,0,1000,4u}, /* root:system */
 {"/dev/misc-sd","/vendor/etc/ueventd.rc",432u,0,1000,5u}, /* root:system */
 {"/dev/block/by-name/misc2","/vendor/etc/ueventd.rc",432u,0,1000,6u}, /* root:system */
 {"/dev/block/by-name/boot","/vendor/etc/ueventd.rc",416u,0,1000,7u}, /* root:system */
 {"/dev/block/by-name/recovery","/vendor/etc/ueventd.rc",416u,0,1000,8u}, /* root:system */
 {"/dev/block/by-name/secro","/vendor/etc/ueventd.rc",416u,0,1000,9u}, /* root:system */
 {"/dev/block/by-name/seccfg","/vendor/etc/ueventd.rc",432u,0,1000,10u}, /* root:system */
 {"/dev/block/by-name/proinfo","/vendor/etc/ueventd.rc",432u,0,1000,11u}, /* root:system */
 {"/dev/block/by-name/nvram","/vendor/etc/ueventd.rc",432u,0,1000,12u}, /* root:system */
 {"/dev/block/by-name/para","/vendor/etc/ueventd.rc",432u,0,1000,13u}, /* root:system */
 {"/dev/block/by-name/logo","/vendor/etc/ueventd.rc",432u,0,1000,14u}, /* root:system */
 {"/dev/block/by-name/frp","/vendor/etc/ueventd.rc",432u,0,1000,15u}, /* root:system */
 {"/dev/block/by-name/md1img","/vendor/etc/ueventd.rc",416u,0,1000,16u}, /* root:system */
 {"/dev/block/by-name/md1img_1","/vendor/etc/ueventd.rc",416u,0,1000,17u}, /* root:system */
 {"/dev/block/by-name/md1img_a","/vendor/etc/ueventd.rc",416u,0,1000,18u}, /* root:system */
 {"/dev/block/by-name/md1img_b","/vendor/etc/ueventd.rc",416u,0,1000,19u}, /* root:system */
 {"/dev/block/by-name/modem","/vendor/etc/ueventd.rc",416u,0,1000,20u}, /* root:system */
 {"/dev/block/by-name/modem_a","/vendor/etc/ueventd.rc",416u,0,1000,21u}, /* root:system */
 {"/dev/block/by-name/modem_b","/vendor/etc/ueventd.rc",416u,0,1000,22u}, /* root:system */
 {"/dev/block/by-name/md1dsp","/vendor/etc/ueventd.rc",432u,0,1000,23u}, /* root:system */
 {"/dev/block/by-name/boot_para","/vendor/etc/ueventd.rc",416u,0,1000,24u}, /* root:system */
 {"/dev/block/by-name/lenovoraw","/vendor/etc/ueventd.rc",432u,1000,1000,25u}, /* system:system */
 {"/dev/otp","/vendor/etc/ueventd.rc",432u,1000,1000,28u}, /* system:system */
 {"/dev/block/by-name/otp","/vendor/etc/ueventd.rc",432u,0,1000,29u}, /* root:system */
 {"/dev/stpwmt","/vendor/etc/ueventd.rc",432u,1000,1000,32u}, /* system:system */
 {"/dev/wmtdetect","/vendor/etc/ueventd.rc",432u,1000,1000,33u}, /* system:system */
 {"/dev/fw_log_wmt","/vendor/etc/ueventd.rc",432u,1000,1000,34u}, /* system:system */
 {"/dev/stpbt","/vendor/etc/ueventd.rc",432u,1002,1002,37u}, /* bluetooth:bluetooth */
 {"/dev/fw_log_bt","/vendor/etc/ueventd.rc",432u,1002,1002,38u}, /* bluetooth:bluetooth */
 {"/dev/stpgps","/vendor/etc/ueventd.rc",432u,1021,1021,41u}, /* gps:gps */
 {"/dev/gps","/vendor/etc/ueventd.rc",432u,1021,1000,42u}, /* gps:system */
 {"/dev/fw_log_gps","/vendor/etc/ueventd.rc",432u,1021,1021,43u}, /* gps:gps */
 {"/dev/gps_emi","/vendor/etc/ueventd.rc",432u,1021,1021,44u}, /* gps:gps */
 {"/dev/stpant","/vendor/etc/ueventd.rc",432u,1000,1000,47u}, /* system:system */
 {"/dev/wmtWifi","/vendor/etc/ueventd.rc",432u,1010,1010,50u}, /* wifi:wifi */
 {"/dev/fw_log_wifi","/vendor/etc/ueventd.rc",432u,1010,1010,51u}, /* wifi:wifi */
 {"/dev/fm","/vendor/etc/ueventd.rc",432u,1013,1013,54u}, /* media:media */
 {"/dev/st21nfc","/vendor/etc/ueventd.rc",432u,1027,1001,57u}, /* nfc:radio */
 {"/dev/st54spi","/vendor/etc/ueventd.rc",432u,1068,1068,58u}, /* secure_element:secure_element */
 {"/dev/btif","/vendor/etc/ueventd.rc",384u,1000,1000,61u}, /* system:system */
 {"/dev/trusty-ipc-dev0","/vendor/etc/ueventd.rc",432u,1000,1000,64u}, /* system:system */
 {"/dev/nebula-ipc-dev0","/vendor/etc/ueventd.rc",432u,1000,1000,65u}, /* system:system */
 {"/dev/block/mmcblk0rpmb","/vendor/etc/ueventd.rc",432u,0,1000,68u}, /* root:system */
 {"/dev/mobicore","/vendor/etc/ueventd.rc",384u,1000,1000,71u}, /* system:system */
 {"/dev/mobicore-user","/vendor/etc/ueventd.rc",438u,1000,1000,72u}, /* system:system */
 {"/dev/t-base-tui","/vendor/etc/ueventd.rc",438u,1000,1000,73u}, /* system:system */
 {"/dev/mt-mdp","/vendor/etc/ueventd.rc",432u,1000,1013,76u}, /* system:media */
 {"/dev/mtk_mdp","/vendor/etc/ueventd.rc",432u,1000,1000,77u}, /* system:system */
 {"/dev/mdp_sync","/vendor/etc/ueventd.rc",432u,1000,1000,78u}, /* system:system */
 {"/dev/camera-sysram","/vendor/etc/ueventd.rc",432u,1000,1047,81u}, /* system:camera */
 {"/dev/camera-mem","/vendor/etc/ueventd.rc",432u,1000,1047,82u}, /* system:camera */
 {"/dev/camera-isp","/vendor/etc/ueventd.rc",432u,1000,1047,83u}, /* system:camera */
 {"/dev/camera-dip","/vendor/etc/ueventd.rc",432u,1000,1047,84u}, /* system:camera */
 {"/dev/camera-tsf","/vendor/etc/ueventd.rc",432u,1000,1047,85u}, /* system:camera */
 {"/dev/camera-dpe","/vendor/etc/ueventd.rc",432u,1000,1047,86u}, /* system:camera */
 {"/dev/camera-mfb","/vendor/etc/ueventd.rc",432u,1000,1047,87u}, /* system:camera */
 {"/dev/camera-rsc","/vendor/etc/ueventd.rc",432u,1000,1047,88u}, /* system:camera */
 {"/dev/camera-owe","/vendor/etc/ueventd.rc",432u,1000,1047,89u}, /* system:camera */
 {"/dev/camera-fdvt","/vendor/etc/ueventd.rc",432u,1000,1047,90u}, /* system:camera */
 {"/dev/camera-wpe","/vendor/etc/ueventd.rc",432u,1000,1047,91u}, /* system:camera */
 {"/dev/camera-pipemgr","/vendor/etc/ueventd.rc",432u,1000,1047,92u}, /* system:camera */
 {"/dev/kd_camera_hw","/vendor/etc/ueventd.rc",432u,1000,1047,93u}, /* system:camera */
 {"/dev/seninf","/vendor/etc/ueventd.rc",432u,1000,1047,94u}, /* system:camera */
 {"/dev/flashlight","/vendor/etc/ueventd.rc",432u,1000,1047,95u}, /* system:camera */
 {"/dev/kd_camera_hw_bus2","/vendor/etc/ueventd.rc",432u,1000,1047,96u}, /* system:camera */
 {"/dev/FM50AF","/vendor/etc/ueventd.rc",432u,1000,1047,97u}, /* system:camera */
 {"/dev/CAM_CAL_DRV","/vendor/etc/ueventd.rc",432u,1000,1047,98u}, /* system:camera */
 {"/dev/MAINAF","/vendor/etc/ueventd.rc",432u,1000,1047,99u}, /* system:camera */
 {"/dev/MAIN2AF","/vendor/etc/ueventd.rc",432u,1000,1047,100u}, /* system:camera */
 {"/dev/SUBAF","/vendor/etc/ueventd.rc",432u,1000,1047,101u}, /* system:camera */
 {"/dev/vpu","/vendor/etc/ueventd.rc",432u,1000,1047,102u}, /* system:camera */
 {"/dev/ccu","/vendor/etc/ueventd.rc",432u,1000,1047,105u}, /* system:camera */
 {"/dev/video*","/vendor/etc/ueventd.rc",432u,1047,1000,108u}, /* camera:system */
 {"/dev/media*","/vendor/etc/ueventd.rc",432u,1047,1000,109u}, /* camera:system */
 {"/dev/v4l-subdev*","/vendor/etc/ueventd.rc",432u,1047,1000,110u}, /* camera:system */
 {"/dev/gzvm","/vendor/etc/ueventd.rc",438u,0,0,113u}, /* root:root */
 {"/dev/bus/usb/*","/vendor/etc/ueventd.rc",432u,0,1018,116u}, /* root:usb */
 {"/dev/ttyUSB0","/vendor/etc/ueventd.rc",432u,1001,1001,117u}, /* radio:radio */
 {"/dev/ttyUSB1","/vendor/etc/ueventd.rc",432u,1001,1001,118u}, /* radio:radio */
 {"/dev/ttyUSB2","/vendor/etc/ueventd.rc",432u,1001,1001,119u}, /* radio:radio */
 {"/dev/ttyUSB3","/vendor/etc/ueventd.rc",432u,1001,1001,120u}, /* radio:radio */
 {"/dev/ttyUSB4","/vendor/etc/ueventd.rc",432u,1001,1001,121u}, /* radio:radio */
 {"/dev/log/ksystem","/vendor/etc/ueventd.rc",384u,0,1007,125u}, /* root:log */
 {"/dev/ccci*","/vendor/etc/ueventd.rc",432u,1001,1001,127u}, /* radio:radio */
 {"/dev/ttyC*","/vendor/etc/ueventd.rc",432u,1001,1001,128u}, /* radio:radio */
 {"/dev/eemcs*","/vendor/etc/ueventd.rc",432u,1001,1001,132u}, /* radio:radio */
 {"/dev/emd*","/vendor/etc/ueventd.rc",432u,1001,1001,133u}, /* radio:radio */
 {"/dev/ccci_pcm_rx","/vendor/etc/ueventd.rc",432u,1041,1041,134u}, /* audio:audio */
 {"/dev/ccci_pcm_tx","/vendor/etc/ueventd.rc",432u,1041,1041,135u}, /* audio:audio */
 {"/dev/ccci_aud","/vendor/etc/ueventd.rc",432u,1041,1041,136u}, /* audio:audio */
 {"/dev/ccci2_aud","/vendor/etc/ueventd.rc",432u,1041,1041,137u}, /* audio:audio */
 {"/dev/ccci3_aud","/vendor/etc/ueventd.rc",432u,1041,1041,138u}, /* audio:audio */
 {"/dev/ccci_raw_audio","/vendor/etc/ueventd.rc",432u,1041,1041,139u}, /* audio:audio */
 {"/dev/ccci3_raw_audio","/vendor/etc/ueventd.rc",432u,1041,1041,140u}, /* audio:audio */
 {"/dev/eemcs_aud","/vendor/etc/ueventd.rc",432u,1041,1041,141u}, /* audio:audio */
 {"/dev/irtx","/vendor/etc/ueventd.rc",432u,1000,1000,142u}, /* system:system */
 {"/dev/lirc*","/vendor/etc/ueventd.rc",432u,0,1000,143u}, /* root:system */
 {"/dev/ir-learning","/vendor/etc/ueventd.rc",432u,1000,1000,144u}, /* system:system */
 {"/dev/pvrsrvkm","/vendor/etc/ueventd.rc",438u,0,0,146u}, /* root:root */
 {"/dev/pvr_sync","/vendor/etc/ueventd.rc",438u,0,0,147u}, /* root:root */
 {"/dev/mtgpio","/vendor/etc/ueventd.rc",384u,1001,0,164u}, /* radio:root */
 {"/dev/ttySDIO*","/vendor/etc/ueventd.rc",432u,1001,1015,168u}, /* radio:sdcard_rw */
 {"/dev/ttyRB*","/vendor/etc/ueventd.rc",432u,1001,1001,169u}, /* radio:radio */
 {"/dev/mali0","/vendor/etc/ueventd.rc",438u,1000,1003,172u}, /* system:graphics */
 {"/dev/ion","/vendor/etc/ueventd.rc",438u,1000,1003,175u}, /* system:graphics */
 {"/dev/dma_heap/mtk_mm","/vendor/etc/ueventd.rc",292u,1000,1000,180u}, /* system:system */
 {"/dev/dma_heap/mtk_mm-uncached","/vendor/etc/ueventd.rc",292u,1000,1000,181u}, /* system:system */
 {"/dev/dma_heap/mtk_svp_page-uncached","/vendor/etc/ueventd.rc",292u,1000,1000,182u}, /* system:system */
 {"/dev/dma_heap/mtk_prot_page-uncached","/vendor/etc/ueventd.rc",292u,1000,1000,183u}, /* system:system */
 {"/dev/dma_heap/mtk_svp_region","/vendor/etc/ueventd.rc",292u,1000,1000,184u}, /* system:system */
 {"/dev/dma_heap/mtk_svp_region-aligned","/vendor/etc/ueventd.rc",292u,1000,1000,185u}, /* system:system */
 {"/dev/dma_heap/mtk_prot_region","/vendor/etc/ueventd.rc",292u,1000,1000,186u}, /* system:system */
 {"/dev/dma_heap/mtk_prot_region-aligned","/vendor/etc/ueventd.rc",292u,1000,1000,187u}, /* system:system */
 {"/dev/dma_heap/mtk_2d_fr_region","/vendor/etc/ueventd.rc",292u,1000,1000,188u}, /* system:system */
 {"/dev/dma_heap/mtk_2d_fr_region-aligned","/vendor/etc/ueventd.rc",292u,1000,1000,189u}, /* system:system */
 {"/dev/dma_heap/mtk_wfd_region","/vendor/etc/ueventd.rc",292u,1000,1000,190u}, /* system:system */
 {"/dev/dma_heap/mtk_wfd_region-aligned","/vendor/etc/ueventd.rc",292u,1000,1000,191u}, /* system:system */
 {"/dev/dma_heap/mtk_wfd_page-uncached","/vendor/etc/ueventd.rc",292u,1000,1000,192u}, /* system:system */
 {"/dev/touch","/vendor/etc/ueventd.rc",432u,0,1000,195u}, /* root:system */
 {"/dev/hotknot","/vendor/etc/ueventd.rc",432u,0,1000,196u}, /* root:system */
 {"/dev/RT_Monitor","/vendor/etc/ueventd.rc",432u,1000,1000,199u}, /* system:system */
 {"/dev/kick_powerkey","/vendor/etc/ueventd.rc",432u,1000,1000,200u}, /* system:system */
 {"/dev/ttyACM0","/vendor/etc/ueventd.rc",432u,1001,1001,203u}, /* radio:radio */
 {"/dev/access-kregistry","/vendor/etc/ueventd.rc",432u,1000,1000,206u}, /* system:system */
 {"/dev/video*","/vendor/etc/ueventd.rc",432u,1013,1000,209u}, /* media:system */
 {"/dev/vcu","/vendor/etc/ueventd.rc",432u,1013,1000,212u}, /* media:system */
 {"/dev/hidraw0","/vendor/etc/ueventd.rc",432u,1000,1000,215u}, /* system:system */
};
const size_t tb_dev_policy_count = 163u;
const struct sys_policy tb_sys_policy[] = {
 {"/sys/devices/platform/trusty.*","trusty_version","/system/etc/ueventd.rc",288u,0,1007,83u},
 {"/sys/devices/virtual/input/input*","enable","/system/etc/ueventd.rc",432u,0,1004,84u},
 {"/sys/devices/virtual/input/input*","poll_delay","/system/etc/ueventd.rc",432u,0,1004,85u},
 {"/sys/devices/virtual/usb_composite/*","enable","/system/etc/ueventd.rc",436u,0,1000,86u},
 {"/sys/devices/system/cpu/cpu*","cpufreq/scaling_max_freq","/system/etc/ueventd.rc",436u,1000,1000,87u},
 {"/sys/devices/system/cpu/cpu*","cpufreq/scaling_min_freq","/system/etc/ueventd.rc",436u,1000,1000,88u},
 {"/sys/devices/virtual/misc/uhid/*/leds/*","brightness","/system/etc/ueventd.rc",436u,1000,1000,89u},
 {"/sys/devices/virtual/misc/uhid/*/leds/*","multi_intensity","/system/etc/ueventd.rc",436u,1000,1000,90u},
 {"/sys/devices/virtual/misc/uhid/*/input/*/*","brightness","/system/etc/ueventd.rc",438u,1000,1000,93u},
 {"/sys/devices/platform/soc/11201000.usb0/11200000.xhci0/usb2/2-1/2-1:1.0/*/input/*/*","brightness","/system/etc/ueventd.rc",438u,1000,1000,95u},
 {"/sys/devices/platform/mt_usb/musb-hdrc/usb1/1-1/1-1:1.0/*/input/*/*","brightness","/system/etc/ueventd.rc",438u,1000,1000,96u},
 {"/sys/devices/platform/11201000.usb0/11200000.xhci0/usb1/1-1/1-1:1.0/*/input/*/*","brightness","/system/etc/ueventd.rc",438u,1000,1000,97u},
 {"/sys/kernel/ccci","modem_info","/vendor/etc/ueventd.rc",420u,1001,1001,129u},
 {"/sys/kernel/ccci","md1_postfix","/vendor/etc/ueventd.rc",420u,1001,1001,130u},
 {"/sys/kernel/ccci","md2_postfix","/vendor/etc/ueventd.rc",420u,1001,1001,131u},
 {"/sys/devices/platform/leds-mt65xx/leds/green","delay_on","/vendor/etc/ueventd.rc",436u,1000,1000,150u},
 {"/sys/devices/platform/leds-mt65xx/leds/green","delay_off","/vendor/etc/ueventd.rc",436u,1000,1000,151u},
 {"/sys/devices/platform/leds-mt65xx/leds/red","delay_on","/vendor/etc/ueventd.rc",436u,1000,1000,152u},
 {"/sys/devices/platform/leds-mt65xx/leds/red","delay_off","/vendor/etc/ueventd.rc",436u,1000,1000,153u},
 {"/sys/devices/platform/leds-mt65xx/leds/blue","delay_on","/vendor/etc/ueventd.rc",436u,1000,1000,154u},
 {"/sys/devices/platform/leds-mt65xx/leds/blue","delay_off","/vendor/etc/ueventd.rc",436u,1000,1000,155u},
 {"/sys/class/leds/green","delay_on","/vendor/etc/ueventd.rc",436u,1000,1000,156u},
 {"/sys/class/leds/green","delay_off","/vendor/etc/ueventd.rc",436u,1000,1000,157u},
 {"/sys/class/leds/red","delay_on","/vendor/etc/ueventd.rc",436u,1000,1000,158u},
 {"/sys/class/leds/red","delay_off","/vendor/etc/ueventd.rc",436u,1000,1000,159u},
 {"/sys/class/leds/blue","delay_on","/vendor/etc/ueventd.rc",436u,1000,1000,160u},
 {"/sys/class/leds/blue","delay_off","/vendor/etc/ueventd.rc",436u,1000,1000,161u},
};
const size_t tb_sys_policy_count = 27u;
const struct subsystem_policy tb_subsystem_policy[] = {
 {"graphics","/dev/graphics"},
 {"drm","/dev/dri"},
 {"input","/dev/input"},
 {"sound","/dev/snd"},
 {"dma_heap","/dev/dma_heap"},
 {"vfio","/dev/vfio"},
};
const size_t tb_subsystem_policy_count = 6u;
const char *tb_firmware_dirs[] = {
 "/etc/firmware/",
 "/odm/firmware/",
 "/vendor/firmware/",
 "/firmware/image/",
};
const size_t tb_firmware_dir_count = 4u;
const int tb_socket_rcvbuf = 16777216;
