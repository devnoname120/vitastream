"""Validate the compiled compatibility selectors and plugin imports against ELF firmware.

Firmware files stay outside the repository. This checks artifacts, not a device.
"""

import argparse
import ctypes
import hashlib
import json
import struct
import subprocess
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
VERSIONS = ('3.60', '3.61', '3.63', '3.65', '3.67', '3.68',
            '3.69', '3.70', '3.71', '3.72', '3.73', '3.74')
MODULES = ('udcd', 'audio', 'dmacmgr')


class Firmware:
    def __init__(self, path, payload=None):
        self.path = path
        self.data = path.read_bytes() if payload is None else payload
        if self.data[:7] != b'\x7fELF\x01\x01\x01':
            raise ValueError(f'Not a decrypted ARM ELF: {path}')
        phoff = struct.unpack_from('<I', self.data, 28)[0]
        size, count = struct.unpack_from('<HH', self.data, 42)
        self.segments = [struct.unpack_from('<8I', self.data, phoff + i * size)
                         for i in range(count)]
        entry = struct.unpack_from('<I', self.data, 24)[0]
        self.info = self.segment(entry >> 30)[entry & 0x3fffffff:]
        self.nid = struct.unpack_from('<I', self.info, 0x34)[0]
        self.name = self.info[4:31].split(b'\0')[0].decode()
        self.text = self.segment(0)
        self.code_hash = hashlib.sha256(self.text[:entry & 0x3fffffff]).hexdigest()
        self.exports = {}
        start, end = struct.unpack_from('<II', self.info, 0x24)
        while start < end:
            size = self.text[start]
            functions = struct.unpack_from('<H', self.text, start + 6)[0]
            if size != 32:
                raise ValueError(f'Unexpected export record: {path}, {size}')
            library, _, nids, entries = struct.unpack_from('<4I', self.text, start + 16)
            for i in range(functions):
                nid = struct.unpack('<I', self.read(nids + 4 * i, 4))[0]
                address = struct.unpack('<I', self.read(entries + 4 * i, 4))[0]
                self.exports[library, nid] = address
            start += size
        assert start == end

    def segment(self, index):
        _, offset, _, _, size, *_ = self.segments[index]
        return self.data[offset:offset + size]

    def read(self, address, size):
        for _, offset, base, _, length, *_ in self.segments:
            if base <= address and address + size <= base + length:
                return self.data[offset + address - base:offset + address - base + size]
        raise ValueError(f'Unmapped address in {self.path}: {address:#x}')


def module_path(kd, name):
    choices = (kd / 'bootimage' / f'{name}.skprx.elf',
               kd / 'bootimage' / f'{name}.skprx', kd / f'{name}.elf')
    return next((p for p in choices if p.is_file()), None)


def load_module(kd, name):
    path = module_path(kd, name)
    if path:
        return Firmware(path)
    # Some decrypted trees retain the bootimage carrier without unpacking its
    # embedded ELFs. Read the bounded file table; leave the archive unchanged.
    carrier = kd / 'bootimage.elf'
    if not carrier.is_file():
        return None
    boot = Firmware(carrier)
    assert boot.name == 'SceKernelBootimage', carrier
    data, base = boot.text, boot.segments[0][2]
    table = struct.unpack_from('<I', data, 0xCC)[0] - base
    assert 0 <= table <= len(data) - 8
    count = struct.unpack_from('<I', data, table)[0]
    assert count < 256 and table + 8 + count * 12 <= len(data)
    for i in range(count):
        name_va, file_va, size = struct.unpack_from('<III', data, table + 8 + i * 12)
        name_offset, file_offset = name_va - base, file_va - base
        assert 0 <= name_offset < len(data)
        assert 0 <= file_offset <= len(data) and size <= len(data) - file_offset
        filename = data[name_offset:data.index(b'\0', name_offset)].decode()
        if filename.rsplit('/', 1)[-1] == name + '.skprx':
            return Firmware(carrier, data[file_offset:file_offset + size])
    return None


def selectors(directory):
    source = directory / 'selectors.c'
    source.write_text('''#include "firmware_profiles.h"
int check(int module, uint32_t nid, const unsigned char *text,
          size_t text_size, size_t data_size) {
  return firmware_module_matches(module, nid, text, text_size, data_size);
}
''')
    library = directory / 'selectors.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(REPO / 'include'), str(source), '-o', str(library)], check=True)
    dll = ctypes.CDLL(str(library))
    dll.check.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_void_p,
                          ctypes.c_size_t, ctypes.c_size_t]
    dll.check.restype = ctypes.c_int
    return dll


def linked_imports(path):
    data = path.read_bytes()
    offset = struct.unpack_from('<I', data, 32)[0]
    size, count, names_index = struct.unpack_from('<HHH', data, 46)
    sections = [struct.unpack_from('<10I', data, offset + i * size) for i in range(count)]
    names = data[sections[names_index][4]:sections[names_index][4] + sections[names_index][5]]
    result = []
    for section in sections:
        name = names[section[0]:].split(b'\0')[0].decode()
        if name.startswith('.vitalink.fstubs.'):
            for record in range(section[4], section[4] + section[5], 16):
                _, library, function, _ = struct.unpack_from('<4I', data, record)
                result.append((name.rsplit('.', 1)[-1], library, function))
    assert result, f'No Vita import records: {path}'
    return result


