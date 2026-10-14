"""Minimal ONNX reader (no dependencies besides numpy): just enough protobuf
parsing to get a model's nodes and tensors."""
import struct

import numpy as np


def _varint(b, i):
    r = s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            return r, i


def _fields(b):
    i = 0
    while i < len(b):
        key, i = _varint(b, i)
        f, t = key >> 3, key & 7
        if t == 0:
            v, i = _varint(b, i)
        elif t == 1:
            v = b[i:i + 8]
            i += 8
        elif t == 5:
            v = b[i:i + 4]
            i += 4
        elif t == 2:
            n, i = _varint(b, i)
            v = b[i:i + n]
            i += n
        else:
            raise ValueError(f'unsupported wire type {t}')
        yield f, t, v


def _ints(b):
    out, i = [], 0
    while i < len(b):
        v, i = _varint(b, i)
        out.append(v)
    return out


def _tensor(b):
    dims, name, dtype, raw, floats, int64s = [], '', 0, None, [], []
    for f, t, v in _fields(b):
        if f == 1:
            dims += _ints(v) if t == 2 else [v]
        elif f == 2:
            dtype = v
        elif f == 4:
            floats += list(np.frombuffer(v, '<f4')) if t == 2 else [struct.unpack('<f', v)[0]]
        elif f == 7:
            int64s += _ints(v) if t == 2 else [v]
        elif f == 8:
            name = v.decode()
        elif f == 9:
            raw = v
    if raw is not None:
        arr = np.frombuffer(raw, {1: '<f4', 6: '<i4', 7: '<i8'}[dtype])
    elif floats:
        arr = np.array(floats, np.float32)
    else:
        arr = np.array(int64s, np.int64)
    return name, (arr.reshape(dims) if dims else arr)


def load(path):
    """Returns (nodes, tensors): nodes as dicts with op/inputs/outputs, tensors
    (initializers and Constant node values) by name."""
    data = open(path, 'rb').read()
    nodes, tensors = [], {}
    for f, _, v in _fields(data):
        if f != 7:  # ModelProto.graph
            continue
        for gf, _, gv in _fields(v):
            if gf == 5:  # GraphProto.initializer
                name, arr = _tensor(gv)
                tensors[name] = arr
            elif gf == 1:  # GraphProto.node
                node = {'op': None, 'inputs': [], 'outputs': []}
                attrs = []
                for nf, _, nv in _fields(gv):
                    if nf == 1:
                        node['inputs'].append(nv.decode())
                    elif nf == 2:
                        node['outputs'].append(nv.decode())
                    elif nf == 4:
                        node['op'] = nv.decode()
                    elif nf == 5:
                        attrs.append(nv)
                if node['op'] == 'Constant':
                    for a in attrs:
                        for af, _, av in _fields(a):
                            if af == 5:  # AttributeProto.t
                                tensors[node['outputs'][0]] = _tensor(av)[1]
                nodes.append(node)
    return nodes, tensors
