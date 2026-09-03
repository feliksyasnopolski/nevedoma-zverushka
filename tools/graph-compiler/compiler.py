#!/usr/bin/env python3
"""Compile the complete mutable Android init corpus into an ordered IR graph."""
from __future__ import annotations
import argparse, hashlib, json
from pathlib import Path
from init_parser import InitParser

POLICY_ONLY_BUILTINS = {
    'bootchart', 'enter_default_mount_ns', 'exec_start', 'init_user0',
    'installkey', 'load_exports', 'loglevel', 'mark_post_data',
    'perform_apex_config', 'remount_userdata', 'verity_update_state',
}

def load_config(path):
    config = json.loads(path.read_text())
    config['_path'] = path
    return config

def early_modules(config):
    manifest = config['_path'].parents[2] / config['early_module_manifest']
    data = json.loads(manifest.read_text())
    return set(data['sets'][config.get('early_module_set', 'H')])

def apply_native_exclusions(ir):
    """Remove only reviewed Android framework commands owned by the host."""
    exclusions = []
    reviewed_memcg = {
        ('chown', ('root', 'system', '/dev/memcg/memory.pressure_level')),
        ('chmod', ('0040', '/dev/memcg/memory.pressure_level')),
        ('mkdir', ('/dev/memcg/apps/', '0755', 'system', 'system')),
        ('mkdir', ('/dev/memcg/system', '0550', 'system', 'system')),
    }
    reviewed_apex_lifecycle = {
        ('exec_start', ('apexd-bootstrap',)),
        ('perform_apex_config', ('--bootstrap',)),
        ('mkdir', ('/metadata/apex', '0700', 'root', 'system')),
        ('mkdir', ('/metadata/apex/sessions', '0700', 'root', 'system')),
        ('restorecon_recursive', ('/metadata/apex',)),
        ('enter_default_mount_ns', ()),
        ('mkdir', ('/data/apex', '0755', 'root', 'system', 'encryption=None')),
        ('mkdir', ('/data/apex/active', '0755', 'root', 'system')),
        ('mkdir', ('/data/apex/backup', '0700', 'root', 'system')),
        ('mkdir', ('/data/apex/decompressed', '0755', 'root', 'system', 'encryption=Require')),
        ('mkdir', ('/data/apex/hashtree', '0700', 'root', 'system')),
        ('mkdir', ('/data/apex/sessions', '0700', 'root', 'system')),
        ('mkdir', ('/data/apex/ota_reserved', '0700', 'root', 'system', 'encryption=Require')),
        ('setprop', ('apexd.status', '')),
        ('wait_for_prop', ('apexd.status', 'activated')),
        ('perform_apex_config', ()),
        ('exec_start', ('apexd-snapshotde',)),
    }
    reviewed_network_policy = {
        ('write', ('/proc/sys/net/netfilter/nf_conntrack_helper', '1')),
    }
    reviewed_update_verifier = {
        ('exec_start', ('update_verifier',)),
    }
    reviewed_framework_boot = {
        ('/system/etc/init/hw/init.rc', 'exec_start', ('aconfigd-platform-init',)),
        ('/system/etc/init/hw/init.rc', 'mark_post_data', ()),
        ('/system/etc/init/hw/init.rc', 'installkey', ('/data',)),
        ('/system/etc/init/hw/init.rc', 'bootchart', ('start',)),
        ('/system/etc/init/hw/init.rc', 'exec_start', ('aconfigd-mainline-init',)),
        ('/system/etc/init/hw/init.rc', 'exec_start', ('derive_sdk',)),
        ('/system/etc/init/hw/init.rc', 'init_user0', ()),
        ('/system/etc/init/hw/init.rc', 'exec_start', ('derive_classpath',)),
        ('/system/etc/init/hw/init.rc', 'load_exports', ('/data/system/environ/classpath',)),
        ('/system/etc/init/hw/init.rc', 'exec_start', ('art_boot',)),
        ('/system/etc/init/hw/init.rc', 'exec_start', ('apexd-snapshotde',)),
        ('/system/etc/init/hw/init.rc', 'verity_update_state', ()),
        ('/system/etc/init/hw/init.rc', 'loglevel', ('${sys.init_log_level}',)),
        ('/system/etc/init/hw/init.rc', 'bootchart', ('stop',)),
        ('/system/etc/init/hw/init.rc', 'remount_userdata', ()),
        ('/system/etc/init/hw/init.boringssl.zygote64_32.rc', 'exec_start', ('boringssl_self_test32',)),
        ('/system/etc/init/hw/init.boringssl.zygote64_32.rc', 'exec_start', ('boringssl_self_test64',)),
        ('/system/etc/init/hw/init.boringssl.zygote64_32.rc', 'exec_start', ('boringssl_self_test_apex32',)),
        ('/system/etc/init/hw/init.boringssl.zygote64_32.rc', 'exec_start', ('boringssl_self_test_apex64',)),
        ('/vendor/etc/boringssl_self_test.zygote64_32.rc', 'exec_start', ('boringssl_self_test32_vendor',)),
        ('/vendor/etc/boringssl_self_test.zygote64_32.rc', 'exec_start', ('boringssl_self_test64_vendor',)),
    }
    for action in ir['actions']:
        kept = []
        for command in action['commands']:
            is_acct_uid = (command['builtin'] == 'mkdir' and
                           command['arguments'][:1] == ['/acct/uid'] and
                           command['source_file'] == '/system/etc/init/hw/init.rc' and
                           command['source_line'] == 28)
            is_memcg = (command['source_file'] == '/system/etc/init/hw/init.rc' and
                        (command['builtin'], tuple(command['arguments'])) in reviewed_memcg)
            is_apex_lifecycle = (command['source_file'] == '/system/etc/init/hw/init.rc' and
                                 (command['builtin'], tuple(command['arguments'])) in reviewed_apex_lifecycle)
            is_network_policy = (command['source_file'] == '/vendor/etc/init/networksetting.rc' and
                                 (command['builtin'], tuple(command['arguments'])) in reviewed_network_policy)
            is_update_verifier = (command['source_file'] == '/system/etc/init/hw/init.rc' and
                                 (command['builtin'], tuple(command['arguments'])) in reviewed_update_verifier)
            is_framework_boot = (command['source_file'], command['builtin'],
                                 tuple(command['arguments'])) in reviewed_framework_boot
            if is_acct_uid or is_memcg or is_apex_lifecycle or is_network_policy or is_update_verifier or is_framework_boot:
                if is_network_policy:
                    reason = ('NATIVE/HOST-OWNED: legacy Android automatic conntrack-helper assignment '
                              'knob; native Ubuntu/network-manager netfilter policy owns networking and '
                              'no retained BSP consumer requires this sysctl')
                elif is_update_verifier:
                    reason = ('NATIVE/HOST-OWNED: A/B OTA update-verifier lifecycle marks an Android boot '
                              'slot successful through the Android boot-control Binder service; native '
                              'Ubuntu has no retained OTA slot consumer or boot-control service')
                elif is_framework_boot:
                    reason = ('NATIVE/HOST-OWNED: Android framework boot helper or diagnostic/BoringSSL '
                              'self-test is outside the retained native vendor substrate')
                else:
                    reason = ('NATIVE/HOST-OWNED: Ubuntu/systemd owns process accounting and cgroups; '
                              'no retained BSP consumer requires Android /acct/uid' if is_acct_uid else
                              'NATIVE/HOST-OWNED: /dev/memcg is Android framework memory/cgroup plumbing; '
                              'no retained BSP consumer requires these exact paths' if is_memcg else
                              'NATIVE/HOST-OWNED: static /opt/tb330fu/android-apex compatibility view replaces '
                              'Android apexd activation, APEX session/OTA lifecycle, and device-mapper setup')
                exclusions.append({
                    'builtin': command['builtin'],
                    'arguments': command['arguments'],
                    'source_file': command['source_file'],
                    'source_line': command['source_line'],
                    'reason': reason,
                })
                continue
            kept.append(command)
        action['commands'] = kept
    return exclusions

