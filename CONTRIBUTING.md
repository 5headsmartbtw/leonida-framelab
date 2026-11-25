# Contributing to Leonida FrameLab

[Русская версия](CONTRIBUTING_RU.md) · [Coordination and task claims](https://github.com/5headsmartbtw/leonida-framelab/issues/4) · [Roadmap](docs/ROADMAP.md)

## No GTA VI Game Files

**GTA VI Game Files are strictly prohibited in every pull request and every commit it contains.** This includes retail, prerelease and leaked material such as executables, archives, game source code, keys, assets, shaders, captures, dumps and extracts. Renaming, compressing, encoding, redacting or repackaging game-derived material does not make it acceptable. The prohibition also covers fixtures, logs containing game data, screenshots, artifacts, Git LFS, and links or attachments that distribute it in PRs or Issues. If you cannot verify that a file is safe to publish, leave it out and use a synthetic replacement.

Use original synthetic scenes, generated test inputs and sanitized technical diagnostics. For third-party source you are authorized to redistribute, record its source, revision and license. Keep proprietary NVIDIA SDK headers, binaries and kernels out of submissions. Preserve upstream Apache-2.0 and MIT notices. Submit new original contributions under the project's MIT license.

## Claim work and submit a PR

1. Check [Coordination #4](https://github.com/5headsmartbtw/leonida-framelab/issues/4), existing Issues and open PRs. For substantial work, post `/claim FL-01` or `/claim #3` in #4. Use one command on the first line and put scope, dependencies and planned tests on later lines. Small fixes and focused reports are welcome without a claim.
2. The bot immediately reserves any available task, including SDK implementation and architecture tasks. You can start work once the bot confirms the claim and posts the task scope and acceptance criteria. All PRs still follow the usual review, CI and branch protection rules.
3. In #4, use `/progress FL-01` to mark your claim in progress and link the board to your update. Put results or blockers below the command. Use `/unclaim FL-01` to release your own claim. Use `/propose` to suggest a new task.
4. Open a focused PR linked to the task. State the change, tested commit, commands, actual results, skipped checks, source provenance and any meaningful AI assistance. Keep unrelated changes in separate PRs.

Post each command in a new comment. Maintainers add new work under **Work available** or **New tasks** in #4 and mark it `Available`. Let queued commands finish before manually editing task statuses, and retain the `framelab-claim` marker when editing other parts of a task row.

## Validation and merge

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python scripts/check_repository.py
python scripts/check_no_game_files.py --base origin/main
```

Run the last command after committing on your branch. It inspects the submitted history as Git objects. CI checks known payload indicators and runs portable tests on Windows and Linux. Maintainers review provenance, especially for source, media and changes to the CI rules. Passing CI alone cannot establish that files are original or redistributable.

For rendering changes, include applicable synthetic image and validation evidence. Record GPU, driver, OS, SDK/runtime, API/backend and exact reproduction commands. Distinguish rendered frames, generated images, API Present counts and physical display measurements. Keep unknown depth, motion vectors or camera metadata unavailable and preserve genuine capability results. See [Build and reproduce](docs/BUILD.md) for GPU prerequisites. Hosted CI does not establish real GPU or retail game compatibility.
