"""Convert the SHA256-verified official realesr-general-x4v3.pth (BSD-3-Clause,
Real-ESRGAN v0.2.5.0) to ncnn .param/.bin without PyTorch.
Usage: python convert_texture_model.py <official .pth> <output directory> (NumPy required).

The graph is the one of the official realesr-animevideov3-x4.param (same SRVGGNetCompact, there num_conv=16), here num_conv=32:
conv3x3(3->64) PReLU, 32 x [conv3x3(64->64) PReLU], conv3x3(64->48), PixelShuffle(4), + nearest x4 of the input.
Weights are stored like the official .bin: per Convolution an fp16 tag 0x01306B47 + fp16 weights, then fp32 bias; PReLU fp32 slopes.
Writes the -x2 variant (bilinear 0.5 Interp appended = exact 2x2 box average; the bicubic 0.5 Interp of
realesr-animevideov3-x2.param returns garbage with this 32-conv graph in the 20220424 build) for 2x output from the same network.
"""
import pickle
import hashlib
import struct
import sys
import zipfile
from pathlib import Path

import numpy as np

FP16_TAG = 0x01306B47


class _Storage:
    def __init__(self, dtype, key):
        self.dtype, self.key = dtype, key


def load_pth(path):
    zf = zipfile.ZipFile(path)
    names = zf.namelist()
    prefix = names[0].split("/", 1)[0]
    dtypes = {"FloatStorage": np.float32, "HalfStorage": np.float16, "DoubleStorage": np.float64, "LongStorage": np.int64,
              "IntStorage": np.int32}

    class U(pickle.Unpickler):
        def find_class(self, module, name):
            if module == "torch._utils" and name == "_rebuild_tensor_v2":
                def rebuild(storage, offset, size, stride, requires_grad=False, hooks=None, metadata=None):
                    raw = np.frombuffer(zf.read("%s/data/%s" % (prefix, storage.key)), dtype=storage.dtype)
                    if not size:
                        return raw[offset:offset + 1].reshape(())
                    arr = np.lib.stride_tricks.as_strided(raw[offset:], shape=size,
                                                          strides=[s * raw.itemsize for s in stride])
                    return np.array(arr)
                return rebuild
            if module == "torch" and name in dtypes:
                return name
            if module == "collections" and name == "OrderedDict":
                import collections
                return collections.OrderedDict
            return super().find_class(module, name)

        def persistent_load(self, pid):
            # ('storage', storage_type, key, location, numel)
            kind, stype, key, location, numel = pid
            return _Storage(dtypes[stype if isinstance(stype, str) else stype], key)

    return U(zf.open("%s/data.pkl" % prefix)).load()


def write_model(out_dir, name, sd, num_conv, downscale=None):
    convs = [0] + [2 * i + 2 for i in range(num_conv)] + [2 * num_conv + 2]
    prelus = [1] + [2 * i + 3 for i in range(num_conv)]
    lines = []
    blobs = 0

    def layer(kind, lname, inputs, outputs, params=""):
        lines.append("%-24s %-24s %d %d %s %s %s" % (kind, lname, len(inputs), len(outputs), " ".join(inputs), " ".join(outputs),
                                                    params))

    layer("Input", "input.1", [], ["data"])
    layer("Split", "splitncnn_input0", ["data"], ["in_a", "in_b"])
    bin_parts = []
    prev = "in_b"
    for i, ci in enumerate(convs):
        w = sd["body.%d.weight" % ci].astype(np.float32)
        b = sd["body.%d.bias" % ci].astype(np.float32)
        out_ch, in_ch, kh, kw = w.shape
        top = "c%d" % i
        layer("Convolution", "Conv_%d" % i, [prev], [top], "0=%d 1=%d 4=1 5=1 6=%d" % (out_ch, kh, w.size))
        bin_parts.append(struct.pack("<I", FP16_TAG) + w.astype(np.float16).tobytes())
        bin_parts.append(b.tobytes())
        prev = top
        if i < len(prelus):
            slope = sd["body.%d.weight" % prelus[i]].astype(np.float32).reshape(-1)
            top = "p%d" % i
            layer("PReLU", "PRelu_%d" % i, [prev], [top], "0=%d" % slope.size)
            bin_parts.append(slope.tobytes())
            prev = top
    layer("PixelShuffle", "DepthToSpace", [prev], ["ps"], "0=4")
    layer("Interp", "Resize_base", ["in_a"], ["base"], "0=1 1=4.000000e+00 2=4.000000e+00")
    if downscale:
        layer("BinaryOp", "Add", ["ps", "base"], ["sum"])
        layer("Interp", "Resize_out", ["sum"], ["output"], "0=2 1=%e 2=%e" % (downscale, downscale))
    else:
        layer("BinaryOp", "Add", ["ps", "base"], ["output"])
    blob_names = set()
    for l in lines:
        parts = l.split()
        ni, no = int(parts[2]), int(parts[3])
        blob_names.update(parts[4:4 + ni + no])
    text = "7767517\n%d %d\n" % (len(lines), len(blob_names)) + "\n".join(lines) + "\n"
    (out_dir / (name + ".param")).write_text(text)
    (out_dir / (name + ".bin")).write_bytes(b"".join(bin_parts))
    return len(lines), sum(len(p) for p in bin_parts)


def main():
    source = Path(sys.argv[1])
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    if hashlib.sha256(source.read_bytes()).hexdigest() != "8dc7edb9ac80ccdc30c3a5dca6616509367f05fbc184ad95b731f05bece96292":
        raise SystemExit("Expected the official v0.2.5.0 realesr-general-x4v3.pth (SHA256 mismatch)")
    gen = load_pth(source)
    gen = gen.get("params", gen)
    keys = sorted(gen, key=lambda k: (int(k.split(".")[1]), k))
    print("keys", len(keys), keys[:3], keys[-2:], "shapes", gen["body.0.weight"].shape, gen["body.66.weight"].shape)
    num_conv = (max(int(k.split(".")[1]) for k in gen) - 2) // 2
    print("num_conv", num_conv)
    sd = {k: gen[k].astype(np.float64) for k in gen}
    result = write_model(out, "realesr-general-x4v3-x2", sd, num_conv, downscale=0.5)
    print("realesr-general-x4v3-x2 layers/bytes", result)


if __name__ == "__main__":
    main()
