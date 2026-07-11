import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "ui_dump.exe" if os.name == "nt" else "ui_dump")

def run(script):
    # script: space-separated ops; ui_dump prints the view after each. See ui_dump.c.
    return subprocess.check_output([EXE] + script.split(), text=True).split()

@unittest.skipUnless(os.path.exists(EXE), "ui_dump not built")
class UI(unittest.TestCase):
    def test_home_is_default(self):
        self.assertEqual(run("reset")[0], "HOME")
    def test_select_jumps_to_view(self):
        # reset, select STATS(3), select HUNTERS(1)
        self.assertEqual(run("reset select 3 select 1"), ["HOME","STATS","RADAR"])
    def test_input_returns_home(self):
        self.assertEqual(run("reset select 4 input"), ["HOME","LIBRARY","HOME"])
    def test_idle_returns_home(self):
        # select STATS then tick after idle -> HOME
        self.assertEqual(run("reset select 3 idle_tick"), ["HOME","STATS","HOME"])
    def test_new_follower_wakes_to_hunters_when_idle(self):
        # on HOME, idle, a new follower -> RADAR (Hunters)
        self.assertEqual(run("reset idle follower1"), ["HOME","HOME","RADAR"])