def add_runtime_module_readiness_barrier(ir):
    """Serialize the first native provider consumer behind stock loader completion.

    Android init starts ``insmod_sh`` asynchronously.  Stock later actions
    are ordered by the existing ``vendor.all.modules.ready=1`` wait, while
    the native graph can reach the early cpufreq write sooner.  Preserve the
    asynchronous service start and compensate only at the first measured
    runtime-provider consumer.
    """
    candidates = []
    for action in ir['actions']:
        for index, command in enumerate(action['commands']):
            if (command['builtin'] == 'write'
                    and command['source_file'] == '/vendor/etc/init/hw/init.cgroup.rc'
                    and command['source_line'] == 15
                    and command['arguments'] == [
                        '/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq',
                        '1800000']):
                candidates.append((action, index))
    if not candidates:
        # Small compiler fixtures intentionally omit the board-specific
        # cgroup action.  The production corpus is checked separately and
        # must contain exactly one measured first consumer.
        return None
    if len(candidates) != 1:
        raise RuntimeError(
            'runtime readiness barrier requires exactly one stock first cpufreq consumer')
    action, index = candidates[0]
    existing = [command for action_item in ir['actions']
                for command in action_item['commands']
                if command['builtin'] == 'wait_for_prop'
                and command['arguments'] == ['vendor.all.modules.ready', '1']]
    if existing:
        raise RuntimeError('runtime readiness barrier already exists in source graph')
    barrier = {
        'builtin': 'wait_for_prop',
        'arguments': ['vendor.all.modules.ready', '1'],
        'wait_mode': 'unbounded',
        'expansions': [],
        'source_file': '/vendor/etc/init/hw/init.cgroup.rc',
        'source_line': 15,
        'provenance': 'NATIVE-SCHEDULING-COMPENSATION / STOCK-ASYNC-LOADER-READINESS',
        'derived_from': {
            'stock_loader': '/vendor/etc/init.insmod.mt8786.cfg',
            'loader_entry_order': ['modprobe|*', 'setprop|vendor.all.modules.ready'],
            'readiness_property': 'vendor.all.modules.ready',
            'readiness_value': '1',
            'wait_mode': 'unbounded',
            'consumer': 'write /sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq 1800000',
            'reason': 'native graph scheduling compensation; start insmod_sh remains asynchronous',
        },
    }
    action['commands'].insert(index, barrier)
    return {
        'builtin': barrier['builtin'],
        'arguments': barrier['arguments'],
        'inserted_before': {
            'source_file': action['commands'][index + 1]['source_file'],
            'source_line': action['commands'][index + 1]['source_line'],
            'arguments': action['commands'][index + 1]['arguments'],
        },
        'provenance': barrier['provenance'],
        'stock_cfg': '/vendor/etc/init.insmod.mt8786.cfg',
        'cfg_order': ['modprobe|*', 'setprop|vendor.all.modules.ready'],
        'wait_mode': 'unbounded',
        'async_service': 'insmod_sh',
    }

