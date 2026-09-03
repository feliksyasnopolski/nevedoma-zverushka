#!/usr/bin/env python3
"""Lossless-enough Android init RC parser for the full mutable corpus.

The parser deliberately records unsupported constructs instead of dropping
them. Compilation may then fail with their exact source provenance.
"""
from __future__ import annotations
import shlex
import re
from pathlib import Path

SUPPORTED_BUILTINS = {
    'bootchart_init','chmod','chown','class_start','class_stop','copy','exec',
    'exec_background','export','fchown','fchmod','insmod','load_persist_props',
    'init_user0','installkey','load_exports','loglevel','mark_post_data','mkdir','mount',
    'parse_apex_configs','perform_apex_config','remount_all','remount_userdata','restorecon','restorecon_recursive',
    'rm','rmdir','setenv','setprop','start','stop','symlink','sysclktz','trigger','exec_start',
    'umount','umount_all','verity_update_state','wait','wait_for_prop','write','setrlimit',
    'bootchart','enter_default_mount_ns'
}

SERVICE_OPTIONS = {
    'capabilities','class','console','critical','disabled','enter_namespace','file',
    'group','ioprio','keycodes','namespace','no_console','onrestart','oom_score_adjust',
    'oneshot','override','priority','reboot_on_failure','restart_period','rlimit',
    'seclabel','shutdown','socket','stdio_to_kmsg','task_profiles','user','updatable',
    # r17 service-parser options retained as metadata by the native IR.
    'interface','writepid','setenv','gentle_kill','timeout_period',
}

def loc(path, line): return {'source_file': path, 'source_line': line}

def words(text, path, line):
    try: return shlex.split(text, comments=True, posix=True)
    except ValueError as exc: raise ValueError(f'{path}:{line}: {exc}') from exc

def logical_lines(lines):
    """Join Android-init physical continuations before tokenization.

    Init's tokenizer accepts backslash-continued stanzas and quoted strings
    spanning physical lines. A line-at-a-time shell lexer misclassifies both
    forms as syntax errors and, worse, loses the following service/action
    context. Keep the first physical line as provenance.
    """
    pending = None
    start = 0
    for number, raw in enumerate(lines, 1):
        if pending is None:
            pending, start = raw, number
        else:
            pending += '\n' + raw
        stripped = pending.rstrip()
        if stripped.endswith('\\'):
            pending = stripped[:-1] + ' '
            continue
        try:
            shlex.split(pending, comments=True, posix=True)
        except ValueError as exc:
            if 'No closing quotation' in str(exc):
                continue
        yield start, pending
        pending = None
    if pending is not None:
        yield start, pending

def trigger(text, path, line):
    terms = [x.strip() for x in text.split('&&')]
    event = None; props = []
    for term in terms:
        if term.startswith('property:'):
            body = term[9:]
            if '=' not in body: raise ValueError(f'{path}:{line}: malformed property trigger')
            name, expected = body.split('=', 1)
            props.append({'name': name, 'expected': expected, 'wildcard': expected == '*', **loc(path,line)})
        elif event is None and term:
            event = term
        else:
            raise ValueError(f'{path}:{line}: unsupported trigger term {term!r}')
    return {'event': event, 'properties': props, **loc(path,line)}

