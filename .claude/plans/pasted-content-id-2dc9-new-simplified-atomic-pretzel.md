C:\Work\WashingParking (work inside stage1/server)
Untouched: Brain/, tram_ai.py, firmware/, platformio.ini, diagram.json, the MQTT protocol, dataset.py DirtSet, dataset_ban_sach/, test_dataset.py, the /cam2 auto-save.

# Plan: hand-labeled dirt dataset `dataset_dirt/` + `/label` camera page + rule-based dust heuristic + session-split evaluation

## Context
For the toy-car demo we need a **hand-labeled-only** set of BẨN/SẠCH frames from the inspection USB camera (`WP_CAM2_SRC`). We use it to tune a rule-based dust check: a CLEAN reference frame plus an ROI, brightness normalized, DIRTY if the changed-area ratio is above a threshold. Later it may feed training. The existing `DirtSet` (`dataset_ban_sach/`) mixes auto-saved and uploaded images whose labels are not verified. Per user decision it stays as is, and the new set is separate. We copy only patterns from Brain's damage dataset: JSON metadata, soft delete, an id regex, a lock, zip.

## Key decisions
- **New `dirt_label.py`:** stdlib only. It imports the helpers `_new_id`, `_check_id`, `_trash`, `_zip`, `image_ext`, `_inside` from `dataset.py` without modifying that file.
- **Layout:** `dataset_dirt/{clean,dirty,_deleted}/` plus append-only `metadata.jsonl` with lines `{id, cls, at, session, camera}`. Delete appends a tombstone line `{id, deleted: true, at}` and moves the file to `_deleted/`, so nothing is ever removed. `camera` = `"cam2:" + mask(src)`.
- **Camera without models:** the camera part of `ai.RealAI` moves into `ai.Cams`; `RealAI` delegates, so behavior is unchanged. In mock mode `/label` starts `Cams` for cam2 only if `WP_CAM2_SRC` is set. cv2/Brain imports are lazy, so mock mode without the env var needs no cv2.
- **Localhost default + optional `WP_WEB_PASS`:** documented commands use `--host 127.0.0.1`. When `WP_WEB_PASS` is set, Basic-auth middleware covers all routes (`secrets.compare_digest`). When it is unset, a startup warning is logged. `demo.py` adds the header only when the env var is set. The ESP uses the broker, so it is unaffected.
- **Evaluation:** choose the threshold on some sessions and measure on held-out sessions. With a single session, report `same_data` explicitly.
- **Deferred:** the training script, torch, `WP_DIRT_MODEL`.

## Steps

### 1. `ai.Cams` refactor  → verify: `test_ai_real.py` passes; mock `import app` needs no cv2
- [ ] Extract `start/stop/frame` (ai.py:116-144) into `class Cams`; `RealAI.frame` and `.cams` delegate to it. Add `Cams.label(role)`, which returns the masked source.

### 2. `server/dirt_label.py`  → verify: `test_dirt_label.py`
- [ ] `DirtLabels(root=WP_DATASET_DIR or server dir)` with `CLASSES = ("clean", "dirty")`. Methods:
  - `new_session()`
  - `add(cls, jpg, session, camera)` (validated)
  - `counts(session=None)`
  - `items()`
  - `samples()` → [(id, cls, session, path)] of non-deleted entries
  - `delete_last(session)` (soft)
  - `image(sid)`
  - `set_reference(session, jpg)` → `dataset_dirt/reference_<session>.jpg`; an existing reference for that session moves to `_deleted/`
  - `reference(session)` → path or None; `latest_reference()` → path of the newest session's reference (for live `/cam2` later)
  - `zip()` → `clean/`, `dirty/`, `metadata.jsonl`, all `reference_<session>.jpg`
- [ ] `test_dirt_label.py` (stdlib, temp dir) covers:
  - add/count per session;
  - bad class, ext, session or id rejected (path traversal);
  - delete-last scoped to its session and soft (file in `_deleted/`, tombstone line);
  - per-session reference: set/replace (old one soft-deleted), `latest_reference`, all references in the zip;
  - zip contents;
  - metadata has no raw camera password (the `camera` value passed in is already masked, and the test asserts the format).

### 3. `server/dust.py` (numpy + cv2, lazy)  → verify: `test_dust.py`
- [ ] `parse_roi("x1,y1,x2,y2")`: fractions 0..1, default whole frame.
- [ ] `changed_ratio(ref, cur, roi, pix_thresh)`:
  - gray → resize cur to ref → crop the ROI;
  - **brightness match**: scale cur so its ROI mean equals the ref ROI mean, clip;
  - GaussianBlur 5 → absdiff > pix_thresh → ratio.
