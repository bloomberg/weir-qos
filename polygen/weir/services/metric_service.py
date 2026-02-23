# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.
from weir.models.user_metrics import (
    UserLevelActiveRequestsUsage,
    UserLevelUsage,
    UserLevelVerbUsage,
)


class MetricService:
    @staticmethod
    def create_user_level_metric(key: str, current_epoch: int) -> UserLevelUsage:
        items = key.split("_")
        if not items or items[0] not in ["verb", "conn"]:
            raise Exception(f"invalid key {key} retrieved from redis-qos")

        def _validate_access_key(usage: UserLevelUsage) -> None:
            if not usage.access_key.isalnum():
                raise Exception(
                    f"access_key={usage.access_key} has invalid format for key {usage.key}"
                )

        if items[0] == "verb":
            if len(items) != 4:
                raise Exception(f"invalid 'verb' key {key} retrieved from redis-qos")

            verb_usage = UserLevelVerbUsage(key, int(items[1]), items[3])
            _validate_access_key(verb_usage)
            return verb_usage

        elif items[0] == "conn":
            req_usage = UserLevelActiveRequestsUsage(key, current_epoch)
            _validate_access_key(req_usage)
            return req_usage
        else:
            raise Exception(f"invalid metric identifier: {items[0]}")

    @staticmethod
    def merge_metrics_by_key[T: UserLevelUsage](input_metrics: list[T]) -> list[T]:
        """
        Merge matching metrics in a list to produce a new list where each access key only appears once,
        and that one instance accounts for all metrics with that access key in the input list.
        """
        key_metrics: dict[tuple[str, str, str, int], T] = {}
        for metric in input_metrics:
            metric_id = (
                type(metric).__name__,
                metric.access_key,
                metric.endpoint,
                metric.epoch,
            )
            if metric_id in key_metrics:
                key_metrics[metric_id].merge_from(metric)
            else:
                key_metrics[metric_id] = metric
        return list(key_metrics.values())
