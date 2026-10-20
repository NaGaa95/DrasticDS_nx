#!/usr/bin/env python3
"""Execute the supported core display ABI and native handoff tests on AArch64.

Requires pyelftools, unicorn and devkitA64. The user's core is read only;
modified images and the freestanding test ELF exist only in a temporary folder.
"""
from __future__ import annotations
import argparse
import io
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X30, UC_ARM64_REG_SP, UC_ARM64_REG_PC
from build_deko3d import validate_dksh

ROOT = Path(__file__).resolve().parents[1]
PAGE = 4096
STACK = 0x70000000
STOP = STACK + 0x10000


def map_elf(uc: Uc, elf: ELFFile, base=0) -> None:
    segments = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    low = min(s['p_vaddr'] for s in segments) & -PAGE
    high = (max(s['p_vaddr'] + s['p_memsz'] for s in segments) + PAGE - 1) & -PAGE
    uc.mem_map(base + low, high - low)
    for segment in segments:
        uc.mem_write(base + segment['p_vaddr'], segment.data())


def call(uc: Uc, address: int, *args: int) -> int:
    for i, value in enumerate(args):
        uc.reg_write(UC_ARM64_REG_X0 + i, value)
    uc.reg_write(UC_ARM64_REG_SP, STACK + 0xF000)
    uc.reg_write(UC_ARM64_REG_X30, STOP)
    uc.emu_start(address, STOP, count=20000000)
    assert uc.reg_read(UC_ARM64_REG_PC) == STOP, 'AArch64 test did not return'
    return uc.reg_read(UC_ARM64_REG_X0)


def core_abi(core: ELFFile) -> None:
    base, pool = 0x10000000, 0x60000000
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    map_elf(uc, core, base)
    uc.mem_map(STACK, 0x20000)
    uc.mem_map(pool, 0x300000)
    state = base + 0x3F2D1F8
    uploads, allocation_args, mutexes = [], [], []
    bound = 0
    plt = core.get_section_by_name('.plt')['sh_addr'] + 32
    syms = core.get_section_by_name('.dynsym')
    functions = {base + plt + i * 16: syms.get_symbol(r['r_info_sym']).name
                 for i, r in enumerate(core.get_section_by_name('.rela.plt').iter_relocations())}

    def intercept(emulator, address, _size, _user):
        nonlocal bound
        name = functions.get(address)
        if name is None:
            return
        regs = [emulator.reg_read(UC_ARM64_REG_X0 + i) for i in range(8)]
        if name == 'glBindTexture':
            bound = regs[1]
        elif name == 'glTexSubImage2D':
            pointer, = struct.unpack('<Q', emulator.mem_read(emulator.reg_read(UC_ARM64_REG_SP), 8))
            uploads.append((bound, regs[4], regs[5], regs[6], regs[7], pointer))
        elif name == 'posix_memalign':
            allocation_args.append(tuple(regs[1:3]))
            emulator.mem_write(regs[0], struct.pack('<Q', pool))
        elif name == 'memset':
            emulator.mem_write(regs[0], bytes([regs[1] & 255]) * regs[2])
        elif name.startswith('pthread_mutex_') or name.startswith('pthread_cond_'):
            mutexes.append((name, regs[0]))
        elif name != 'glDrawArrays':
            raise AssertionError(f'Unexpected core display dependency: {name}')
        emulator.reg_write(UC_ARM64_REG_X0, 0)
        emulator.reg_write(UC_ARM64_REG_PC, emulator.reg_read(UC_ARM64_REG_X30))

    uc.hook_add(UC_HOOK_CODE, intercept)
    # Run the real allocator after the single alignment-instruction patch.
    uc.mem_write(base + 0x1CC30, struct.pack('<I', 0x52820001))
    call(uc, base + 0x1CBEC)
    assert allocation_args == [(4096, 0x300000)]
    assert struct.unpack('<2Q', uc.mem_read(state, 16)) == (pool, pool + 0x180000)
    assert ('pthread_mutex_init', state + 0x98C) in mutexes
    assert ('pthread_cond_init', state + 0x9B4) in mutexes
    uc.mem_write(base + 0x1CB9C, struct.pack('<I', 0xD503201F))
    notifications = sum(name == 'pthread_cond_signal' for name, _ in mutexes)
    call(uc, base + 0x1CB5C, 0)
    call(uc, base + 0x1CB5C, 1)
    assert bytes(uc.mem_read(state + 0x988, 1)) == b'\x03'
    assert sum(name == 'pthread_cond_signal' for name, _ in mutexes) == notifications
    call(uc, base + 0x1CB14)
    assert sum(name == 'pthread_cond_signal' for name, _ in mutexes) == notifications + 1
    cases = 0
    for bits in (16, 32):
        for top in (0, 1):
            for bottom in (0, 1):
                for writer in (0, 1):
                    for swap in (0, 1):
                        uploads.clear()
                        uc.mem_write(state + 0x958, struct.pack('<4I', writer, bits,
                            0x8363 if bits == 16 else 0x1401,
                            0x1907 if bits == 16 else 0x1908))
                        uc.mem_write(state + 0x968, struct.pack('<2I', top, bottom))
                        uc.mem_write(state + 0x988, b'\x03')
                        call(uc, base + 0x19030, 0, 0, 101, 102, swap)
                        assert len(uploads) == 2
                        for texture, w, h, fmt, typ, pointer in uploads:
                            screen = swap if texture == 101 else (swap ^ 1)
                            resolution = (top, bottom)[screen]
                            assert (w, h) == (256 << resolution, 192 << resolution)
                            assert pointer == pool + (writer ^ 1) * 0x180000 + screen * 0xC0000
                            assert (fmt, typ) == ((0x1907, 0x8363) if bits == 16 else (0x1908, 0x1401))
                        cases += 1
    print(f'Core ABI: allocator, publish notifications, screen pointers, channel formats, mixed resolutions and swap verified ({cases} cases)')


