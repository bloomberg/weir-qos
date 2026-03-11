# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.
from __future__ import annotations

import argparse
import errno
import functools
import json
import logging
import logging.handlers
import os
import queue
import socket
import statistics
import sys
import threading
import time
import warnings
from collections.abc import Callable
from concurrent import futures
from dataclasses import dataclass
from enum import Enum
from hashlib import sha1
from typing import Any, Iterable, NamedTuple

import redis
import yaml
from weir.models.user_metrics import (
    UsageValue,
    UserLevelActiveRequestsUsage,
    UserLevelUsage,
)
from weir.models.violations import Violations
from weir.services.metric_service import MetricService

MB = 1048576  # 1024*1024

RELOAD_FIFO_NAME = "polygen_reload.fifo"
RELOAD_LIMITS_REQ = "reload_limits"
CACHE_LIMIT_FILE_NAME = "cache_limits.json"
DEFAULT_QOS_ID = "common"

REDIS_KEY_TYPE_VERB = "verb"
REDIS_KEY_TYPE_CONN = "conn"

QOS_VERB_LIMIT_NOT_CONFIGURED = -1
DEFAULT_VERB_RATE_LIMIT_IF_QOS_IS_NOT_CONFIGURED = 1000  # requests/sec
DEFAULT_VERB_BDW_LIMIT_IF_QOS_IS_NOT_CONFIGURED = 250  # MB/sec
DEFAULT_AREQ_LIMIT_IF_QOS_IS_NOT_CONFIGURED = 5000  # concurrent connections
VERB_LIMITING_BANDWIDTH_CATEGORY_PATTERN = "_bnd_"
AREQ_LIMITING_CATEGORY_PATTERN = "_conns"

MAX_LOG_FILE_BYTES = 100 * MB
USECS_IN_SEC = 1_000_000
MSECS_IN_SEC = 1000


class Direction(Enum):
    Up = 1
    Down = 2

    @staticmethod
    def from_str(s: str) -> Direction:
        if s == "up":
            return Direction.Up
        elif s == "dwn":
            return Direction.Down
        else:
            raise ValueError(f"Invalid connection direction: {s}")

    def __str__(self) -> str:
        return "up" if self == Direction.Up else "dwn"


class DemandKey(NamedTuple):
    user_key: str
    direction: Direction


@dataclass
class LimitConfig:
    user_to_qos_id: dict[str, str]
    qos: dict[str, dict[str, float]]


# This maps a (user-access-key, transfer-direction) tuple to:
#   A map from an HAProxy instance ID to number of active requests
# For example:
#   demand[("AKIAIOSFODNN7EXAMPLE", Direction.Up)]["831014f97da7bc6f"] = 4
# NOTE: We specifically keep the instance ID separate from the rest of the demand map key because
#       when computing limit share we need to aggregate demand for a user across all instances
DemandMap = dict[DemandKey, dict[str, int]]

HaproxyServerMap = dict[str, list["HaproxyServer"]]


def init_logger(
    logger_name: str, backup_count: int, logger_file: str | None, log_level: str
) -> logging.Logger:
    """
    General log setup.
    """
    root_logger = logging.getLogger()
    try:
        root_logger.setLevel(log_level.upper())
    except ValueError:
        root_logger.setLevel("DEBUG")

    log_formatter = logging.Formatter(
        "%(asctime)s - [%(threadName)s] - %(levelname)s - %(message)s"
    )
    if (logger_file is not None) and len(logger_file):
        file_handler = logging.handlers.RotatingFileHandler(
            logger_file, maxBytes=MAX_LOG_FILE_BYTES, backupCount=backup_count
        )
        file_handler.setFormatter(log_formatter)
        root_logger.addHandler(file_handler)
    else:
        stream_handler = logging.StreamHandler(sys.stdout)
        stream_handler.setFormatter(log_formatter)
        root_logger.addHandler(stream_handler)

    # Log all warnings (from our code or any 3rd-party code) via standard logging infrastructure
    warnings.simplefilter("default")
    logging.captureWarnings(True)

    my_logger = logging.getLogger(logger_name)
    return my_logger


