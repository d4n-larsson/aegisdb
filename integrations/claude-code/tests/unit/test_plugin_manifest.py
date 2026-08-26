"""The plugin and marketplace manifests, checked the way a user's install would.

These are the only files in the repo whose consumer is a *package manager*: a
typo in them is not a stack trace, it is `/plugin install` failing on somebody
else's machine, or — worse — succeeding and showing them the wrong thing. Real
installs are how the two bugs pinned here were found, so what is asserted is what
an install actually reads.

The version check is the load-bearing one. `plugin.json` carries a version by
hand, and nothing about publishing a release touches it: the first install from
the published marketplace reported `0.8.3` on the day v0.9.0 shipped. Tying it
to the newest release notes catches that at review time, on the release-prep PR
that adds them, which is the one moment someone is already thinking about the
version.
"""
import json
import os
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
PKG_ROOT = os.path.dirname(os.path.dirname(_HERE))       # integrations/claude-code
REPO_ROOT = os.path.dirname(os.path.dirname(PKG_ROOT))

PLUGIN = os.path.join(PKG_ROOT, ".claude-plugin", "plugin.json")
MARKETPLACE = os.path.join(REPO_ROOT, ".claude-plugin", "marketplace.json")
VERSIONS = os.path.join(REPO_ROOT, "docs", "versions")


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def newest_release() -> str:
    """The highest `docs/versions/vX.Y.Z.md`, by number rather than by string —
    v0.10.0 must not sort below v0.9.0."""
    versions = [f[1:-3] for f in os.listdir(VERSIONS)
                if f.startswith("v") and f.endswith(".md")]
    return max(versions, key=lambda v: [int(x) for x in v.split(".")])


class TestPluginManifest(unittest.TestCase):
    def test_it_declares_what_an_install_shows(self):
        doc = load(PLUGIN)
        self.assertEqual(doc["name"], "aegisdb-memory")
        for field in ("version", "description", "author"):
            self.assertTrue(doc.get(field), f"plugin.json needs {field}")

    def test_the_version_matches_the_newest_release_notes(self):
        """Found by installing from the published marketplace on release day:
        `claude plugin details` said 0.8.3 while v0.9.0 was live. Nothing in
        tagging or publishing touches this file, so only a check keeps it true.

        If this fails on a release-prep PR, bump `plugin.json` — that is the
        point of it failing there."""
        self.assertEqual(load(PLUGIN)["version"], newest_release())


class TestMarketplaceManifest(unittest.TestCase):
    def test_it_has_what_the_schema_requires(self):
        doc = load(MARKETPLACE)
        self.assertTrue(doc.get("name"))
        self.assertTrue((doc.get("owner") or {}).get("name"))
        self.assertTrue(doc.get("plugins"))

    def test_every_entry_points_at_a_real_plugin(self):
        """A relative source resolves from the marketplace *root* — the
        directory holding `.claude-plugin/`, not `.claude-plugin/` itself. Off
        by one directory and the install fails only on a user's machine."""
        for entry in load(MARKETPLACE)["plugins"]:
            self.assertTrue(entry.get("name"))
            source = entry["source"]
            self.assertIsInstance(source, str,
                                  "a non-path source needs its own check")
            root = os.path.normpath(os.path.join(REPO_ROOT, source))
            self.assertTrue(os.path.isdir(root), f"{source} is not a directory")
            self.assertTrue(
                os.path.isfile(os.path.join(root, ".claude-plugin",
                                            "plugin.json")),
                f"{source} has no .claude-plugin/plugin.json")

    def test_the_two_manifests_agree_on_the_name(self):
        """`/plugin install <name>@<marketplace>` uses the marketplace's name
        for the plugin; a disagreement means the install string in the README
        names something the plugin itself does not answer to."""
        names = [e["name"] for e in load(MARKETPLACE)["plugins"]]
        self.assertIn(load(PLUGIN)["name"], names)


class TestComponentsExist(unittest.TestCase):
    """What the inventory promised on a real install: two components, no hooks
    and no MCP servers. The absences are deliberate — a globally registered
    server or hook would collide with the per-project ones `aegisdb-init`
    writes, giving two `memory` servers and hooks that fire twice."""

    def test_the_skill_and_the_command_are_where_a_plugin_root_expects_them(self):
        self.assertTrue(os.path.isfile(
            os.path.join(PKG_ROOT, "skills", "aegis-setup", "SKILL.md")))
        self.assertTrue(os.path.isfile(
            os.path.join(PKG_ROOT, "commands", "aegis-doctor.md")))

    def test_it_registers_no_hooks_and_no_mcp_servers(self):
        self.assertFalse(os.path.exists(os.path.join(PKG_ROOT, "hooks",
                                                     "hooks.json")))
        # `.mcp.json` at a plugin root registers servers for every project the
        # plugin is enabled in. The repo's own is a different file, at the root.
        self.assertFalse(os.path.exists(os.path.join(PKG_ROOT, ".mcp.json")))


if __name__ == "__main__":
    unittest.main()
