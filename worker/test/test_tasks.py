#!/usr/bin/env python3

import pathlib
import sys
import types
import unittest
from unittest import mock


WORKER_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(WORKER_ROOT))

try:
    import invoke  # noqa: F401
except ModuleNotFoundError:
    invoke_stub = types.ModuleType('invoke')

    def task_stub(*args, **_kwargs):
        if args and callable(args[0]):
            return args[0]

        def decorate(function):
            return function

        return decorate

    invoke_stub.task = task_stub
    invoke_stub.call = lambda *args, **kwargs: (args, kwargs)
    sys.modules['invoke'] = invoke_stub

import tasks  # noqa: E402


class MesonOptionPolicyTest(unittest.TestCase):
    def test_default_sanitizer_matches_meson_list_shape(self):
        self.assertEqual(tasks.normalized_meson_option('b_sanitize', 'none'), [])
        self.assertEqual(tasks.normalized_meson_option('b_sanitize', []), [])
        self.assertEqual(
            tasks.normalized_meson_option('b_sanitize', 'undefined,address'),
            ['address', 'undefined'],
        )
        self.assertEqual(
            tasks.normalized_meson_option('b_sanitize', ['undefined', 'address']),
            ['address', 'undefined'],
        )

    def test_compiler_arguments_match_meson_list_shape(self):
        expected = ['-O1', '-DNAME=a b']

        self.assertEqual(tasks.normalized_meson_option('c_args', []), [])
        self.assertEqual(tasks.normalized_meson_option('c_args', expected), expected)
        self.assertEqual(
            tasks.normalized_meson_option('cpp_args', '-O1 "-DNAME=a b"'),
            expected,
        )

    def test_unchanged_default_build_reconfigures_without_wipe(self):
        desired = tasks.desired_sticky_options({})
        current = dict(desired)

        with mock.patch.object(tasks, 'current_meson_options', return_value=current):
            self.assertEqual(
                tasks.meson_reconfigure_arg(configured=True, desired_options=desired),
                '--reconfigure',
            )

    def test_explicit_sanitizers_match_introspected_list(self):
        explicit = tasks.meson_option_values('-Db_sanitize=address,undefined')
        desired = tasks.desired_sticky_options(explicit)
        current = dict(desired)

        self.assertEqual(desired['b_sanitize'], ['address', 'undefined'])
        with mock.patch.object(tasks, 'current_meson_options', return_value=current):
            self.assertEqual(
                tasks.meson_reconfigure_arg(configured=True, desired_options=desired),
                '--reconfigure',
            )

    def test_normal_setup_wipes_reused_forced_liburing_build(self):
        desired = tasks.desired_sticky_options({})
        current = dict(desired)
        current['ms_enable_liburing'] = True
        current['ms_force_liburing'] = True

        with mock.patch.object(tasks, 'current_meson_options', return_value=current):
            self.assertEqual(
                tasks.meson_reconfigure_arg(configured=True, desired_options=desired),
                '--wipe',
            )

        self.assertFalse(desired['ms_enable_liburing'])
        self.assertFalse(desired['ms_force_liburing'])
        self.assertFalse(desired['ms_disable_liburing'])

    def test_explicit_force_remains_opt_in(self):
        names = tasks.meson_option_names('-Dms_force_liburing=true')
        values = tasks.meson_option_values('-Dms_force_liburing=true')
        desired = tasks.desired_sticky_options(values)
        defaults = tasks.meson_default_args(names)

        self.assertTrue(desired['ms_force_liburing'])
        self.assertFalse(desired['ms_enable_liburing'])
        self.assertFalse(desired['ms_disable_liburing'])
        self.assertNotIn('-Dms_force_liburing=', defaults)
        self.assertIn('-Dms_enable_liburing=false', defaults)
        self.assertIn('-Dms_disable_liburing=false', defaults)


if __name__ == '__main__':
    unittest.main()
