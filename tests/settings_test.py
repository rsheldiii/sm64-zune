"""tools/settings.py: reading platform/zune_settings.h and checking what a build asks for."""
import os
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import settings


def values(*defines):
    return {setting.name: value for setting, value, _ in settings.resolve(defines)}


class SettingsTest(unittest.TestCase):
    def setUp(self):
        os.environ.pop('SM64ZUNE_HOST_ADDRESS', None)
        settings.LOCAL = Path('/nonexistent/settings.local')   # the tests must not depend on yours

    def test_the_defaults_are_those_of_the_header(self):
        self.assertEqual(values(), {'ZUNE_FRAME_SKIP': 1, 'ZUNE_TOUCH_BUTTONS': 2, 'ZUNE_TOUCH_OPACITY': 80,
                                    'ZUNE_LOG': 0, 'ZUNE_LOG_HOST': '', 'ZUNE_PROFILE': 0})

    def test_words_and_numbers(self):
        chosen = values('ZUNE_FRAME_SKIP=never', 'ZUNE_TOUCH_BUTTONS=1', 'ZUNE_TOUCH_OPACITY=10')
        self.assertEqual((chosen['ZUNE_FRAME_SKIP'], chosen['ZUNE_TOUCH_BUTTONS'], chosen['ZUNE_TOUCH_OPACITY']), (0, 1, 10))

    def test_the_last_define_wins(self):
        self.assertEqual(values('ZUNE_TOUCH_OPACITY=20', 'ZUNE_TOUCH_OPACITY=30')['ZUNE_TOUCH_OPACITY'], 30)

    def test_mistakes_stop_the_build(self):
        for define in ('ZUNE_FRAME_SKIP=3', 'ZUNE_FRAME_SKIP=sometimes', 'ZUNE_TOUCH_OPACITY=9', 'ZUNE_TOUCH_OPACITY=101',
                       'ZUNE_FRAMESKIP=1', 'ZUNE_FRAME_SKIP', '=1', 'ZUNE_LOG=2', 'ZUNE_LOG_HOST=example.com',
                       'ZUNE_LOG_HOST=1.2.3', 'ZUNE_LOG_HOST=1.2.3.4"', 'ZUNE_PROFILE=on'):
            with self.assertRaises(SystemExit, msg=define):
                values(define)

    def test_a_log_needs_somewhere_to_go(self):
        with self.assertRaises(SystemExit):
            values('ZUNE_LOG=on')
        os.environ['SM64ZUNE_HOST_ADDRESS'] = '192.168.7.9'
        self.assertEqual(values('ZUNE_LOG=on')['ZUNE_LOG_HOST'], '192.168.7.9')
        self.assertEqual(values('ZUNE_LOG=on', 'ZUNE_LOG_HOST=10.0.0.2')['ZUNE_LOG_HOST'], '10.0.0.2')
        self.assertEqual(values()['ZUNE_LOG_HOST'], '')   # no log, no address in the game
        self.assertEqual(values('ZUNE_LOG=on', 'ZUNE_PROFILE=on')['ZUNE_PROFILE'], 1)

    def test_the_header_a_build_gets(self):
        os.environ['SM64ZUNE_HOST_ADDRESS'] = '192.168.7.9'
        header = settings.build_header('20260101-000000', settings.resolve(['ZUNE_LOG=on']))
        for line in ('#define ZUNE_BUILD "20260101-000000"', '#define ZUNE_LOG 1', '#define ZUNE_LOG_HOST "192.168.7.9"',
                     '#define ZUNE_FRAME_SKIP 1',
                     '#define ZUNE_SETTINGS "ZUNE_FRAME_SKIP=1 ZUNE_TOUCH_BUTTONS=2 ZUNE_TOUCH_OPACITY=80 ZUNE_LOG=1 '
                     'ZUNE_LOG_HOST=192.168.7.9 ZUNE_PROFILE=0"'):
            self.assertIn(line + '\n', header)


if __name__ == '__main__':
    unittest.main()
