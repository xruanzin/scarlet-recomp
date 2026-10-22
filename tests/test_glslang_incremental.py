import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('scarlet_setup', Path(__file__).resolve().parents[1] / 'scripts/setup.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)

class GlslangIncrementalTest(unittest.TestCase):
    def test_missing_unchanged_and_modified_inputs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            work, deps = root / 'work', root / 'deps'
            work.mkdir()
            (deps / 'include').mkdir(parents=True)
            (deps / 'lib').mkdir()
            inputs = [work / 'build_glslang.bat', work / 'vk_glslang_shim.c', deps / 'include/api.h', deps / 'lib/glslang.lib']
            for p in inputs:
                p.write_text('fixture')
                os.utime(p, ns=(1000000000, 1000000000))
            self.assertTrue(setup.glslang_needs_build(work, deps))
            dll = work / 'ssbu_glslang.dll'
            dll.write_text('fixture')
            os.utime(dll, ns=(2000000000, 2000000000))
            self.assertFalse(setup.glslang_needs_build(work, deps))
            os.utime(inputs[-1], ns=(3000000000, 3000000000))
            self.assertTrue(setup.glslang_needs_build(work, deps))
            os.utime(inputs[-1], ns=(1000000000, 1000000000))
            inputs[1].unlink()
            self.assertTrue(setup.glslang_needs_build(work, deps))
