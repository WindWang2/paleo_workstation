"""Audit screening must not turn missing/synthetic input into measured evidence."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('audit', Path(__file__).resolve().parents[1] /
                                            'tools/audit_well_facies_inputs.py')
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class AuditTests(unittest.TestCase):
    def test_missing_md_never_extrapolates(self):
        row = dict(well_name='W1', horizon='H1', top_md='', base_md='12')
        result = audit.screen(row, list(range(20)), [70]*20, [(0, 20, '')], 1, 4)
        self.assertFalse(result['ready'])
        self.assertIn('MD', result['reason'])

    def test_nulls_and_depth_gaps_break_window(self):
        row = dict(well_name='W1', horizon='H1', top_md='0', base_md='20')
        result = audit.screen(row, [0, 1, 2, 3, 10, 11, 12],
                             [70, 70, None, 70, 70, 70, 70], [(0, 20, '')], 1, 4)
        self.assertEqual(result['longest_supported_run'], 3)
        self.assertFalse(result['ready'])

    def test_lithology_gap_breaks_window(self):
        row = dict(well_name='W1', horizon='H1', top_md='0', base_md='8')
        result = audit.screen(row, list(range(8)), [70]*8, [(0, 3, ''), (5, 8, '')], 1, 4)
        self.assertFalse(result['ready'])
        self.assertEqual(result['lithology_coverage'], .75)

    def test_synthetic_input_can_run_but_remains_flagged(self):
        row = dict(well_name='W1', horizon='H1', top_md='0', base_md='8')
        result = audit.screen(row, list(range(8)), [70]*8, [(0, 8, 'GR推导（合成）')], 1, 4)
        self.assertTrue(result['ready'])
        self.assertEqual(result['synthetic_lithology_coverage'], 1)

    def test_las_header_step_does_not_replace_actual_grid(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'W1.las'
            path.write_text('~Well\nSTEP.M 1 :\nNULL. -9999.99 :\n'
                            '# PALEO_SYNTHETIC_INPUT test\n~Curve\nDEPT.M :\nGR.API :\n'
                            '~A\n0 70\n0.125 -9999.99\n0.25 80\n', encoding='utf-8')
            profile, _, values = audit.las_profile(path)
            self.assertEqual(profile['actual_median_step'], .125)
            self.assertTrue(profile['step_disagreement'])
            self.assertEqual(profile['provenance'], 'explicit_synthetic')
            self.assertIsNone(values[1])

    def test_overlapping_lithology_never_double_counts(self):
        self.assertEqual(audit.merged_length([(0, 8, ''), (3, 12, '')], 0, 10), 10)


if __name__ == '__main__':
    unittest.main()
