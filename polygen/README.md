# Polygen

The policy generator (or "polygen") periodically collects usage data from Redis, compares it to configured limits for each user and if any limitations need to be applied, notifies all the HAProxy instances.

# Limit data

All limits are loaded from a file at `~/weir_<zone>_cache_limits.json` where `<zone>` is the value of the `zone` config option.

This is a JSON file that maps user identifiers to named limit tiers, and then specifies the limits for each of those tiers.
For example:

```
{
    "user_to_qos_id": {
        "user1key901234567890": "DEFAULT",
        "common": "DEFAULT"
    },
    "qos": {
        "DEFAULT": {
            "user_DELETE": 2,
            "user_GET": 2,
            "user_HEAD": 2,
            "user_POST": 2,
            "user_PUT": 2,
            "user_bnd_up": 1,
            "user_bnd_dwn": 1,
            "user_conns": 3
        }
    }
}
```

In the above `user1key901234567890` is the identifier of a user and `DEFAULT` is the name of a limit tier, with the value of each limit to apply.

Note that there is another user identifier `common`.
This is a special value used by the S3 implementation to represent any request for which no identifier can be determined (e.g if the relevant headers were not provided in a request).
The QoS ID `DEFAULT` is also special: across all implementations, any user that does not have a policy explicitly defined (IE does not exist in the `user_to_qos_id` map) will fall back to using the values defined in the `DEFAULT` policy.

# Live reload
The "dynamic file" with regularly-changing user info can also be live-reloaded so pull in changes without restarting polygen.
To achieve this, polygen monitors a posix FIFO file whose path is `/tmp/weir_<zone>_polygen_reload.fifo`. To trigger a reload, write `reload_limits` to that fifo, for example with `echo "reload_limits" > /tmp/weir_dev_polygen_reload.fifo`.
