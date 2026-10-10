"""Deployment variants of an exported YOLO ONNX (NMS inside, float input [1,3,S,S]):
  <stem>-u8.onnx       the same network with a byte input image_u8 [1,S,S,3] (RGB, letterboxed) and the /255 inside the
                       graph, so the program uploads 3 bytes a pixel instead of 12 and skips its own float conversion
  <stem>-u8-fp16.onnx  the same with the backbone, neck and head towers in fp16; the head's box decode (anchors and
                       strides) and the NMS tail stay fp32 so the coordinates keep their sub-pixel precision

python onnx_variants.py <src.onnx> <out dir> <stem> [image=<png for the check>]
"""
import collections
import pathlib
import sys
import time

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

src, out_dir, stem = sys.argv[1:4]
out_dir = pathlib.Path(out_dir); out_dir.mkdir(parents=True, exist_ok=True)
options = dict(item.split("=", 1) for item in sys.argv[4:])
source = onnx.load(src)


def head_index(graph):
    numbers = [int(n.name.split("/")[2].split(".")[1]) for n in graph.node
               if n.name.startswith("/model/model.") and n.name.split("/")[2].split(".")[1].isdigit()]
    return max(numbers)


def to_fp16(model):
    """fp16 for the convolutional body. The head's decode (the nodes under /model/model.N/ other than the cv2/cv3 towers)
    and everything after the head stay fp32, with Cast nodes at the boundaries, so the coordinates keep their sub-pixel
    precision: an fp16 anchor near 160 grid units has a resolution of 0.125 units, a whole pixel at stride 8.
    (onnxconverter-common's converter produced a model ONNX Runtime refused, so the conversion is done here.)"""
    model = onnx.shape_inference.infer_shapes(onnx.ModelProto.FromString(model.SerializeToString()))
    graph = model.graph
    head = f"/model/model.{head_index(graph)}/"
    fp32_ops = {"Resize", "Upsample", "NonMaxSuppression", "TopK", "Range", "CumSum", "RoiAlign", "Cast"}
    def in_fp32(node):
        if node.op_type in fp32_ops or not node.name.startswith("/model/"):
            return True
        return node.name.startswith(head) and not (node.name.startswith(head + "cv2.") or node.name.startswith(head + "cv3."))
    half = [not in_fp32(n) for n in graph.node]
    FLOAT, HALF = TensorProto.FLOAT, TensorProto.FLOAT16
    elem = {}   # tensor -> element type in the fp32 model
    for vi in list(graph.input) + list(graph.output) + list(graph.value_info):
        elem[vi.name] = vi.type.tensor_type.elem_type
    for init in graph.initializer:
        elem[init.name] = init.data_type
    consumers = collections.defaultdict(list)
    for i, n in enumerate(graph.node):
        for t in n.input:
            consumers[t].append(i)
    def to_half(tensor):
        values = np.clip(numpy_helper.to_array(tensor).astype(np.float32), -65504, 65504)
        tensor.CopyFrom(numpy_helper.from_array(values.astype(np.float16), tensor.name))
    is16 = {}   # float tensors that are fp16 after the conversion
    for init in graph.initializer:   # weights and constants read only by fp16 nodes become fp16; shared ones stay fp32
        if init.data_type == FLOAT and consumers[init.name] and all(half[i] for i in consumers[init.name]):
            to_half(init); is16[init.name] = True
    for i, n in enumerate(graph.node):
        if n.op_type == "Constant":
            value = next((a for a in n.attribute if a.name == "value"), None)
            if value is not None:
                elem[n.output[0]] = value.t.data_type
                if value.t.data_type == FLOAT and consumers[n.output[0]] and all(half[j] for j in consumers[n.output[0]]):
                    to_half(value.t); is16[n.output[0]] = True
            half[i] = False
            continue
        for o in n.output:
            if elem.get(o) == FLOAT:
                is16[o] = half[i]
    nodes = []; casts = {}
    for i, n in enumerate(graph.node):
        for k, t in enumerate(n.input):
            if elem.get(t) != FLOAT:
                continue   # ints, bools, shapes: untouched
            want16 = half[i]; have16 = is16.get(t, False)
            if want16 == have16:
                continue
            name = t + ("_fp16" if want16 else "_fp32")
            if name not in casts:
                casts[name] = helper.make_node("Cast", [t], [name], to=HALF if want16 else FLOAT, name=f"/fp16/Cast_{len(casts)}")
                nodes.append(casts[name])
            n.input[k] = name
        nodes.append(n)
    del graph.node[:]; graph.node.extend(nodes)
    for vi in graph.value_info:
        if is16.get(vi.name):
            vi.type.tensor_type.elem_type = HALF
    onnx.checker.check_model(model)
    return model, sum(1 for h in half if not h)