def avg_time[R, **P](
    avg_run_time_list: list[float],
    sample_size: int,
    zone: str,
    logger: logging.Logger,
    num: int,
) -> Callable[[Callable[P, R]], Callable[P, R]]:
    def decorator_action(func: Callable[P, R]) -> Callable[P, R]:
        @functools.wraps(func)
        def wrapper_action(*args: P.args, **kwargs: P.kwargs) -> R:
            if len(avg_run_time_list) >= sample_size:
                avg_time = str(int(statistics.mean(avg_run_time_list)))
                logger.info(f"zone={zone} func={func.__name__} average_time={avg_time}")
                avg_run_time_list.clear()
            time_in_microsecs_begin = int(time.time() * USECS_IN_SEC)
            ret = func(*args, **kwargs)
            time_in_microsecs_end = int(time.time() * USECS_IN_SEC)
            total_run_time = time_in_microsecs_end - time_in_microsecs_begin
            avg_run_time_list.append(total_run_time / num)
            return ret

        return wrapper_action

    return decorator_action


class HaproxyServer:
    queue: queue.Queue[str]

    def __init__(
        self,
        logger: logging.Logger,
        zone: str,
        endpoint: str,
        host: str,
        port: int,
        sleep_time_milliseconds: int,
        queue_size: int,
    ) -> None:
        self.logger = logger
        self.zone = zone
        self.endpoint = endpoint
        self.host = host
        self.port = port
        self.sleep_time_milliseconds = sleep_time_milliseconds
        self.queue_max_size = queue_size
        self.queue = queue.Queue(maxsize=queue_size)
        self.haproxy_connection: socket.socket | None = None

    def _establish_new_haproxy_connection(self, remote_addr: tuple[str, int]) -> None:
        if self.haproxy_connection is not None:
            self.haproxy_connection.close()
        self.haproxy_connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.haproxy_connection.connect(remote_addr)
        self.logger.debug(f"Successfully connected to haproxy @ {remote_addr}")

    def _send_policies(self, message: str) -> None:
        if len(message) <= 0:
            return
        remote: tuple[str, int] = (self.host, self.port)
        self.logger.debug(
            f"Sending policies to endpoint {self.endpoint} haproxy {remote} message: {message}"
        )
        try_count: int = 0
        max_send_policy_tries: int = 2
        while try_count < max_send_policy_tries:
            try_count += 1
            try:
                if self.haproxy_connection is None or try_count > 1:
                    self._establish_new_haproxy_connection(remote)
                self.haproxy_connection.sendall(message.encode())  # type: ignore
            except OSError as se:  # log and retry if a socket error is received
                self.logger.warning(
                    f"Error on {try_count} attempted connections to {remote}: {se}"
                )
            except Exception as e:  # no retry
                self.logger.exception(
                    f"_send_policies had exception on remote {remote}, except: {e}"
                )
                return
            else:  # success
                return
        self.logger.error(
            f"Exhausted all {max_send_policy_tries} retries connection to {remote}"
        )

    def add_message(self, message: str) -> None:
        try:
            self.queue.put(message, block=False)
        except queue.Full:
            self.logger.error(
                f"Policy message queue for haproxy server {(self.host, self.port)} "
                f"is full! size: {self.queue.qsize()}, "
                f"max queue size: {self.queue_max_size}"
            )

    def run(self) -> None:
        self.logger.info(
            f"Start send_policies thread for haproxy_server: {(self.host, self.port)}"
        )
        avg_send_policy_run_time_list: list[float] = []

        @avg_time(avg_send_policy_run_time_list, 100, self.zone, self.logger, 1)
        def send_policy_to_haproxy(message: str) -> None:
            while True:
                try:
                    message += self.queue.get(block=False)
                except queue.Empty:
                    if message and message != "":
                        self._send_policies(
                            "".join(("policies\n", message, "\nEND_OF_POLICIES\n"))
                        )
                        message = ""
                    break
                except Exception as e:
                    self.logger.warning(f"HaproxyServer had exception, except: {e}")

                self.queue.task_done()

        while True:
            try:
                message: str = self.queue.get(block=True)
                send_policy_to_haproxy(message)
            except Exception as e:
                self.logger.warning(f"HaproxyServer had exception, except: {e}")
            finally:
                self.queue.task_done()

            time.sleep((self.sleep_time_milliseconds / 2) * 0.001)


