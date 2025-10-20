# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.
import argparse
import logging
import logging.handlers
import math
import multiprocessing
import sys
import time
from collections import defaultdict
from typing import TYPE_CHECKING, Any

import requests
import yaml
from prometheus_client import parser
from requests.exceptions import Timeout

logger = logging.getLogger("qos_metrics_publisher")

if TYPE_CHECKING:
    Metric = tuple[float, dict[str, str]]
    MetricsDict = dict[str, list[Metric]]
    DataPoints = dict[int, MetricsDict]

    MetricsQueue = multiprocessing.Queue[DataPoints]


def init_logger(filename: str, log_level: str) -> None:
    try:
        logger.setLevel(log_level.upper())
    except ValueError:
        logger.setLevel("DEBUG")

    logging_formatter = logging.Formatter("%(asctime)s - %(levelname)s - %(message)s")

    if filename:
        file_handler = logging.handlers.RotatingFileHandler(
            filename, maxBytes=104857600, backupCount=5
        )
        file_handler.setFormatter(logging_formatter)
        logger.addHandler(file_handler)
    else:
        stdout_handler = logging.StreamHandler(sys.stdout)
        stdout_handler.setFormatter(logging_formatter)
        logger.addHandler(stdout_handler)


#
# publisher process that sends generated datapoints to log file
#
class Publisher:
    EPOCHS_PROCESSED_SET_MAX_ELEMS = 10

    def __init__(self) -> None:
        self.metrics_processed: dict[str, list[int]] = defaultdict(list)

    def publish_metrics(self, metrics: "MetricsDict", epoch: int) -> None:
        for metric_name in list(metrics.keys()):
            if epoch in self.metrics_processed[metric_name]:
                del metrics[metric_name]
            else:
                self.metrics_processed[metric_name].append(epoch)

        for metric_name, metric_values in metrics.items():
            logger.info(f"publishing metrics: {metric_name} for epoch: {epoch}")
            for val in metric_values:
                logger.info(f"value: {val[0]}, tags: {val[1]}")

        for metric_name, timestamps in self.metrics_processed.items():
            if len(timestamps) > self.EPOCHS_PROCESSED_SET_MAX_ELEMS:
                timestamps.remove(min(timestamps))


def poll_metrics() -> None:
    logger.info("QoS publisher starting")

    request_timeout = 60
    qos_metrics_publishing_period_sec = 1
    max_wait_secs_after_failure = 60
    max_try_count_after_failure = math.ceil(math.log2(max_wait_secs_after_failure))
    try_count = 0

    publisher = Publisher()
    datapoints: "DataPoints" = defaultdict(lambda: defaultdict(list))

    while True:
        try:
            response = requests.get(PROMETHEUS_EXPOSER, timeout=request_timeout)
            if response.status_code != 200:
                raise Exception(
                    f"unexpected response code '{response.status_code}': {response}"
                )

            prom_metrics = parser.text_string_to_metric_families(response.text)
            for m in prom_metrics:
                for s in m.samples:
                    tags = dict(s.labels)
                    epoch = int(tags["epoch"])
                    datapoints[epoch][m.name].append((s.value, tags))
        except Timeout:
            logger.error(f"query to {PROMETHEUS_EXPOSER} timed out. Try again.")
            continue
        except Exception as ex:
            try_count = min(try_count + 1, max_try_count_after_failure)
            logger.exception(f"query failed to {PROMETHEUS_EXPOSER}: {ex}")
            time.sleep(min(pow(2, try_count), max_wait_secs_after_failure))
            continue
        else:
            try_count = 0

        try:
            for epoch, metrics in datapoints.items():
                publisher.publish_metrics(metrics, epoch)
            datapoints.clear()
        except Exception as ex:
            logger.exception(f"failed to publish QoS metrics: {ex}")

        time.sleep(qos_metrics_publishing_period_sec)


def load_config_file(config_file: str) -> dict[str, Any]:
    with open(config_file) as f:
        return yaml.safe_load(f)  # type: ignore


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="QoS Metrics Publisher")
    parser.add_argument("config_file", help="path to config file")
    return parser.parse_args()


if __name__ == "__main__":
    CONFIG_FILE = load_config_file(parse_args().config_file)
    init_logger(
        CONFIG_FILE.get("qos_metrics_file_name", ""),
        CONFIG_FILE.get("log_level", ""),
    )
    METRICS_ENDPOINT_PORT: int = CONFIG_FILE.get("qos_metrics_endpoint_port", -1)
    if METRICS_ENDPOINT_PORT == -1:
        sys.exit()

    PROMETHEUS_EXPOSER = f"http://localhost:{METRICS_ENDPOINT_PORT}/metrics"

    poll_metrics()
