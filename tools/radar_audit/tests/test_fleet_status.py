import os, subprocess, unittest
HERE=os.path.dirname(__file__); TOOL=os.path.dirname(HERE)
EXE=os.path.join(TOOL,"fleet_dump.exe" if os.name=="nt" else "fleet_dump")
def run(s): return subprocess.check_output([EXE]+s.split(),text=True).strip().splitlines()

@unittest.skipUnless(os.path.exists(EXE),"fleet_dump not built")
class FS(unittest.TestCase):
    def test_upsert_counts_distinct_nodes(self):
        # upsert node 5 (12 devices) then node 7 (8) -> count 2
        self.assertEqual(run("up 5 12 up 7 8 count"), ["2"])
    def test_upsert_same_node_updates_not_adds(self):
        self.assertEqual(run("up 5 12 up 5 16 count at0"), ["1","id=5 dev=16 alive=1"])
    def test_stale_node_reads_not_alive(self):
        # upsert node 5, advance past stale, query
        self.assertEqual(run("up 5 12 wait at0"), ["id=5 dev=12 alive=0"])