def reject_unreviewed_policy_builtins(ir):
    for action in ir['actions']:
        for command in action['commands']:
            if command['builtin'] in POLICY_ONLY_BUILTINS:
                ir['unsupported'].append({
                    'kind': 'builtin',
                    'builtin': command['builtin'],
                    'message': 'policy-only builtin has no reviewed native exclusion',
                    'context_kind': 'action',
                    'context_id': action.get('id'),
                    **{k: command[k] for k in ('source_file', 'source_line')},
                })
def compile_corpus(root: Path, out: Path, config, fail_unsupported=True):
    ir = InitParser(root, config.get('properties')).parse(); suppressed=[]; class_expansions=[]
    native_exclusions = apply_native_exclusions(ir)
    reject_unreviewed_policy_builtins(ir)
    runtime_readiness_barrier = add_runtime_module_readiness_barrier(ir)
    early = early_modules(config)
    for action in ir['actions']:
        kept=[]
        for command in action['commands']:
            if command['builtin'] in {'insmod','modprobe'} and command['arguments']:
                module = command['arguments'][0].rsplit('/',1)[-1]
                if module in early:
                    suppressed.append({'module':module,'source_file':command['source_file'],'source_line':command['source_line'],'reason':'already established by stock first-stage 154-module closure'})
                    continue
            kept.append(command)
        action['commands']=kept
    services_by_class={}
    for service in ir['services']:
        for option in service['options']:
            if option['name']=='class' and option['arguments']:
                services_by_class.setdefault(option['arguments'][0], []).append(service)
    for action in ir['actions']:
        expanded=[]
        for command in action['commands']:
            if command['builtin'] != 'class_start':
                expanded.append(command); continue
            if len(command['arguments']) != 1:
                ir['unsupported'].append({'kind':'builtin','builtin':'class_start','message':'requires one class',**{k:command[k] for k in ('source_file','source_line')}}); continue
            for service in services_by_class.get(command['arguments'][0],[]):
                if any(o['name']=='disabled' for o in service['options']): continue
                expanded.append({'builtin':'start','arguments':[service['name']], 'derived_from':{'builtin':'class_start','class':command['arguments'][0],**{k:command[k] for k in ('source_file','source_line')}}, **{k:command[k] for k in ('source_file','source_line')}})
                class_expansions.append({'class':command['arguments'][0],'service':service['name'],**{k:command[k] for k in ('source_file','source_line')}})
        action['commands']=expanded
    if fail_unsupported and ir['unsupported']:
        first=ir['unsupported'][0]; raise RuntimeError(f"unsupported init semantics at {first['source_file']}:{first['source_line']}: {first.get('builtin') or first.get('message') or first.get('text')}")
    graph={'metadata':{'format':'tb330fu-full-corpus-graph-v1','compiler':'tools/graph-compiler/compiler.py','source_corpus':root.name,'ordering':'AOSP r17 LoadBootScripts plus Parser::EndFile import order','subsystem_labels':'not used','property_semantics':'FIFO event queue; matching actions append in declaration order; wildcard requires a non-empty existing property; event/property predicates are ANDed; every setprop notification is queued including same-value sets','dynamic_import_properties':config.get('properties', {})},'files':ir['files'],'imports':ir['imports'],'actions':ir['actions'],'services':ir['services'],'unsupported':ir['unsupported'],'transformations':{'early_module_suppressions':suppressed,'class_start_expansions':class_expansions,'native_exclusions':native_exclusions,'runtime_readiness_barrier':runtime_readiness_barrier}}
    graph['counts']={'files':len(ir['files']),'imports':len(ir['imports']),'dynamic_imports_resolved':sum(1 for x in ir['imports'] if '${' in x.get('raw_target','')),'actions':len(ir['actions']),'commands':sum(len(x['commands']) for x in ir['actions']),'services':len(ir['services']),'property_triggers':sum(len(x['trigger']['properties']) for x in ir['actions']),'unsupported':len(ir['unsupported']),'early_module_suppressions':len(suppressed),'class_start_expansions':len(class_expansions)}
    payload=json.dumps(graph,sort_keys=True,indent=2)+'\n'; graph['metadata']['sha256']=hashlib.sha256(payload.encode()).hexdigest()
    out.mkdir(parents=True,exist_ok=True); (out/'full-corpus-graph.json').write_text(json.dumps(graph,sort_keys=True,indent=2)+'\n'); return graph

def main():
    project = Path(__file__).resolve().parents[2]
    ap=argparse.ArgumentParser(); ap.add_argument('--corpus',type=Path,default=project/'vendor/init-corpus/mutable'); ap.add_argument('--config',type=Path,default=project/'config/graph/compiler.json'); ap.add_argument('--out',type=Path,default=project/'generated/full-corpus'); ap.add_argument('--report-unsupported',action='store_true'); args=ap.parse_args()
    try: g=compile_corpus(args.corpus,args.out,load_config(args.config),fail_unsupported=not args.report_unsupported); print(json.dumps(g['counts'],sort_keys=True)); return 0
    except RuntimeError as exc: print(f'FULL-CORPUS-COMPILER: FAIL: {exc}'); return 2
if __name__=='__main__': raise SystemExit(main())
