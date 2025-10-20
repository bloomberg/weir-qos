#!/usr/bin/bash

# This is intended as a quick test of the bandwidth limiting functionality.
# Run the full set of docker containers with `docker compose up` and then
# run this and watch the output to see it restrict throughput.

curl http://localhost:9001/10gb > /dev/null
