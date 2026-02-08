import unittest

from config_parser import parse_config


class ConfigParserTests(unittest.TestCase):
    def test_parses_minimal_config(self):
        raw = '{"indexRoot":"/Users/rex"}'
        cfg = parse_config(raw)
        self.assertEqual(cfg["indexRoot"], "/Users/rex")

    def test_raises_on_empty(self):
        with self.assertRaises(ValueError):
            parse_config("")


if __name__ == "__main__":
    unittest.main()
# note line 1
# note line 2