def verify(kd, dll, builds, hardware, version):
    modules = {}
    exports = {}
    for kind, name in enumerate(MODULES):
        firmware = load_module(kd, name)
        expected = ('SceUdcd', 'SceAudio', 'SceKernelDmacMgr')[kind]
        assert firmware.name == expected, (firmware.path, firmware.name)
        memory = ctypes.create_string_buffer(firmware.text)
        data_size = firmware.segments[1][5]
        assert dll.check(kind, firmware.nid, memory, len(firmware.text), data_size), (
            hardware, version, name, hex(firmware.nid))
        # A known identity must not bypass bounds or changed code.
        assert not dll.check(kind, firmware.nid, memory, 1, data_size)
        assert not dll.check(kind, firmware.nid, memory, len(firmware.text), 1)
        for nid in (0, 0xFFFFFFFF, 0x12345678):
            assert not dll.check(kind, nid, memory, len(firmware.text), data_size)
        context = (0x128C, 0x35C4, 0x5F0)[kind]
        altered = bytearray(firmware.text)
        altered[context] ^= 1
        assert not dll.check(kind, firmware.nid, ctypes.create_string_buffer(bytes(altered)),
                             len(altered), data_size)
        if kind == 1:
            altered = bytearray(firmware.text)
            altered[0x7FD8] ^= 1
            assert not dll.check(kind, firmware.nid, ctypes.create_string_buffer(bytes(altered)),
                                 len(altered), data_size)
        modules[name] = {'nid': f'0x{firmware.nid:08X}',
                         'sha256': hashlib.sha256(firmware.data).hexdigest(),
                         'code_sha256': firmware.code_hash,
                         'text_size': len(firmware.text), 'data_size': data_size}
        exports.update(firmware.exports)
    for name in ('sysmem', 'threadmgr', 'modulemgr', 'display', 'lowio',
                 'iofilemgr', 'oled', 'lcd'):
        firmware = load_module(kd, name)
        if firmware:
            exports.update(firmware.exports)
    imports_verified = {}
    for label, imports in builds.items():
        checked = 0
        for name, library, function in imports:
            if name in ('taihenForKernel', 'taihenModuleUtils'):
                continue
            assert (library, function) in exports, (
                hardware, version, label, name, hex(library), hex(function))
            checked += 1
        imports_verified[label] = checked
    return {'hardware': hardware, 'firmware': version, 'modules': modules,
            'imports_verified': imports_verified, 'device_tested': False}


def run(args, dll):
    builds = {p.stem: linked_imports(p) for p in args.plugin_elf}
    records, unavailable = [], []
    for hardware in ('official', 'testkit', 'devkit'):
        for version in VERSIONS:
            folder = version if hardware == 'official' else version + '0.011'
            kd = args.firmware_root / hardware / folder / 'fs_dec/os0/kd'
            missing = [name for name in MODULES if load_module(kd, name) is None]
            if missing:
                unavailable.append({'hardware': hardware, 'firmware': version,
                                    'missing_modules': missing})
                continue
            records.append(verify(kd, dll, builds, hardware, version))
    for version in VERSIONS:
        peers = [r for r in records if r['firmware'] == version]
        assert any(r['hardware'] == 'official' for r in peers), ('missing retail', version)
        for module in MODULES:
            assert len({r['modules'][module]['sha256'] for r in peers}) == 1, (
                version, module, 'hardware variants differ')
    assert len({r['modules']['audio']['code_sha256'] for r in records}) == 1
    assert len({r['modules']['udcd']['code_sha256'] for r in records}) == 2
    assert len({r['modules']['dmacmgr']['code_sha256'] for r in records}) == 2
    additional = []
    for root in args.additional_root:
        record = verify(root / 'fs_dec/os0/kd', dll, builds,
                        'additional_tool_build', root.name)
        record['source'] = root.name
        additional.append(record)
    known_hashes = {name: {r['modules'][name]['code_sha256'] for r in records}
                    for name in MODULES}
    for record in additional:
        for name in MODULES:
            assert record['modules'][name]['code_sha256'] in known_hashes[name], (
                record['source'], name, 'different tool executable code')
    source_hashes = {name: hashlib.sha256((REPO / name).read_bytes()).hexdigest()
                     for name in ('src/main.c', 'src/audio.c', 'src/firmware.c',
                                  'include/firmware.h', 'include/firmware_profiles.h')}
    return {'verified': records, 'unavailable': unavailable,
            'additional_tool_builds': additional,
            'source_sha256': source_hashes,
            'plugin_elf_sha256': {p.stem: hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in args.plugin_elf}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware-root', type=Path, required=True)
    parser.add_argument('--plugin-elf', type=Path, action='append', required=True)
    parser.add_argument('--additional-root', type=Path, action='append', default=[])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        result = run(args, selectors(Path(directory)))
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(result["verified"])} firmware/hardware pairs verified; '
          f'{len(result["additional_tool_builds"])} additional tool builds verified; '
          f'{len(result["unavailable"])} unavailable. No device test performed.')
