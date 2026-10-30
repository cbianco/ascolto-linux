#!/usr/bin/env python3
"""Build-only reference conversion. Native inference never invokes Python.

Uses the pinned NVIDIA converter, leaves routed matrices BF16, omits unused MTP
and timestamp heads, and promotes perception weights to F32 without losing BF16
values. Requires torch, transformers==5.5.1, safetensors, numpy, pyyaml and
sentencepiece in a separate build environment. No weights are downloaded here.
"""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REVISION = 'a99e33f698257b8b69ca7834f19c73582eb1fa7e'
WEIGHTS_SHA256 = '03142dde5176c1d4df21ee829642745a1d670ea4d79e1a4684c64d53195acff2'
WEIGHTS_BYTES = 67496013734
NEMO_REVISION = '66d50d91b75a406becd95fbaf39d2c9c13e95d7c'
LLAMA_REVISION = 'bd4f514db14d87fded667787a7a963bfbaa98e89'

def verify_file(path, size, expected, algorithm='sha256'):
    if path.stat().st_size != size:
        raise ValueError(f'{path}: wrong size')
    with path.open('rb') as source:
        digest = hashlib.new(algorithm)
        if algorithm == 'sha1':
            digest.update(f'blob {size}\0'.encode())
        while block := source.read(8 * 1024 * 1024):
            digest.update(block)
    if digest.hexdigest() != expected:
        raise ValueError(f'{path}: integrity mismatch')

def validate_experts(source, output, llama):
    import numpy as np
    sys.path.insert(0, str(llama / 'gguf-py'))
    import gguf
    weights = source / 'model.safetensors'
    with weights.open('rb') as file:
        length = int.from_bytes(file.read(8), 'little')
        header = json.loads(file.read(length))
    reader = gguf.GGUFReader(output)
    checked = 0
    for tensor in reader.tensors:
        name = tensor.name
        if not name.endswith(('.ffn_up_exps.weight', '.ffn_down_exps.weight')):
            continue
        layer = int(name.split('.')[1])
        leaf = 'gate_and_up_projs' if '.ffn_up_exps.' in name else 'down_projs'
        original = header[f'llm.model.layers.{layer}.mixer.experts.{leaf}']
        if original['dtype'] != 'BF16' or tensor.tensor_type != gguf.GGMLQuantizationType.BF16:
            raise ValueError(f'{name}: routed dtype changed')
        shape = original['shape']
        raw = np.memmap(weights, mode='r', dtype='<u2', offset=8 + length + original['data_offsets'][0], shape=tuple(shape))
        converted = tensor.data.view('<u2').reshape(shape[0], shape[2], shape[1])
        for expert in range(shape[0]):
            if not np.array_equal(converted[expert], raw[expert].T):
                raise ValueError(f'{name}: BF16 payload differs at expert {expert}')
        checked += 1
        print(f'Verified exact BF16 values: {name}', flush=True)
    if checked != 46:
        raise ValueError(f'Expected 46 routed projection tensors, got {checked}')
    return checked

def validate_encoder(source, output, pee):
    import numpy as np
    import gguf
    weights = source / 'model.safetensors'
    with weights.open('rb') as file:
        length = int.from_bytes(file.read(8), 'little')
        header = json.loads(file.read(length))
    reader = gguf.GGUFReader(output)
    converted = {tensor.name: tensor for tensor in reader.tensors}
    checked = 0
    for name, info in header.items():
        if not name.startswith('perception.') or '_extra_state' in name:
            continue
        short = name[len('perception.'):]
        target = pee.remap(short)
        if short.endswith('preprocessor.featurizer.fb') or short == 'preprocessor.featurizer.fb':
            target = 'preprocessor.fb'
        if target is None:
            continue
        if info['dtype'] != 'BF16':
            raise ValueError(f'{name}: unexpected reference perception dtype')
        raw = np.memmap(weights, mode='r', dtype='<u2', offset=8 + length + info['data_offsets'][0], shape=tuple(info['shape']))
        expected_bits = raw.astype('<u4') << 16
        actual_bits = converted[target].data.view('<u4')
        if not np.array_equal(actual_bits.reshape(-1), expected_bits.reshape(-1)):
            raise ValueError(f'{target}: perception BF16 values were not preserved exactly in F32')
        checked += 1
    if checked < 100:
        raise ValueError('Reference encoder validation covered too few tensors')
    print(f'Verified exact BF16-to-F32 values: {checked} perception tensors', flush=True)
    return checked

