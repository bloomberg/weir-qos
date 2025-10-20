#!/usr/bin/bash

# This is intended as a quick test of the bandwidth limiting functionality
# for authenticated requests specifically.
# Run the full set of docker containers with `docker compose up` and then
# run this and watch the output to see it restrict throughput.

curl -H "Authorization: AWS user1key901234567890:user1key901234567890" http://localhost:9001/10gb > /dev/null