- [ ] Ratios are computed per sample **against the reference of its own session**. Samples whose session has no reference are skipped and counted in the report as "N ảnh bỏ qua: phiên chưa có ảnh tham chiếu".
- [ ] `evaluate(samples, current_thresh, skipped=0)` (pure Python, samples = [(session, cls, ratio)]):
  - with ≥2 sessions, sort them and hold out the last ~30% (≥1) for test;
  - tune the threshold on the rest (the candidate midpoint that maximizes balanced accuracy);
  - report per-split per-class n/min/median/max, plus P/R/confusion on test (DIRTY = positive) at the suggested threshold and at `WP_DUST_RATIO`;
  - a single session → `same_data: true` and the message "ngưỡng chọn và đo trên cùng dữ liệu";
  - warnings: a class with <30 samples ("mẫu nhỏ – chỉ là chỉ báo thô"), or a class missing from a split.
- [ ] Env vars `WP_DUST_ROI`, `WP_DUST_PIX` (25), `WP_DUST_RATIO` (0.05), validated when used.
- [ ] `test_dust.py` covers:
  - identical frames → 0;
  - a globally brighter frame → ≈0;
  - a patch inside the ROI → the expected ratio;
  - a patch outside the ROI → 0;
  - a bad ROI is rejected;
  - `evaluate`: a perfect split, an overlap, a single session → `same_data`, and disjoint session splits;
  - a helper that pairs samples with their session's reference: samples without a reference are skipped and counted, and each sample is never compared with another session's reference.

### 4. Routes + page  → verify: mock HTTP smoke
- [ ] `app.py` routes (both modes; they write only to `dataset_dirt/`):
  - `GET /label` → `web/label.html`
  - `GET /label/preview.jpg` (503 with a masked reason)
  - `POST /label/session`
  - `POST /label/capture?cls=&session=`
  - `POST /label/delete_last?session=`
  - `GET /label/counts?session=`
  - `POST /label/reference?session=` (current session's CLEAN reference)
  - `GET /label/eval` (409 with a message if there is no reference or no cv2)
  - `GET /label/dataset_dirt.zip`
- [ ] Basic-auth middleware and the startup warning.
- [ ] `web/label.html` (Vietnamese, plain JS):
  - preview refreshes every ~1 s while visible;
  - **BẨN** / **SẠCH** buttons capture and label in one click;
  - counters (total + session), Xoá ảnh vừa chụp, Phiên mới (shows the id), Chụp ảnh tham chiếu (SẠCH) **for the current session** (shows whether this session has one);
  - Đánh giá table (split description / same-data notice, suggested threshold, P/R, warnings);
  - Tải zip.
  - A link to it in the `index.html` header is the only `index.html` change.

### 5. Config/docs
- [ ] `.gitignore`: `stage1/server/dataset_dirt/`.
- [ ] `stage1/README.md`:
  - a short **two-dataset table**:
    - `dataset_ban_sach/`: auto-save from `/cam2` + uploads, labels not verified;
    - `dataset_dirt/`: hand-labeled only via `/label`, used for threshold tuning and future training;
  - the `/label` section;
  - env vars `WP_WEB_PASS`, `WP_DUST_*`;
  - the `127.0.0.1` default (`0.0.0.0` only with `WP_WEB_PASS`);
  - the **heuristic disclaimer**;
  - limits: camera auto-exposure (brightness matching only partly compensates), car-position shift, shadows and reflections;
  - recommend a fixed stop position, locked exposure/white balance, and re-capturing the reference after any lighting change;
  - references are per session (`reference_<session>.jpg`): eval compares each image with its own session's reference and skips sessions without one; the future live `/cam2` will use the latest reference.
- [ ] Skip unless time allows: the `eval_dust.py` CLI and the root `CLAUDE.md` edit (if skipped, report CLAUDE.md as stale).

## Verification (what will actually run)
- `PYTHONIOENCODING=utf-8 .venv/Scripts/python test_flow.py` → "18 test đạt".
- `test_dataset.py` → 30 (unchanged), `test_dirt_label.py`, `test_dust.py`, `test_ai_real.py` (after the `Cams` refactor).
- Mock smoke, `uvicorn app:app --host 127.0.0.1` with no `WP_CAM2_SRC`:
  - `/label` 200, preview 503, session/counts OK;
  - mock `/cam1` `/cam2` JSON unchanged;
  - `WP_WEB_PASS=x` → 401 without the header, 200 with it;
  - logs contain no `rtsp://<user>:`;
  - `git status` shows no change under `dataset_ban_sach/`.
- If a USB cam is attached: capture a few frames per class, set the session's reference, run eval; otherwise report it as not run.
- Not verified unless run: real camera capture, eval numbers on toy-car data.
