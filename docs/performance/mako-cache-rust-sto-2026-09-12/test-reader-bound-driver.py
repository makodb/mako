import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('reader_bound', Path(__file__).with_name('run-reader-bound.py'))
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class BoundChecks(unittest.TestCase):
    def test_exact_position_balanced_grid(self):
        self.assertEqual(driver.WORKERS, (1, 4, 32))
        grid = set()
        for workers in driver.WORKERS:
            for engine in driver.reader_pair.IDENTITIES:
                positions = {variant: [] for variant in driver.VARIANTS}
                for repetition in range(1, 4):
                    for position, variant in enumerate(driver.variant_order(repetition)):
                        grid.add((workers, engine, repetition, variant))
                        positions[variant].append(position)
                self.assertTrue(all(sorted(value) == [0, 1, 2] for value in positions.values()))
        self.assertEqual(len(grid), 54)
        self.assertEqual(driver.VARIANTS, ('base', 'reader', 'bound'))

    def test_each_duplicate_path_and_hash_is_rejected(self):
        binaries = {f'{engine}-{variant}': Path(f'/{engine}-{variant}')
                    for engine in driver.reader_pair.IDENTITIES for variant in driver.VARIANTS}
        hashes = {key: key for key in binaries}
        driver.validate_inputs(binaries, hashes)
        for engine in driver.reader_pair.IDENTITIES:
            for left, right in [('base', 'reader'), ('base', 'bound'), ('reader', 'bound')]:
                for field in ('path', 'hash'):
                    paths, values = dict(binaries), dict(hashes)
                    changed = paths if field == 'path' else values
                    changed[f'{engine}-{left}'] = changed[f'{engine}-{right}']
                    with self.subTest(engine=engine, left=left, right=right, field=field), self.assertRaises(RuntimeError):
                        driver.validate_inputs(paths, values)


if __name__ == '__main__':
    unittest.main()