class InitParser:
    def __init__(self, root: Path, properties=None):
        self.root = root; self.files = []; self.actions = []; self.services = []
        self.imports = []; self.unsupported = []; self._seen = set(); self._order = 0
        self.properties = {
            'ro.hardware': 'mt6768',
            'ro.zygote': 'zygote64_32',
            'ro.vendor.rc': '/vendor/etc/init/hw/',
        }
        if properties:
            self.properties.update(properties)

    def path_for(self, value):
        value = value[1:] if value.startswith('/') else value
        return self.root / value

    def expand_import(self, value):
        """Expand imports using the parse-time property environment.

        This mirrors init's ExpandProps contract for the import path: a
        missing property without a default is an expansion error; a ``:-``
        default is used only when the property is absent or empty.
        """
        unresolved = []
        pattern = re.compile(r'\$\{([^}:]+)(?::-(.*?))?\}')
        def replace(match):
            name, default = match.group(1), match.group(2)
            value = self.properties.get(name, '')
            if value:
                return value
            if default is not None:
                return default
            unresolved.append(name)
            return match.group(0)
        return pattern.sub(replace, value), unresolved

    def import_targets(self, value):
        """Return exact ParseConfig targets; AOSP does not glob imports."""
        path = self.path_for(value)
        if path.is_file():
            return [path]
        if path.is_dir():
            return sorted((x for x in path.iterdir() if x.is_file()), key=lambda x: str(x))
        return []

    def parse_file(self, path: Path):
        rel = '/' + path.relative_to(self.root).as_posix()
        self.files.append({'path': rel, 'parse_order': len(self.files)})
        lines = path.read_text(errors='strict').splitlines()
        current = None
        pending_imports = []
        for number, raw in logical_lines(lines):
            line = raw.strip()
            if not line or line.startswith('#'): continue
            try: tok = words(line, rel, number)
            except ValueError as exc: self.unsupported.append({'kind':'syntax','message':str(exc),**loc(rel,number)}); continue
            if not tok: continue
            if tok[0] == 'import':
                if len(tok) != 2:
                    self.unsupported.append({'kind':'import','message':'import requires one path',**loc(rel,number)}); continue
                import_name, unresolved = self.expand_import(tok[1])
                if unresolved:
                    self.unsupported.append({'kind':'import', 'raw_target':tok[1],
                                             'expanded_target':import_name,
                                             'unresolved_properties':unresolved,
                                             'message':f"could not expand import: {', '.join(unresolved)}",
                                             **loc(rel,number)})
                    continue
                pending_imports.append((tok[1], import_name, number))
                continue
            if tok[0] == 'on':
                try: current = {'kind':'action','id':f'action-{self._order:06d}','trigger':trigger(' '.join(tok[1:]),rel,number),'commands':[],**loc(rel,number),'declaration_order':self._order}; self._order += 1
                except ValueError as exc: self.unsupported.append({'kind':'trigger','message':str(exc),'context_kind':'action',**loc(rel,number)}); current = None
                if current is not None: self.actions.append(current)
                continue
            if tok[0] == 'service':
                if len(tok) < 3: self.unsupported.append({'kind':'service','message':'service requires name and executable',**loc(rel,number)}); current=None; continue
                current = {'kind':'service','name':tok[1],'executable':tok[2],'argv':tok[2:], 'options':[], 'onrestart':[], **loc(rel,number),'declaration_order':self._order}; self._order += 1; self.services.append(current); continue
            if tok[0] in SERVICE_OPTIONS and current and current['kind']=='service':
                if tok[0] == 'onrestart': current['onrestart'].append({'commands':tok[1:],**loc(rel,number)})
                else: current['options'].append({'name':tok[0],'arguments':tok[1:],**loc(rel,number)})
                continue
            if current and current['kind']=='action':
                command = {'builtin':tok[0],'arguments':tok[1:],**loc(rel,number)}
                if tok[0] not in SUPPORTED_BUILTINS:
                    self.unsupported.append({'kind':'builtin','builtin':tok[0],'context_kind':'action','context_id':current['id'],'trigger':current['trigger'],**loc(rel,number)})
                current['commands'].append(command); continue
            self.unsupported.append({'kind':'top-level','text':line,
                                     'context_kind':current['kind'] if current else None,
                                     'context_name':current.get('name') if current and current['kind']=='service' else None,
                                     **loc(rel,number)})

        # ImportParser::EndFile() performs imports after all sections in this
        # file have been closed. It retains source order and does not dedupe.
        for raw_name, import_name, import_line in pending_imports:
            targets = self.import_targets(import_name)
            if not targets:
                self.unsupported.append({'kind':'import', 'raw_target':raw_name,
                                         'expanded_target':import_name,
                                         'message':f'import target not found: {raw_name}',
                                         **loc(rel,import_line)})
            for target in targets:
                target_path = Path(target)
                target_rel = '/' + target_path.relative_to(self.root).as_posix()
                self.imports.append({'from':rel,'to':target_rel,'raw_target':raw_name,
                                     'expanded_target':import_name,**loc(rel,import_line)})
                self.parse_file(target_path)

    def parse(self):
        # Android 15 r17 LoadBootScripts() normal-boot order. The root
        # init.rc itself is parsed first; each later directory is scanned by
        # Parser::ParseConfigDir(). Imported files may consequently be parsed
        # more than once, exactly as in AOSP.
        roots = [self.path_for('/system/etc/init/hw/init.rc')]
        dirs = ['/system/etc/init', '/system_ext/etc/init', '/vendor/etc/init',
                '/odm/etc/init', '/product/etc/init']
        for path in roots:
            if path.is_file(): self.parse_file(path)
        for directory in dirs:
            path = self.path_for(directory)
            if not path.is_dir(): continue
            for child in sorted((x for x in path.iterdir() if x.is_file()), key=lambda x: str(x)):
                self.parse_file(child)
        return {'format':'tb330fu-android-init-ir-v1','files':self.files,'imports':self.imports,'actions':self.actions,'services':self.services,'unsupported':self.unsupported}
