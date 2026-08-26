# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller 스펙 - 윈도우에서 실행할 것.

    pip install pyinstaller
    pyinstaller hough_viewer.spec

스켈레톤(YOLO)이 필요 없으면 torch/ultralytics 를 빼서 1.5GB 를 줄일 수 있다.
    set BOCHIA_NO_YOLO=1
    pyinstaller hough_viewer.spec
"""
import os

skip_yolo = os.environ.get("BOCHIA_NO_YOLO") == "1"

excludes = ["matplotlib", "tkinter", "pandas", "scipy", "IPython", "pytest"]
hiddenimports = []
if skip_yolo:
    excludes += ["torch", "torchvision", "ultralytics"]
else:
    hiddenimports += ["ultralytics", "torch"]

a = Analysis(
    ["hough_viewer.py"],
    pathex=[],
    binaries=[],
    datas=[],
    hiddenimports=hiddenimports,
    hookspath=[],
    runtime_hooks=[],
    excludes=excludes,
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz, a.scripts, [],
    exclude_binaries=True,
    name="BochiaBallFinder",
    debug=False,
    strip=False,
    upx=False,
    console=True,          # 로그를 봐야 하므로 콘솔 유지
)
coll = COLLECT(
    exe, a.binaries, a.datas,
    strip=False, upx=False,
    name="BochiaBallFinder",
)