class Policies:
    def __init__(self, logger: logging.Logger) -> None:
        self.logger = logger
        self.epoch = 0
        self.violations = Violations()

    def new_epoch(self, epoch_time: float) -> None:
        self.epoch = int(epoch_time)
        self.violations = Violations()

    def add_violation(
        self,
        epoch_time: float,
        metric: UserLevelUsage,
        verb: UsageValue,
        diff_ratio: float = 1.0,
    ) -> None:
        epoch_sec = int(epoch_time)
        if epoch_sec > self.epoch:
            self.new_epoch(epoch_time)

        self.violations.add_violation(metric, verb, diff_ratio)

    def prepare_message(
        self, haproxy_servers_map: HaproxyServerMap, epoch_time: float
    ) -> None:
        # message example:
        # Note: 1554317654056379 is the epoch in usec resolution (i.e., 1554317654.056379)
        # \n1554317654056379,user_GET,AKIAIOSFODNN7EXAMPLE:1.2,AKIAIOSFODNN8EXAMPLE:1.7\n1554317654066010,ip_PUT,1.2.3.4
        for endpoint in self.violations.endpoints():
            if endpoint not in haproxy_servers_map:
                self.logger.warning(f"Invalid endpoint: {endpoint}")
                continue

            messages = self.violations.generate_violation_message(endpoint, epoch_time)
            for message in messages:
                self.logger.info(f"Violation message: {message}")
                for server in haproxy_servers_map[endpoint]:
                    server.add_message(message)


class UnknownUsers:
    def __init__(self, report_time_seconds: int, logger: logging.Logger):
        self.users: set[str] = set()
        self.report_time_seconds = report_time_seconds
        self.logger = logger
        self.last_report_time_seconds = 0

    def add(self, user: str) -> None:
        self.users.add(user)

    def report(self) -> None:
        if self.report_time_seconds <= 0:
            return
        now = int(time.time())
        if now - self.last_report_time_seconds > self.report_time_seconds:
            self.last_report_time_seconds = now
            if len(self.users) > 0:
                self.logger.warning(f"Users with no QoS limits:{self.users}")
                self.users = set()


