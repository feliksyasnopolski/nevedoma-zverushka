import json, tempfile, unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).parents[1]))
from init_parser import InitParser
from compiler import compile_corpus

class FullCorpusTests(unittest.TestCase):
    def config(self):
        return {'properties': {'ro.hardware': 'mt6768', 'ro.zygote': 'zygote64_32', 'ro.vendor.rc': '/vendor/etc/init/hw/'}, '_path': Path(__file__).parents[3] / 'config' / 'graph' / 'compiler.json', 'early_module_manifest': 'manifests/stock-early-module-closure.json', 'early_module_set': 'H'}

    def make(self, text, imported=None):
        root=Path(tempfile.mkdtemp()); (root/'system/etc/init/hw').mkdir(parents=True)
        (root/'system/etc/init/hw/init.rc').write_text(text)
        if imported is not None: (root/'system/etc/init/imported.rc').write_text(imported)
        return root

    def test_import_order_and_declaration_order(self):
        root=self.make('import /system/etc/init/imported.rc\non boot\n  write /tmp/main 1\n', 'on boot\n  write /tmp/imported 1\n')
        ir=InitParser(root).parse()
        self.assertEqual([x['path'] for x in ir['files']], ['/system/etc/init/hw/init.rc','/system/etc/init/imported.rc','/system/etc/init/imported.rc'])
        self.assertEqual([x['source_file'] for x in ir['actions']], ['/system/etc/init/hw/init.rc','/system/etc/init/imported.rc','/system/etc/init/imported.rc'])

    def test_dynamic_import_expansion_and_provenance(self):
        root = self.make('import ${ro.hardware}.rc\nimport ${ro.vendor.rc}child.rc\n')
        (root/'mt6768.rc').write_text('on boot\n  write /tmp/hardware 1\n')
        (root/'vendor/etc/init').mkdir(parents=True)
        (root/'vendor/etc/init/child.rc').write_text('on boot\n  write /tmp/vendor 1\n')
        ir = InitParser(root, {'ro.hardware':'mt6768', 'ro.vendor.rc':'/vendor/etc/init/'}).parse()
        self.assertEqual([x['to'] for x in ir['imports']], ['/mt6768.rc','/vendor/etc/init/child.rc'])
        self.assertEqual(ir['imports'][0]['raw_target'], '${ro.hardware}.rc')
        self.assertEqual(ir['imports'][0]['expanded_target'], 'mt6768.rc')

    def test_missing_dynamic_property_is_fail_closed(self):
        ir = InitParser(self.make('import ${missing}.rc\n'), {}).parse()
        self.assertEqual(ir['unsupported'][0]['kind'], 'import')
        self.assertEqual(ir['unsupported'][0]['unresolved_properties'], ['missing'])

    def test_missing_static_import_is_recorded(self):
        ir = InitParser(self.make('import absent.rc\n')).parse()
        self.assertIn('import target not found: absent.rc', ir['unsupported'][0]['message'])

    def test_import_does_not_glob_wildcards(self):
        root = self.make('import child*.rc\n')
        (root/'child-one.rc').write_text('on boot\n  write /tmp/one 1\n')
        ir = InitParser(root).parse()
        self.assertEqual(ir['imports'], [])
        self.assertIn('import target not found: child*.rc', ir['unsupported'][0]['message'])

    def test_same_trigger_property_wildcard_and_compound(self):
        root=self.make('on boot\n  setprop foo bar\non property:foo=* && boot\n  write /tmp/a 1\non property:missing=*\n  write /tmp/b 1\n')
        ir=InitParser(root).parse(); acts=ir['actions']
        self.assertEqual([a['declaration_order'] for a in acts], [0,1,2])
        self.assertTrue(acts[1]['trigger']['properties'][0]['wildcard'])
        self.assertEqual((acts[1]['trigger']['event'], acts[1]['trigger']['properties'][0]['name']), ('boot','foo'))
        self.assertTrue(acts[2]['trigger']['properties'][0]['wildcard'])

    def test_class_start_expands_enabled_services_in_global_service_order(self):
        root=self.make('on boot\n  class_start hal\nservice first /bin/first\n  class hal\nservice disabled /bin/disabled\n  class hal\n  disabled\nservice second /bin/second\n  class hal\n')
        g=compile_corpus(root,root/'out',self.config(),fail_unsupported=True)
        self.assertEqual([c['arguments'][0] for c in g['actions'][0]['commands']], ['first','second'])
        self.assertEqual(g['transformations']['class_start_expansions'][0]['service'],'first')

    def test_unsupported_builtin_is_reported_and_not_dropped(self):
        root=self.make('on boot\n  reboot bootloader\n')
        ir=InitParser(root).parse()
        self.assertEqual(ir['actions'][0]['commands'][0]['builtin'],'reboot')
        self.assertEqual(ir['unsupported'][0]['builtin'],'reboot')
        with self.assertRaisesRegex(RuntimeError,'reboot'):
            compile_corpus(root,root/'out',self.config())

    def test_stock_multiline_continuations_and_unindented_sections(self):
        root=self.make('service demo /bin/demo \\\n        --flag=value\n  class hal\n  reboot_on_failure reboot,demo-failed\n\n'
                       'on boot\nwrite /tmp/value "line one\nline two"\n')
        ir=InitParser(root).parse()
        self.assertEqual(ir['services'][0]['argv'], ['/bin/demo','--flag=value'])
        self.assertEqual([o['name'] for o in ir['services'][0]['options']], ['class','reboot_on_failure'])
        self.assertEqual(ir['actions'][0]['commands'][0]['arguments'], ['/tmp/value','line one\nline two'])

    def test_numeric_memlock_setrlimit_is_retained_in_order(self):
        root = self.make('on boot\n'
                         '  write /tmp/before 1\n'
                         '  setrlimit 8 -1 -1\n'
                         '  write /tmp/after 1\n')
        graph = compile_corpus(root, root/'out', self.config(), fail_unsupported=True)
        commands = graph['actions'][0]['commands']
        self.assertEqual([c['builtin'] for c in commands], ['write', 'setrlimit', 'write'])
        self.assertEqual(commands[1]['arguments'], ['8', '-1', '-1'])

    def test_reviewed_acct_uid_exclusion_is_explicit_and_provenanced(self):
        root = self.make('on early-init\n  mkdir /acct/uid\n')
        action = InitParser(root).parse()['actions'][0]
        action['commands'][0]['source_file'] = '/system/etc/init/hw/init.rc'
        action['commands'][0]['source_line'] = 28
        from compiler import apply_native_exclusions
        ir = {'actions': [action]}
        exclusions = apply_native_exclusions(ir)
        self.assertEqual(ir['actions'][0]['commands'], [])
        self.assertEqual(exclusions[0]['arguments'], ['/acct/uid'])
        self.assertIn('NATIVE/HOST-OWNED', exclusions[0]['reason'])

    def test_runtime_readiness_barrier_is_single_and_precedes_first_cpufreq_write(self):
        from compiler import add_runtime_module_readiness_barrier
        consumer = self.cmd('write', '/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq', '1800000')
        consumer.update({'source_file': '/vendor/etc/init/hw/init.cgroup.rc', 'source_line': 15})
        ir = {'actions': [self.action('early', {'event': 'early-init', 'properties': []}, [
            self.cmd('setprop', 'vendor.all.modules.ready', '0'), consumer]) ]}
        record = add_runtime_module_readiness_barrier(ir)
        commands = ir['actions'][0]['commands']
        self.assertEqual([c['builtin'] for c in commands], ['setprop', 'wait_for_prop', 'write'])
        self.assertEqual(commands[1]['arguments'], ['vendor.all.modules.ready', '1'])
        self.assertEqual(sum(c['builtin'] == 'wait_for_prop' for c in commands), 1)
        self.assertEqual(record['provenance'], 'NATIVE-SCHEDULING-COMPENSATION / STOCK-ASYNC-LOADER-READINESS')
        self.assertEqual(commands[1]['derived_from']['loader_entry_order'], ['modprobe|*', 'setprop|vendor.all.modules.ready'])
        self.assertEqual(commands[1]['wait_mode'], 'unbounded')

    def test_nf_conntrack_helper_is_exact_host_owned_carve(self):
        from compiler import apply_native_exclusions
        command = self.cmd('write', '/proc/sys/net/netfilter/nf_conntrack_helper', '1')
        command.update({'source_file': '/vendor/etc/init/networksetting.rc', 'source_line': 17})
        ir = {'actions': [self.action('networksetting', {'event': 'early-init', 'properties': []}, [command])]}
        exclusions = apply_native_exclusions(ir)
        self.assertEqual(ir['actions'][0]['commands'], [])
        self.assertEqual(exclusions[0]['arguments'], ['/proc/sys/net/netfilter/nf_conntrack_helper', '1'])
        self.assertIn('legacy Android automatic conntrack-helper assignment knob', exclusions[0]['reason'])

    def test_reviewed_framework_exec_start_is_explicitly_excluded(self):
        from compiler import apply_native_exclusions
        command = self.cmd('exec_start', 'derive_sdk')
        command.update({'source_file': '/system/etc/init/hw/init.rc', 'source_line': 1011})
        ir = {'actions': [self.action('boot', {'event': 'post-fs-data', 'properties': []}, [command])]}
        exclusions = apply_native_exclusions(ir)
        self.assertEqual(ir['actions'][0]['commands'], [])
        self.assertEqual(exclusions[0]['builtin'], 'exec_start')
        self.assertIn('framework boot helper', exclusions[0]['reason'])

    def test_wait_for_prop_is_supported_and_provenanced(self):
        root = self.make('on boot\n  wait_for_prop vendor.ready 1\n')
        ir = InitParser(root).parse()
        self.assertEqual(ir['unsupported'], [])
        self.assertEqual(ir['actions'][0]['commands'][0]['builtin'], 'wait_for_prop')

    def test_unreviewed_exec_start_remains_fail_closed(self):
        root = self.make('on boot\n  exec_start future_service\n')
        with self.assertRaisesRegex(RuntimeError, 'exec_start'):
            compile_corpus(root, root/'out', self.config())

    def action(self, ident, trigger, commands):
        return {'id': ident, 'trigger': trigger, 'commands': commands}

    def cmd(self, builtin, *args):
        return {'builtin': builtin, 'arguments': list(args)}

    def prop(self, name, expected, wildcard=False):
        return {'name': name, 'expected': expected, 'wildcard': wildcard}

if __name__ == '__main__': unittest.main()
