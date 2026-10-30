"""Independent byte-level fixtures for the native BF16 package repacker."""
import hashlib
import json
import os
from pathlib import Path
import resource
import struct
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
sha = lambda data: hashlib.sha256(data).hexdigest()

class NativePreparation(unittest.TestCase):
    def fixture(self, root):
        package, resources = root / 'package', root / 'resources'
        package.mkdir(); resources.mkdir()
        # Include BF16 subnormals, signed zero, infinities and NaN payload bits.
        raw = [0x0001, 0x8001, 0x0000, 0x8000, 0x3f80, 0xbf80, 0x7f80, 0xff80, 0x7fc3, 0x0080, 0x3e80, 0x4000]
        source = b'SAFETENSORS' + struct.pack('<12H', *raw) + b'end'
        (package / 'model.safetensors').write_bytes(source)
        header = b'GGUF' + b'\0' * 28
        constant = b'constant tensor'
        (resources / 'constants.bin').write_bytes(constant)
        outputs = []
        for i, name in enumerate(('decoder.gguf', 'encoder.gguf')):
            prefix = 'decoder' if i == 0 else 'encoder'
            (resources / (prefix + '.header')).write_bytes(header)
            if i == 0:
                # Two experts, each original matrix [in=2,out=3].
                order = [expert * 6 + row * 3 + col for expert in range(2) for col in range(3) for row in range(2)]
                payload = struct.pack('<12H', *(raw[x] for x in order))
                entry = dict(kind='transpose_bf16', source_shape=[2, 2, 3], source_offset=11, source_bytes=24)
            else:
                payload = struct.pack('<12I', *(x << 16 for x in raw))
                entry = dict(kind='bf16_to_f32', source_offset=11, source_bytes=24)
            entry.update(offset=len(header), bytes=len(payload))
            expected = header + payload
            expected += b'\0' * (-len(expected) % 32)
            permuted = struct.pack('<12H', *(raw[:3] + raw[6:9] + raw[3:6] + raw[9:12]))
            tensors = [entry, dict(kind='permute_bf16', source_shape=[4,3], heads=1,
                                  source_offset=11, source_bytes=24, offset=len(expected), bytes=len(permuted))]
            expected += permuted
            expected += b'\0' * (-len(expected) % 32)
            tensors.append(dict(kind='copy', source_offset=len(source)-3, source_bytes=3, offset=len(expected), bytes=3))
            expected += b'end'
            expected += b'\0' * (-len(expected) % 32)
            tensors.append(dict(kind='constant', offset=len(expected), bytes=len(constant), constant_offset=0))
            expected += constant
            expected += b'\0' * (-len(expected) % 32)
            outputs.append(dict(name=name, bytes=len(expected), sha256=sha(expected), header_sha256=sha(header), tensors=tensors))
            (root / name).write_bytes(expected)
        recipe = dict(version=1, source_bytes=len(source), source_sha256=sha(source),
                      constants_sha256=sha(constant), outputs=outputs)
        serialized = (json.dumps(recipe) + '\n').encode(); (resources / 'recipe.json').write_bytes(serialized)
        return package, resources, sha(serialized), recipe

    def invoke(self, package, resources, digest, *extra, **kwargs):
        return subprocess.run([BINARY, str(package), str(resources), digest, *map(str, extra)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs)

    def test_exact_bits_transpose_cast_and_padding(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); package, resources, digest, _ = self.fixture(root)
            result = self.invoke(package, resources, digest)
            self.assertEqual(result.returncode, 0, result.stderr)
            for name in ('decoder.gguf', 'encoder.gguf'):
                self.assertEqual((package / name).read_bytes(), (root / name).read_bytes())
                self.assertEqual((package / name).stat().st_mode & 0o777, 0o600)
            self.assertFalse(list(package.glob('*.writing')))

    def test_corrupt_source_and_resource(self):
        for corrupted in ('model.safetensors', 'decoder.header', 'constants.bin', 'recipe.json'):
            with self.subTest(corrupted=corrupted), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); package, resources, digest, _ = self.fixture(root)
                path = package / corrupted if corrupted == 'model.safetensors' else resources / corrupted
                data = bytearray(path.read_bytes()); data[0] ^= 1; path.write_bytes(data)
                self.assertNotEqual(self.invoke(package, resources, digest).returncode, 0)
                self.assertFalse(list(package.glob('*.gguf*')))

    def test_verified_descriptor_survives_path_replacement(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); package, resources, digest, recipe = self.fixture(root)
            self.assertEqual(sha((package / 'model.safetensors').read_bytes()), recipe['source_sha256'])
            result = self.invoke(package, resources, digest, 'verified-replaced')
            self.assertEqual(result.returncode, 0, result.stderr)
            for name in ('decoder.gguf', 'encoder.gguf'):
                self.assertEqual((package / name).read_bytes(), (root / name).read_bytes())

    def test_symlink_and_exclusive_output(self):
        for item in ('model.safetensors', 'decoder.gguf.writing', 'decoder.gguf'):
            with self.subTest(item=item), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); package, resources, digest, _ = self.fixture(root)
                outside = root / 'outside'; outside.write_bytes((package / 'model.safetensors').read_bytes() if item == 'model.safetensors' else b'untouched')
                path = package / item
                if path.exists(): path.unlink()
                expected = outside.read_bytes()
                path.symlink_to(outside)
                self.assertNotEqual(self.invoke(package, resources, digest).returncode, 0)
                self.assertEqual(outside.read_bytes(), expected)
                self.assertTrue(path.is_symlink())

    def test_cancellation_through_every_phase(self):
        completed = 0
        for calls in range(1, 120):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); package, resources, digest, _ = self.fixture(root)
                result = self.invoke(package, resources, digest, calls)
                if result.returncode:
                    self.assertIn(b'cancelled', result.stderr)
                    self.assertFalse(list(package.glob('*.gguf*')))
                else:
                    completed += 1
        self.assertGreater(completed, 0)

    def test_invalid_extent_and_overflow(self):
        for change in (dict(offset=31), dict(bytes=2**64 - 1), dict(source_offset=2**64 - 1), dict(source_shape=[2, 2**63, 3])):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); package, resources, _, recipe = self.fixture(root)
                recipe['outputs'][0]['tensors'][0].update(change)
                serialized = json.dumps(recipe).encode(); (resources / 'recipe.json').write_bytes(serialized)
                self.assertNotEqual(self.invoke(package, resources, sha(serialized)).returncode, 0)
                self.assertFalse(list(package.glob('*.gguf*')))

    def test_short_write_cleanup(self):
        def limit():
            import signal
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (40, 40))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); package, resources, digest, _ = self.fixture(root)
            result = self.invoke(package, resources, digest, preexec_fn=limit)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(list(package.glob('*.gguf*')))

if __name__ == '__main__':
    unittest.main()
