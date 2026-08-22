# Release notes

One file per released version, named `v<MAJOR>.<MINOR>.<PATCH>.md`
(e.g. `v0.7.0.md`). Written by hand and reviewed as part of the change that
warrants them, not generated at tag time.

## They are published verbatim

`.github/workflows/docker.yml` reads `docs/versions/v${VERSION}.md` when a `v*`
tag is pushed and uses it as the **GitHub Release body**, appending the published
image tags, the immutable digest, and GitHub's auto-generated commit changelog.
So the file is the release announcement — write it for someone deciding whether
to upgrade, not as a commit log.

Two consequences worth knowing:

- **The tag must point at a commit that already contains its own notes.** Add the
  file, merge it, *then* tag. Tagging first publishes a release without them.
- **A missing file warns, it does not fail.** The tag and the image are already
  published by the time the body is composed, so the release still happens with
  image details only. The body can be edited on GitHub afterwards — but the
  checked-in file is the reviewed copy, so prefer fixing it and re-releasing the
  prose there.

## What to cover

Look at `v0.7.0.md` for the shape. What has consistently earned its place:

- **Behavioural changes first**, with how to observe them before they bite —
  a `dry_run` to try, or a setting that reproduces the old behaviour exactly.
- **New on-disk state or flags**, since those are what an operator has to plan
  for.
- **Known limitations**, stated plainly rather than left to be discovered.
- **Measured numbers** rather than estimated ones when sizing is involved.
