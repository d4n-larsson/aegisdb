"""Unit tests for the stats-JSON -> Prometheus exposition renderer.

Pure, no network: feed a representative ``stats`` response and assert the
exposition text is well-formed and carries the expected samples. Stdlib
unittest, so no pytest or install needed.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from aegis_exporter import Config, _esc_label, _fmt, render  # noqa: E402


# A representative single-node stats response (quotas + replication omitted, as
# on a plain single-tenant server) plus a tenant/replication-bearing variant.
SAMPLE = {
    "version": "0.4.7", "phase": 4, "uptime_ms": 12345,
    "durability": "interval", "records": 7, "tombstones": 2,
    "log_bytes": 4096, "log_flush_pending": False, "next_id": 8,
    "indexes": {"time": 7, "tags": 3, "semantic": 7, "working": 0},
    "memory": {"hash_bytes": 100, "time_bytes": 200, "tag_bytes": 300,
               "semantic_bytes": 400, "index_bytes_total": 1000,
               "index_bytes_limit": 55000},
    "metrics": {"requests": 42, "errors": 1, "unauthorized": 0,
                "dispatch_micros": 5000000,
                "memories_merged": 4, "memories_forgotten": 11, "memories_purged": 2,
                "by_op": {"insert": 7, "get": 30, "stats": 5}},
}


def _lines(text):
    return [ln for ln in text.splitlines() if ln and not ln.startswith("#")]


def _value(text, needle):
    for ln in _lines(text):
        if ln.startswith(needle) and ln[len(needle)] in " {":
            return ln.rsplit(" ", 1)[1]
    return None


class TestFormat(unittest.TestCase):
    def test_fmt_bool(self):
        self.assertEqual(_fmt(True), "1")
        self.assertEqual(_fmt(False), "0")

    def test_fmt_int_stays_integral(self):
        self.assertEqual(_fmt(42), "42")
        self.assertEqual(_fmt(8), "8")

    def test_fmt_float_roundtrips(self):
        self.assertEqual(float(_fmt(1.5)), 1.5)

    def test_esc_label(self):
        self.assertEqual(_esc_label('a"b\\c\nd'), 'a\\"b\\\\c\\nd')


class TestRender(unittest.TestCase):
    def test_up_and_selfmetrics(self):
        text = render(SAMPLE, up=True, scrape_seconds=0.01)
        self.assertEqual(_value(text, "aegisdb_up"), "1")
        self.assertIsNotNone(_value(text, "aegisdb_scrape_duration_seconds"))

    def test_core_gauges(self):
        text = render(SAMPLE, up=True)
        self.assertEqual(_value(text, "aegisdb_records"), "7")
        self.assertEqual(_value(text, "aegisdb_tombstones"), "2")
        self.assertEqual(_value(text, "aegisdb_log_bytes"), "4096")
        self.assertEqual(_value(text, "aegisdb_next_id"), "8")
        # uptime converted ms -> s
        self.assertEqual(float(_value(text, "aegisdb_uptime_seconds")), 12.345)

    def test_info_metric_labels(self):
        text = render(SAMPLE, up=True)
        self.assertIn('aegisdb_info{version="0.4.7",phase="4",'
                      'durability="interval"} 1', text)

    def test_index_bytes_labeled(self):
        text = render(SAMPLE, up=True)
        self.assertIn('aegisdb_index_bytes{index="semantic"} 400', text)
        self.assertEqual(_value(text, "aegisdb_index_bytes_total"), "1000")
        self.assertEqual(_value(text, "aegisdb_index_bytes_limit"), "55000")

    def test_counters_and_dispatch_seconds(self):
        text = render(SAMPLE, up=True)
        self.assertEqual(_value(text, "aegisdb_requests_total"), "42")
        self.assertEqual(_value(text, "aegisdb_errors_total"), "1")
        # 5_000_000 micros -> 5.0 seconds
        self.assertEqual(float(_value(text, "aegisdb_dispatch_seconds_total")), 5.0)
        self.assertIn('aegisdb_requests_by_op_total{op="get"} 30', text)

    def test_memory_quality_counters(self):
        text = render(SAMPLE, up=True)
        self.assertEqual(_value(text, "aegisdb_memories_merged_total"), "4")
        self.assertEqual(_value(text, "aegisdb_memories_forgotten_total"), "11")
        self.assertEqual(_value(text, "aegisdb_memories_purged_total"), "2")
        self.assertIn("# TYPE aegisdb_memories_forgotten_total counter", text)

    def test_memory_quality_absent_when_missing(self):
        # older servers without these fields -> the metrics are simply omitted
        s = dict(SAMPLE, metrics={"requests": 1, "errors": 0, "unauthorized": 0})
        text = render(s, up=True)
        self.assertNotIn("aegisdb_memories_forgotten_total", text)

    def test_type_headers_present(self):
        text = render(SAMPLE, up=True)
        self.assertIn("# TYPE aegisdb_requests_total counter", text)
        self.assertIn("# TYPE aegisdb_records gauge", text)

    def test_down_emits_only_liveness(self):
        text = render({}, up=False, scrape_seconds=0.5, error="UNAUTHORIZED")
        self.assertEqual(_value(text, "aegisdb_up"), "0")
        self.assertIn('aegisdb_scrape_error{error="UNAUTHORIZED"} 1', text)
        # No server metrics when down.
        self.assertIsNone(_value(text, "aegisdb_records"))

    def test_tenants_and_replication_when_present(self):
        stats = dict(SAMPLE,
                     tenants=[{"namespace": "team-a", "records": 5, "bytes": 900}],
                     replication={"role": "replica", "connected": True,
                                  "applied_offset": 10, "primary_offset": 25,
                                  "lag_bytes": 15})
        text = render(stats, up=True)
        self.assertIn('aegisdb_tenant_records{namespace="team-a"} 5', text)
        self.assertIn('aegisdb_replication_lag_bytes{role="replica"} 15', text)
        self.assertEqual(
            _value(text.replace('{role="replica"}', ""),
                   "aegisdb_replication_connected"), "1")

    def test_optional_blocks_absent_by_default(self):
        text = render(SAMPLE, up=True)
        self.assertNotIn("aegisdb_tenant_records", text)
        self.assertNotIn("aegisdb_replication", text)


class TestConfig(unittest.TestCase):
    def test_defaults(self):
        cfg = Config(env={})
        self.assertEqual(cfg.host, "127.0.0.1")
        self.assertEqual(cfg.port, 9470)
        self.assertEqual(cfg.exporter_port, 9471)
        self.assertEqual(cfg.auth_token, "")

    def test_env_override(self):
        cfg = Config(env={"AEGIS_HOST": "aegisdb", "AEGIS_PORT": "1234",
                          "AEGIS_EXPORTER_PORT": "9999",
                          "AEGIS_AUTH_TOKEN": "sekret"})
        self.assertEqual(cfg.host, "aegisdb")
        self.assertEqual(cfg.port, 1234)
        self.assertEqual(cfg.exporter_port, 9999)
        self.assertEqual(cfg.auth_token, "sekret")


if __name__ == "__main__":
    unittest.main()