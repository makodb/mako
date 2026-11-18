import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('reader_layout', Path(__file__).with_name('run-reader-layout.py'))
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class LayoutChecks(unittest.TestCase):
    def test_exact_position_balanced_grid(self):
        positions = {variant: [] for variant in driver.VARIANTS}
        grid = set()
        for repetition in range(1, 4):
            order = driver.variant_order(repetition)
            for index, variant in enumerate(order):
                positions[variant].append(index)
                for engine in driver.reader_pair.IDENTITIES:
                    grid.add((repetition, engine, variant))
        self.assertEqual(len(grid), 18)
        self.assertTrue(all(sorted(value) == [0, 1, 2] for value in positions.values()))

    def test_each_duplicate_path_and_hash_is_rejected(self):
        binaries = {f'{engine}-{variant}': Path(f'/{engine}-{variant}')
                    for engine in driver.reader_pair.IDENTITIES for variant in driver.VARIANTS}
        hashes = {key: key for key in binaries}
        driver.validate_inputs(binaries, hashes)
        for engine in driver.reader_pair.IDENTITIES:
            for left, right in [('base', 'reader'), ('base', 'matched'), ('reader', 'matched')]:
                for field in ('path', 'hash'):
                    paths, values = dict(binaries), dict(hashes)
                    changed = paths if field == 'path' else values
                    changed[f'{engine}-{left}'] = changed[f'{engine}-{right}']
                    with self.subTest(engine=engine, left=left, right=right, field=field), self.assertRaises(RuntimeError):
                        driver.validate_inputs(paths, values)


if __name__ == '__main__':
    unittest.main()
