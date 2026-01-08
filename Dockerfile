FROM redis:6.0.18 AS redis
WORKDIR /workspace
ENTRYPOINT ["redis-server", "/workspace/config/redis_localdev.conf"]


FROM ubuntu:24.04 AS python-base
ENV PATH=/workspace/venv/bin:$PATH
RUN <<EOF
    apt-get update
    apt-get install -y --no-install-recommends python3 python3-pip python3-venv
    apt-get clean
    rm -rf /var/lib/apt/lists/*  # Remove the apt cache lists to keep image size down
    python3 -m venv /workspace/venv
EOF

FROM python-base AS python-polygen
ARG pip_mirror=https://pypi.python.org/simple
WORKDIR /workspace
RUN mkdir -p "/workspace/logs"
COPY ./polygen/requirements.txt /workspace/
RUN pip3 install --no-cache-dir -i ${pip_mirror} -r requirements.txt
COPY ./polygen/policy_generator.py /workspace/
COPY ./polygen/weir/models/*.py /workspace/weir/models/
COPY ./polygen/weir/services/*.py /workspace/weir/services/
ENTRYPOINT ["python3", "/workspace/policy_generator.py", "/workspace/config/policy_generator_localdev.yml"]


FROM python-base AS python-polygen-metrics
ARG pip_mirror=https://pypi.python.org/simple
WORKDIR /workspace
RUN mkdir -p "/workspace/logs"
COPY ./polygen/requirements.txt /workspace/
RUN pip3 install --no-cache-dir -i ${pip_mirror} -r requirements.txt
COPY ./polygen/qos_metrics_exposer.py /workspace/
COPY ./polygen/weir/models/*.py /workspace/weir/models/
COPY ./polygen/weir/services/*.py /workspace/weir/services/
ENTRYPOINT ["python3", "/workspace/qos_metrics_exposer.py", "/workspace/config/policy_generator_localdev.yml"]


FROM python-base AS python-filesrv
ARG pip_mirror=https://pypi.python.org/simple
WORKDIR /workspace
STOPSIGNAL SIGINT
COPY ./integration-tests/randsrv.py .
RUN pip3 install --no-cache-dir -i ${pip_mirror} flask==3.0.3
ENTRYPOINT ["python3", "randsrv.py", "--port", "9000"]


FROM ubuntu:24.04 AS syslogsrv
ARG git_proxy
WORKDIR /workspace
RUN <<EOF
    apt-get update
    apt-get install -y --no-install-recommends cmake g++ make git ca-certificates
    apt-get clean
    rm -rf /var/lib/apt/lists/*  # Remove the apt cache lists to keep image size down
EOF

COPY ./syslog_server ./
RUN <<EOF
    git config --global http.proxy "${git_proxy}"
    cmake -B build -S . -D WEIR_FETCH_DEPENDENCIES=on
    cmake --build ./build -j
EOF

ENTRYPOINT ["/workspace/build/src/syslog-server", "/workspace/config/syslog_server.localdev.yml"]


FROM ubuntu:24.04 AS haproxy
ARG git_proxy
WORKDIR /workspace
STOPSIGNAL SIGUSR1
RUN <<EOF
    apt-get update
    apt-get install -y --no-install-recommends cmake g++ make git lua5.3 lua5.3-dev libpcre3-dev libssl-dev ca-certificates
    apt-get clean
    rm -rf /var/lib/apt/lists/*  # Remove the apt cache lists to keep image size down
    git config --global http.proxy "${git_proxy}"
    git config --global user.email "docker-build@example.com"
    git config --global user.name "Docker Build"
EOF

COPY ./haproxy-lua/patches/ ./patches/
COPY ./haproxy-lua/added-files/ ./added-files/
COPY ./haproxy-lua/src/ ./src/
COPY ./haproxy-lua/tests/ ./tests/
COPY ./haproxy-lua/activate.sh ./
COPY ./haproxy-lua/CMakeLists.txt ./
RUN <<EOF
    cmake -B build -S .
    cmake --build ./build
EOF

CMD ["/workspace/haproxy-source/haproxy", "-f", "/workspace/config/haproxy_localdev.conf"]
