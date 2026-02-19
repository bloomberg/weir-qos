# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.

import logging
import unittest
from typing import Any as AnyType
from unittest.mock import ANY as ANY_VALUE
from unittest.mock import Mock

from policy_generator import (
    DEFAULT_VERB_BDW_LIMIT_IF_QOS_IS_NOT_CONFIGURED,
    DEFAULT_VERB_RATE_LIMIT_IF_QOS_IS_NOT_CONFIGURED,
    MB,
    VERB_LIMITING_BANDWIDTH_CATEGORY_PATTERN,
    DemandKey,
    DemandMap,
    Direction,
    HaproxyServerMap,
    LimitConfig,
    Policies,
    PolicyGenerator,
)
from weir.models.user_metrics import UsageValue, UserLevelVerbUsage

SAMPLE_KEY_LIMITS: LimitConfig = LimitConfig(
    user_to_qos_id={
        "MYACCESSKEY1": "SILVER",
        "MYACCESSKEY2": "GOLD",
        "MYACCESSKEY3": "PLATINUM",
    },
    qos={
        "DEFAULT": {
            "user_DELETE": 100,
            "user_GET": 100,
            "user_HEAD": 100,
            "user_POST": 100,
            "user_PUT": 100,
            "user_bnd_up": 100,
            "user_bnd_dwn": 100,
            "user_conns": 10,
        },
        "SILVER": {
            "user_DELETE": 200,
            "user_GET": 200,
            "user_HEAD": 200,
            "user_POST": 200,
            "user_PUT": 200,
            "user_bnd_up": 200,
            "user_bnd_dwn": 200,
        },
        "GOLD": {
            "user_DELETE": 300,
            "user_GET": 300,
            "user_HEAD": 300,
            "user_POST": 300,
            "user_PUT": 300,
            "user_bnd_up": 300,
            "user_bnd_dwn": 300,
        },
        "PLATINUM": {
            "user_DELETE": 400,
            "user_GET": 400,
            "user_HEAD": 400,
            "user_POST": 400,
            "user_PUT": 400,
            "user_bnd_up": 400,
            "user_bnd_dwn": 400,
            "user_conns": 30,
        },
    },
)


SPECIAL_USER = "common"
SAMPLE_SPECIAL_USER_LIMITS: dict[str, dict[str, AnyType]] = {
    "user_to_qos_id": {"common": "COMMON_QOS"},
    "qos": {
        "COMMON_QOS": {
            "user_DELETE": 75,
            "user_GET": 75,
            "user_HEAD": 75,
            "user_POST": 75,
            "user_PUT": 75,
            "user_bnd_up": 75,
            "user_bnd_dwn": 75,
        }
    },
}


# Some constants used in testcases
SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO: float = 10.0
A_VERY_HIGH_VALUE: int = 100000


class StubbedPolicyGenerator(PolicyGenerator):
    def __init__(self) -> None:
        pass

    def __getattribute__(self, name: str) -> AnyType:
        try:
            return object.__getattribute__(self, name)
        except AttributeError:
            m = Mock()
            object.__setattr__(self, name, m)
            return m


