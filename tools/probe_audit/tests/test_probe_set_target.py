import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "probe_dump.exe" if os.name == "nt" else "probe_dump")


def settarget(seq):
    out = subprocess.check_output([EXE, "--settarget", "1", "8"] + [str(x) for x in seq], text=True)
    return [int(x) for x in out.split()]


@unittest.skipUnless(os.path.exists(EXE), "probe_dump not built")
class SetTarget(unittest.TestCase):
    def test_grow_shrink_clamp(self):
        # init 8 -> [8]; then set 4, 16, 0(->1), 99(->16)
        self.assertEqual(settarget([4, 16, 0, 99]), [8, 4, 16, 1, 16])


if __name__ == "__main__":
    unittest.main()
