#!/usr/bin/env python3
"""Create small audited resources for deterministic native reference repacking."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('converted', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--nemo', required=True, type=Path)
    parser.add_argument('--llama', required=True, type=Path)
    args = parser.parse_args()
    sys.path.insert(0, str(args.nemo.resolve()))
    sys.path.insert(0, str(args.llama.resolve() / 'gguf-py'))
    import gguf
    import numpy as np
    from conversion.pee import remap
    source = args.reference / 'model.safetensors'
    report = json.loads((args.converted / 'conversion.json').read_text())
    config = json.loads((args.reference / 'llm_backbone/config.json').read_text())
    with source.open('rb') as file:
        size = int.from_bytes(file.read(8), 'little')
        header = json.loads(file.read(size))
    names = gguf.get_tensor_name_map(gguf.MODEL_ARCH.NEMOTRON_H_MOE, 52)
    mapping = {}
    for name, info in header.items():
        if name.startswith('llm.mtp.') or '_extra_state' in name:
            continue
        target = None
        if name.startswith('perception.'):
            short = name[len('perception.'):]
            target = remap(short)
            if short == 'preprocessor.featurizer.fb':
                target = 'preprocessor.fb'
        elif name.startswith('llm.'):
            special = {'llm.model.embed_tokens.weight': 'backbone.embeddings.weight',
                       'llm.model.norm.weight': 'backbone.norm_f.weight', 'llm.lm_head.weight': 'lm_head.weight'}
            canonical = special.get(name, name.replace('llm.model.layers.', 'backbone.layers.'))
            canonical = canonical.replace('._fp32_params.', '.').replace('mixer.dt_bias', 'mixer.dt.bias')
            leaf = canonical.rsplit('.', 1)[-1]
            if leaf in ('gate_and_up_projs', 'down_projs'):
                layer = int(canonical.split('.')[2])
                projection = 'up_proj' if leaf == 'gate_and_up_projs' else 'down_proj'
                canonical = f'model.layers.{layer}.mlp.experts.{projection}.weight'
            target = names.get_name(canonical, try_suffixes=('.weight', '.bias'))
        if target:
            if target in mapping:
                raise ValueError(f'Duplicate native source mapping: {target}')
            mapping[target] = (name, info)
    if args.destination.exists():
        raise ValueError('Recipe destination already exists')
    args.destination.mkdir(parents=True)
    constants = bytearray()
    outputs = []
    for prefix in ('decoder', 'encoder'):
        path = args.converted / (prefix + '.gguf')
        reader = gguf.GGUFReader(path)
        with path.open('rb') as file:
            serialized = file.read(reader.data_offset)
        (args.destination / (prefix + '.header')).write_bytes(serialized)
        recipes = []
        for tensor in reader.tensors:
            entry = dict(name=tensor.name, offset=int(tensor.data_offset), bytes=int(tensor.data.nbytes))
            candidate = mapping.get(tensor.name)
            matched = False
            if candidate:
                source_name, info = candidate
                begin, end = info['data_offsets']
                entry.update(source_name=source_name, source_offset=8 + size + begin,
                             source_bytes=end - begin, source_dtype=info['dtype'])
                if '.ffn_up_exps.weight' in tensor.name or '.ffn_down_exps.weight' in tensor.name:
                    # prepare_nemotron_bf16 already compared every expert's original bits.
                    if info['dtype'] != 'BF16' or tensor.tensor_type != gguf.GGMLQuantizationType.BF16:
                        raise ValueError('Invalid expert recipe dtype')
                    entry.update(kind='transpose_bf16', source_shape=info['shape'])
                    matched = True
                else:
                    if info['dtype'] not in ('BF16', 'F32'):
                        raise ValueError(f'Unexpected native source dtype: {source_name}')
                    raw = np.memmap(source, mode='r', dtype='<u2' if info['dtype'] == 'BF16' else '<u4',
                                    offset=8 + size + begin, shape=tuple(info['shape']))
                    if info['dtype'] == 'BF16' and tensor.tensor_type == gguf.GGMLQuantizationType.F32:
                        expected = raw.astype('<u4') << 16
                        actual = tensor.data.view('<u4').reshape(-1)
                        matched = np.array_equal(expected.reshape(-1), actual)
                        entry['kind'] = 'bf16_to_f32'
                    else:
                        actual = tensor.data.view(raw.dtype).reshape(-1)
                        matched = np.array_equal(raw.reshape(-1), actual)
                        entry['kind'] = 'copy'
                    if not matched and info['dtype'] == 'BF16' and tensor.tensor_type == gguf.GGMLQuantizationType.BF16 and tensor.name.endswith(('.attn_q.weight', '.attn_k.weight')):
                        heads = config['num_attention_heads'] if '.attn_q.' in tensor.name else config['num_key_value_heads']
                        shape = info['shape']
                        expected = raw.reshape(heads, 2, shape[0] // heads // 2, shape[1]).swapaxes(1, 2).reshape(-1)
                        matched = np.array_equal(expected, tensor.data.view('<u2').reshape(-1))
                        entry.update(kind='permute_bf16', heads=heads, source_shape=shape)
            if not matched:
                if tensor.data.nbytes > 1024 * 1024:
                    raise ValueError(f'No bounded native conversion for {tensor.name}')
                entry = dict(name=tensor.name, offset=int(tensor.data_offset), bytes=int(tensor.data.nbytes),
                             kind='constant', constant_offset=len(constants))
                constants.extend(tensor.data.tobytes())
            recipes.append(entry)
        outputs.append(dict(name=path.name, bytes=report['files'][path.name]['bytes'],
                            sha256=report['files'][path.name]['sha256'],
                            header_sha256=hashlib.sha256(serialized).hexdigest(), tensors=recipes))
    (args.destination / 'constants.bin').write_bytes(constants)
    recipe = dict(version=1, reference_revision=report['reference_revision'],
                  source_sha256=report['reference_sha256'], source_bytes=source.stat().st_size,
                  constants_sha256=hashlib.sha256(constants).hexdigest(), outputs=outputs,
                  converter_files=report['converter_files'])
    (args.destination / 'recipe.json').write_text(json.dumps(recipe, indent=2) + '\n')
    if 'conversion_environment' in report:
        provenance = dict(recipe_sha256=hashlib.sha256((args.destination / 'recipe.json').read_bytes()).hexdigest(),
                          reference_sha256=report['reference_sha256'], environment=report['conversion_environment'])
        (args.destination / 'BUILD-ENVIRONMENT.json').write_text(json.dumps(provenance, indent=2) + '\n')
    shutil.copyfile(args.reference / 'LICENSE', args.destination / 'LICENSE')
    print(f'Native recipe: {len(constants)} constant bytes; {args.destination}', flush=True)

if __name__ == '__main__':
    main()
