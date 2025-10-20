# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.
from abc import ABC, abstractmethod
from enum import Enum
from typing import TYPE_CHECKING, Any, no_type_check

import redis

if TYPE_CHECKING:
    DataPoints = dict[int, dict[str, list[tuple[int, dict[str, str]]]]]


class UsageScope(Enum):
    USER_LEVEL = "user"
    BUCKET_LEVEL = "bucket"
    IP_LEVEL = "ip"


class UsageType(Enum):
    UNKNOWN = 0
    VERB = 1
    ACTIVE_REQUESTS = 2


class UsageValue(Enum):
    VERB_GET = "GET"
    VERB_PUT = "PUT"
    VERB_POST = "POST"
    VERB_DELETE = "DELETE"
    VERB_HEAD = "HEAD"
    VERB_LISTOBJECTSV2 = "LISTOBJECTSV2"
    VERB_LISTMULTIPARTUPLOADS = "LISTMULTIPARTUPLOADS"
    VERB_LISTOBJECTVERSIONS = "LISTOBJECTVERSIONS"
    VERB_LISTBUCKETS = "LISTBUCKETS"
    VERB_LISTOBJECTS = "LISTOBJECTS"
    VERB_GETOBJECT = "GETOBJECT"
    VERB_DELETEOBJECTS = "DELETEOBJECTS"
    VERB_DELETEOBJECT = "DELETEOBJECT"
    VERB_CREATEBUCKET = "CREATEBUCKET"
    THRU_TYPE_READ = "bnd_dwn"
    THRU_TYPE_WRITE = "bnd_up"
    REQUESTS_BLOCK = "reqs_block"
    REQUESTS_UNBLOCK = "reqs_unblock"

    @staticmethod
    def from_string(s: str) -> "UsageValue":
        metric_val_dict = {
            "bnd_up": UsageValue.THRU_TYPE_WRITE,
            "bnd_dwn": UsageValue.THRU_TYPE_READ,
            "get": UsageValue.VERB_GET,
            "put": UsageValue.VERB_PUT,
            "post": UsageValue.VERB_POST,
            "delete": UsageValue.VERB_DELETE,
            "listobjectsv2": UsageValue.VERB_LISTOBJECTSV2,
            "listmultipartuploads": UsageValue.VERB_LISTMULTIPARTUPLOADS,
            "listobjectversions": UsageValue.VERB_LISTOBJECTVERSIONS,
            "listbuckets": UsageValue.VERB_LISTBUCKETS,
            "listobjects": UsageValue.VERB_LISTOBJECTS,
            "getobject": UsageValue.VERB_GETOBJECT,
            "deleteobjects": UsageValue.VERB_DELETEOBJECTS,
            "deleteobject": UsageValue.VERB_DELETEOBJECT,
            "createbucket": UsageValue.VERB_CREATEBUCKET,
            "head": UsageValue.VERB_HEAD,
            "reqs_block": UsageValue.REQUESTS_BLOCK,
            "reqs_unblock": UsageValue.REQUESTS_UNBLOCK,
        }

        return metric_val_dict[s.lower()]

    @staticmethod
    def verb_values() -> list["UsageValue"]:
        return [
            UsageValue.VERB_GET,
            UsageValue.VERB_PUT,
            UsageValue.VERB_POST,
            UsageValue.VERB_DELETE,
            UsageValue.VERB_HEAD,
            UsageValue.VERB_LISTOBJECTSV2,
            UsageValue.VERB_LISTMULTIPARTUPLOADS,
            UsageValue.VERB_LISTOBJECTVERSIONS,
            UsageValue.VERB_LISTBUCKETS,
            UsageValue.VERB_LISTOBJECTS,
            UsageValue.VERB_GETOBJECT,
            UsageValue.VERB_DELETEOBJECTS,
            UsageValue.VERB_DELETEOBJECT,
            UsageValue.VERB_CREATEBUCKET,
        ]

    @staticmethod
    def throughput_values() -> list["UsageValue"]:
        return [UsageValue.THRU_TYPE_WRITE, UsageValue.THRU_TYPE_READ]

    @staticmethod
    def requests_values() -> list["UsageValue"]:
        return [UsageValue.REQUESTS_BLOCK, UsageValue.REQUESTS_UNBLOCK]


class Metric:
    # metric names to be reported
    READ_THRU = "down_bandwidth_used"
    WRITE_THRU = "up_bandwidth_used"
    IO_CNT_PER_VERB = "requests_sec"
    CONNECTIONS = "connections"


