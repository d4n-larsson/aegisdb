# Convenience targets for the AegisDB ↔ Claude Code integration.
# Tests are stdlib unittest, so they run without pytest or any extra install.

PYTHON ?= python3

.PHONY: test unit integration contract

# Full suite (unit + contract + integration). Integration/contract tests launch
# the aegisdb binary from ../../build and skip automatically if it is absent.
# Each subdir is discovered separately (they are not Python packages).
test:
	@fail=0; \
	for d in unit contract integration; do \
		echo "== tests/$$d =="; \
		$(PYTHON) -m unittest discover -s tests/$$d -p 'test_*.py' || fail=1; \
	done; \
	exit $$fail

unit:
	$(PYTHON) -m unittest discover -s tests/unit -p 'test_*.py'

integration:
	$(PYTHON) -m unittest discover -s tests/integration -p 'test_*.py'

contract:
	$(PYTHON) -m unittest discover -s tests/contract -p 'test_*.py'