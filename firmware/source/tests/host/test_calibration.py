"""Numerical/error-path verification with explicitly synthetic inputs, no hardware claim."""
import importlib.util, pathlib, unittest
p=pathlib.Path(__file__).resolve().parents[2]/'tools/CODEX_fit_ina226_calibration.py'
s=importlib.util.spec_from_file_location('cal',p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class Calibration(unittest.TestCase):
    def test_known_synthetic_gain_and_offset(self):
        rows=[dict(reference_current_a=x,raw_shunt=(0.0102*x+0.000005)/2.5e-6,reference_instrument='SYNTHETIC TEST') for x in [0,0.1,0.4]]
        r=m.fit(rows);self.assertAlmostEqual(r['r_shunt_ohm'],0.0102);self.assertAlmostEqual(r['offset_shunt_v'],0.000005);self.assertEqual(r['status'],'CANDIDATE_NOT_APPLIED')
    def test_bad_or_unproven_input(self):
        for rows in [[],[dict(reference_current_a=0,raw_shunt=0,reference_instrument='')]*3,
                     [dict(reference_current_a=x,raw_shunt=-x*4000,reference_instrument='SYNTHETIC') for x in [0,.1,.4]]]:
            with self.assertRaises(ValueError):m.fit(rows)
if __name__=='__main__':unittest.main()
