# syntax=docker/dockerfile:1
#
# Prometheus exporter sidecar for AegisDB. The exporter is a single dependency-
# free stdlib script, so the image just carries it on a slim Python base — no
# pip install, no build step. Wired as the `metrics` compose profile:
#   docker compose --profile metrics up
FROM python:3.12-slim AS runtime

RUN useradd --system --no-create-home --user-group aegis

WORKDIR /app
COPY aegis_exporter.py /app/aegis_exporter.py

USER aegis
# /metrics is served on AEGIS_EXPORTER_PORT (default 9471); Prometheus scrapes it.
EXPOSE 9471
ENTRYPOINT ["python3", "/app/aegis_exporter.py"]