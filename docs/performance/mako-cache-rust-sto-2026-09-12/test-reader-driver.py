import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


driver = load('reader_driver', 'run-reader-fence.py')
fixtures = load('main_fixtures', 'test-main-driver.py')


class ReaderChecks(unittest.TestCase):
    def test_duplicate_variant_paths_or_hashes_are_rejected(self):
        binaries = {f'{engine}-{variant}': Path(f'/{engine}-{variant}')
                    for engine in driver.IDENTITIES for variant in ('base', 'reader')}
        hashes = {key: key for key in binaries}
        driver.validate_variant_inputs(binaries, hashes)
        for engine in driver.IDENTITIES:
            for field in ('paths', 'hashes'):
                bad_paths, bad_hashes = dict(binaries), dict(hashes)
                changed = bad_paths if field == 'paths' else bad_hashes
                changed[f'{engine}-reader'] = changed[f'{engine}-base']
                with self.subTest(engine=engine, field=field), self.assertRaises(RuntimeError):
                    driver.validate_variant_inputs(bad_paths, bad_hashes)

    def test_complete_paired_order(self):
        cells = [(r, w, engine, variant) for r in range(1, 4) for w in (1, 4, 32)
                 for engine, variant in driver.order(r)]
        self.assertEqual(len(cells), 36)
        self.assertEqual(len(set(cells)), 36)
        self.assertEqual(driver.order(2), list(reversed(driver.order(1))))
        for r in range(1, 4):
            order = driver.order(r)
            self.assertEqual(order[0][0], order[1][0])
            self.assertEqual(order[2][0], order[3][0])

    def test_exact_reader_validation_and_failure_retention(self):
        for engine in ('c-abi', 'sto'):
            for defect in (None, 'identity', 'sample_checksum', 'sample_bytes',
                           'recovery_checksum', 'conflicts', 'warmup_conflicts',
                           'hash', 'child'):
                with self.subTest(engine=engine, defect=defect), tempfile.TemporaryDirectory() as temporary:
                    directory = Path(temporary).resolve()
                    binary = directory / 'binary'
                    binary.touch()
                    config = driver.main_pair.configuration('read', 1, 'low', 4)
                    initial = {'boost': '0', 'governors': {str(c): 'schedutil' for c in range(64)}}
                    calls = []

                    def execute(command, phase_dir):
                        calls.append(command)
                        if phase_dir.name == 'sample':
                            (directory / 'rocks').mkdir()
                            record = fixtures.sample(config, driver.IDENTITIES[engine])
                            protocol = driver.main_pair.SAMPLE_PROTOCOL
                            if defect == 'identity':
                                record['engine'] = 'wrong'
                            if defect == 'sample_checksum':
                                record['checksum'] = 1
                            if defect == 'sample_bytes':
                                record['log_key_bytes'] = 0
                            if defect in ('conflicts', 'warmup_conflicts'):
                                record[defect] = 1
                        else:
                            record = fixtures.recovery(config, driver.IDENTITIES[engine])
                            protocol = driver.main_pair.RECOVERY_PROTOCOL
                            self.assertEqual(command[-1], '0')
                            if defect == 'recovery_checksum':
                                record['checksum'] = 1
                        if defect == 'child':
                            raise RuntimeError('child failed')
                        (phase_dir / 'stdout.txt').write_text(fixtures.encode(record, protocol))
                        return {'completed': True, 'returncode': 0}

                    def digest(path):
                        return 'changed' if defect == 'hash' and calls and path == binary else 'frozen'

                    with mock.patch.object(driver.common, 'machine', return_value=initial), \
                         mock.patch.object(driver.common, 'digest', side_effect=digest), \
                         mock.patch.object(driver.common, 'execute', side_effect=execute):
                        if defect is None:
                            result = driver.run_cell(binary, 'frozen', engine, config, directory, initial)
                            self.assertTrue(result['completed'] and result['scratch_cleaned'])
                        else:
                            with self.assertRaises(RuntimeError):
                                driver.run_cell(binary, 'frozen', engine, config, directory, initial)
                    self.assertEqual((directory / 'rocks').exists(), defect is not None)
                    self.assertEqual(len(calls), 2 if defect in (None, 'recovery_checksum') else 1)
                    self.assertEqual(calls[0][4:6], ['-c', '0-3'])


if __name__ == '__main__':
    unittest.main()
