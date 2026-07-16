# README images

Drop the Grafana dashboard screenshot here as `dashboard.png`, then uncomment
the `<img>` slot near the top of the repo `README.md`.

To capture it: `docker compose --profile monitoring up`, open
<http://127.0.0.1:3000>, let the timeseries panels fill in for a few minutes
(send some traffic so the request/op panels move), then screenshot the dashboard
in dark theme at a wide viewport.