"""The native equivalence harness renders: its C++ is a %-format template, and a
stray `%` in it once failed a lift forty minutes in, after every verdict."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ps2decomp import equivalence  # noqa: E402


class TestHarness(unittest.TestCase):
    def test_the_driver_and_bodies_render(self):
        body = "void F_00100000(lifted::Context& c) { c.r[2] = 1; }"
        driver, bodies = equivalence.sources({0x00100000: (body, body, 0)})
        self.assertIn("P_00100000", driver)
        self.assertIn("S_00100000", driver)
        self.assertIn("% 9", driver)               # the small-value cases survive formatting
        self.assertEqual(len(bodies), 1)
        self.assertIn("void P_00100000", bodies[0])

    def test_the_check_builds_and_tells_forms_apart(self):
        # Built and run, with the toolchain at hand: on Linux the driver kept
        # MSVC's exports, did not build, and every lift skipped the check.
        import tempfile

        from ps2decomp import cppcheck
        try:
            cppcheck.toolchain()
        except Exception as exc:                   # no compiler here
            self.skipTest(str(exc))
        same = "void F_00100000(lifted::Context& c) { c.r[2] = c.r[4] + 1; }"
        other = "void F_00100000(lifted::Context& c) { c.r[2] = c.r[4] + 2; }"
        with tempfile.TemporaryDirectory() as work:
            found = equivalence.check({0x00100000: (same, same, 0), 0x00100004: (same.replace("00100000", "00100004"),
                                       other.replace("00100000", "00100004"), 0)}, work, cases=50)
        self.assertIsNone(found[0x00100000])
        self.assertIsNotNone(found[0x00100004])


if __name__ == "__main__":
    unittest.main()
