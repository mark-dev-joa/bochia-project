Boccia Image Ball Detector - Windows 10/11 x64

Extract the entire ZIP, then run boccia_image_detector.exe.
Drag a photo onto the window. No image resizing is needed.
Ball boxes, confidence, original pixel coordinates and detection time (ms) are shown.
The trained YOLO26n 1280x960 model, Korean font, OpenCV, ONNX Runtime and Visual C++ runtime are included.
This portable package runs on CPU. Python, CUDA, camera SDK and separate VC++ installation are not required.
An OpenGL 3.3 capable graphics driver is required for the window.
Keep the executable, DLLs, models, fonts, data and licenses together when copying to another PC.
Detection time excludes file reading, model initialization/warm-up and drawing.

Model: YOLO26n fine-tuned on this project's boccia recordings; one class (ball).
Input: RGB uint8 [1,960,1280,3], letterboxed; normalization is inside the model.
The model is for detection in individual photos; 3D coordinates require camera calibration.
