#!/usr/bin/bash

# This is intended as a quick test of the exclusion mechanism for health checks.
# Run the full set of docker containers with `docker compose up` and then
# run this and watch the output to see it all requests succeed

for ((i = 0 ; i < 1000 ; i++)); do
    # This should never get rejected, the healthcheck URL is handled specially so that we don't accidentally take down our DNS endpoints when unauthenticated QoS gets busy
    curl -s -w "%{http_code}\n" -o /dev/null http://localhost:9001/healthcheck
    sleep 0.05
done
