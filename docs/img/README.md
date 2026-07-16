# README images

`dashboard.png` — the bundled Grafana dashboard, shown at the top of the repo
`README.md`.

To refresh it: `docker compose --profile monitoring up`, open
<http://127.0.0.1:3000>, let the timeseries panels fill in for a few minutes
(send some traffic so the request/op panels move), then screenshot the dashboard
in dark theme at a wide viewport.