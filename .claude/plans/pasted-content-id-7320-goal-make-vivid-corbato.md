C:\Work\WashingParking  (branch stage1-sim)

# Stage1: real AI behind AI_MODE=real (mock stays default)

## Context
Stage1 (`stage1/server`, FastAPI + MQTT + ESP32) still takes plate and DIRTY/CLEAN values from the request body (`ai.py` mock). Goal: let `/cam1` and `/cam2` run the real Brain models on a camera frame or an uploaded image, behind `AI_MODE=real`. Leave mock mode and the board work untouched, and never fake a dirt result. `Brain/` stays unmodified: import only, and no runtime writes.

## Part 1: facts (measured 2026-11-16 with `Brain\.venv` Python 3.12, read-only script)
- **Reusable as-is:**
  - `station.vision.Detector` (`xe_best.onnx`, 1 class `xe`, 416 px, onnxruntime).
  - `station.vision.doc_anh` (reads images from Unicode paths).
  - `station.plate.PlateReader(hub)`, which needs only `hub.log` and returns `(plate, conf, box, confirmed)`. It uses `bien_so.onnx`, `rec.onnx` and RapidOCR.
  - `station.camera.CamThread`, used with an explicit source and `on_src_change=None`, so it never rewrites `station.json`.
- **Do not import:** `station.db`, `inspection`, `dataset`, `server`. They write `parking.db`, `inspections/` and `station.json`.
- **Model files:**
  - Present: `xe_best.onnx`, `bien_so.onnx` (plate detector, required), `rec.onnx`, plus a backup at `Backup/xe_best.onnx`.
  - Missing: `hu_hong.onnx` (damage). `station.json` names it, but `DamageDetector` is off. The damage dataset has 3 classes and a handful of images.
  - **No dirt model exists**, so "use the damage model for now" isn't available either.
- **Measured on the sample images:**
  - Model load time: 1.8 s.
  - Car detection: about 20 ms per image.
  - Plate reading: median 331 ms and max 602 ms on 64×50 crops, 290–710 ms on full frames.
  - Plate crops (64): 60 read, **0 cross-confirmed**, and 55 match the plate text in the filename. Those filenames are the old program's own reads, so this is agreement with earlier logs, not accuracy.
  - The 5 wrong reads had confidence between 0.57 and 0.81.
  - Full frames in `dataset_hu_hong` (5 at 1280/1920 px): 4 with a car found (0.54–0.76). Only 1 plate was read: `60G343187`, confidence 0.996, not confirmed.
  - `inspections/*/car.jpg` (6 car crops): no car detected at threshold 0.45, and no plate read.
- **Gap:** no labelled full-frame car-plus-plate image exists. The image tests cover the OCR stage. The "visit created from a real read" path is proven only by the live test or by photos you supply.

## Key decisions
- **Mock routes stay byte-identical.** The JSON schemas and responses of `/cam1` and `/cam2` don't change, because `demo.py` and Wokwi depend on them. Real-mode routes are registered only when `AI_MODE=real`, so `python-multipart` is never needed in mock mode.
- **No heavy imports at top level** of `ai.py` or `app.py`. The real backend is loaded lazily, so a mock run needs no cv2 or onnxruntime.
- **Plate acceptance rule:** accept when `confirmed`, or when `conf >= WP_PLATE_MIN_CONF` (default 0.95). Reason: confirmation never fires on crops; every measured wrong read was ≤ 0.81; `plate.py` documents a wrong read at 0.88. The test reports how many crops the rule accepts, and how many of those accepted reads are wrong (the target is 0).
- **Real `/cam2`:** detects whether a car is present and returns `condition: null` with the reason "chưa có mô hình bẩn/sạch – nhân viên chọn DIRTY/CLEAN" (no dirt/clean model yet; staff picks). The visit stays `INSPECTING`. Staff send `condition=DIRTY|CLEAN` as a form field on the same endpoint. That manual value is the only way to move on.
- **Manual plate:** real `/cam1` accepts an optional `plate` form field. If the AI rejects a read, staff retype the plate and the visit is created from that.
- **Cameras (USB webcam + IP camera):**
  - Set by `WP_CAM1_SRC` and `WP_CAM2_SRC`: a number (USB index) or an `rtsp://` URL. If both are equal, one `CamThread` is shared.
  - The RTSP URL with its password goes in an env variable only, never into repo, README, tests or logs. Logs show the masked form via `station.camera.mask`.
- One `threading.Lock` around inference, because the handlers run in a threadpool.

