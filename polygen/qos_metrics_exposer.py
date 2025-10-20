# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.
from __future__ import annotations

import argparse
import logging
import logging.handlers
import multiprocessing
import queue
import re
import sys
import time
from collections import deque
from typing import TYPE_CHECKING, Any, NamedTuple, NoReturn, cast

import prometheus_client
import redis
import yaml
from prometheus_client import Gauge, start_http_server
from weir.models.user_metrics import Metric, UsageType, UserLevelUsage
from weir.services.metric_service import MetricService

prometheus_client.REGISTRY.unregister(prometheus_client.GC_COLLECTOR)
prometheus_client.REGISTRY.unregister(prometheus_client.PLATFORM_COLLECTOR)
prometheus_client.REGISTRY.unregister(prometheus_client.PROCESS_COLLECTOR)

logger = logging.getLogger("qos_metrics_exposer")

if TYPE_CHECKING:
    DataPoints = dict[int, dict[str, list[tuple[int, dict[str, str]]]]]

    MetricsQueue = multiprocessing.Queue[DataPoints]


class PrometheusExposingMetrics(NamedTuple):
    gauge: Gauge
    cleanup_q: deque


def init_logger(filename: str, log_level: str) -> None:
    try:
        try:
            logger.setLevel(log_level.upper())
        except ValueError:
            logger.setLevel("DEBUG")

        log_formatter = logging.Formatter("%(asctime)s - %(levelname)s - %(message)s")
        if (filename is not None) and (len(filename) > 0):
            file_handler = logging.handlers.RotatingFileHandler(
                filename, maxBytes=104857600, backupCount=5
            )
            file_handler.setFormatter(log_formatter)
            logger.addHandler(file_handler)

        else:
            stream_handler = logging.StreamHandler(sys.stdout)
            stream_handler.setFormatter(log_formatter)
            logger.addHandler(stream_handler)
    except Exception as ex:
        logging.exception(f"failed to create logger: {ex}")


#
# gathering data from redis-qos and creating metrics datapoints out of them
#
def process_data_retrieved_from_redis(metrics: list[UserLevelUsage]) -> "DataPoints":
    datapoints: "DataPoints" = {}

    per_user_metrics = MetricService.merge_metrics_by_key(metrics)
    for metric in per_user_metrics:
        if metric.epoch not in datapoints:
            datapoints[metric.epoch] = {
                Metric.READ_THRU: [],
                Metric.WRITE_THRU: [],
                Metric.IO_CNT_PER_VERB: [],
                Metric.CONNECTIONS: [],
            }
        metric.load_data_into_datapoints(datapoints)

    return datapoints


def retrieve_data_from_redis(
    rs_conn: redis.Redis, current_epoch: int
) -> list[UserLevelUsage]:
    metrics: list[UserLevelUsage] = []
    _epochs_found = set()

    # retrieve keys from redis-qos
    # We're not using asyncio, so we don't care about the Awaitable case
    encoded_redis_keys = cast(Any, rs_conn.keys("*_user_*"))
    decoded_redis_keys: list[str] = []
    for encoded_key in encoded_redis_keys:
        try:
            decoded_redis_keys.append(encoded_key.decode())
        except Exception as ex:
            logger.warning(
                f"failed to decode a redis key '{encoded_key!r}': {ex}"
            )  # type:ignore
            continue

    for decoded_key in decoded_redis_keys:  # type:ignore
        try:
            metric = MetricService.create_user_level_metric(decoded_key, current_epoch)
            if metric.metric_type == UsageType.VERB:
                _epochs_found.add(metric.epoch)
            metrics.append(metric)
        except Exception as ex:
            logger.warning(ex)

    # currently redis-qos uses 2 second TTL for 'verb_' keys:
    #  - exclude the current epoch because it may still be updated by the syslog_servers
    #  - we don't need to exclude older ones because redis wouldn't return an expired key
    epochs_found = sorted(_epochs_found)
    excluded_epochs = (
        epochs_found.pop()
        if epochs_found and epochs_found[-1] >= current_epoch
        else None
    )

    if not metrics:
        return metrics

    logger.info(
        f"curr_time={time.time():.1f} epochs_found={epochs_found} epochs_to_exclude={excluded_epochs}"
    )

    for metric in metrics:
        if metric.metric_type == UsageType.VERB and metric.epoch not in epochs_found:
            metrics.remove(metric)
            continue

        try:
            metric.load_data_from_redis(rs_conn)
        except Exception as ex:
            logger.warning(f"failed to load data from redis for {metric}", exc_info=ex)
            metrics.remove(metric)

    return metrics


#
# main process that handles gathering data from redis-qos
#
def get_redis_server_connection(redis_server: str) -> redis.Redis:
    while True:
        try:
            host, port = redis_server.split(":")
            rs_conn = redis.Redis(host=host, port=int(port), db=0)
            if not rs_conn.ping():
                raise Exception("connection established but redis is not pinging")
            return rs_conn
        except Exception as ex:
            logger.exception(f"failed to connect {redis_server}: {ex}")
            time.sleep(1)


MIN_SLEEP_TIME_SEC_PER_ROUND = 0.1  # 100 msec