class TestPolicyGenerator(unittest.TestCase):
    def setUp(self) -> None:
        self.stubbed_polygen = StubbedPolicyGenerator()
        self.stubbed_polygen.logger = logging.getLogger("TestPolicyGenerator")

    def _pick_a_known_acckey(self) -> str:
        return list(SAMPLE_KEY_LIMITS.user_to_qos_id.keys())[0]

    def _pick_an_unknown_acckey(self) -> str:
        acckey = "an_unrecognised_key"
        self.assertNotIn(acckey, SAMPLE_KEY_LIMITS.user_to_qos_id.keys())
        self.assertNotIn(acckey, SAMPLE_SPECIAL_USER_LIMITS["user_to_qos_id"].keys())
        return acckey

    def _pick_a_supported_category(self, bnd: bool = False) -> str:
        pattern = VERB_LIMITING_BANDWIDTH_CATEGORY_PATTERN if bnd else ""
        categories = [
            c for c in SAMPLE_KEY_LIMITS.qos["DEFAULT"].keys() if pattern in c
        ]
        return categories[0]

    def _pick_an_unsupported_category(self) -> str:
        cat = "user_OPTIONS"
        self.assertNotIn(cat, SAMPLE_KEY_LIMITS.qos["DEFAULT"].keys())
        return cat

    def test__is_limit_reached_verb_type__all_limits_missing_rate(self) -> None:
        """When no limit info is found in the configurations, use the hard-coded limit"""
        self.stubbed_polygen.key_limits = LimitConfig({}, {})
        user_key = self._pick_a_known_acckey()

        cat = self._pick_a_supported_category()
        limit_reached, diff_ratio = self.stubbed_polygen._is_limit_reached_verb_type(
            cat,
            user_key,
            A_VERY_HIGH_VALUE,
        )
        self.assertTrue(limit_reached)
        self.assertEqual(
            float(diff_ratio),
            float(A_VERY_HIGH_VALUE / DEFAULT_VERB_RATE_LIMIT_IF_QOS_IS_NOT_CONFIGURED),
        )

    def test__is_limit_reached_verb_type__all_limits_missing_bnd(self) -> None:
        """When no limit info is found in the configurations, use the hard-coded limit"""
        self.stubbed_polygen.key_limits = LimitConfig({}, {})

        user_key = self._pick_a_known_acckey()
        cat = self._pick_a_supported_category(bnd=True)
        limit_reached, diff_ratio = self.stubbed_polygen._is_limit_reached_verb_type(
            cat,
            user_key,
            A_VERY_HIGH_VALUE * MB,
        )
        self.assertTrue(limit_reached)
        self.assertEqual(
            float(diff_ratio),
            float(A_VERY_HIGH_VALUE / DEFAULT_VERB_BDW_LIMIT_IF_QOS_IS_NOT_CONFIGURED),
        )

    def test__is_limit_reached_verb_type__key_limits_missing_rate(self) -> None:
        """When no limit info is found in the configurations, use the hard-coded limit.
        Special users file is not meant to provide DEFAULT limits.
        """
        self.stubbed_polygen.key_limits = LimitConfig({}, {})

        user_key = self._pick_a_known_acckey()
        limit_reached, diff_ratio = self.stubbed_polygen._is_limit_reached_verb_type(
            self._pick_a_supported_category(),
            user_key,  # not a special user
            A_VERY_HIGH_VALUE,
        )
        self.assertTrue(limit_reached)
        self.assertEqual(
            float(diff_ratio),
            float(A_VERY_HIGH_VALUE / DEFAULT_VERB_RATE_LIMIT_IF_QOS_IS_NOT_CONFIGURED),
        )

    def test__is_limit_reached_verb_type__key_limits_missing_bandwidth(self) -> None:
        """When no limit info is found in the configurations, use the hard-coded limit.
        Special users file is not meant to provide DEFAULT limits.
        """
        self.stubbed_polygen.key_limits = LimitConfig({}, {})

        user_key = self._pick_a_known_acckey()
        limit_reached, diff_ratio = self.stubbed_polygen._is_limit_reached_verb_type(
            self._pick_a_supported_category(bnd=True),
            user_key,  # not a special user
            A_VERY_HIGH_VALUE * MB,
        )
        print(f"{diff_ratio}")
        self.assertTrue(limit_reached)
        self.assertEqual(
            float(diff_ratio),
            float(A_VERY_HIGH_VALUE / DEFAULT_VERB_BDW_LIMIT_IF_QOS_IS_NOT_CONFIGURED),
        )

    def test__is_limit_reached_verb_type__unrecognised_key_rate(self) -> None:
        """When a key is unknown, we apply the DEFAULT limits"""
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        cat = self._pick_a_supported_category()
        limit_reached, diff_ratio = self.stubbed_polygen._is_limit_reached_verb_type(
            cat,
            self._pick_an_unknown_acckey(),
            SAMPLE_KEY_LIMITS.qos["DEFAULT"][cat]
            * SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO,
        )
        self.assertTrue(limit_reached)
        self.assertEqual(diff_ratio, SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO)

    def test__is_limit_reached_verb_type__unrecognised_key_bnd(self) -> None:
        """When a key is unknown, we apply the DEFAULT limits"""
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        cat = self._pick_a_supported_category(bnd=True)
        limit_reached, diff_ratio = self.stubbed_polygen._is_limit_reached_verb_type(
            cat,
            self._pick_an_unknown_acckey(),
            SAMPLE_KEY_LIMITS.qos["DEFAULT"][cat]
            * SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO
            * MB,
        )
        self.assertTrue(limit_reached)
        self.assertEqual(diff_ratio, SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO)

    def test__is_limit_reached_verb_type__special_user_but_custom_limits_missing(
        self,
    ) -> None:
        """If special user limits are missing, special user is treated as unrecognised key."""
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        cat = self._pick_a_supported_category()
        limit_reached, diff_ratio = self.stubbed_polygen._is_limit_reached_verb_type(
            cat,
            SPECIAL_USER,
            SAMPLE_KEY_LIMITS.qos["DEFAULT"][cat]
            * SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO,
        )
        self.assertTrue(limit_reached)
        self.assertEqual(diff_ratio, SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO)

    def test__is_limit_reached_verb_type__unrecognised_category(self) -> None:
        """When a category is unknown, we apply the hard-coded limit"""
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        cat = self._pick_an_unsupported_category()
        user_key = self._pick_a_known_acckey()
        value = (
            DEFAULT_VERB_RATE_LIMIT_IF_QOS_IS_NOT_CONFIGURED
            * SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO
        )
        limit_reached, diff_ratio = self.stubbed_polygen._is_limit_reached_verb_type(
            cat, user_key, value
        )
        self.assertTrue(limit_reached)
        self.assertEqual(diff_ratio, SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO)

    def test__is_limit_reached_verb_type__unrecognised_category_with_special_user(
        self,
    ) -> None:
        """When a category is unknown, we apply the hard-coded limits"""
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        limit_reached, diff_ratio = self.stubbed_polygen._is_limit_reached_verb_type(
            self._pick_an_unsupported_category(),
            SPECIAL_USER,
            DEFAULT_VERB_RATE_LIMIT_IF_QOS_IS_NOT_CONFIGURED
            * SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO,
        )
        self.assertTrue(limit_reached)
        self.assertEqual(diff_ratio, SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO)

    def test__is_limit_reached_verb_type__unrecognised_category_with_unknown_user(
        self,
    ) -> None:
        """When a category is unknown, we apply the DEFAULT limits"""
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        limit_reached, diff_ratio = self.stubbed_polygen._is_limit_reached_verb_type(
            self._pick_an_unsupported_category(),
            self._pick_an_unknown_acckey(),
            DEFAULT_VERB_RATE_LIMIT_IF_QOS_IS_NOT_CONFIGURED
            * SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO,
        )
        self.assertTrue(limit_reached)
        self.assertEqual(diff_ratio, SPECIAL_USER_TESTS_EXPECTED_DIFF_RATIO)

    def _check_result(
        self,
        accesskey: str,
        test_value: int,
        expected_limit_reached: bool,
        expected_diff_ratio: float,
    ) -> None:
        for verb in [
            "GET",
            "PUT",
            "HEAD",
            "POST",
            "DELETE",
            "bnd_up",
            "bnd_dwn",
        ]:
            cat = f"user_{verb}"
            (
                limit_reached,
                diff_ratio,
            ) = self.stubbed_polygen._is_limit_reached_verb_type(
                cat,
                accesskey,
                test_value if "bnd" not in verb else test_value * MB,
            )
            self.assertEqual(limit_reached, expected_limit_reached)
            self.assertEqual(diff_ratio, expected_diff_ratio)

    def test__is_limit_reached_verb_type__limits_found_in_cache_file(self) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        accesskey_with_no_custom_limits = self._pick_an_unknown_acckey()
        self._check_result(accesskey_with_no_custom_limits, 200, True, 2.0)

        accesskey_with_platinum_limits = "MYACCESSKEY3"
        self._check_result(accesskey_with_platinum_limits, 1200, True, 3.0)

    def test__get_limit__returns_default_conn_limit_if_user_has_only_verb_limits_defined(
        self,
    ) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS
        limit = self.stubbed_polygen._get_limit(
            "user_conns",
            "MYACCESSKEY1",
        )
        self.assertTrue("user_conns" not in SAMPLE_KEY_LIMITS.qos["SILVER"])
        self.assertEqual(limit, SAMPLE_KEY_LIMITS.qos["DEFAULT"]["user_conns"])
        self.assertEqual(limit, 10)

    def test__get_limit__returns_user_conn_limit_if_user_has_one_defined(
        self,
    ) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS
        limit = self.stubbed_polygen._get_limit(
            "user_conns",
            "MYACCESSKEY3",
        )
        self.assertTrue("user_conns" in SAMPLE_KEY_LIMITS.qos["PLATINUM"])
        self.assertEqual(limit, SAMPLE_KEY_LIMITS.qos["PLATINUM"]["user_conns"])
        self.assertEqual(limit, 30)

    def test__check_all_conn_key_violations__adds_blocks(self) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        self.stubbed_polygen.blocked_users = {}
        self.stubbed_polygen._check_all_conn_key_violations(
            keys=["conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev.dc"],
            conn_counts=["11"],
            epoch_time=1.0,
        )

        self.assertEqual(len(self.stubbed_polygen.blocked_users), 1)
        self.assertIn("AKIAIOSFODNN7EXAMPLE", self.stubbed_polygen.blocked_users)

        self.stubbed_polygen.policies.add_violation.assert_called_once_with(  # type: ignore [attr-defined]
            ANY_VALUE, ANY_VALUE, UsageValue.REQUESTS_BLOCK
        )

    def test__check_all_conn_key_violations__does_not_issues_heartbeat_for_users_over_their_limit_but_recently_violated(
        self,
    ) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        self.stubbed_polygen.blocked_users = {"AKIAIOSFODNN7EXAMPLE": 1.0}
        self.stubbed_polygen.reqs_unblock_backoff_time_ms = 500
        self.stubbed_polygen.reqs_unblock_ratio = 0.8
        self.stubbed_polygen._check_all_conn_key_violations(
            keys=["conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev.dc"],
            conn_counts=["12"],
            epoch_time=1.1,
        )

        self.assertEqual(len(self.stubbed_polygen.blocked_users), 1)
        self.assertIn("AKIAIOSFODNN7EXAMPLE", self.stubbed_polygen.blocked_users)
        self.stubbed_polygen.policies.add_violation.assert_not_called()  # type: ignore [attr-defined]

    def test__check_all_conn_key_violations__issues_a_heartbeat_violation_for_users_who_stay_over_their_limit(
        self,
    ) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        self.stubbed_polygen.blocked_users = {"AKIAIOSFODNN7EXAMPLE": 1.0}
        self.stubbed_polygen.reqs_unblock_backoff_time_ms = 500
        self.stubbed_polygen.reqs_unblock_ratio = 0.8
        self.stubbed_polygen._check_all_conn_key_violations(
            keys=["conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev.dc"],
            conn_counts=["12"],
            epoch_time=2.0,
        )

        self.assertEqual(len(self.stubbed_polygen.blocked_users), 1)
        self.assertIn("AKIAIOSFODNN7EXAMPLE", self.stubbed_polygen.blocked_users)
        self.stubbed_polygen.policies.add_violation.assert_called_once_with(  # type: ignore [attr-defined]
            ANY_VALUE, ANY_VALUE, UsageValue.REQUESTS_BLOCK
        )

    def test__check_all_conn_key_violations__issues_heartbeat_violation_for_users_below_the_limit_but_above_block_ratio(
        self,
    ) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        self.stubbed_polygen.blocked_users = {"AKIAIOSFODNN7EXAMPLE": 1.0}
        self.stubbed_polygen.reqs_unblock_backoff_time_ms = 500
        self.stubbed_polygen.reqs_unblock_ratio = 0.8
        self.stubbed_polygen._check_all_conn_key_violations(
            keys=["conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev.dc"],
            conn_counts=["9"],
            epoch_time=2.0,
        )

        self.assertEqual(len(self.stubbed_polygen.blocked_users), 1)
        self.assertIn("AKIAIOSFODNN7EXAMPLE", self.stubbed_polygen.blocked_users)
        self.stubbed_polygen.policies.add_violation.assert_called_once_with(  # type: ignore [attr-defined]
            ANY_VALUE, ANY_VALUE, UsageValue.REQUESTS_BLOCK
        )

    def test__check_all_conn_key_violations__removes_blocks(self) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        self.stubbed_polygen.blocked_users = {"AKIAIOSFODNN7EXAMPLE": 1.0}
        self.stubbed_polygen.reqs_unblock_backoff_time_ms = 500
        self.stubbed_polygen.reqs_unblock_ratio = 0.8
        self.stubbed_polygen._check_all_conn_key_violations(
            keys=["conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev.dc"],
            conn_counts=["5"],
            epoch_time=2.0,
        )

        self.assertEqual(len(self.stubbed_polygen.blocked_users), 0)
        self.stubbed_polygen.policies.add_violation.assert_called_once_with(  # type: ignore [attr-defined]
            ANY_VALUE, ANY_VALUE, UsageValue.REQUESTS_UNBLOCK
        )

    def test__check_all_conn_key_violations__maintains_blocks_by_ratio(self) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        self.stubbed_polygen.blocked_users = {"AKIAIOSFODNN7EXAMPLE": 1.0}
        self.stubbed_polygen.reqs_unblock_backoff_time_ms = 500
        self.stubbed_polygen.reqs_unblock_ratio = 0.8
        self.stubbed_polygen._check_all_conn_key_violations(
            keys=["conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev.dc"],
            conn_counts=["9"],
            epoch_time=2.0,
        )

        self.assertEqual(len(self.stubbed_polygen.blocked_users), 1)
        self.assertIn("AKIAIOSFODNN7EXAMPLE", self.stubbed_polygen.blocked_users)
        self.stubbed_polygen.policies.add_violation.assert_called_once_with(  # type: ignore [attr-defined]
            ANY_VALUE, ANY_VALUE, UsageValue.REQUESTS_BLOCK
        )

    def test__limit_share_with_one_user_two_instances_splits_correctly(
        self,
    ) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        total_demand: DemandMap = {
            DemandKey("MYACCESSKEY1", Direction.Down): {
                "instance1": 100,
                "instance2": 300,
            },
        }

        limit_share = self.stubbed_polygen.compute_bandwidth_limit_share(total_demand)

        # Should be a 25-75% split between instances
        user1_total_limit = (
            SAMPLE_KEY_LIMITS.qos[SAMPLE_KEY_LIMITS.user_to_qos_id["MYACCESSKEY1"]][
                "user_bnd_dwn"
            ]
            * MB
        )
        self.assertEqual(
            limit_share,
            {
                DemandKey("MYACCESSKEY1", Direction.Down): {
                    "instance1": int(user1_total_limit * 0.25),
                    "instance2": int(user1_total_limit * 0.75),
                }
            },
        )

    def test__limit_share_with_one_user_in_two_directions_sets_limits_independently(
        self,
    ) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        total_demand: DemandMap = {
            DemandKey("MYACCESSKEY1", Direction.Down): {
                "instance1": 100,
                "instance2": 300,
            },
            DemandKey("MYACCESSKEY1", Direction.Up): {
                "instance1": 100,
                "instance3": 100,
            },
        }

        limit_share = self.stubbed_polygen.compute_bandwidth_limit_share(total_demand)

        # Should be a 25-75% split between instances
        total_down_limit = (
            SAMPLE_KEY_LIMITS.qos[SAMPLE_KEY_LIMITS.user_to_qos_id["MYACCESSKEY1"]][
                "user_bnd_dwn"
            ]
            * MB
        )
        total_up_limit = (
            SAMPLE_KEY_LIMITS.qos[SAMPLE_KEY_LIMITS.user_to_qos_id["MYACCESSKEY1"]][
                "user_bnd_up"
            ]
            * MB
        )
        self.assertEqual(
            limit_share,
            {
                DemandKey("MYACCESSKEY1", Direction.Down): {
                    "instance1": int(total_down_limit * 0.25),
                    "instance2": int(total_down_limit * 0.75),
                },
                DemandKey("MYACCESSKEY1", Direction.Up): {
                    "instance1": int(total_up_limit * 0.5),
                    "instance3": int(total_up_limit * 0.5),
                },
            },
        )

    def test__limit_share_with_two_users_two_instances_splits_correctly(
        self,
    ) -> None:
        self.stubbed_polygen.key_limits = SAMPLE_KEY_LIMITS

        total_demand: DemandMap = {
            DemandKey("MYACCESSKEY1", Direction.Down): {
                "instance1": 100,
                "instance2": 300,
            },
            DemandKey("MYACCESSKEY2", Direction.Down): {
                "instance1": 100,
            },
        }

        limit_share = self.stubbed_polygen.compute_bandwidth_limit_share(total_demand)

        # Should be a 25-75% split for user 1, who should be unaffected by user 2
        user1_total_limit = (
            SAMPLE_KEY_LIMITS.qos[SAMPLE_KEY_LIMITS.user_to_qos_id["MYACCESSKEY1"]][
                "user_bnd_dwn"
            ]
            * MB
        )
        user2_total_limit = (
            SAMPLE_KEY_LIMITS.qos[SAMPLE_KEY_LIMITS.user_to_qos_id["MYACCESSKEY2"]][
                "user_bnd_dwn"
            ]
            * MB
        )
        self.assertEqual(
            limit_share,
            {
                DemandKey("MYACCESSKEY1", Direction.Down): {
                    "instance1": int(user1_total_limit * 0.25),
                    "instance2": int(user1_total_limit * 0.75),
                },
                DemandKey("MYACCESSKEY2", Direction.Down): {
                    "instance1": user2_total_limit,
                },
            },
        )

    def test__violations_are_sent_to_all_haproxies_on_a_given_endpoint(self) -> None:
        epoch = 12345
        policies = Policies(logging.getLogger("TestPolicyGenerator"))
        policies.add_violation(
            epoch,
            UserLevelVerbUsage("key", epoch, "key$endpoint1"),
            UsageValue.VERB_GET,
        )

        haproxies: HaproxyServerMap = {"endpoint1": [Mock(), Mock()]}
        policies.prepare_message(haproxies, epoch)

        haproxies["endpoint1"][0].add_message.assert_called_once()  # type: ignore [attr-defined]
        haproxies["endpoint1"][1].add_message.assert_called_once()  # type: ignore [attr-defined]


if __name__ == "__main__":
    unittest.main()
