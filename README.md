# A Quality of Service (QoS) Framework for Distributed HTTP Servers

- [Rationale](#rationale)
- [Quick start](#quick-start)
- [Building](#building)
- [Running](#running)
- [Contributions](#contributions)
- [License](#license)
- [Code of Conduct](#code-of-conduct)
- [Security Vulnerability Reporting](#security-vulnerability-reporting)

## Rationale

This project was originally designed to provide user-level Quality of Service (QoS) for [S3 object storage](https://docs.aws.amazon.com/AmazonS3/latest/API/Welcome.html) endpoints backed by [Ceph](https://docs.ceph.com/en/quincy/radosgw/index.html).

It's not specific to the S3 API though, this framework can be used to provide user-level QoS for almost any HTTP service.
The only requirement is that a user can be uniquely identified from the header of each request.
For S3 this identifier is [a user's access key](https://docs.aws.amazon.com/AmazonS3/latest/API/RESTAuthentication.html#ConstructingTheAuthenticationHeader).

A user can be assigned several types of QoS limits:

- *Request rate limit*: Limit a user to making 100 GET requests/second, for example
- *Data transfer limit*: Limit a user to 10MB/second of upload bandwidth, for example
- *Concurrent request limit*: Limit a user to having at most 5 requests in-flight, for example

A detailed overview of the framework and its components is found at
[Architectural Overview of the QoS Framework](docs/architecture_overview.md).

## Quick Start

```sh
git clone https://github.com/bloomberg/<TBD>
cd <TBD>
docker compose up
```

This will spin up containers for each component of the system, with the main HTTP endpoint available on port `9001`.
This example setup includes a dummy HTTP server that will return any amount of data that you request (the data itself is meaningless).

For example you can see bandwidth limiting in action by running:

```sh
curl http://localhost:9001/10gb > /dev/null
```

If you want to compare that to what would have happened had it not been limited, you can make the same request to the backend server directly by going to to port `9000` instead:

```sh
curl http://localhost:9000/10gb > /dev/null
```

## Building

If you're just running the project to try it out, the docker build will handle everything for you.
The instructions below are for building outside of docker (e.g for development purposes).

The build process is orchestrated with CMake and covers building HAProxy and the syslog server.
To build the project, run following commands from root of repository:
The following commands (run from the root of the repository) will build the project and run the unit tests:

```console
cmake -B build -S . -DWEIR_FETCH_DEPENDENCIES=ON -DCMAKE_BUILD_TYPE=Debug -DWEIR_HAPROXY_REPO_URL=https://github.com/haproxy/haproxy.git
cmake --build ./build
ctest --verbose --test-dir ./build
```

The build exposes 3 flags, all of which are optional:

- [WEIR_FETCH_DEPENDENCIES](./syslog_server/README.md), to fetch and build dependencies from source.
- [WEIR_HAPROXY_REPO_URL](./haproxy-lua/README.md), to overwrite the remote from which to pull upstream haproxy source.
- PYPI_MIRROR, to overwrite the pypi URL from which to install python dependencies.

The example above also specifies `-DCMAKE_BUILD_TYPE=Debug` so that debug symbols are generated and optimisations are disabled, which makes debugging much easier during development.

## Running

A minimum reference cluster can be spun up with docker-compose by running `docker compose up` from the build directory.
Once running:

- A simple file server will be available (with QoS) at `http://localhost:9001`.
- Metrics will be available in Prometheus format at `http://localhost:9005/metrics`. If you prefer a push model for metrics, [qos_metrics_publisher.py](./polygen/qos_metrics_publisher.py) is an example of this, periodically writing metric values to stdout or file.
- Limits can be live updated by following the process [described here](./polygen/README.md).

## Contributions

We :heart: contributions.

Have you had a good experience with this project? Why not share some love and contribute code, or just let us know about any issues you had with it?

We welcome issue reports [here](../../issues); please use the [ISSUE_TEMPLATE](ISSUE_TEMPLATE.md) for your issue, so that we can be sure you're providing the necessary information.

Before sending a [Pull Request](../../pulls), please make sure you read our [Contribution Guidelines](CONTRIBUTING.md).

## License

Please read the [LICENSE](LICENSE) file.

## Code of Conduct

This project has adopted a [Code of Conduct](https://github.com/bloomberg/.github/blob/main/CODE_OF_CONDUCT.md).
If you have any concerns about the Code, or behavior which you have experienced
in the project, please contact us at opensource@bloomberg.net.

## Security Vulnerability Reporting

If you believe you have identified a security vulnerability in this project, please send email to the project
team at opensource@bloomberg.net, detailing the suspected issue and any methods you've found to reproduce it.

Please do NOT open an issue in the GitHub repository, as we'd prefer to keep vulnerability reports private until
we've had an opportunity to review and address them.