class PolicyGenerator:
    def __init__(self, config_file: str) -> None:
        self.config_file = config_file
        self.config = self._load_config(config_file)
        self.logger = init_logger(
            "policy_generator",
            10,
            self.config.get("log_file_name", None),
            self.config["log_level"],
        )
        self.logger.info(f"Config file {config_file} loaded.")
        report_time_seconds = self.config.get("unknown_users_report_time_seconds", 60)
        self.unknown_users = UnknownUsers(report_time_seconds, self.logger)
        self.sleep_time_milliseconds = self.config["sleep_time"]
        self.zone = self.config["zone"]
        self.haproxies, self.haproxies_map = self._get_haproxies_from_config(
            self.config
        )
        self.key_limits_path = os.path.join(
            os.path.expanduser("~"),
            "_".join(("weir", self.zone, CACHE_LIMIT_FILE_NAME)),
        )
        self.key_limits = self._load_limits_from_file(self.key_limits_path)
        self.logger.info(
            f"Initialized per-key limits (Only non-DEFAULT keys are listed): {self.key_limits}"
        )
        self.redis_keys_batch = self.config["redis_keys_batch"]
        self.polygen_lua_path = self.config["polygen_lua_path"]

        self.reqs_unblock_backoff_time_ms: int = self.config.get(
            "requests_unblock_backoff_time_ms", 200
        )
        self.reqs_unblock_ratio: float = self.config.get("requests_unblock_ratio", 0.95)
        self.blocked_users: dict[str, float] = dict()

        self.default_active_request_if_qos_not_configured = self.config.get(
            "default_active_request_if_qos_not_configured",
            DEFAULT_AREQ_LIMIT_IF_QOS_IS_NOT_CONFIGURED,
        )

        # QoS redis server
        redis_configs = self.config["redis_server"].split(":")
        self.redis_server = redis.Redis(
            host=redis_configs[0],
            port=int(redis_configs[1]),
            db=0,
            decode_responses=True,
        )
        self.logger.info(f"Connecting to redisServer {self.config['redis_server']}")

        # thread pool for checking policy violations
        self.avgChkVioRunTimeList: list[float] = []
        self._load_redis_get_fields_lua()
        self.policies = Policies(self.logger)
        self.vio_chk_thread_num = self.config["violation_check_thread_num"]
        self.vio_chk_executor = futures.ThreadPoolExecutor(self.vio_chk_thread_num)
        self.logger.info("Check_violation threadpool created.")

        # thread pool for sending policies
        self.send_policies_executor = futures.ThreadPoolExecutor(
            thread_name_prefix="send_policy", max_workers=len(self.haproxies)
        )
        self.send_policies_executor.map(self._send_policies, self.haproxies)
        self.logger.info(
            f"Send_policy threadpool created for {len(self.haproxies)} haproxies."
        )

        # fifo file for reloading limits
        self.reload_fifo_path = os.path.join(
            "/tmp", "_".join(("weir", self.config["zone"], RELOAD_FIFO_NAME))
        )
        self.make_fifo(self.reload_fifo_path)
        self.should_reload_limits = False

        self.logger.info("Creating thread to monitor reload_fifo...")
        self.x = threading.Thread(target=self._monitor_reload_fifo)
        self.x.start()

        self.logger.info("PolicyGenerator initilization completed")

    def _load_config(self, config_file: str) -> dict[str, Any]:
        try:
            with open(config_file) as f:
                config = yaml.safe_load(f)
        except yaml.YAMLError as e:
            # We need the config to setup the logger, so we have to rely on `print` instead here
            print(f"YAML error: {e}")
            raise
        except OSError:
            print(f"Could not open/load config file {config_file}")
            raise
        return config

    def _load_limits_from_file(self, file_path: str) -> LimitConfig:
        self.logger.info(f"Loading limits from file {file_path}")
        if not os.path.isfile(file_path):
            self.logger.error(f"no {file_path} existed, nothing was cached")
            return LimitConfig({}, {})
        try:
            with open(file_path, "r") as targeted_file:
                config_dict = json.load(targeted_file)
                return LimitConfig(**config_dict)
        except Exception as e:
            self.logger.exception(f"Failed to load json from {file_path} {e}")
            return LimitConfig({}, {})

    def _get_haproxies_from_config(
        self, config: dict[str, Any]
    ) -> tuple[list[HaproxyServer], HaproxyServerMap]:
        self.policy_msg_queue_size = config["policy_msg_queue_size"]

        haproxies: list[HaproxyServer] = []
        haproxies_map: HaproxyServerMap = {}
        for endpoint in config["haproxy_servers"]:
            for k in config["haproxy_servers"][endpoint]:
                items = k.strip().split(":")
                if len(items) == 2:
                    haproxy_server = HaproxyServer(
                        self.logger,
                        self.zone,
                        endpoint,
                        items[0],
                        int(items[1]),
                        config["sleep_time"],
                        self.policy_msg_queue_size,
                    )
                    haproxies.append(haproxy_server)

                    if endpoint not in haproxies_map:
                        haproxies_map[endpoint] = []
                    haproxies_map[endpoint].append(haproxy_server)

                    self.logger.info(
                        f"Haproxy machine for endpoint {endpoint} - {items[0]}:{items[1]}"
                    )
        return haproxies, haproxies_map

    def _send_policies(self, haproxy_server: HaproxyServer) -> None:
        haproxy_server.run()

    @staticmethod
    def make_fifo(fifo_path: str) -> None:
        try:
            os.mkfifo(fifo_path)
            os.chmod(fifo_path, 0o666)
        except OSError as e:
            os.chmod(fifo_path, 0o666)
            if e.errno != errno.EEXIST:
                raise

    def _monitor_reload_fifo(self) -> None:
        # users write "reload_limits" to the zone FIFO file to trigger the limits reload for this specific zone
        # example command:
        # echo "reload_limits" > /tmp/weir_dev_polygen_reload.fifo
        while True:
            with open(self.reload_fifo_path) as fifo:
                self.logger.info("Reload FIFO opened")
                while True:
                    data = fifo.read()
                    if len(data) == 0:
                        self.logger.info("Writer closed the FIFO")
                        break
                    request = data.strip()
                    if request == RELOAD_LIMITS_REQ:
                        self.logger.info("Receive FIFO reload_limits request.")
                        self.should_reload_limits = True

    def _load_redis_get_fields_lua(self) -> None:
        with open(self.polygen_lua_path) as f:
            self.redis_get_lua = f.read()
        self.redis_get_sha1 = sha1(self.redis_get_lua.encode("utf-8")).hexdigest()

    def _get_limit(self, cat: str, key: str) -> float:
        # Try to get the configured QoS ID for the user
        qos_id = self.key_limits.user_to_qos_id.get(key)
        if qos_id and isinstance(qos_id, str):
            qos_id_limits = self.key_limits.qos.get(qos_id, {})
            if isinstance(qos_id_limits, dict):
                limit = qos_id_limits.get(cat, QOS_VERB_LIMIT_NOT_CONFIGURED)
                if limit != QOS_VERB_LIMIT_NOT_CONFIGURED:
                    self.logger.debug(
                        f"For {key} {cat}, {limit} is found in the configuration"
                    )
                    return limit

        # Fallback to DEFAULT limits if not found
        self.unknown_users.add(key)
        default_policy_name = self.key_limits.user_to_qos_id.get(
            DEFAULT_QOS_ID, "DEFAULT"
        )
        qos_id_limits = self.key_limits.qos.get(default_policy_name, {})
        if isinstance(qos_id_limits, dict):
            limit = qos_id_limits.get(cat, QOS_VERB_LIMIT_NOT_CONFIGURED)
            if limit != QOS_VERB_LIMIT_NOT_CONFIGURED:
                self.logger.debug(
                    f"For {key} {cat}, {limit} is using {DEFAULT_QOS_ID} configured limit"
                )
                return limit

        # Fallback to hard-coded value if DEFAULT limits are not defined
        limit = self._use_hard_coded_limit(cat)
        self.logger.warning(f"For {key} {cat}, {limit} is using hard-coded limit")
        return limit

    def _use_hard_coded_limit(self, cat: str) -> float:
        """
        Under normal conditions, we shouldn't be here. If we are here, it means
        either DEFAULT limits are not defined (yet) or the given cat(egory) is not
        found in the configurations. At this point, we currently fallback using
        hard-coded default limits.

        Note that we currently support verb rate limiting only for 5 verb types
        PUT, GET, HEAD, DELETE and POST. Any other HTTP methods (verbs) such as
        OPTIONS, TRACE currently use a non-zero hard-coded limit regardless of
        tenancy. If such a method becomes critical for some user workflow, we can
        add it in the QoS policy and make it a tenancy-based limit.
        upload and download ("user_bnd_{up, down}") categories already have valid
        existing entries in DEFAULT limits so 'unknown category' is an issue only
        for rate-limiting.
        """
        if VERB_LIMITING_BANDWIDTH_CATEGORY_PATTERN in cat:
            return DEFAULT_VERB_BDW_LIMIT_IF_QOS_IS_NOT_CONFIGURED
        elif AREQ_LIMITING_CATEGORY_PATTERN in cat:
            return self.default_active_request_if_qos_not_configured
        else:
            return DEFAULT_VERB_RATE_LIMIT_IF_QOS_IS_NOT_CONFIGURED

    def _is_limit_reached_verb_type(
        self, cat: str, key: str, val: float
    ) -> tuple[bool, float]:
        limit = float(self._get_limit(cat, key))
        self.logger.debug(f"Limit is {limit} for {cat} {key} current val is {val}")

        # e.g., val=100 & limit=50, violation is larger than the limit by a factor of 2
        factor_mapping = {"user_bnd_up": MB, "user_bnd_dwn": MB}
        if val < limit * factor_mapping.get(cat, 1):
            return (False, 0.0)
        else:
            difference_ratio = float(val) / (limit * factor_mapping.get(cat, 1))
            return (True, float("{:.1f}".format(difference_ratio)))

    def _check_violation_per_key_verb(
        self, key: str, redis_scan_result: list[str], epoch_time: float
    ) -> None:
        """
        Example:
        key: verb_1599322430_user_AKIAIOSFODNN7EXAMPLE$dev.dc
        field/value pairs: PUT-3, GET-5, bnd_dwn-12345, bnd_up-54321
        """

        try:
            metric = MetricService.create_user_level_metric(key, int(epoch_time))
        except Exception as ex:
            self.logger.warning(f"Could not parse key {key} due to {ex}")
            return

        self.logger.debug(f"{epoch_time} {metric}")
        for i in range(0, len(redis_scan_result), 2):
            val = float(redis_scan_result[i + 1])
            limit_reached, diff_ratio = self._is_limit_reached_verb_type(
                "_".join((metric.scope.value, redis_scan_result[i])),
                metric.access_key,
                val,
            )
            if limit_reached:
                self.policies.add_violation(
                    epoch_time,
                    metric,
                    UsageValue.from_string(redis_scan_result[i]),
                    diff_ratio,
                )
            else:
                self.logger.debug(
                    f"No violation found for {metric.access_key} {metric.scope} {val}"
                )

    def _is_limit_reached_conn(self, key: str, val: int) -> tuple[bool, float]:
        cat = "user_conns"
        conn_limit = int(self._get_limit(cat, key))
        self.logger.debug(f"Limit is {conn_limit} for {cat} {key} current val is {val}")

        ratio: float = val / conn_limit
        return (ratio >= 1, ratio)

    def _check_all_conn_key_violations(
        self, keys: list[str], conn_counts: list[str], epoch_time: float
    ) -> None:
        assert len(keys) == len(conn_counts)
        unmerged_metrics: list[UserLevelActiveRequestsUsage] = []
        for string_index, raw_metric_string in enumerate(keys):
            metric = MetricService.create_user_level_metric(
                raw_metric_string, int(epoch_time)
            )
            assert isinstance(metric, UserLevelActiveRequestsUsage)
            metric.data = int(conn_counts[string_index])
            unmerged_metrics.append(metric)
        metrics = MetricService.merge_metrics_by_key(unmerged_metrics)

        for metric in metrics:
            limit_reached, diff_ratio = self._is_limit_reached_conn(
                metric.access_key, metric.data
            )
            is_blocked = metric.access_key in self.blocked_users
            ready_for_heartbeat = not is_blocked or (
                (
                    self.blocked_users[metric.access_key]
                    + (self.reqs_unblock_backoff_time_ms / MSECS_IN_SEC)
                )
                < epoch_time
            )

            self.logger.debug(f"{epoch_time} {metric}")
            if (
                (  # You're not blocked but you should be
                    limit_reached and not is_blocked
                )
                or (  # You are blocked and we've not told you for a while
                    limit_reached and ready_for_heartbeat
                )
                or (  # You're are blocked and you're below your limit but not far enough for us to unblock you
                    not limit_reached
                    and is_blocked
                    and ready_for_heartbeat
                    and (diff_ratio > self.reqs_unblock_ratio)
                )
            ):
                self.policies.add_violation(
                    epoch_time, metric, UsageValue.REQUESTS_BLOCK
                )
                self.blocked_users[metric.access_key] = epoch_time

            elif is_blocked and (diff_ratio <= self.reqs_unblock_ratio):
                # You are blocked but you shouldn't be
                self.policies.add_violation(
                    epoch_time, metric, UsageValue.REQUESTS_UNBLOCK
                )
                del self.blocked_users[metric.access_key]

    def _check_violation(
        self, keys: list[str], redis_key_type: str, epoch_time: float
    ) -> None:
        self.logger.debug(f"_check_violation for {len(keys)} keys")

        @avg_time(self.avgChkVioRunTimeList, 5000, self.zone, self.logger, len(keys))
        def check_violation_key(keys: list[str], redis_key_type: str) -> None:
            redis_scan_result = self.call_redis_eval(
                self.redis_get_lua, self.redis_get_sha1, keys
            )
            self.logger.debug(f"redis result: {redis_scan_result}")

            if redis_key_type == REDIS_KEY_TYPE_CONN:
                self._check_all_conn_key_violations(keys, redis_scan_result, epoch_time)
            else:
                for i in range(len(keys)):
                    self._check_violation_per_key_verb(
                        keys[i], redis_scan_result[i], epoch_time
                    )

        try:
            check_violation_key(keys, redis_key_type)
        except Exception as e:
            self.logger.warning("_check_violation had exception:", exc_info=e)

        self.policies.prepare_message(self.haproxies_map, epoch_time)

    def call_redis_eval(self, script: str, script_sha1: str, keys: list[str]) -> Any:
        try:
            return self.redis_server.evalsha(script_sha1, len(keys), *keys)
        except redis.exceptions.NoScriptError:
            return self.redis_server.eval(script, len(keys), *keys)

    def reload_limits(self) -> None:
        self.logger.info(f"Reloading limits from config file {self.key_limits_path}")
        self.should_reload_limits = False
        self.key_limits = self._load_limits_from_file(self.key_limits_path)
        self.logger.info(
            f"Current per-key limits (Only non-DEFAULT keys are listed): {self.key_limits} "
        )

    def submit_violation_check(
        self, keys: list[str], redis_key_type: str, epoch_time: float
    ) -> None:
        if len(keys) > 0:
            self.vio_chk_executor.submit(
                self._check_violation, keys, redis_key_type, epoch_time
            )

    def compute_bandwidth_limit_share(self, demand: DemandMap) -> DemandMap:
        """
        Takes in the aggregated demand map and outputs the limit shares to send to all haproxy instances.
        The resulting DemandMap has one entry for each user/direction/instance triplet.
        """
        keys_to_send = set([DemandKey(k.user_key, k.direction) for k in demand.keys()])

        result: DemandMap = {}
        for key in keys_to_send:
            all_instances = set([])
            current_total_user_demand = 0
            for instance_id, instance_demand in demand.get(key, {}).items():
                current_total_user_demand += instance_demand
                all_instances.add(instance_id)
            if current_total_user_demand == 0:
                continue

            user_limit = {
                Direction.Up: self._get_limit("user_bnd_up", key.user_key) * MB,
                Direction.Down: self._get_limit("user_bnd_dwn", key.user_key) * MB,
            }

            result[key] = {}
            for instance_id in all_instances:
                instance_current_demand = demand.get(key, {}).get(instance_id, 0)
                share = instance_current_demand / current_total_user_demand

                limit = int(user_limit[key.direction] * share)
                result[key][instance_id] = limit
        return result