def handoff(core: ELFFile, compiler: Path, libnx: Path, shaders: Path | None) -> None:
    with tempfile.TemporaryDirectory(prefix='drastic-native-test-') as folder:
        binary = Path(folder) / 'fixture.elf'
        subprocess.run([str(compiler), '-O2', '-ffreestanding', '-fno-builtin',
            '-fno-pic', '-fno-pie', '-nostdlib', '-I' + str(libnx / 'include'),
            '-Wl,-Ttext=0x100000,-e,bridge_test',
            str(ROOT / 'tools/tests/native_frame_fixture.c'), '-o', str(binary)], check=True)
        fixture = ELFFile(io.BytesIO(binary.read_bytes()))
        names = {s.name: s['st_value'] for s in fixture.get_section_by_name('.symtab').iter_symbols()}
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        map_elf(uc, fixture)
        uc.mem_map(STACK, 0x20000)
        for segment in core.iter_segments():
            if segment['p_type'] == 'PT_LOAD':
                uc.mem_write(names['module_image'] + segment['p_vaddr'], segment.data())
        line = call(uc, names['bridge_test'])
        assert line == 0, f'Native handoff assertion failed: native_frame_fixture.c:{line}'
        print('Native handoff: 4096 producer flips, DMA leases, mixed formats, snapshot races, dirty retention, detach and invalid profiles passed')
        if shaders:
            count = 0
            data_address = STACK + 0x20000
            uc.mem_map(data_address, 8 * 1024 * 1024)
            for file in shaders.rglob('*'):
                if not file.is_file() or file.suffix not in ('.dksh', '.bin') or ('_dk_' not in file.name and file.suffix != '.dksh'):
                    continue
                stage = 0 if '.vert.' in file.name or '_vert.' in file.name else 4
                data = file.read_bytes()
                validate_dksh(data, 'vert' if stage == 0 else 'frag')
                uc.mem_write(data_address, data)
                assert call(uc, names['dksh_check'], data_address, len(data), stage) == 1, file
                for offset, value in ((0, 0), (8, 0xFFFFFF00), (12, 0xFFFFFF00), (16, 0xFFFFFFF0), (20, 2)):
                    bad = bytearray(data)
                    struct.pack_into('<I', bad, offset, value)
                    uc.mem_write(data_address, bytes(bad))
                    assert call(uc, names['dksh_check'], data_address, len(data), stage) == 0
                uc.mem_write(data_address, data)
                assert call(uc, names['dksh_check'], data_address, len(data), stage ^ 4) == 0
                assert call(uc, names['dksh_check'], data_address, len(data)-1, stage) == 0
                count += 1
            assert count > 0, 'No native shaders found to validate'
            print(f'Native DKSH: {count} shader stages accepted; truncated, corrupted and wrong-stage blobs rejected by the AArch64 loader')


