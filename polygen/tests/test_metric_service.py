# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.
import unittest

from weir.models.user_metrics import UserLevelActiveRequestsUsage, UserLevelVerbUsage
from weir.services.metric_service import MetricService


class TestMetricService(unittest.TestCase):
    def test__create_user_level_metrics_correctly_parses_a_verb_string(self) -> None:
        metric = MetricService.create_user_level_metric(
            "verb_987654321_user_AKIAIOSFODNN7EXAMPLE$staging", 123
        )
        self.assertIsInstance(metric, UserLevelVerbUsage)
        self.assertEqual(metric.epoch, 987654321)
        self.assertEqual(metric.access_key, "AKIAIOSFODNN7EXAMPLE")
        self.assertEqual(metric.endpoint, "staging")

    def test__create_user_level_metrics_correctly_parses_a_v1_active_request_string(
        self,
    ) -> None:
        metric = MetricService.create_user_level_metric(
            "conn_user_AKIAIOSFODNN7EXAMPLE$staging", 123
        )
        # Regular assert lets mypy assume below that it is the correct type
        assert isinstance(metric, UserLevelActiveRequestsUsage)
        self.assertEqual(metric.epoch, 123)
        self.assertEqual(metric.access_key, "AKIAIOSFODNN7EXAMPLE")
        self.assertEqual(metric.endpoint, "staging")
        self.assertIsNone(metric.direction)
        self.assertIsNone(metric.instance_id)

    def test__create_user_level_metrics_correctly_parses_a_v2_active_request_string(
        self,
    ) -> None:
        metric = MetricService.create_user_level_metric(
            "conn_v2_user_down_instance1234_AKIAIOSFODNN7EXAMPLE$staging", 123
        )
        # Regular assert lets mypy assume below that it is the correct type
        assert isinstance(metric, UserLevelActiveRequestsUsage)
        self.assertEqual(metric.epoch, 123)
        self.assertEqual(metric.access_key, "AKIAIOSFODNN7EXAMPLE")
        self.assertEqual(metric.endpoint, "staging")
        self.assertEqual(metric.direction, "down")
        self.assertEqual(metric.instance_id, "instance1234")

    def test__merging_active_requests_merges_two_metrics_with_matching_access_key_and_different_directions(
        self,
    ) -> None:
        in_metrics = [
            UserLevelActiveRequestsUsage(
                "conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev", 123
            ),
            UserLevelActiveRequestsUsage(
                "conn_v2_user_down_instance1234_AKIAIOSFODNN7EXAMPLE$dev", 123
            ),
        ]
        in_metrics[0].data = 3
        in_metrics[1].data = 5
        out_metrics = MetricService.merge_metrics_by_key(in_metrics)
        self.assertEqual(len(out_metrics), 1)
        self.assertEqual(out_metrics[0].access_key, "AKIAIOSFODNN7EXAMPLE")
        self.assertEqual(out_metrics[0].data, 8)

    def test__merging_active_requests_merges_two_metrics_with_matching_access_key_and_different_instanceids(
        self,
    ) -> None:
        in_metrics = [
            UserLevelActiveRequestsUsage(
                "conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev", 123
            ),
            UserLevelActiveRequestsUsage(
                "conn_v2_user_up_instance5678_AKIAIOSFODNN7EXAMPLE$dev", 123
            ),
        ]
        in_metrics[0].data = 3
        in_metrics[1].data = 5
        out_metrics = MetricService.merge_metrics_by_key(in_metrics)
        self.assertEqual(len(out_metrics), 1)
        self.assertEqual(out_metrics[0].access_key, "AKIAIOSFODNN7EXAMPLE")
        self.assertEqual(out_metrics[0].data, 8)

    def test__merging_active_requests_doesnt_merge_two_metrics_with_matching_access_key_and_different_endpoints(
        self,
    ) -> None:
        in_metrics = [
            UserLevelActiveRequestsUsage(
                "conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev", 123
            ),
            UserLevelActiveRequestsUsage(
                "conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$prod", 123
            ),
        ]
        in_metrics[0].data = 3
        in_metrics[1].data = 5
        out_metrics = MetricService.merge_metrics_by_key(in_metrics)
        self.assertEqual(len(out_metrics), 2)
        self.assertEqual(out_metrics[0].endpoint, "dev")
        self.assertEqual(out_metrics[1].endpoint, "prod")
        self.assertEqual(out_metrics[0].data, 3)
        self.assertEqual(out_metrics[1].data, 5)

    def test__merging_active_requests_doesnt_merge_two_metrics_with_matching_access_key_and_different_timestamps(
        self,
    ) -> None:
        in_metrics = [
            UserLevelActiveRequestsUsage(
                "conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev", 123
            ),
            UserLevelActiveRequestsUsage(
                "conn_v2_user_down_instance1234_AKIAIOSFODNN7EXAMPLE$dev", 456
            ),
        ]
        in_metrics[0].data = 3
        in_metrics[1].data = 5
        out_metrics = MetricService.merge_metrics_by_key(in_metrics)
        self.assertEqual(len(out_metrics), 2)
        self.assertEqual(out_metrics[0].epoch, 123)
        self.assertEqual(out_metrics[1].epoch, 456)
        self.assertEqual(out_metrics[0].data, 3)
        self.assertEqual(out_metrics[1].data, 5)

    def test__merging_active_requests_doesnt_merge_two_metrics_with_matching_access_key_and_different_access_keys(
        self,
    ) -> None:
        in_metrics = [
            UserLevelActiveRequestsUsage(
                "conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev", 123
            ),
            UserLevelActiveRequestsUsage(
                "conn_v2_user_down_instance1234_AKIAIOSFODNN8EXAMPLE$dev", 123
            ),
        ]
        in_metrics[0].data = 3
        in_metrics[1].data = 5
        out_metrics = MetricService.merge_metrics_by_key(in_metrics)
        self.assertEqual(len(out_metrics), 2)
        self.assertEqual(out_metrics[0].access_key, "AKIAIOSFODNN7EXAMPLE")
        self.assertEqual(out_metrics[1].access_key, "AKIAIOSFODNN8EXAMPLE")
        self.assertEqual(out_metrics[0].data, 3)
        self.assertEqual(out_metrics[1].data, 5)

    def test__merging_doesnt_merge_two_metrics_with_matching_fields_but_different_types(
        self,
    ) -> None:
        in_metrics = [
            UserLevelActiveRequestsUsage(
                "conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev", 123
            ),
            UserLevelVerbUsage(
                "verb_987654321_user_AKIAIOSFODNN7EXAMPLE$dev",
                123,
                "AKIAIOSFODNN7EXAMPLE$dev",
            ),
        ]

        out_metrics = MetricService.merge_metrics_by_key(in_metrics)
        self.assertEqual(len(out_metrics), 2)
        self.assertIsInstance(out_metrics[0], UserLevelActiveRequestsUsage)
        self.assertIsInstance(out_metrics[1], UserLevelVerbUsage)


if __name__ == "__main__":
    unittest.main()