def with_byte_input(model):
    model = onnx.ModelProto.FromString(model.SerializeToString())   # a copy
    graph = model.graph; given = graph.input[0]
    elem = given.type.tensor_type.elem_type; dims = [d.dim_value for d in given.type.tensor_type.shape.dim]
    assert len(dims) == 4 and dims[1] == 3, dims
    n, c, h, w = dims
    byte_input = helper.make_tensor_value_info("image_u8", TensorProto.UINT8, [n, h, w, c])
    scale = numpy_helper.from_array(np.array(1 / 255, dtype=np.float16 if elem == TensorProto.FLOAT16 else np.float32), "pre_scale")
    nodes = [helper.make_node("Cast", ["image_u8"], ["pre_float"], to=elem, name="/pre/Cast"),
             helper.make_node("Transpose", ["pre_float"], ["pre_nchw"], perm=[0, 3, 1, 2], name="/pre/Transpose"),
             helper.make_node("Mul", ["pre_nchw", "pre_scale"], [given.name], name="/pre/Mul")]
    graph.initializer.append(scale)
    for i, node in enumerate(nodes):
        graph.node.insert(i, node)
    graph.input.remove(given); graph.input.insert(0, byte_input)
    onnx.checker.check_model(model)
    return model


fp32_u8 = with_byte_input(source)
fp16_body, blocked = to_fp16(onnx.load(src))
fp16_u8 = with_byte_input(fp16_body)
paths = {"fp32-u8": out_dir / f"{stem}-u8.onnx", "fp16-u8": out_dir / f"{stem}-u8-fp16.onnx"}
onnx.save(fp32_u8, paths["fp32-u8"]); onnx.save(fp16_u8, paths["fp16-u8"])
print(f"{paths['fp32-u8']} ({paths['fp32-u8'].stat().st_size / 1048576:.1f} MB), {paths['fp16-u8']} ({paths['fp16-u8'].stat().st_size / 1048576:.1f} MB); {blocked} nodes kept fp32")

if "image" in options:   # the three models on one image, through ONNX Runtime on the CPU: the boxes must agree
    import cv2
    import onnxruntime as ort
    image = cv2.imread(options["image"]); size = [d.dim_value for d in source.graph.input[0].type.tensor_type.shape.dim][2]
    h, w = image.shape[:2]; s = size / max(h, w); nw, nh = round(w * s), round(h * s)
    canvas = np.full((size, size, 3), 114, np.uint8); px, py = (size - nw) // 2, (size - nh) // 2
    canvas[py:py + nh, px:px + nw] = cv2.resize(image, (nw, nh), interpolation=cv2.INTER_LINEAR)
    rgb = np.ascontiguousarray(canvas[:, :, ::-1])
    def run(path, blob):
        session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"]); name = session.get_inputs()[0].name
        session.run(None, {name: blob}); started = time.perf_counter(); out = session.run(None, {name: blob})[0][0]
        return out[out[:, 4] >= .25], (time.perf_counter() - started) * 1000
    base, t0 = run(src, rgb.transpose(2, 0, 1)[None].astype(np.float32) / 255)
    u8, t1 = run(paths["fp32-u8"], rgb[None])
    half, t2 = run(paths["fp16-u8"], rgb[None])
    print(f"boxes at 0.25: float {len(base)} ({t0:.0f} ms CPU), fp32-u8 {len(u8)} ({t1:.0f} ms), fp16-u8 {len(half)} ({t2:.0f} ms)")
    for name, other in (("fp32-u8", u8), ("fp16-u8", half)):
        if len(other) == len(base):
            print(f"  {name}: max box difference {np.abs(other[:, :4] - base[:, :4]).max():.3f} px, max confidence difference {np.abs(other[:, 4] - base[:, 4]).max():.4f}")
        else:
            print(f"  {name}: different box count")
