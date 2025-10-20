#!/usr/bin/python3
# Copyright 2024 Bloomberg Finance L.P.
# Distributed under the terms of the Apache 2.0 license.

import concurrent.futures
import time

import requests

ACCESS_KEY = "user1key901234567890"
TEST_USER_CONN_LIMIT = 3


def send_req(req_num: int) -> tuple[int, int]:
    with requests.Session() as s1:
        if req_num > TEST_USER_CONN_LIMIT:
            time.sleep(0.1)  # Small sleep for violation to reach haproxy.
        print(f"Starting Request: {req_num}")

        resp = s1.get(
            "http://localhost:9001/test5.bin",
            stream=False,
            params={"x-amz-credential": ACCESS_KEY},
        )
        data = resp.content
        assert data
        return req_num, resp.status_code


def test_number_of_active_requests_is_limited() -> None:
    res: dict[int, int] = dict()

    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as execu:
        fs = {execu.submit(send_req, i): i for i in range(1, 6)}
        for f in concurrent.futures.as_completed(fs):
            req_num, status_code = f.result()
            res[req_num] = status_code

    for i in range(1, 6):
        code = 200 if i <= TEST_USER_CONN_LIMIT else 503
        print(f"Asserting request {i} response code is {code}")
        assert res[i] == code, f"Request {i} should have {code} but has {res[i]}"

    _, status_code = send_req(6)

    print("Asserting request 6 response code is 200")
    assert status_code == 200


if __name__ == "__main__":
    test_number_of_active_requests_is_limited()