def redis_consumer_process(
    metrics_queue: "MetricsQueue", redis_server: str
) -> NoReturn:
    rs_conn: redis.Redis | None = None

    logger.info("redis consumer starting")

    while True:
        try:
            if rs_conn is None or not rs_conn.ping():
                rs_conn = get_redis_server_connection(redis_server)

            start_time = time.time()
            start_epoch = int(start_time)

            redis_data = retrieve_data_from_redis(rs_conn, start_epoch)
            if len(redis_data) > 0:
                metrics_queue.put(process_data_retrieved_from_redis(redis_data))

            end_time = time.time()
            end_epoch = int(end_time)
            elapsed_time = end_time - start_time
            if start_epoch == end_epoch:
                time_sec_till_next_epoch = max(0, end_epoch + 1 - end_time)
            else:
                time_sec_till_next_epoch = 0

            if not redis_data:
                sleep_time = max(
                    MIN_SLEEP_TIME_SEC_PER_ROUND, time_sec_till_next_epoch * 0.25
                )
                logger.debug(
                    f"round did not find metrics data after elapsed_time={elapsed_time:.3f} seconds "
                    f"curr_time={end_time:.1f} check again in sleep_for={sleep_time:.3f} seconds"
                )
                time.sleep(sleep_time)
            else:
                # to avoid checking too early, wait a bit in the next epoch
                sleep_time = time_sec_till_next_epoch + MIN_SLEEP_TIME_SEC_PER_ROUND
                logger.info(
                    f"round finished in elapsed_time={elapsed_time:.3f} seconds "
                    f"sleep_for={sleep_time:.3f} seconds"
                )
                time.sleep(sleep_time)
        except Exception:
            rs_conn = None
            logger.exception("failed to process redis data")


def labels_by_metric_type(mtype: str) -> list[str]:
    label_names = ["user", "cloudStorageEndpoint", "epoch"]
    if mtype == Metric.IO_CNT_PER_VERB:
        return label_names + ["httpRequestMethod"]
    else:
        return label_names


def process_datapoints_into_metrics(
    gauges: dict[str, PrometheusExposingMetrics], datapoints: "DataPoints"
) -> int:
    latest_epoch_seen = 0
    for epoch, metrics in datapoints.items():
        for metric_type, values in metrics.items():
            for value, tags in values:
                tags["epoch"] = str(epoch)
                latest_epoch_seen = max(latest_epoch_seen, epoch)
                gauges[metric_type].gauge.labels(**tags).set(value)
                gauges[metric_type].cleanup_q.append(dict(tags))
    return latest_epoch_seen


def cleanup_old_metrics(
    gauges: dict[str, PrometheusExposingMetrics], latest_epoch_seen: int
) -> None:
    oldest_metric_age_sec = 5
    for mtype, pem in gauges.items():
        while len(pem.cleanup_q) > 0:
            next_metric_epoch = int(pem.cleanup_q[0]["epoch"])
            if latest_epoch_seen - next_metric_epoch <= oldest_metric_age_sec:
                break
            label_values = pem.cleanup_q.popleft()
            try:
                pem.gauge.remove(
                    *[label_values[label] for label in labels_by_metric_type(mtype)]
                )
            except KeyError:
                pass


def run_publisher_process(metrics_queue: "MetricsQueue") -> None:
    logger.info("metrics publisher starting")

    guages_clear_timeout_sec = 5

    start_http_server(METRICS_ENDPOINT_PORT)

    read_thru = Gauge(
        Metric.READ_THRU,
        "total downloaded data (bytes) in the given epoch for the identified user",
        labelnames=labels_by_metric_type(Metric.READ_THRU),
    )
    write_thru = Gauge(
        Metric.WRITE_THRU,
        "total uploaded data (bytes) in the given epoch for the identified user",
        labelnames=labels_by_metric_type(Metric.WRITE_THRU),
    )
    io_cnt = Gauge(
        Metric.IO_CNT_PER_VERB,
        "total number of requests for the given verb type in the given epoch for the identified user",
        labelnames=labels_by_metric_type(Metric.IO_CNT_PER_VERB),
    )
    concurrent_conns = Gauge(
        Metric.CONNECTIONS,
        "total number of concurrent connections for the identified user",
        labelnames=labels_by_metric_type(Metric.CONNECTIONS),
    )

    gauges = {
        Metric.READ_THRU: PrometheusExposingMetrics(read_thru, deque()),
        Metric.WRITE_THRU: PrometheusExposingMetrics(write_thru, deque()),
        Metric.IO_CNT_PER_VERB: PrometheusExposingMetrics(io_cnt, deque()),
        Metric.CONNECTIONS: PrometheusExposingMetrics(concurrent_conns, deque()),
    }

    latest_epoch_seen = 0
    while True:
        try:
            datapoints = metrics_queue.get(timeout=guages_clear_timeout_sec)
        except queue.Empty:
            for pem in gauges.values():
                pem.gauge.clear()
            continue
        try:
            if not datapoints:
                continue
            latest_epoch_seen = max(
                latest_epoch_seen, process_datapoints_into_metrics(gauges, datapoints)
            )
            cleanup_old_metrics(gauges, latest_epoch_seen)
        except Exception:
            logger.exception("failed to publish QoS metrics")


#
# main - initial settings and generating producer-consumer processes
#


def main(qos_redis_server: str) -> None:
    metrics_queue: "MetricsQueue" = multiprocessing.Queue()
    publisher_process = multiprocessing.Process(
        target=run_publisher_process, args=(metrics_queue,)
    )
    publisher_process.start()
    redis_consumer_process(metrics_queue, qos_redis_server)


def load_config_file(config_file: str) -> dict[str, Any]:
    with open(config_file) as f:
        return yaml.safe_load(f)  # type: ignore


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="QoS Metrics Publisher")
    parser.add_argument("config_file", help="path to config file")
    return parser.parse_args()


if __name__ == "__main__":
    config_file = load_config_file(parse_args().config_file)
    init_logger(
        re.sub(
            r"(\w+)(\.log)$",
            r"\1_exposer\2",
            config_file.get("qos_metrics_log_file_name", ""),
        ),
        config_file["log_level"],
    )
    METRICS_ENDPOINT_PORT: int = config_file.get("qos_metrics_endpoint_port", -1)

    main(config_file["redis_server"])
