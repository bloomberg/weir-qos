# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.
from collections import defaultdict
from typing import KeysView

from .user_metrics import UsageScope, UsageValue, UserLevelUsage

USECS_IN_SEC = 1_000_000


class UserLevelViolation:
    def __init__(self, usage_value: UsageValue) -> None:
        self.scope: UsageScope = UsageScope.USER_LEVEL
        self.new_keys: set[str] = set()
        self.sent_keys: set[str] = set()
        self.violation_ratios: dict[str, float] = dict()
        self.usage_value = usage_value

    def add_new_key(
        self, key: str, diff_ratio: float | None = None, remove_sent: bool = False
    ) -> None:
        self.new_keys.add(key)
        if diff_ratio:
            self.violation_ratios[key] = diff_ratio
        if remove_sent:
            self.sent_keys.remove(key)

    def generate_violation_message(self, epoch_time: float) -> str:
        if self.usage_value in UsageValue.throughput_values():
            return self._generate_bnd_violation_message(epoch_time)
        elif self.usage_value in UsageValue.requests_values():
            return self._generate_reqs_violation_message()
        else:
            return self._generate_verb_violation_message(epoch_time)

    def _generate_verb_violation_message(self, epoch_time: float) -> str:
        return ",".join(
            [
                str(int(epoch_time * USECS_IN_SEC)),
                "_".join([self.scope.value, self.usage_value.value]),
                ",".join(self.new_keys),
            ]
        )

    def _generate_bnd_violation_message(self, epoch_time: float) -> str:
        return ",".join(
            [
                str(int(epoch_time * USECS_IN_SEC)),
                "_".join([self.scope.value, self.usage_value.value]),
                ",".join((f"{k}:{self.violation_ratios[k]}" for k in self.new_keys)),
            ]
        )

    def _generate_reqs_violation_message(self) -> str:
        return ",".join(
            [
                "_".join([self.scope.value, self.usage_value.value]),
                ",".join(self.new_keys),
            ]
        )


class EndpointViolations:
    def __init__(self) -> None:
        self.violations: dict[UsageValue, UserLevelViolation] = dict()
        for usage_value in UsageValue:
            self.violations[usage_value] = UserLevelViolation(usage_value)


class Violations:
    """
    If, in the same epoch, a user exceeds limit more than once and
    the latest diff_ratio reading is more than the previous reading
    by the following factor, resend violation message with the new
    diff_ratio data.
    """

    DIFF_RATIO_RESEND_FACTOR = 0.15

    def __init__(self) -> None:
        self.endpoint_violations: dict[str, EndpointViolations] = defaultdict(
            lambda: EndpointViolations()
        )

    def add_violation(
        self,
        metric: UserLevelUsage,
        metric_value: UsageValue,
        diff_ratio: float | None = None,
    ) -> None:
        violation = self.endpoint_violations[metric.endpoint].violations[metric_value]

        if metric.access_key not in violation.sent_keys:
            violation.add_new_key(metric.access_key, diff_ratio)
        elif metric_value in UsageValue.throughput_values() and diff_ratio is not None:
            # Only throughput values care about diff ratios.
            sent_diff_ratio = violation.violation_ratios.get(metric.access_key, 0)
            if (diff_ratio - sent_diff_ratio) > self.DIFF_RATIO_RESEND_FACTOR:
                violation.add_new_key(metric.access_key, diff_ratio, remove_sent=True)

    def endpoints(self) -> KeysView[str]:
        return self.endpoint_violations.keys()

    def generate_violation_message(self, endpoint: str, epoch_time: float) -> list[str]:
        violation_messages = []
        for _, violations in self.endpoint_violations[endpoint].violations.items():
            if len(violations.new_keys):
                msg = violations.generate_violation_message(epoch_time)

                violation_messages.append(msg)

                for new_key in violations.new_keys:
                    violations.sent_keys.add(new_key)

                violations.new_keys = set()

        return violation_messages
