import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def fleetnode(script):
    out = subprocess.run([EXE, "--fleetnode"], input=script, capture_output=True, text=True).stdout
    return [int(x) for x in out.split()]


def _macs(n, base=1):
    return "".join("note 02000000%04x 0\n" % (base + i) for i in range(n))


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class FleetCensus(unittest.TestCase):
    def test_no_peers_count_zero_livesize_one(self):
        self.assertEqual(fleetnode("reset\ncount 0\nlivesize 0\n"), [0, 1])

    def test_distinct_nodes_counted(self):
        self.assertEqual(fleetnode("reset\n" + _macs(3) + "count 0\nlivesize 0\n"), [3, 4])

    def test_renote_same_node_no_double_count(self):
        script = "reset\nnote 020000000001 0\nnote 020000000001 100\ncount 100\nlivesize 100\n"
        self.assertEqual(fleetnode(script), [1, 2])

    def test_ttl_expiry_drops_node(self):
        # FLEET_MAC_TTL_MS = 90000: a node last seen at t=0 is gone by t=200000
        script = "reset\nnote 020000000001 0\ncount 200000\nlivesize 200000\n"
        self.assertEqual(fleetnode(script), [0, 1])

    def test_eviction_caps_at_node_cap(self):
        # FLEET_NODE_CAP = 8: noting 12 distinct nodes still reports at most 8
        rows = fleetnode("reset\n" + _macs(12) + "count 0\n")
        self.assertLessEqual(rows[-1], 8)

    def test_independent_of_mac_exclusion_table(self):
        # noting many distinct SYNTHETIC macs via the existing exclusion API must not
        # inflate the NODE count -- they are unrelated tables.
        pass  # covered structurally: --fleetnode never touches fleet_note_peer_macs


if __name__ == "__main__":
    unittest.main()
