# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.
import unittest

from weir.models.user_metrics import UserLevelUsage, UserLevelVerbUsage


class TestUserMetrics(unittest.TestCase):
    def test__decode_redis_data_successfully_decodes_none_to_none(self) -> None:
        input_data = None
        output = UserLevelUsage.decode_redis_data(input_data)
        self.assertEqual(output, None)

    def test__decode_redis_data_successfully_decodes_bytes_to_string(self) -> None:
        input_data = b"qweasd"
        output = UserLevelUsage.decode_redis_data(input_data)
        self.assertIsInstance(output, str)
        self.assertEqual(output, "qweasd")

    def test__decode_redis_data_throws_when_decoding_a_custom_type(self) -> None:
        input_data = UserLevelVerbUsage("key", 1234, "account$key")
        with self.assertRaises(Exception):
            UserLevelUsage.decode_redis_data(input_data)


if __name__ == "__main__":
    unittest.main()
