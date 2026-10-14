"""Converts Google's speech embedding model (as the ONNX file openWakeWord
ships, embedding_model.onnx) to models/speech_embedding.bin for
src/embedding.cpp.

  https://github.com/dscripka/openWakeWord/releases/download/v0.5.1/embedding_model.onnx

The model is Google's speech_embedding (TFHub), Apache-2.0.

File format (little endian): "VHTE", uint32 layer count, then per conv layer:
int32 out, in, kh, kw, has_bias, then out*in*kh*kw float weights (OIHW) and,
if has_bias, out float biases.

Usage: python export_embedding.py embedding_model.onnx speech_embedding.bin
"""
import struct
import sys

import numpy as np

from onnx_reader import load

nodes, tensors = load(sys.argv[1])
convs = [n for n in nodes if n['op'] == 'Conv']
with open(sys.argv[2], 'wb') as f:
    f.write(b'VHTE')
    f.write(struct.pack('<I', len(convs)))
    for n in convs:
        w = tensors[n['inputs'][1]].astype('<f4')
        b = tensors[n['inputs'][2]].astype('<f4') if len(n['inputs']) > 2 else None
        o, i, kh, kw = w.shape
        f.write(struct.pack('<5i', o, i, kh, kw, b is not None))
        f.write(w.tobytes())
        if b is not None:
            f.write(b.tobytes())
        print(f'{n["outputs"][0]}: {o}x{i}x{kh}x{kw}{" + bias" if b is not None else ""}')
assert np.allclose(tensors['const_fold_opt__287'], -0.4), 'unexpected activation floor'