class UserLevelUsage(ABC):
    key: str = ""
    epoch: int = 0
    access_key: str = ""
    metric_type: UsageType = UsageType.UNKNOWN
    endpoint: str = ""
    scope: UsageScope = UsageScope.USER_LEVEL

    @abstractmethod
    def load_data_from_redis(self, rs_conn: redis.Redis) -> None:
        pass

    @abstractmethod
    def load_data_into_datapoints(self, datapoints: "DataPoints") -> None:
        pass

    @abstractmethod
    def merge_from(self, other: "UserLevelUsage") -> None:
        pass

    def get_standard_tags(self) -> dict[str, str]:
        tags = {}
        if self.access_key:
            tags["user"] = self.access_key

        if self.endpoint:
            tags["cloudStorageEndpoint"] = self.endpoint

        return tags

    def __str__(self) -> str:
        return f"{self.endpoint} {self.scope} {self.access_key}"

    @no_type_check
    @staticmethod
    def decode_redis_data(input_redis_data: Any) -> Any:
        if input_redis_data is None:
            return None
        if isinstance(input_redis_data, (str, int)):
            decoded_redis_data = input_redis_data
        elif isinstance(input_redis_data, bytes):
            decoded_redis_data = input_redis_data.decode()
        elif isinstance(input_redis_data, (list, tuple)):
            decoded_redis_data = []
            for i in input_redis_data:
                decoded_redis_data.append(UserLevelUsage.decode_redis_data(i))
            if isinstance(input_redis_data, tuple):
                decoded_redis_data = tuple(decoded_redis_data)
        elif isinstance(input_redis_data, dict):
            decoded_redis_data = {}
            for k, v in input_redis_data.items():
                decoded_redis_data[UserLevelUsage.decode_redis_data(k)] = (
                    UserLevelUsage.decode_redis_data(v)
                )
        else:
            raise Exception(
                f"data {input_redis_data} has unexpected type {type(input_redis_data)}"
            )
        return decoded_redis_data


class UserLevelVerbUsage(UserLevelUsage):
    def __init__(self, key: str, epoch: int, acc_key: str) -> None:
        self.epoch = epoch
        self.key = key
        self.metric_type = UsageType.VERB
        self.data: dict[str, int] = dict()

        if len(acc_key.split("$")) != 2:
            raise Exception(
                f"invalid user access key and endpoint pair: {acc_key} for {key}"
            )

        self.access_key, self.endpoint = acc_key.split("$")

    def load_data_from_redis(self, rs_conn: redis.Redis) -> None:
        encoded_data = rs_conn.hgetall(self.key)
        decoded_data = UserLevelUsage.decode_redis_data(encoded_data)
        if decoded_data is not None:
            self.data = decoded_data

    def load_data_into_datapoints(self, datapoints: "DataPoints") -> None:
        if not self.data:
            return

        download_thru = int(self.data.get(UsageValue.THRU_TYPE_READ.value, 0))
        upload_thru = int(self.data.get(UsageValue.THRU_TYPE_WRITE.value, 0))
        tags = self.get_standard_tags()
        if download_thru != 0:
            datapoints[self.epoch][Metric.READ_THRU].append((download_thru, tags))

        if upload_thru != 0:
            datapoints[self.epoch][Metric.WRITE_THRU].append((upload_thru, tags))

        for v in UsageValue.verb_values():
            verb_io_cnt = int(self.data.get(v.value, 0))
            if verb_io_cnt != 0:
                datapoints[self.epoch][Metric.IO_CNT_PER_VERB].append(
                    (verb_io_cnt, self.add_verb_tag(tags, v.value))
                )

    def merge_from(self, other: "UserLevelUsage") -> None:
        raise NotImplementedError(
            "Verb usage merging is not currently supported and should not currently be needed"
        )

    def add_verb_tag(self, tags: dict[str, str], verb: str) -> dict[str, str]:
        new_tags = dict(tags)
        new_tags["httpRequestMethod"] = verb

        return new_tags


class UserLevelActiveRequestsUsage(UserLevelUsage):
    def __init__(self, key: str, epoch: int) -> None:
        """
        Example:
        key: conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev.dc
        """
        self.epoch = epoch
        self.key = key
        self.metric_type = UsageType.ACTIVE_REQUESTS
        self.data = 0

        items = key.split("_")
        assert items[0] == "conn"
        if len(items) < 2:
            raise Exception(f"Invalid active-requests key {key}: Key too short")

        if items[1] == "user":
            if len(items) != 3:
                raise Exception(f"Invalid v1 active-requests key {key}")
            self.direction = None
            self.instance_id = None
            acc_key = items[2]
        elif items[1] == "v2":
            if not (len(items) == 6 and items[2] == "user"):
                raise Exception(f"Invalid v2 active-requests key {key}")
            self.direction = items[3]
            self.instance_id = items[4]
            acc_key = items[5]
        else:
            raise Exception(f"Invalid active-requests key {key}: Unrecognised version")

        if len(acc_key.split("$")) != 2:
            raise Exception(
                f"invalid user accesskey and endpoint pair: {acc_key} for {key}"
            )

        self.access_key, self.endpoint = acc_key.split("$")

    def load_data_from_redis(self, rs_conn: redis.Redis) -> None:
        # If we have an instance-id then we're using the new format
        if self.instance_id:
            # We might get None back from redis if the key doesn't exist/has been deleted by the time we get here
            decoded_data = UserLevelUsage.decode_redis_data(rs_conn.get(self.key))
            if decoded_data is not None:
                self.data = int(decoded_data)
        else:
            self.data = UserLevelUsage.decode_redis_data(rs_conn.scard(self.key))

    def load_data_into_datapoints(self, datapoints: "DataPoints") -> None:
        if self.data != 0:
            datapoints[self.epoch][Metric.CONNECTIONS].append(
                (self.data, self.get_standard_tags())
            )

    def merge_from(self, other: UserLevelUsage) -> None:
        assert isinstance(other, UserLevelActiveRequestsUsage)
        assert self.access_key == other.access_key
        self.data += other.data
