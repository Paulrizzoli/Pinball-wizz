"""Downloads the Real-ESRGAN weights and converts them to ONNX for make_hd_pack.py.

Only needed when building MakeHDPack yourself (CI does this automatically):
    pip install torch spandrel onnx
    python tools/hdpack/export_models.py
"""
import os
import sys
import urllib.request

import torch
import spandrel

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = {
    # Crisp, flat-shaded look for the table art.
    "realesrgan_x4_anime6b": "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.2.4/RealESRGAN_x4plus_anime_6B.pth",
}


def main(out_dir=HERE):
    for name, url in MODELS.items():
        onnx_path = os.path.join(out_dir, name + ".onnx")
        if os.path.exists(onnx_path):
            print("exists:", onnx_path)
            continue
        pth = os.path.join(out_dir, name + ".pth")
        if not os.path.exists(pth):
            print("downloading", url)
            urllib.request.urlretrieve(url, pth)
        net = spandrel.ModelLoader().load_from_file(pth).eval().model
        torch.onnx.export(net, torch.rand(1, 3, 64, 64), onnx_path, input_names=["input"], output_names=["output"],
                          dynamic_axes={"input": {2: "h", 3: "w"}, "output": {2: "H", 3: "W"}}, opset_version=17,
                          dynamo=False)
        print("wrote", onnx_path)


if __name__ == "__main__":
    main(*sys.argv[1:])
