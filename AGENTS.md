# Instructions for coding agents

## Scope and provenance

- Read [CONTRIBUTING.md](CONTRIBUTING.md), [the roadmap](docs/ROADMAP.md) and the linked task before editing. Check for overlapping Issues and PRs. Follow the claim process for substantial community work.
- **Never add GTA VI Game Files to a commit, PR, Issue, fixture, attachment, artifact or Git LFS.** This includes leaked or extracted code, binaries, archives, assets, captures, dumps, keys and derived data. Do not download or use these as development inputs. Use original synthetic inputs and sanitized logs.
- Preserve third-party attribution and license notices. Keep proprietary NVIDIA files and credentials outside the repo. Keep changes and staging limited to the task.

## Engineering boundaries

- `fg/FrameInputs.hpp` is API neutral. Keep Vulkan and vendor types at the adapter boundary. Preserve ownership, completion, continuity and reset behavior.
- `sample/` is the independent Vulkan reference scene. Preserve normal Present fallback, VSync, resize and genuine capability checks. Do not manufacture missing depth, motion vectors or camera inputs.
- Treat the Leonida audit as read-only evidence. Do not infer retail compatibility from synthetic results or change external checkouts, drivers or SDK installations as part of a FrameLab patch.

## Verify and report

- Run the portable CMake/CTest commands from [docs/BUILD.md](docs/BUILD.md) and `python scripts/check_repository.py`. After committing on a contribution branch, run `python scripts/check_no_game_files.py --base origin/main`.
- For GPU changes, report what ran on actual hardware and what was skipped. Include exact commands and tested revision. Distinguish source frames, generated images, Present calls and display measurements.
- Disclose material AI assistance in the PR. Report unverified behavior as unverified. Do not weaken CI to turn a failure into a pass.
