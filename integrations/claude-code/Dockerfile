# syntax=docker/dockerfile:1
#
# Runtime image for the operator-scheduled summarization sidecar (and the other
# aegis_mcp console scripts). The C server has its own Dockerfile at the repo
# root; this one carries the Python integration so `docker compose --profile
# summarize up` can run `aegisdb-summarize` on an interval.
FROM python:3.12-slim AS runtime

# The package version is normally derived from git via setuptools-scm, but this
# build context is just the integration directory (no repo root, no .git), so
# pin it — this also keeps the build fully offline.
ARG PKG_VERSION=0.0.0
ENV SETUPTOOLS_SCM_PRETEND_VERSION=${PKG_VERSION}

# Which optional dependency extra to bake in for the chat backend, e.g.
# "[anthropic]" or "[openai]". The default `none`/`fake` backends need none; the
# `claude-code` backend needs the `claude` CLI, which is NOT in this image, so a
# containerised sidecar normally uses `anthropic` or `openai` + an API key.
ARG EXTRAS=[anthropic]

RUN useradd --system --no-create-home --user-group aegis

WORKDIR /app
COPY . /app
RUN pip install --no-cache-dir ".${EXTRAS}"

USER aegis
# Default to a single distillation pass; the compose sidecar overrides this with
# an interval loop, and `docker compose run --rm --entrypoint aegisdb-summarize
# summarize --dry-run` previews without writing.
ENTRYPOINT ["aegisdb-summarize"]