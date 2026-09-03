#pragma once

// One reviewed name surface shared by parser support and the JSON executor.
// Handler ownership remains runtime-specific.
static constexpr const char* kIslandBuiltinNames[] = {
    "bootchart", "chmod", "chown", "class_reset", "class_restart", "class_start", "class_stop", "copy",
    "domainname", "enable", "enter_default_mount_ns", "exec", "exec_background", "exec_start", "export",
    "hostname", "ifup", "init_user0", "insmod", "installkey", "load_exports", "load_persist_props",
    "load_system_props", "loglevel", "mark_post_data", "mkdir", "mount", "mount_all", "perform_apex_config",
    "powerctl", "remount_userdata", "restart", "restorecon", "restorecon_recursive", "rm", "rmdir", "setprop",
    "setrlimit", "start", "stop", "swapon_all", "symlink", "sysclktz", "trigger", "umount", "umount_all",
    "update_linker_config", "verity_update_state", "wait", "wait_for_prop", "write", "switch_root",
    "copy_per_line", "interface_start", "interface_stop", "interface_restart"};
