# A Quality of Service (QoS) Framework for Distributed HTTP Servers

- [Rationale](#rationale)
- [Quick start](#quick-start)
- [Building](#building)
- [Running](#running)
- [Contributions](#contributions)
- [License](#license)
- [Code of Conduct](#code-of-conduct)

## Rationale

This project was originally designed to provide user-level Quality of Service (QoS) for [S3 object storage](https://docs.aws.amazon.com/AmazonS3/latest/API/Welcome.html) endpoints backed by [Ceph](https://docs.ceph.com/en/latest/radosgw/).

It is not specific to the S3 API though. This framework can be used to provide user-level QoS for almost any HTTP service.
The only requirement is that a user can be uniquely identified from the header of each request.
For S3, this identifier is [a user's access key](https://docs.aws.amazon.com/AmazonS3/latest/API/RESTAuthentication.html#ConstructingTheAuthenticationHeader).

A user can be assigned several types of QoS limits:

- *Request rate limit*: Limit a user to making 100 GET requests/second, for example
- *Data transfer limit*: Limit a user to 10 MB/second of upload bandwidth, for example
- *Concurrent request limit*: Limit a user to having at most 5 requests in-flight, for example

A detailed overview of the framework and its components can be found here:
[Architectural Overview of the QoS Framework](docs/architecture_overview.md).

## Quick Start

```sh
git clone https://github.com/bloomberg/weir-qos.git
cd weir-qos
docker compose up
```

This will spin up containers for each component of the system, with the main HTTP endpoint available on port `9001`.
This example setup includes a dummy HTTP server that will return any amount of data that you request (the data itself is meaningless).

For example, you can see bandwidth limiting in action by running:

```sh
curl http://localhost:9001/10gb > /dev/null
```

If you want to compare that to what would have happened had it not been limited, you can make the same request to the back-end server directly by going to port `9000` instead:

```sh
curl http://localhost:9000/10gb > /dev/null
```

Note that for now this depends on Docker's host networking mode, which is not supported on Mac.

## Building

If you're just running the project to try it out, the Docker build will handle everything for you.
The instructions below are for building it outside of Docker (e.g., for development purposes).

The build process is orchestrated with CMake and covers building HAProxy and the syslog server.
To build the project and run the unit tests, run the following commands from the root of the repository:

```console
cmake -B build -S . -DWEIR_FETCH_DEPENDENCIES=ON -DCMAKE_BUILD_TYPE=Debug -DWEIR_HAPROXY_REPO_URL=https://github.com/haproxy/haproxy.git
cmake --build ./build
ctest --verbose --test-dir ./build
```

The build exposes several flags, all of which are optional:

- [WEIR_FETCH_DEPENDENCIES](./syslog_server/README.md), to fetch and build dependencies from the source.
- [WEIR_HAPROXY_REPO_URL](./haproxy-lua/README.md), to overwrite the remote from which to pull the upstream haproxy source.
- PYPI_MIRROR, to overwrite the PyPI URL from which to install Python dependencies.

The example above also specifies `-DCMAKE_BUILD_TYPE=Debug` so that debug symbols are generated and optimisations are disabled. This makes debugging much easier during development.

## Running

A minimum reference cluster can be spun up with docker-compose by running `docker compose up` from the build directory.
Once it is running:

- A simple file server will be available (with QoS) at `http://localhost:9001`.
- Metrics will be available in Prometheus format at `http://localhost:9005/metrics`. If you prefer a push model for metrics, [qos_metrics_publisher.py](./polygen/qos_metrics_publisher.py) is an example of this, in which metric values are periodically written to stdout or file.
- Limits can be updated live by following the process [described here](./polygen/README.md).

## Contributing

Contributions are what make the open source community such an amazing place to
learn, inspire, and create. Any contributions you make are **greatly
appreciated**. For detailed contributing guidelines, please see
[CONTRIBUTING.md](CONTRIBUTING.md)

## License

Distributed under the Apache-2.0 License. See [LICENSE](LICENSE) for more information.

## Code of Conduct

This project has adopted a [Code of Conduct](https://github.com/bloomberg/.github/blob/main/CODE_OF_CONDUCT.md).
If you have any concerns about the Code, or behavior which you have experienced
in the project, please contact us at opensource@bloomberg.net.