# we query violates from redis and send back policies to haproxies
def check_loop(
    policy_generator: PolicyGenerator, redis_key_type: str, sleep_time_milliseconds: int
) -> None:
    avg_pol_gen_loop_run_time_list: list[float] = []

    @avg_time(
        avg_pol_gen_loop_run_time_list,
        1000,
        policy_generator.zone,
        policy_generator.logger,
        1,
    )
    def check_loop_epoch() -> None:
        epoch_time = time.time()
        epoch_sec = int(epoch_time)
        cursor = 0
        if redis_key_type == REDIS_KEY_TYPE_VERB:
            scan_pattern = f"verb_{epoch_sec}_*"
        elif redis_key_type == REDIS_KEY_TYPE_CONN:
            scan_pattern = "conn_*"
        else:
            raise ValueError(f"Invalid redis key type: {redis_key_type}")

        all_keys_to_check = set()
        while True:
            try:
                redis_scan_result = policy_generator.redis_server.scan(
                    cursor, match=scan_pattern, count=policy_generator.redis_keys_batch
                )
                # Redis mandates that SCAN will return 2 elements: the length of the result and an array of results
                assert isinstance(redis_scan_result, tuple)
                assert len(redis_scan_result) == 2
            except Exception as e:
                policy_generator.logger.warning(
                    f"Redis SCAN failed with exception: {e}"
                )
                policy_generator.logger.warning(
                    f"Redis result for epoch {epoch_sec}: {redis_scan_result}"
                )
                # We need to find the cause of the underlying problem if this happens.
                # We break here since the cursor in redis_scan_result might
                # have been messed up here.
                break

            epoch_time = time.time()
            if int(epoch_time) != epoch_sec:
                policy_generator.logger.debug(
                    f"Redis scan started at {epoch_time} spilled over the next second"
                )
                return

            all_keys_to_check.update(redis_scan_result[1])
            cursor = int(redis_scan_result[0])
            if cursor == 0:
                break
        if len(all_keys_to_check) > 0:
            policy_generator.submit_violation_check(
                list(all_keys_to_check), redis_key_type, epoch_time
            )

    while True:
        # check reload_limits
        if policy_generator.should_reload_limits:
            policy_generator.reload_limits()
        policy_generator.unknown_users.report()
        try:
            check_loop_epoch()
        except Exception as e:
            policy_generator.logger.warning(f"check_loop had exception, except: {e}")

        time.sleep(sleep_time_milliseconds * 0.001)


