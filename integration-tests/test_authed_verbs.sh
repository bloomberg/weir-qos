#!/usr/bin/bash

# This is intended as a quick test of the request rate limiting functionality
# for authenticated requests specifically.
# Run the full set of docker containers with `docker compose up` and then
# run this and watch the output to see it restrict throughput.

for ((i = 0 ; i < 10000 ; i++)); do
    curl -s -w "%{http_code}\n" -o /dev/null -H "Authorization: AWS user1key901234567890:user1key901234567890" http://localhost:9001/128
    sleep 0.05
done
