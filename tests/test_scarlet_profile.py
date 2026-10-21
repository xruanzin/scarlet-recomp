import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import scarlet_profile as profile
import scarlet_ingest as ingest


class ScarletProfileTest(unittest.TestCase):
    def test_known_target(self):
        self.assertEqual(profile.TITLE_ID_HEX, "0100A3D008C5C000")
        self.assertEqual(profile.BASE_VERSION, "1.0.0")
        self.assertEqual(profile.STATIC_MODULES, ("main", "sdk", "subsdk0"))
        self.assertTrue(profile.matches_main_build_id(profile.MAIN_BUILD_ID))
        self.assertTrue(profile.matches_main_build_id(profile.MAIN_BUILD_ID_PREFIX))
        self.assertFalse(profile.matches_main_build_id("00" * 20))

    def test_read_nso_build_id(self):
        raw = bytearray(0x60)
        raw[:4] = b"NSO0"
        raw[0x40:0x54] = bytes.fromhex(profile.MAIN_BUILD_ID)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "main")
            with open(p, "wb") as f:
                f.write(raw)
            self.assertEqual(ingest.read_nso_build_id(p), profile.MAIN_BUILD_ID)

    def test_find_exefs(self):
        with tempfile.TemporaryDirectory() as d:
            os.mkdir(os.path.join(d, "exefs"))
            self.assertEqual(ingest.find_exefs(d), os.path.join(d, "exefs"))


if __name__ == "__main__":
    unittest.main()