def demand_check_loop(
    policy_generator: PolicyGenerator, sleep_time_milliseconds: int
) -> None:
    avg_pol_gen_loop_run_time_list: list[float] = []

    def aggregate_demand_from_conn_v2(
        demand: DemandMap, connections: Iterable[tuple[str, str | None]]
    ) -> None:
        # Example:
        # connections:
        # [('conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev.dc`, 3), ...]
        # demand: { key: { instance: {up: 0, down: 0}}}
        for conn_key, conn_count in connections:
            # Could be None if the key we retrieve was deleted between getting the key and getting the value
            if conn_count is None:
                continue

            key_components = conn_key.split("_")
            if len(key_components) < 6:
                policy_generator.logger.warning(f"Invalid connection key {conn_key}")
                return
            direction_str, instance_id, user_endpoint = key_components[3:]
            user_key = user_endpoint.split("$")[0]

            direction = Direction.from_str(direction_str)
            key = DemandKey(user_key, direction)
            if key not in demand:
                demand[key] = {}
            if instance_id not in demand[key]:
                demand[key][instance_id] = 0
            demand[key][instance_id] += int(conn_count)

    @avg_time(
        avg_pol_gen_loop_run_time_list,
        1000,
        policy_generator.zone,
        policy_generator.logger,
        1,
    )
    def limit_share_check_loop(haproxies: list[HaproxyServer]) -> None:
        try:
            # SCAN is permitted to return duplicates. So we need to filter those out and just keep
            # a list of unique keys, otherwise we can double-count some entries
            conn_keys = list(
                set(
                    policy_generator.redis_server.scan_iter(
                        match="conn_v2_*", count=policy_generator.redis_keys_batch
                    )
                )
            )
            conn_counts = policy_generator.redis_server.mget(conn_keys)
            assert isinstance(conn_counts, list)
        except Exception as e:
            policy_generator.logger.warning(
                "Failed to collect demand info from redis", exc_info=e
            )
            return

        demand: DemandMap = {}
        aggregate_demand_from_conn_v2(demand, zip(conn_keys, conn_counts))

        epoch_ms = int(time.time() * 1000)
        limit_share = policy_generator.compute_bandwidth_limit_share(demand)
        limit_share_msgs = []
        for key in limit_share:
            user_share = []
            for instance_id in limit_share[key]:
                demand_val = limit_share[key][instance_id]
                if demand_val > 0:
                    dir_str = str(key.direction)
                    user_share.append(f"{instance_id}_{dir_str}_{demand_val}")
            limit_share_msgs.append(f"{epoch_ms},{key.user_key},{','.join(user_share)}")

        if len(limit_share_msgs) > 0:
            limit_share_str = "\n".join(
                ("limit_share", *limit_share_msgs, "end_limit_share\n")
            )
            policy_generator.logger.debug(
                f"Sending limit-share message to all HAProxies: {limit_share_str}\n"
                + f"Limit-share computed from demand: {str(demand)}"
            )
            try:
                for server in haproxies:
                    server._send_policies(limit_share_str)
            except Exception as e:
                policy_generator.logger.warning(
                    "Failed to send limit-share info to haproxy ", exc_info=e
                )
                return

    # We shouldn't need to send limit-share info anywhere near as often as we send violations
    # because that info translates to a limit that can be applied for a while, rather
    # than an instantaneous instruction to stop sending.
    # We pick a fairly arbitrary multiplier to apply to the usually-configured
    # sleep time as a starting point.
    demand_sleep_multiplier = 100
    haproxies, _ = policy_generator._get_haproxies_from_config(policy_generator.config)
    while True:
        limit_share_check_loop(haproxies)
        time.sleep(demand_sleep_multiplier * sleep_time_milliseconds * 0.001)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Weir QoS Policy Generator")
    parser.add_argument("config_file", help="Path to config file")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    config_file = args.config_file
    try:
        policy_generator = PolicyGenerator(config_file)
    except Exception as e:
        # When something happened during starting up, this program will raise Exception
        # But there might be some other threads started when initializing policy generator
        # are still running, on call person needs to stop and restart it if needed
        logging.exception(f"Policy Generator initialization failed: {e}")
        raise

    verb_thread = threading.Thread(
        target=check_loop,
        args=(
            policy_generator,
            REDIS_KEY_TYPE_VERB,
            policy_generator.sleep_time_milliseconds,
        ),
    )
    verb_thread.start()

    conn_thread = threading.Thread(
        target=check_loop,
        args=(
            policy_generator,
            REDIS_KEY_TYPE_CONN,
            policy_generator.sleep_time_milliseconds,
        ),
    )
    conn_thread.start()

    demand_thread = threading.Thread(
        target=demand_check_loop,
        args=(
            policy_generator,
            policy_generator.sleep_time_milliseconds,
        ),
    )
    demand_thread.start()

    demand_thread.join()
    verb_thread.join()
    conn_thread.join()