## Files
- `stage1/server/ai.py`:
  - Mock functions unchanged.
  - Add `MODE = os.environ.get("AI_MODE", "mock")`, validated against `mock` and `real`.
  - Add `class RealAI`, which appends the Brain dir (`WP_BRAIN_DIR`, default `../../Brain`) to `sys.path` after setting `sys.dont_write_bytecode`. Methods:
    - `cam1(img)` returns `{vehicle:{found,conf,box}, plate, conf, confirmed, ok, reason, ms:{car,plate,total}}`.
    - `cam2(img)` returns `{vehicle, condition:None, reason, ms}`.
    - `frame(role)` returns the latest `CamThread` frame.
    - `start()` and `stop()` handle the camera threads.
- `stage1/server/app.py`: in the `if ai.MODE == "real":` branch, define multipart `/cam1` (`file`, `plate`) and `/cam2` (`file`, `condition`), start and stop the cameras in the lifespan, and save the cam1 frame to `photos/<ts>.jpg` as `photo_path`.
  - Response: `{visit, ai}`.
  - Low confidence: HTTP 422 `{detail:"cần nhập biển số thủ công", ai}` ("plate must be entered manually"). No visit is created.
  - Mock branch: the existing code, untouched.
- `stage1/server/requirements-ai.txt`: numpy 2.0.2, opencv-python 5.0.0.93, onnxruntime 1.19.2, rapidocr-onnxruntime 1.4.4 (same pins as Brain), plus python-multipart, pinned to the version that installs. Installed into `stage1/server/.venv`.
- `stage1/server/test_ai_real.py`: standard library only, prints a report like `test_flow.py`.
- `stage1/README.md`: a "AI thật" section in Vietnamese covering the env vars, how to install, and the dirt-model gap.
- `.gitignore`: add `stage1/server/photos/`.

## Steps (each with a verify check)
1. [ ] Gate before the change: `test_flow.py` prints `18 test đạt`. (Already observed.)
2. [ ] Implement `ai.py` and `app.py` as above. Verify: with `AI_MODE` unset and before installing the AI deps, `test_flow.py` still prints `18 test đạt`, and `uvicorn app:app` starts and serves the unchanged `/cam1` JSON in Swagger.
3. [ ] Install `requirements-ai.txt` into the stage1 venv. Verify: `python -c "import cv2, onnxruntime, rapidocr_onnxruntime, multipart"`.
4. [ ] `test_ai_real.py`, run on the 64 crops, 5 full frames and 6 car crops. It must assert:
   - A blank image and a no-plate image give `ok=False`.
   - `cam2` always returns `condition is None`.
   - No accepted read disagrees with the earlier log. First, view one crop per plate group (6 groups) to confirm the labels.

   It also prints the load time, ms per stage (p50/max), and the accepted, rejected and disagreeing counts.
5. [ ] HTTP smoke with `AI_MODE=real`, `WP_DB` pointed at a scratch DB, and no broker or ESP:
   - `curl -F file=@<full frame> /cam1`: a confident read creates a visit, which ends in `ERROR esp offline: OPEN_GATE`. That is expected with no ESP.
   - A crop the rule rejects gives 422.
   - `-F plate=...` gives a visit.
   - `/cam2` with a file gives 422 and `condition null`.

   Report the wall time of each call.
6. [ ] Live test, which needs you present:
   - `WP_CAM1_SRC=0` (laptop webcam), then `WP_CAM1_SRC=rtsp://…` (IP camera).
   - `POST /cam1` with no file, pointed at a real plate, 3 tries per camera. Report the result and the ms per stage.
   - Mark "verified" only what actually ran.
7. [ ] Gate after the change: `test_flow.py` prints `18 test đạt`, and `git diff` shows the mock routes and `demo.py` unchanged. Wokwi tests are not re-run, since the firmware and mock routes are unchanged. Say so explicitly.

## Smallest honest path for dirt (proposal only, not implemented)
1. Collect about 200 or more labelled DIRTY/CLEAN car crops from the inspection camera. The dataset/label page in Brain could be reused as a pattern.
2. Train a 2-class classifier and export it to ONNX.
3. Add an optional `WP_DIRT_MODEL` loader to `RealAI.cam2`.

Until then, staff choose.

## User changes (2026-11-16)
1. Ground truth: the user supplies about 20 car photos, with the true plate as the filename, in `stage1/server/test_images/gt/`. `test_ai_real.py` reports the exact-match rate and how many **accepted** reads are wrong (the target is 0). Agreement with old logs is never reported as accuracy. If the folder is empty, the test says "ground truth: SKIPPED (no photos)".
2. RTSP: the user sets the env variable. **Do not read `Brain/station.json`.** Never print the URL; show only `mask()`'s output.
3. README documents that stage1 imports from `Brain/` (untracked, renamed folder), giving the expected path and `WP_BRAIN_DIR`. `RealAI` fails with a clear message if Brain or a model is missing.
4. Run steps 1–5 now, then stop before step 6 and tell the user exactly what to prepare.

## Open questions
- Whether 0.95 is the right threshold is unknown until the live test. It is adjustable through `WP_PLATE_MIN_CONF`.
- None blocking.
