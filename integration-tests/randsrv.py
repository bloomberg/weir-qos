"""
This is a simple HTTP server whose job is to send and receive arbitrary and
easily-configurable quantities of data. In particular it will accept POST
requests and read the entire request body, regardless of the amount of data
provided. It will also provide arbitrary quantities of data in response to
a GET request that specifies who much data it wants. This is done by giving
the required amount of data in the URL. For example:
`http://localhost:8080/2gb` will generate a response with 2GB of payload.
"""

import argparse
from collections.abc import Generator

from flask import Flask, request
from flask.typing import ResponseReturnValue, ResponseValue

app = Flask(__name__)

KILOBYTES = 1024
MEGABYTES = 1024 * KILOBYTES
GIGABYTES = 1024 * MEGABYTES
RESPONSE_SOURCE_DATA = (
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=" * 1000
)


def parse_data_quantity(quantity: str) -> int:
    quantity = quantity.lower()
    if quantity.endswith("g") or quantity.endswith("gb"):
        return int(float(quantity[:-2]) * GIGABYTES)
    elif quantity.endswith("m") or quantity.endswith("mb"):
        return int(float(quantity[:-2]) * MEGABYTES)
    elif quantity.endswith("k") or quantity.endswith("kb"):
        return int(float(quantity[:-2]) * KILOBYTES)
    return int(quantity)


def generate_response_data(num_bytes: int) -> Generator[str]:
    outstanding = num_bytes
    while outstanding > len(RESPONSE_SOURCE_DATA):
        yield RESPONSE_SOURCE_DATA
        outstanding -= len(RESPONSE_SOURCE_DATA)
    yield RESPONSE_SOURCE_DATA[:outstanding]


@app.route("/", methods=["GET", "POST", "PUT"])
def generate_output_noparam() -> ResponseValue:
    total_bytes_received = 0
    bytes_received = 1
    while bytes_received != 0:
        bytes_received = len(request.stream.read(4 * KILOBYTES))
        total_bytes_received += bytes_received
    print(
        f"{request.method} request has {total_bytes_received} bytes of actual content"
    )
    default_response_bytes = 32
    return generate_response_data(default_response_bytes)


@app.route("/<data_quantity>", methods=["GET"])
def generate_output(data_quantity: str) -> ResponseReturnValue:
    try:
        num_bytes = parse_data_quantity(data_quantity)
    except Exception:
        print(f"Invalid GET data quantity: {data_quantity}, defaulting to 0 bytes...")
        num_bytes = 0
    print(f"Received GET request for {num_bytes} bytes")

    return generate_response_data(num_bytes), {"Content-Type": "text/plain"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-p", "--port", help="Port on which the HTTP server should listen", default=8080
    )
    args = parser.parse_args()

    app.run(port=int(args.port))