def verify_converter(nemo, llama, parent):
    subprocess.run(['git', '-C', str(nemo), 'diff', '--exit-code', 'HEAD', '--',
                    'conversion', 'patches', 'cmake/llama_cpp.cmake'], check=True, stdout=subprocess.DEVNULL)
    hashes = {}
    with tempfile.TemporaryDirectory(prefix='ascolto-converter-check-', dir=parent) as tmp:
        expected = Path(tmp) / 'llama'
        subprocess.run(['cmake', f'-DSOURCE_DIR={nemo / "llama.cpp"}', f'-DPATCH_DIR={nemo / "patches"}',
                        f'-DDEST_DIR={expected}', '-P', str(nemo / 'cmake/llama_cpp.cmake')], check=True)
        paths = [Path('convert_hf_to_gguf.py')]
        for directory in ('conversion', 'gguf-py'):
            actual = {p.relative_to(llama) for p in (llama / directory).rglob('*.py')}
            reference = {p.relative_to(expected) for p in (expected / directory).rglob('*.py')}
            if actual != reference:
                raise ValueError('Executed converter file set differs from the pinned source')
            paths.extend(sorted(reference))
        for relative in paths:
            data = (llama / relative).read_bytes()
            if data != (expected / relative).read_bytes():
                raise ValueError(f'Executed converter differs from pinned source: {relative}')
            hashes[str(relative)] = hashlib.sha256(data).hexdigest()
    for path in (nemo / 'conversion').rglob('*.py'):
        hashes['nemo/' + str(path.relative_to(nemo))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return hashes

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='Verified NVIDIA checkpoint directory')
    parser.add_argument('destination', type=Path, help='New experimental native GGUF package directory')
    parser.add_argument('--nemo', required=True, type=Path, help='Pinned NeMo-Speech.cpp checkout')
    parser.add_argument('--llama', required=True, type=Path, help='Private patched llama.cpp source directory')
    args = parser.parse_args()
    source, output, nemo, llama = (path.resolve() for path in (args.source, args.destination, args.nemo, args.llama))
    revision = subprocess.check_output(['git', '-C', str(nemo), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != NEMO_REVISION:
        raise ValueError('Conversion requires the pinned NeMo source')
    submodule = subprocess.check_output(['git', '-C', str(nemo / 'llama.cpp'), 'rev-parse', 'HEAD'], text=True).strip()
    if submodule != LLAMA_REVISION or not (llama / 'conversion/nemotron.py').is_file():
        raise ValueError('Conversion requires the pinned patched llama converter')
    if output.exists():
        raise ValueError(f'{output}: destination already exists')
    converter_hashes = verify_converter(nemo, llama, output.parent)
    # The checkpoint remains alongside the converted files for reproducibility.
    if shutil.disk_usage(output.parent).free < 80 * 1024**3:
        raise ValueError('Conversion needs at least 80 GiB free beside the downloaded reference')
    print('Verifying original checkpoint SHA-256...', flush=True)
    verify_file(source / 'model.safetensors', WEIGHTS_BYTES, WEIGHTS_SHA256)
    verify_file(source / 'tokenizer.json', 17078144, 'd071004cb4450d3aa31f87aab979cd09ffc99c4e709e6cf72c324346cd948d0d')
    verify_file(source / 'config.json', 439079, 'b3e0b879564373bc346dd79215006db483e1f5f3', 'sha1')
    verify_file(source / 'tokenizer_config.json', 11827, '4769119ddddb089e5916ac95d48480360541b2b2', 'sha1')
    verify_file(source / 'llm_backbone/config.json', 2403, 'f2faa27b7fae5ff95c950d820e6cf493a3d12d87', 'sha1')
    verify_file(source / 'LICENSE', 2694, '6061ea30334268d5a2b1c7731392dcc4a3b321df', 'sha1')
    output.mkdir()
    work = output / '.conversion'
    work.mkdir()
    sys.path.insert(0, str(nemo))
    sys.path.insert(0, str(llama / 'gguf-py'))
    from conversion import pee
    from conversion.salm import stage, checkpoint_id
    from conversion.salm_encoder import write_encoder_file
    identity = checkpoint_id(source)
    stage(source, work / 'hf')
    env = dict(os.environ, TOKENIZERS_PARALLELISM='false', OMP_NUM_THREADS='4')
    subprocess.run([sys.executable, str(nemo / 'conversion/salm_decoder_hf.py'), str(llama), str(work / 'hf'),
        '--outfile', str(output / 'decoder.gguf'), '--outtype', 'bf16', '--no-mtp',
        '--model-name', 'Nemotron 3.5 Transcribe BF16 Reference',
        '--nemo-speech-key', f'nemo_speech.checkpoint_id={identity}',
        '--nemo-speech-key', 'nemo_speech.speech_model=encoder.gguf'], env=env, check=True)
    # Upstream unconditionally narrows the subpixel Conv1D to F16. The private
    # runtime supports its F32 im2col/matmul path, so preserve these weights too.
    import gguf
    pee._pick_dtype = lambda *_: (gguf.GGMLQuantizationType.F32, None)
    pee.convert(source, work / 'encoder.gguf', 'f32')
    write_encoder_file(work / 'encoder.gguf', None, output / 'encoder.gguf',
        name='Nemotron 3.5 Transcribe Encoder F32 Reference', checkpoint_id=identity)
    reader = gguf.GGUFReader(output / 'encoder.gguf')
    if any(t.tensor_type != gguf.GGMLQuantizationType.F32 for t in reader.tensors):
        raise ValueError('Reference encoder conversion narrowed a tensor')
    checked = validate_experts(source, output / 'decoder.gguf', llama)
    checked_encoder = validate_encoder(source, output / 'encoder.gguf', pee)
    shutil.copyfile(source / 'LICENSE', output / 'LICENSE')
    report = dict(reference_revision=REVISION, reference_sha256=WEIGHTS_SHA256,
        nemo_revision=NEMO_REVISION, llama_revision=LLAMA_REVISION,
        converter_files=converter_hashes,
        exact_bf16_expert_tensors=checked, exact_bf16_to_f32_perception_tensors=checked_encoder,
        encoder_dtype='F32', mtp=False, timestamps=False,
        inference='native C++', files={})
    report['conversion_environment'] = dict(python=sys.version.split()[0],
        packages={name: importlib.metadata.version(name) for name in
                  ('torch', 'transformers', 'safetensors', 'numpy', 'PyYAML', 'sentencepiece')})
    for name in ('decoder.gguf', 'encoder.gguf', 'LICENSE'):
        path = output / name
        with path.open('rb') as file:
            report['files'][name] = dict(bytes=path.stat().st_size, sha256=hashlib.file_digest(file, 'sha256').hexdigest())
    (output / 'conversion.json').write_text(json.dumps(report, indent=2) + '\n')
    shutil.rmtree(work)
    print(f'Verified experimental BF16 package: {output}', flush=True)

if __name__ == '__main__':
    main()
