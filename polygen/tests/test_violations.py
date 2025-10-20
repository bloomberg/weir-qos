# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.
import unittest

from weir.models.user_metrics import UsageValue
from weir.models.violations import Violations
from weir.services.metric_service import MetricService


class TestViolations(unittest.TestCase):
    def test__generate_violation_message__verb_get(self) -> None:
        """
        Tests that a GET verb violation is able to be properly generated.
        Due to usage of an unordered set as the underlying container,
        message generation is not deterministic so splitting logic is
        used to verify correctness.
        """

        v = Violations()
        endpoint = "dev.dc"
        epoch = 1599322430
        keys = [
            "AKIAIOSFODNN6EXAMPLE",
            "AKIAIOSFODNN7EXAMPLE",
        ]

        for acc_key in keys:
            key = f"verb_{epoch}_user_{acc_key}${endpoint}"
            metric = MetricService.create_user_level_metric(key, epoch)
            v.add_violation(metric, UsageValue.VERB_GET)

        msgs = v.generate_violation_message(endpoint, epoch)
        self.assertEqual(len(msgs), 1)
        msg = msgs[0]
        self.assertEqual(len(msg), 67)

        self.assertEqual(msg.split(",")[0], f"{epoch * 1000000}")
        self.assertEqual(msg.split(",")[1], "user_GET")

        vio_keys = msg.split(",")[2:]

        for key in keys:
            self.assertIn(key, vio_keys)

    def test__generate_violation_message__all(self) -> None:
        v = Violations()

        endpoint = "dev.dc"
        epoch = 1599322430
        acc_key = "AKIAIOSFODNN6EXAMPLE"
        key = f"verb_{epoch}_user_{acc_key}${endpoint}"
        metric = MetricService.create_user_level_metric(key, epoch)

        for verb in UsageValue.verb_values():
            v.add_violation(metric, verb)

        for thru in UsageValue.throughput_values():
            v.add_violation(metric, thru, 1.5)

        for req in UsageValue.requests_values():
            v.add_violation(metric, req)

        expected = [
            f"{epoch * 1000000},user_GET,{acc_key}",
            f"{epoch * 1000000},user_PUT,{acc_key}",
            f"{epoch * 1000000},user_POST,{acc_key}",
            f"{epoch * 1000000},user_DELETE,{acc_key}",
            f"{epoch * 1000000},user_HEAD,{acc_key}",
            f"{epoch * 1000000},user_LISTOBJECTSV2,{acc_key}",
            f"{epoch * 1000000},user_LISTMULTIPARTUPLOADS,{acc_key}",
            f"{epoch * 1000000},user_LISTOBJECTVERSIONS,{acc_key}",
            f"{epoch * 1000000},user_LISTBUCKETS,{acc_key}",
            f"{epoch * 1000000},user_LISTOBJECTS,{acc_key}",
            f"{epoch * 1000000},user_GETOBJECT,{acc_key}",
            f"{epoch * 1000000},user_DELETEOBJECTS,{acc_key}",
            f"{epoch * 1000000},user_DELETEOBJECT,{acc_key}",
            f"{epoch * 1000000},user_CREATEBUCKET,{acc_key}",
            f"{epoch * 1000000},user_bnd_dwn,{acc_key}:1.5",
            f"{epoch * 1000000},user_bnd_up,{acc_key}:1.5",
            f"user_reqs_block,{acc_key}",
            f"user_reqs_unblock,{acc_key}",
        ]
        msgs = v.generate_violation_message(endpoint, epoch)
        self.assertEqual(len(msgs), len(expected))
        self.assertEqual(msgs, expected)

    def test__generate_violation_message__bnd_up(self) -> None:
        """
        Tests that a bandwidth upload violation is able to be properly generated.
        Due to usage of an unordered set as the underlying container,
        message generation is not deterministic so splitting logic is
        used to verify correctness.
        """

        v = Violations()
        endpoint = "dev.dc"
        epoch = 1599322430
        keys = [("AKIAIOSFODNN6EXAMPLE", 1.6), ("AKIAIOSFODNN7EXAMPLE", 1.4)]

        for acc_key, diff_ratio in keys:
            key = f"verb_{epoch}_user_{acc_key}${endpoint}"
            metric = MetricService.create_user_level_metric(key, epoch)
            v.add_violation(metric, UsageValue.THRU_TYPE_WRITE, diff_ratio)

        msgs = v.generate_violation_message(endpoint, epoch)
        self.assertEqual(len(msgs), 1)
        msg = msgs[0]
        self.assertEqual(len(msg), 78)

        self.assertEqual(msg.split(",")[0], f"{epoch * 1000000}")
        self.assertEqual(msg.split(",")[1], "user_bnd_up")

        vio_keys = msg.split(",")[2:]

        for acc_key, diff_ratio in keys:
            self.assertIn(f"{acc_key}:{diff_ratio}", vio_keys)

    def test__generate_violation_message__no_duplicates(self) -> None:
        """
        Tests that violation messages don't send the same key more than once.
        """

        v = Violations()
        epoch = 1730129134.120075

        acc_key = "AKIAIOSFODNN6EXAMPLE"
        key = f"verb_1599322430_user_{acc_key}$dev.dc"
        metric = MetricService.create_user_level_metric(key, int(epoch))
        v.add_violation(metric, UsageValue.VERB_HEAD)

        msgs = v.generate_violation_message("dev.dc", epoch)
        self.assertEqual(len(msgs), 1)
        msg = msgs[0]
        self.assertEqual(msg, f"{int(epoch * 1000000)},user_HEAD,{acc_key}")

        v.add_violation(metric, UsageValue.VERB_HEAD)
        msgs = v.generate_violation_message("dev.dc", epoch)
        self.assertEqual(len(msgs), 0)

        # Other violation types should not count as duplicates
        v.add_violation(metric, UsageValue.VERB_GET)
        msgs = v.generate_violation_message("dev.dc", epoch)
        self.assertEqual(len(msgs), 1)
        msg = msgs[0]
        self.assertEqual(msg, f"{int(epoch * 1000000)},user_GET,{acc_key}")

    def test__generate_violation_message__thru_diff_ratio_check(self) -> None:
        """
        Tests that throughput violations respect diff ratio logic.
        """

        v = Violations()
        epoch = 1730129134.120075
        acc_key = "AKIAIOSFODNN6EXAMPLE"
        key = f"verb_1599322430_user_{acc_key}$dev.dc"
        metric = MetricService.create_user_level_metric(key, int(epoch))

        v.add_violation(metric, UsageValue.THRU_TYPE_READ, 1.2)
        msgs = v.generate_violation_message("dev.dc", epoch)
        self.assertEqual(len(msgs), 1)
        msg = msgs[0]
        self.assertEqual(msg, f"{int(epoch * 1000000)},user_bnd_dwn,{acc_key}:1.2")

        # Ratio diff is less than resend factor so no message should be generated.
        v.add_violation(metric, UsageValue.THRU_TYPE_READ, 1.3)
        msgs = v.generate_violation_message("dev.dc", epoch)
        self.assertEqual(len(msgs), 0)

        # Ratio diff is greater than resend factor so message should be resent.
        v.add_violation(metric, UsageValue.THRU_TYPE_READ, 1.4)
        msgs = v.generate_violation_message("dev.dc", epoch)
        self.assertEqual(len(msgs), 1)
        msg = msgs[0]
        self.assertEqual(msg, f"{int(epoch * 1000000)},user_bnd_dwn,{acc_key}:1.4")

    def test__generate_violation_message__verb_diff_ratio_check(self) -> None:
        """
        Tests that verb violations don't respect diff ratio logic.
        """

        v = Violations()
        epoch = 1730129134.120075
        acc_key = "AKIAIOSFODNN6EXAMPLE"
        key = f"verb_1599322430_user_{acc_key}$dev.dc"
        metric = MetricService.create_user_level_metric(key, int(epoch))

        v.add_violation(metric, UsageValue.VERB_HEAD, 1.2)
        msgs = v.generate_violation_message("dev.dc", epoch)
        self.assertEqual(len(msgs), 1)
        msg = msgs[0]
        self.assertEqual(msg, f"{int(epoch * 1000000)},user_HEAD,{acc_key}")

        # Ratio diff doesn't apply to verbs so this should be ignored.
        v.add_violation(metric, UsageValue.VERB_HEAD, 1.4)
        msgs = v.generate_violation_message("dev.dc", epoch)
        self.assertEqual(len(msgs), 0)


if __name__ == "__main__":
    unittest.main()
