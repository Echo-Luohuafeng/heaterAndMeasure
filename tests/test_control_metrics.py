from pathlib import Path
import importlib.util
import unittest
spec=importlib.util.spec_from_file_location('evaluate_control',Path(__file__).resolve().parents[1]/'tools/evaluate_control.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class MetricsTests(unittest.TestCase):
    def rows(self):
        rows=[]
        for k in range(101):
            row={'mode':'1','boot_id':'boot','run_id':'1','config_id':'2','algorithm':'1',
                 'cycle_start_us':str(k*20000),'valid_mask':'15','limited_mask':'0','blocked_mask':'0','computation_us':'100'}
            for i in range(1,5):row.update({f'R{i}':'110.1',f'target{i}':'110',f'duty{i}':'1.5'})
            rows.append(row)
        return rows
    def test_known_error_and_output_integrals(self):
        c=module.evaluate(self.rows(),.2,1)['experiments'][0]['channels'][0]
        self.assertAlmostEqual(c['rmse_ohm'],.1)
        self.assertAlmostEqual(c['iae_ohm_s'],.2)
        self.assertAlmostEqual(c['ise_ohm2_s'],.02)
        self.assertAlmostEqual(c['pwm_integral_percent_s'],3)
        self.assertEqual(c['settling_time_s'],0)
    def test_gap_restarts_settling_window(self):
        rows=self.rows()
        for k,r in enumerate(rows):
            if k>=25:r['cycle_start_us']=str(int(r['cycle_start_us'])+1000000)
        c=module.evaluate(rows,.2,1)['experiments'][0]['channels'][0]
        self.assertAlmostEqual(c['settling_time_s'],1.5)
        self.assertEqual(c['missing_intervals'],1)
    def test_manual_runs_excluded(self):
        rows=self.rows()
        for r in rows:r['mode']='0'
        self.assertEqual(module.evaluate(rows)['experiments'],[])
if __name__=='__main__':unittest.main()
