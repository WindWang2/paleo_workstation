"""Numerical regression for the explicitly requested source preparation tool."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("prepare", Path(__file__).parents[1] / "tools/prepare_comp_data.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class FactorIntervals(unittest.TestCase):
    def test_clipping_and_same_class_overlap_are_counted_once(self):
        result = prepare.interval_factors(10, 30, [(0, 20, "sand"), (15, 25, "sand"), (25, 40, "mud")])
        self.assertEqual(result["sand_thick"], 15)
        self.assertEqual(result["sand_ratio"], .75)
        self.assertEqual(result["max_sand_thick"], 15)
        self.assertEqual(result["sand_beds"], 1)

    def test_uncovered_and_conflicting_intervals_do_not_become_zero_sand(self):
        for segments in [[(0, 5, "mud")], [(0, 10, "sand"), (4, 6, "mud")], [(0, 10, "unknown")]]:
            result = prepare.interval_factors(0, 10, segments)
            self.assertIsNone(result["sand_thick"])
            self.assertIsNone(result["sand_ratio"])

    def test_real_zero_sand_is_distinct_from_missing(self):
        result = prepare.interval_factors(0, 10, [(0, 10, "mud")])
        self.assertEqual(result["sand_thick"], 0)
        self.assertEqual(result["sand_ratio"], 0)
        self.assertEqual(result["coverage_ratio"], 1)

    def test_mud_rich_and_sand_rich_names_are_not_swapped(self):
        self.assertEqual(prepare.lithology_kind("粉砂质泥岩"), "mud")
        self.assertEqual(prepare.lithology_kind("泥质粉砂岩"), "sand")


if __name__ == "__main__":
    unittest.main()