def package_shaders(nro: Path, output: Path) -> None:
    data = nro.read_bytes()
    expected_version = re.search(r'^APP_VERSION\s*:=\s*(\S+)',
        (ROOT / 'Makefile').read_text(), re.M).group(1)

    def nacp_version(blob: bytes) -> str:
        assert len(blob) >= 0x3070
        return blob[0x3060:0x3070].split(b'\0', 1)[0].decode('ascii')

    def nro_version(blob: bytes) -> str:
        asset, = struct.unpack_from('<I', blob, 0x18)
        assert blob[asset:asset+4] == b'ASET'
        offset, length = struct.unpack_from('<2Q', blob, asset+24)
        return nacp_version(blob[asset+offset:asset+offset+length])

    assert nro_version(data) == expected_version, 'Launcher NACP version is stale'
    assert data[0x10:0x14] == b'NRO0', 'Not an NRO'
    aset, = struct.unpack_from('<I', data, 0x18)
    assert data[aset:aset+4] == b'ASET', 'NRO has no assets'
    relative, size = struct.unpack_from('<2Q', data, aset + 40)
    romfs = memoryview(data)[aset+relative:aset+relative+size]
    header = struct.unpack_from('<10Q', romfs)
    assert header[0] == 0x50
    directories, files, content = header[3], header[7], header[9]
    entries: dict[str, bytes] = {}
    visited = set()

    def name(offset: int, length: int) -> str:
        value = bytes(romfs[offset:offset+length]).decode('utf-8')
        assert '/' not in value and '\\' not in value and value not in ('.', '..')
        return value

    def visit(offset: int, parent: str) -> None:
        assert offset not in visited, 'Cyclic RomFS directory'
        visited.add(offset)
        _parent, _sibling, child, file, _hash, length = struct.unpack_from('<6I', romfs, directories+offset)
        current = name(directories+offset+24, length)
        path = f'{parent}/{current}'.strip('/')
        while file != 0xFFFFFFFF:
            _p, sibling, start, length, _h, nl = struct.unpack_from('<2I2Q2I', romfs, files+file)
            filename = name(files+file+32, nl)
            assert content + start + length <= len(romfs)
            entries[f'{path}/{filename}'.strip('/')] = bytes(romfs[content+start:content+start+length])
            file = sibling
        while child != 0xFFFFFFFF:
            visit(child, path)
            child, = struct.unpack_from('<I', romfs, directories+child+4)

    visit(0, '')
    manifests = [path for path in entries if path.startswith('shaders/') and path.endswith('.dfx')]
    assert len(manifests) == 56
    native_count = 0
    for manifest in manifests:
        text = entries[manifest].decode('utf-8-sig')
        count = len(re.findall(r'<pass>', text))
        pack = manifest + '.nxdk/'
        marker = entries[pack + 'pack.info'].decode('utf-8')
        assert 'format=1\n' in marker and f'passes={count}\n' in marker
        for index in range(count):
            for stage in ('vert', 'frag'):
                validate_dksh(entries[f'{pack}pass{index}.{stage}.dksh'], stage)
                native_count += 1
    hosts = [blob for path, blob in entries.items() if path.endswith('/DrasticDS_nx.nro')]
    assert len(hosts) == 1 and b'Deko3D initialized:' in hosts[0]
    assert nro_version(hosts[0]) == expected_version, 'Embedded host NACP version is stale'
    assert nacp_version(entries['fwd/default.nacp']) == expected_version, 'Forwarder NACP version is stale'
    assert 'backgrounds/DS Logo.png' not in entries
    assert not any(path.startswith('backgrounds/licenses/ds-logo/') for path in entries)
    assert entries['backgrounds/licenses/pocketarch/LICENSE'] == (ROOT / 'third_party/backgrounds/pocketarch/LICENSE').read_bytes()
    assert entries['backgrounds/NOTICE.md'] == (ROOT / 'third_party/backgrounds/NOTICE.md').read_bytes()
    for name in ('Nintendo-DS.png', 'Nintendo-DS.png.layout'):
        assert entries['backgrounds/'+name.replace('Nintendo-DS','Nintendo DS')] == (ROOT / 'third_party/backgrounds/pngkey-ds' / name).read_bytes()
    for file in (ROOT / 'third_party/backgrounds/lnkd').glob('*.png*'):
        assert entries['backgrounds/lnkd/'+file.name] == file.read_bytes()
    assert b'LSFG Deko3D: preparing native shader cache' in hosts[0]
    assert not any(Path(path).name.lower() == 'lossless.dll' for path in entries)
    assert not any('bios' in Path(path).name.lower() or Path(path).name.lower().startswith('nds_firmware') for path in entries)
    for path, blob in entries.items():
        if not path.startswith('shaders/'):
            continue
        destination = output / Path(path).relative_to('shaders')
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(blob)
    print(f'Packaged NRO: native host with LSFG, {len(manifests)} shader packs / {native_count} native stages, no DLL or DS BIOS/firmware')


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--core', type=Path, required=True)
    p.add_argument('--compiler', type=Path, required=True)
    p.add_argument('--libnx', type=Path, required=True)
    p.add_argument('--shaders', type=Path)
    p.add_argument('--package', type=Path, help='also inspect the final launcher NRO and its bundled shader packs')
    p.add_argument('--host-compiler', type=Path, help='optional host clang++/g++ to exercise the real custom loader')
    args = p.parse_args()
    core = ELFFile(io.BytesIO(args.core.read_bytes()))
    core_abi(core)
    if args.package:
        with tempfile.TemporaryDirectory(prefix='drastic-package-test-') as folder:
            temporary = Path(folder)
            shaders = temporary / 'shaders'
            package_shaders(args.package, shaders)
            handoff(core, args.compiler, args.libnx, shaders)
            if args.host_compiler:
                executable = temporary / 'custom-test.exe'
                subprocess.run([str(args.host_compiler), '-std=c++20', '-O2',
                    str(ROOT / 'tools/tests/custom_deko_fixture.cpp'), '-o', str(executable)], check=True)
                environment = dict(os.environ)
                environment['PATH'] = str(args.host_compiler.resolve().parent) + os.pathsep + environment.get('PATH', '')
                subprocess.run([str(executable), str(shaders)], check=True, env=environment)
    else:
        handoff(core, args.compiler, args.libnx, args.shaders)


if __name__ == '__main__':
    main()
