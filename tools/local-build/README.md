# Local engine build (Docker)

Builds `prosperoweb.elf` on your PC the way the CI "PS5 build and package"
job does: same Ubuntu 24.04 packages, same payload SDK (v0.41.1), same
scripts (`build-deps.sh`, `fetch-webkit.sh`, `build-webkit.sh all`, the
frontend, `package.sh`). Use it to iterate on WebKit patches without waiting
for CI (7-8 minutes a push).

```bash
tools/local-build/run.sh setup   # once: image, dependencies, WebKit fetched and patched
tools/local-build/run.sh build   # WebKit + frontend + package -> output/local-build/dist/
tools/local-build/run.sh sync    # just copy the edited sources into the build tree
tools/local-build/run.sh shell   # a shell in the build tree
JOBS=4 tools/local-build/run.sh build   # fewer compile jobs (default 6)
```

The engine ends up in `output/local-build/dist/prosperoweb/prosperoweb.elf`.
Deploy it like a CI engine (EVO quit, then close; FTP it to
`/mnt/usb0/prosperoweb.elf`; launch).

## How it works

- The work tree lives in the Docker volume `pw-work` (ext4 inside Docker's VM:
  fast, unlike a Windows bind mount). It also holds ccache (`/work/ccache`).
  Docker's disk is on the drive Docker Desktop is set to use: plan on 25-40 GB.
- The repo files come from `git archive HEAD`, so **commit before you build**
  (the Windows checkout has CRLF line endings, which break shell scripts).
- WebKit is fetched and patched by `fetch-webkit.sh` once. After that `sync`
  copies the WebKit files that patch 0022 touches from your working copy
  (line endings stripped), so an edit in `third_party/webkit/...` is picked up
  without committing, and ninja rebuilds only what changed.
- After an edit, regenerate and check the patch the usual way
  (`make_0022.sh` / `check_0022.sh`): the container builds the working-copy
  files, CI builds the patch series, and they must agree.

## Memory

Docker Desktop's VM has what `.wslconfig` gives it (7.6 GB here). WebKit
compiles need about 1 GB a job, so the default is `JOBS=6`; lower it if the
build is killed (exit 137).

## First build time

The first build has no ccache and compiles all of WebKit (an hour or more).
Later builds recompile the files you changed and relink.
