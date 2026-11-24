# Contributing

Issues and focused pull requests are welcome. State the exact GPU, driver, OS, SDK/runtime versions, API/backend and reproduction command. Distinguish real rendered frames, generated images, API Present counts and physical display measurements.

Run the portable contract tests and `python scripts/check_repository.py`. GPU-related changes need applicable image/validation evidence; CI does not own the external NVIDIA runtime or prove GPU compatibility.

Keep unknown depth/MV/camera unavailable. Do not replace capability failures with successful values. Do not include game data, proprietary NVIDIA headers/binaries/kernels, captures of third-party apps, credentials or absolute personal paths in new reports. Own synthetic scenes and source-level diagnostics are appropriate. Test fixtures must be synthetic.

Preserve upstream Apache-2.0/MIT notices. New original contributions are submitted under this project's MIT license; changes to third-party portions must also comply with those files' existing licenses. Disclose meaningful AI assistance and the actual checks performed. No contributor assigns ownership of upstream code.
